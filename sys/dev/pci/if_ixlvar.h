/*	$Id$ */

/*
 * Copyright (c) 2013-2015, Intel Corporation
 * All rights reserved.

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *  3. Neither the name of the Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 2016,2017 David Gwynne <dlg@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _IXL_VAR_H_
#define _IXL_VAR_H_

/* Packet Classifier Types for filters */
/* bits 0-28 are reserved for future use */
#define IXL_PCT_NONF_IPV4_UDP_UCAST	(1ULL << 29)	/* 722 */
#define IXL_PCT_NONF_IPV4_UDP_MCAST	(1ULL << 30)	/* 722 */
#define IXL_PCT_NONF_IPV4_UDP		(1ULL << 31)
#define IXL_PCT_NONF_IPV4_TCP_SYN_NOACK	(1ULL << 32)	/* 722 */
#define IXL_PCT_NONF_IPV4_TCP		(1ULL << 33)
#define IXL_PCT_NONF_IPV4_SCTP		(1ULL << 34)
#define IXL_PCT_NONF_IPV4_OTHER		(1ULL << 35)
#define IXL_PCT_FRAG_IPV4		(1ULL << 36)
/* bits 37-38 are reserved for future use */
#define IXL_PCT_NONF_IPV6_UDP_UCAST	(1ULL << 39)	/* 722 */
#define IXL_PCT_NONF_IPV6_UDP_MCAST	(1ULL << 40)	/* 722 */
#define IXL_PCT_NONF_IPV6_UDP		(1ULL << 41)
#define IXL_PCT_NONF_IPV6_TCP_SYN_NOACK	(1ULL << 42)	/* 722 */
#define IXL_PCT_NONF_IPV6_TCP		(1ULL << 43)
#define IXL_PCT_NONF_IPV6_SCTP		(1ULL << 44)
#define IXL_PCT_NONF_IPV6_OTHER		(1ULL << 45)
#define IXL_PCT_FRAG_IPV6		(1ULL << 46)
/* bit 47 is reserved for future use */
#define IXL_PCT_FCOE_OX			(1ULL << 48)
#define IXL_PCT_FCOE_RX			(1ULL << 49)
#define IXL_PCT_FCOE_OTHER		(1ULL << 50)
/* bits 51-62 are reserved for future use */
#define IXL_PCT_L2_PAYLOAD		(1ULL << 63)

#define IXL_RSS_HENA_BASE_DEFAULT		\
	IXL_PCT_NONF_IPV4_UDP |			\
	IXL_PCT_NONF_IPV4_TCP |			\
	IXL_PCT_NONF_IPV4_SCTP |		\
	IXL_PCT_NONF_IPV4_OTHER |		\
	IXL_PCT_FRAG_IPV4 |			\
	IXL_PCT_NONF_IPV6_UDP |			\
	IXL_PCT_NONF_IPV6_TCP |			\
	IXL_PCT_NONF_IPV6_SCTP |		\
	IXL_PCT_NONF_IPV6_OTHER |		\
	IXL_PCT_FRAG_IPV6 |			\
	IXL_PCT_L2_PAYLOAD

#define IXL_RSS_HENA_BASE_710		IXL_RSS_HENA_BASE_DEFAULT
#define IXL_RSS_HENA_BASE_722		IXL_RSS_HENA_BASE_DEFAULT | \
	IXL_PCT_NONF_IPV4_UDP_UCAST |		\
	IXL_PCT_NONF_IPV4_UDP_MCAST |		\
	IXL_PCT_NONF_IPV6_UDP_UCAST |		\
	IXL_PCT_NONF_IPV6_UDP_MCAST |		\
	IXL_PCT_NONF_IPV4_TCP_SYN_NOACK |	\
	IXL_PCT_NONF_IPV6_TCP_SYN_NOACK

#endif /* _IXL_VAR_H_ */

