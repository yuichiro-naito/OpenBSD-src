/*	$OpenBSD: gmon.c,v 1.32 2020/10/12 22:08:33 deraadt Exp $ */
/*-
 * Copyright (c) 1983, 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/time.h>
#include <sys/gmon.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

struct gmonparam _gmonparam = { GMON_PROF_OFF };

#include <pthread.h>
#include <thread_private.h>

SLIST_HEAD(, gmonparam) _gmonfree = SLIST_HEAD_INITIALIZER(_gmonfree);
SLIST_HEAD(, gmonparam) _gmoninuse = SLIST_HEAD_INITIALIZER(_gmoninuse);
void* _gmonlock = NULL;
pthread_key_t _gmonkey;
struct gmonparam _gmondummy;

static int	s_scale;
/* see profil(2) where this is describe (incorrectly) */
#define		SCALE_1_TO_1	0x10000L

#define ERR(s) write(STDERR_FILENO, s, sizeof(s))

PROTO_NORMAL(moncontrol);
PROTO_DEPRECATED(monstartup);
static int hertz(void);

static void _gmon_destructor(void *);
struct gmonparam *_gmon_alloc(void);
static void _gmon_merge(void);
static void _gmon_merge_two(struct gmonparam *, struct gmonparam *);

void
monstartup(u_long lowpc, u_long highpc)
{
	int o;
	void *addr;
	struct gmonparam *p = &_gmonparam;

	/*
	 * round lowpc and highpc to multiples of the density we're using
	 * so the rest of the scaling (here and in gprof) stays in ints.
	 */
	p->lowpc = ROUNDDOWN(lowpc, HISTFRACTION * sizeof(HISTCOUNTER));
	p->highpc = ROUNDUP(highpc, HISTFRACTION * sizeof(HISTCOUNTER));
	p->textsize = p->highpc - p->lowpc;
	p->kcountsize = p->textsize / HISTFRACTION;
	p->hashfraction = HASHFRACTION;
	p->fromssize = p->textsize / p->hashfraction;
	p->tolimit = p->textsize * ARCDENSITY / 100;
	if (p->tolimit < MINARCS)
		p->tolimit = MINARCS;
	else if (p->tolimit > MAXARCS)
		p->tolimit = MAXARCS;
	p->tossize = p->tolimit * sizeof(struct tostruct);

	addr = mmap(NULL, p->kcountsize,  PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE, -1, 0);
	if (addr == MAP_FAILED)
		goto mapfailed;
	p->kcount = addr;

	addr = mmap(NULL, p->fromssize,  PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE, -1, 0);
	if (addr == MAP_FAILED)
		goto mapfailed;
	p->froms = addr;

	addr = mmap(NULL, p->tossize,  PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE, -1, 0);
	if (addr == MAP_FAILED)
		goto mapfailed;
	p->tos = addr;
	p->tos[0].link = 0;

	o = p->highpc - p->lowpc;
	if (p->kcountsize < o) {
#ifndef notdef
		s_scale = ((float)p->kcountsize / o ) * SCALE_1_TO_1;
#else /* avoid floating point */
		int quot = o / p->kcountsize;

		if (quot >= 0x10000)
			s_scale = 1;
		else if (quot >= 0x100)
			s_scale = 0x10000 / quot;
		else if (o >= 0x800000)
			s_scale = 0x1000000 / (o / (p->kcountsize >> 8));
		else
			s_scale = 0x1000000 / ((o << 8) / p->kcountsize);
#endif
	} else
		s_scale = SCALE_1_TO_1;

	_gmondummy.state = GMON_PROF_BUSY;
	pthread_key_create(&_gmonkey, _gmon_destructor);

	moncontrol(1);
	return;

mapfailed:
	if (p->kcount != NULL) {
		munmap(p->kcount, p->kcountsize);
		p->kcount = NULL;
	}
	if (p->froms != NULL) {
		munmap(p->froms, p->fromssize);
		p->froms = NULL;
	}
	if (p->tos != NULL) {
		munmap(p->tos, p->tossize);
		p->tos = NULL;
	}
	ERR("monstartup: out of memory\n");
}
__strong_alias(_monstartup,monstartup);

static void
_gmon_destructor(void *arg)
{
	struct gmonparam *p = arg, *q, **prev;

	if (p == &_gmondummy)
		return;

	pthread_setspecific(_gmonkey, &_gmondummy);

	_MUTEX_LOCK(&_gmonlock);
	SLIST_REMOVE(&_gmoninuse, p, gmonparam, next);
	SLIST_INSERT_HEAD(&_gmonfree, p, next);
	_MUTEX_UNLOCK(&_gmonlock);

	pthread_setspecific(_gmonkey, NULL);
}

struct gmonparam *
_gmon_alloc(void)
{
	void *addr;
	struct gmonparam *p;

	_MUTEX_LOCK(&_gmonlock);
	p = SLIST_FIRST(&_gmonfree);
	if (p != NULL) {
		SLIST_REMOVE_HEAD(&_gmonfree, next);
		SLIST_INSERT_HEAD(&_gmoninuse, p ,next);
	} else {
		_MUTEX_UNLOCK(&_gmonlock);
		p = mmap(NULL, sizeof (struct gmonparam),
			 PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
		if (p == MAP_FAILED)
			goto mapfailed_2;
		*p = _gmonparam;
		p->kcount = NULL;
		p->kcountsize = 0;
		p->froms = NULL;
		p->tos = NULL;
		addr = mmap(NULL, p->fromssize, PROT_READ|PROT_WRITE,
			    MAP_ANON|MAP_PRIVATE, -1, 0);
		if (addr == MAP_FAILED)
			goto mapfailed;
		p->froms = addr;

		addr = mmap(NULL, p->tossize, PROT_READ|PROT_WRITE,
			    MAP_ANON|MAP_PRIVATE, -1, 0);
		if (addr == MAP_FAILED)
			goto mapfailed;
		p->tos = addr;
		_MUTEX_LOCK(&_gmonlock);
		SLIST_INSERT_HEAD(&_gmoninuse, p ,next);
	}
	_MUTEX_UNLOCK(&_gmonlock);
	pthread_setspecific(_gmonkey, p);

	return p;

mapfailed:
	if (p->froms != NULL) {
		munmap(p->froms, p->fromssize);
		p->froms = NULL;
	}
	if (p->tos != NULL) {
		munmap(p->tos, p->tossize);
		p->tos = NULL;
	}
mapfailed_2:
	pthread_setspecific(_gmonkey, NULL);
	ERR("_gmon_alloc: out of memory\n");
	return NULL;
}

static void
_gmon_merge_two(struct gmonparam *p, struct gmonparam *q)
{
	u_long fromindex;
	u_short *frompcindex, qtoindex, toindex;
	u_long selfpc;
	u_long endfrom;
	long count;
	struct tostruct *top;

	endfrom = (q->fromssize / sizeof(*q->froms));
	for (fromindex = 0; fromindex < endfrom; fromindex++) {
		if (q->froms[fromindex] == 0)
			continue;
		for (qtoindex = q->froms[fromindex]; qtoindex != 0;
		     qtoindex = q->tos[qtoindex].link) {
			selfpc = q->tos[qtoindex].selfpc;
			count = q->tos[qtoindex].count;
			/* cribbed from mcount */
			frompcindex = &p->froms[fromindex];
			toindex = *frompcindex;
			if (toindex == 0) {
				/*
				 *      first time traversing this arc
				 */
				toindex = ++p->tos[0].link;
				if (toindex >= p->tolimit)
					/* halt further profiling */
					goto overflow;

				*frompcindex = (u_short)toindex;
				top = &p->tos[(size_t)toindex];
				top->selfpc = selfpc;
				top->count = count;
				top->link = 0;
				goto done;
			}
			top = &p->tos[(size_t)toindex];
			if (top->selfpc == selfpc) {
				/*
				 * arc at front of chain; usual case.
				 */
				top->count+= count;
				goto done;
			}
			/*
			 * have to go looking down chain for it.
			 * top points to what we are looking at,
			 * we know it is not at the head of the chain.
			 */
			for (; /* goto done */; ) {
				if (top->link == 0) {
					/*
					 * top is end of the chain and
					 * none of the chain had
					 * top->selfpc == selfpc.  so
					 * we allocate a new tostruct
					 * and link it to the head of
					 * the chain.
					 */
					toindex = ++p->tos[0].link;
					if (toindex >= p->tolimit)
						goto overflow;
					top = &p->tos[(size_t)toindex];
					top->selfpc = selfpc;
					top->count = count;
					top->link = *frompcindex;
					*frompcindex = (u_short)toindex;
					goto done;
				}
				/*
				 * otherwise, check the next arc on the chain.
				 */
				top = &p->tos[top->link];
				if (top->selfpc == selfpc) {
					/*
					 * there it is.
					 * add to its count.
					 */
					top->count += count;
					goto done;
				}

			}

		done: ;
		}

	}
overflow: ;

}

static void
_gmon_merge(void)
{
	struct gmonparam *q;

	_MUTEX_LOCK(&_gmonlock);

	SLIST_FOREACH(q, &_gmonfree, next)
		_gmon_merge_two(&_gmonparam, q);

	SLIST_FOREACH(q, &_gmoninuse, next) {
		q->state = GMON_PROF_OFF;
		_gmon_merge_two(&_gmonparam, q);
	}

	_MUTEX_UNLOCK(&_gmonlock);
}


void
_mcleanup(void)
{
	int fd;
	int fromindex;
	int endfrom;
	u_long frompc;
	int toindex;
	struct rawarc rawarc;
	struct gmonparam *p = &_gmonparam;
	struct gmonhdr gmonhdr, *hdr;
	struct clockinfo clockinfo;
	const int mib[2] = { CTL_KERN, KERN_CLOCKRATE };
	size_t size;
	char *profdir;
	char *proffile;
	char  buf[PATH_MAX];
#ifdef DEBUG
	int log, len;
	char dbuf[200];
#endif

	if (p->state == GMON_PROF_ERROR)
		ERR("_mcleanup: tos overflow\n");

	size = sizeof(clockinfo);
	if (sysctl(mib, 2, &clockinfo, &size, NULL, 0) == -1) {
		/*
		 * Best guess
		 */
		clockinfo.profhz = hertz();
	} else if (clockinfo.profhz == 0) {
		if (clockinfo.hz != 0)
			clockinfo.profhz = clockinfo.hz;
		else
			clockinfo.profhz = hertz();
	}

	moncontrol(0);

	if (issetugid() == 0 && (profdir = getenv("PROFDIR")) != NULL) {
		char *s, *t, *limit;
		pid_t pid;
		long divisor;

		/* If PROFDIR contains a null value, no profiling
		   output is produced */
		if (*profdir == '\0') {
			return;
		}

		limit = buf + sizeof buf - 1 - 10 - 1 -
		    strlen(__progname) - 1;
		t = buf;
		s = profdir;
		while((*t = *s) != '\0' && t < limit) {
			t++;
			s++;
		}
		*t++ = '/';

		/*
		 * Copy and convert pid from a pid_t to a string.  For
		 * best performance, divisor should be initialized to
		 * the largest power of 10 less than PID_MAX.
		 */
		pid = getpid();
		divisor=10000;
		while (divisor > pid) divisor /= 10;	/* skip leading zeros */
		do {
			*t++ = (pid/divisor) + '0';
			pid %= divisor;
		} while (divisor /= 10);
		*t++ = '.';

		s = __progname;
		while ((*t++ = *s++) != '\0')
			;

		proffile = buf;
	} else {
		proffile = "gmon.out";
	}

	fd = open(proffile , O_CREAT|O_TRUNC|O_WRONLY, 0664);
	if (fd == -1) {
		perror( proffile );
		return;
	}
#ifdef DEBUG
	log = open("gmon.log", O_CREAT|O_TRUNC|O_WRONLY, 0664);
	if (log == -1) {
		perror("mcount: gmon.log");
		close(fd);
		return;
	}
	snprintf(dbuf, sizeof dbuf, "[mcleanup1] kcount 0x%x ssiz %d\n",
	    p->kcount, p->kcountsize);
	write(log, dbuf, strlen(dbuf));
#endif

	_gmon_merge();

	hdr = (struct gmonhdr *)&gmonhdr;
	bzero(hdr, sizeof(*hdr));
	hdr->lpc = p->lowpc;
	hdr->hpc = p->highpc;
	hdr->ncnt = p->kcountsize + sizeof(gmonhdr);
	hdr->version = GMONVERSION;
	hdr->profrate = clockinfo.profhz;
	write(fd, (char *)hdr, sizeof *hdr);
	write(fd, p->kcount, p->kcountsize);
	endfrom = p->fromssize / sizeof(*p->froms);
	for (fromindex = 0; fromindex < endfrom; fromindex++) {
		if (p->froms[fromindex] == 0)
			continue;

		frompc = p->lowpc;
		frompc += fromindex * p->hashfraction * sizeof(*p->froms);
		for (toindex = p->froms[fromindex]; toindex != 0;
		     toindex = p->tos[toindex].link) {
#ifdef DEBUG
			(void) snprintf(dbuf, sizeof dbuf,
			"[mcleanup2] frompc 0x%x selfpc 0x%x count %d\n" ,
				frompc, p->tos[toindex].selfpc,
				p->tos[toindex].count);
			write(log, dbuf, strlen(dbuf));
#endif
			rawarc.raw_frompc = frompc;
			rawarc.raw_selfpc = p->tos[toindex].selfpc;
			rawarc.raw_count = p->tos[toindex].count;
			write(fd, &rawarc, sizeof rawarc);
		}
	}
	close(fd);
#ifdef notyet
	if (p->kcount != NULL) {
		munmap(p->kcount, p->kcountsize);
		p->kcount = NULL;
	}
	if (p->froms != NULL) {
		munmap(p->froms, p->fromssize);
		p->froms = NULL;
	}
	if (p->tos != NULL) {
		munmap(p->tos, p->tossize);
		p->tos = NULL;
	}
#endif
}

/*
 * Control profiling
 *	profiling is what mcount checks to see if
 *	all the data structures are ready.
 */
void
moncontrol(int mode)
{
	struct gmonparam *p = &_gmonparam;

	if (mode) {
		/* start */
		profil((char *)p->kcount, p->kcountsize, p->lowpc,
		    s_scale);
		p->state = GMON_PROF_ON;
	} else {
		/* stop */
		profil(NULL, 0, 0, 0);
		p->state = GMON_PROF_OFF;
	}
}
DEF_WEAK(moncontrol);

/*
 * discover the tick frequency of the machine
 * if something goes wrong, we return 0, an impossible hertz.
 */
static int
hertz(void)
{
	struct itimerval tim;

	tim.it_interval.tv_sec = 0;
	tim.it_interval.tv_usec = 1;
	tim.it_value.tv_sec = 0;
	tim.it_value.tv_usec = 0;
	setitimer(ITIMER_REAL, &tim, 0);
	setitimer(ITIMER_REAL, 0, &tim);
	if (tim.it_interval.tv_usec < 2)
		return(0);
	return (1000000 / tim.it_interval.tv_usec);
}
