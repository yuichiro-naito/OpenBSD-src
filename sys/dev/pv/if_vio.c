/*	$OpenBSD: if_vio.c,v 1.56 2024/09/19 06:23:38 sf Exp $	*/

/*
 * Copyright (c) 2012 Stefan Fritsch, Alexander Fiveg.
 * Copyright (c) 2010 Minoura Makoto.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "bpfilter.h"
#include "vlan.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/mbuf.h>
#include <sys/sockio.h>
#include <sys/timeout.h>

#include <dev/pv/virtioreg.h>
#include <dev/pv/virtiovar.h>

#include <net/if.h>
#include <net/if_media.h>
#include <net/route.h>

#include <netinet/if_ether.h>
#include <netinet/tcp.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>
#include <netinet/udp.h>

#if NBPFILTER > 0
#include <net/bpf.h>
#endif

#if VIRTIO_DEBUG
#define DPRINTF(x...) printf(x)
#else
#define DPRINTF(x...)
#endif

/*
 * if_vioreg.h:
 */
/* Configuration registers */
#define VIRTIO_NET_CONFIG_MAC		0 /* 8bit x 6byte */
#define VIRTIO_NET_CONFIG_STATUS	6 /* 16bit */

/* Feature bits */
#define VIRTIO_NET_F_CSUM			(1ULL<<0)
#define VIRTIO_NET_F_GUEST_CSUM			(1ULL<<1)
#define VIRTIO_NET_F_CTRL_GUEST_OFFLOADS        (1ULL<<2)
#define VIRTIO_NET_F_MTU                        (1ULL<<3)
#define VIRTIO_NET_F_MAC			(1ULL<<5)
#define VIRTIO_NET_F_GSO			(1ULL<<6)
#define VIRTIO_NET_F_GUEST_TSO4			(1ULL<<7)
#define VIRTIO_NET_F_GUEST_TSO6			(1ULL<<8)
#define VIRTIO_NET_F_GUEST_ECN			(1ULL<<9)
#define VIRTIO_NET_F_GUEST_UFO			(1ULL<<10)
#define VIRTIO_NET_F_HOST_TSO4			(1ULL<<11)
#define VIRTIO_NET_F_HOST_TSO6			(1ULL<<12)
#define VIRTIO_NET_F_HOST_ECN			(1ULL<<13)
#define VIRTIO_NET_F_HOST_UFO			(1ULL<<14)
#define VIRTIO_NET_F_MRG_RXBUF			(1ULL<<15)
#define VIRTIO_NET_F_STATUS			(1ULL<<16)
#define VIRTIO_NET_F_CTRL_VQ			(1ULL<<17)
#define VIRTIO_NET_F_CTRL_RX			(1ULL<<18)
#define VIRTIO_NET_F_CTRL_VLAN			(1ULL<<19)
#define VIRTIO_NET_F_CTRL_RX_EXTRA		(1ULL<<20)
#define VIRTIO_NET_F_GUEST_ANNOUNCE		(1ULL<<21)
#define VIRTIO_NET_F_MQ				(1ULL<<22)
#define VIRTIO_NET_F_CTRL_MAC_ADDR		(1ULL<<23)
#define VIRTIO_NET_F_HOST_USO			(1ULL<<56)
#define VIRTIO_NET_F_HASH_REPORT		(1ULL<<57)
#define VIRTIO_NET_F_GUEST_HDRLEN		(1ULL<<59)
#define VIRTIO_NET_F_RSS			(1ULL<<60)
#define VIRTIO_NET_F_RSC_EXT			(1ULL<<61)
#define VIRTIO_NET_F_STANDBY			(1ULL<<62)
#define VIRTIO_NET_F_SPEED_DUPLEX		(1ULL<<63)
/*
 * Config(8) flags. The lowest byte is reserved for generic virtio stuff.
 */

/* Workaround for vlan related bug in qemu < version 2.0 */
#define CONFFLAG_QEMU_VLAN_BUG		(1<<8)

static const struct virtio_feature_name virtio_net_feature_names[] = {
#if VIRTIO_DEBUG
	{ VIRTIO_NET_F_CSUM,			"CSum" },
	{ VIRTIO_NET_F_GUEST_CSUM,		"GuestCSum" },
	{ VIRTIO_NET_F_CTRL_GUEST_OFFLOADS,	"CtrlGuestOffl" },
	{ VIRTIO_NET_F_MTU,			"MTU", },
	{ VIRTIO_NET_F_MAC,			"MAC" },
	{ VIRTIO_NET_F_GSO,			"GSO" },
	{ VIRTIO_NET_F_GUEST_TSO4,		"GuestTSO4" },
	{ VIRTIO_NET_F_GUEST_TSO6,		"GuestTSO6" },
	{ VIRTIO_NET_F_GUEST_ECN,		"GuestECN" },
	{ VIRTIO_NET_F_GUEST_UFO,		"GuestUFO" },
	{ VIRTIO_NET_F_HOST_TSO4,		"HostTSO4" },
	{ VIRTIO_NET_F_HOST_TSO6,		"HostTSO6" },
	{ VIRTIO_NET_F_HOST_ECN,		"HostECN" },
	{ VIRTIO_NET_F_HOST_UFO,		"HostUFO" },
	{ VIRTIO_NET_F_MRG_RXBUF,		"MrgRXBuf" },
	{ VIRTIO_NET_F_STATUS,			"Status" },
	{ VIRTIO_NET_F_CTRL_VQ,			"CtrlVQ" },
	{ VIRTIO_NET_F_CTRL_RX,			"CtrlRX" },
	{ VIRTIO_NET_F_CTRL_VLAN,		"CtrlVLAN" },
	{ VIRTIO_NET_F_CTRL_RX_EXTRA,		"CtrlRXExtra" },
	{ VIRTIO_NET_F_GUEST_ANNOUNCE,		"GuestAnnounce" },
	{ VIRTIO_NET_F_MQ,			"MQ" },
	{ VIRTIO_NET_F_CTRL_MAC_ADDR,		"CtrlMAC" },
	{ VIRTIO_NET_F_HOST_USO,		"HostUso" },
	{ VIRTIO_NET_F_HASH_REPORT,		"HashRpt" },
	{ VIRTIO_NET_F_GUEST_HDRLEN,		"GuestHdrlen" },
	{ VIRTIO_NET_F_RSS,			"RSS" },
	{ VIRTIO_NET_F_RSC_EXT,			"RSSExt" },
	{ VIRTIO_NET_F_STANDBY,			"Stdby" },
	{ VIRTIO_NET_F_SPEED_DUPLEX,		"SpdDplx" },
#endif
	{ 0,					NULL }
};

/* Status */
#define VIRTIO_NET_S_LINK_UP	1

/* Packet header structure */
struct virtio_net_hdr {
	uint8_t		flags;
	uint8_t		gso_type;
	uint16_t	hdr_len;
	uint16_t	gso_size;
	uint16_t	csum_start;
	uint16_t	csum_offset;

	/* only present if VIRTIO_NET_F_MRG_RXBUF is negotiated */
	uint16_t	num_buffers;
} __packed;

#define VIRTIO_NET_HDR_F_NEEDS_CSUM	1 /* flags */
#define VIRTIO_NET_HDR_F_DATA_VALID	2 /* flags */
#define VIRTIO_NET_HDR_GSO_NONE		0 /* gso_type */
#define VIRTIO_NET_HDR_GSO_TCPV4	1 /* gso_type */
#define VIRTIO_NET_HDR_GSO_UDP		3 /* gso_type */
#define VIRTIO_NET_HDR_GSO_TCPV6	4 /* gso_type */
#define VIRTIO_NET_HDR_GSO_ECN		0x80 /* gso_type, |'ed */

#define VIRTIO_NET_MAX_GSO_LEN		(65536+ETHER_HDR_LEN)

/* Control virtqueue */
struct virtio_net_ctrl_cmd {
	uint8_t	class;
	uint8_t	command;
} __packed;
#define VIRTIO_NET_CTRL_RX		0
# define VIRTIO_NET_CTRL_RX_PROMISC	0
# define VIRTIO_NET_CTRL_RX_ALLMULTI	1

#define VIRTIO_NET_CTRL_MAC		1
# define VIRTIO_NET_CTRL_MAC_TABLE_SET	0

#define VIRTIO_NET_CTRL_VLAN		2
# define VIRTIO_NET_CTRL_VLAN_ADD	0
# define VIRTIO_NET_CTRL_VLAN_DEL	1

#define VIRTIO_NET_CTRL_GUEST_OFFLOADS	5
# define VIRTIO_NET_CTRL_GUEST_OFFLOADS_SET	0

struct virtio_net_ctrl_status {
	uint8_t	ack;
} __packed;
#define VIRTIO_NET_OK			0
#define VIRTIO_NET_ERR			1

struct virtio_net_ctrl_rx {
	uint8_t	onoff;
} __packed;

struct virtio_net_ctrl_guest_offloads {
	uint64_t offloads;
} __packed;

struct virtio_net_ctrl_mac_tbl {
	uint32_t nentries;
	uint8_t macs[][ETHER_ADDR_LEN];
} __packed;

struct virtio_net_ctrl_vlan {
	uint16_t id;
} __packed;

/*
 * if_viovar.h:
 */
enum vio_ctrl_state {
	FREE, INUSE, DONE, RESET
};

struct vio_queue {
	struct vio_softc	 *viq_sc;
	struct virtio_net_hdr	 *viq_txhdrs;
	bus_dmamap_t		 *viq_arrays;
#define viq_rxdmamaps viq_arrays
	bus_dmamap_t		 *viq_txdmamaps;
	struct mbuf		**viq_rxmbufs;
	struct mbuf		**viq_txmbufs;
	struct if_rxring	  viq_rxring;
	struct virtqueue	 *viq_rxvq;
	struct virtqueue	 *viq_txvq;
};

struct vio_softc {
	struct device		sc_dev;

	struct virtio_softc	*sc_virtio;
	struct virtqueue	*sc_ctl_vq;

	struct arpcom		sc_ac;
	struct ifmedia		sc_media;

	short			sc_ifflags;

	/* bus_dmamem */
	bus_dma_segment_t	sc_dma_seg;
	bus_dmamap_t		sc_dma_map;
	size_t			sc_dma_size;
	caddr_t			sc_dma_kva;

	int			sc_hdr_size;
	struct virtio_net_ctrl_cmd *sc_ctrl_cmd;
	struct virtio_net_ctrl_status *sc_ctrl_status;
	struct virtio_net_ctrl_rx *sc_ctrl_rx;
	struct virtio_net_ctrl_guest_offloads *sc_ctrl_guest_offloads;
	struct virtio_net_ctrl_mac_tbl *sc_ctrl_mac_tbl_uc;
#define sc_ctrl_mac_info sc_ctrl_mac_tbl_uc
	struct virtio_net_ctrl_mac_tbl *sc_ctrl_mac_tbl_mc;

	struct vio_queue	*sc_q;
	uint16_t		sc_nqueues;

	enum vio_ctrl_state	sc_ctrl_inuse;

	struct timeout		sc_txtick, sc_rxtick;
};

#define VIO_DMAMEM_OFFSET(sc, p) ((caddr_t)(p) - (sc)->sc_dma_kva)
#define VIO_DMAMEM_SYNC(vsc, sc, p, size, flags)		\
	bus_dmamap_sync((vsc)->sc_dmat, (sc)->sc_dma_map,	\
	    VIO_DMAMEM_OFFSET((sc), (p)), (size), (flags))
#define VIO_HAVE_MRG_RXBUF(sc)					\
	((sc)->sc_hdr_size == sizeof(struct virtio_net_hdr))

#define VIRTIO_NET_CTRL_MAC_MC_ENTRIES	64 /* for more entries, use ALLMULTI */
#define VIRTIO_NET_CTRL_MAC_UC_ENTRIES	 1 /* one entry for own unicast addr */
#define VIRTIO_NET_CTRL_TIMEOUT		(5*1000*1000*1000ULL) /* 5 seconds */

#define VIO_CTRL_MAC_INFO_SIZE					\
	(2*sizeof(struct virtio_net_ctrl_mac_tbl) +		\
	 (VIRTIO_NET_CTRL_MAC_MC_ENTRIES +			\
	  VIRTIO_NET_CTRL_MAC_UC_ENTRIES) * ETHER_ADDR_LEN)

/* cfattach interface functions */
int	vio_match(struct device *, void *, void *);
void	vio_attach(struct device *, struct device *, void *);

/* ifnet interface functions */
int	vio_init(struct ifnet *);
void	vio_stop(struct ifnet *, int);
void	vio_start(struct ifnet *);
int	vio_ioctl(struct ifnet *, u_long, caddr_t);
void	vio_get_lladdr(struct arpcom *ac, struct virtio_softc *vsc);
void	vio_put_lladdr(struct arpcom *ac, struct virtio_softc *vsc);

/* rx */
int	vio_add_rx_mbuf(struct vio_softc *, struct vio_queue *, int);
void	vio_free_rx_mbuf(struct vio_softc *, struct vio_queue *, int);
void	vio_populate_rx_mbufs(struct vio_softc *, struct vio_queue *);
int	vio_rxeof(struct vio_queue *);
int	vio_rx_intr(struct virtqueue *);
void	vio_rx_drain(struct vio_softc *);
void	vio_rxtick(void *);

/* tx */
int	vio_tx_intr(struct virtqueue *);
int	vio_txeof(struct virtqueue *);
void	vio_tx_drain(struct vio_softc *);
int	vio_encap(struct vio_queue *, int, struct mbuf *);
void	vio_txtick(void *);

/* other control */
void	vio_link_state(struct ifnet *);
int	vio_config_change(struct virtio_softc *);
int	vio_ctrl_rx(struct vio_softc *, int, int);
int	vio_ctrl_guest_offloads(struct vio_softc *, uint64_t);
int	vio_set_rx_filter(struct vio_softc *);
void	vio_iff(struct vio_softc *);
int	vio_media_change(struct ifnet *);
void	vio_media_status(struct ifnet *, struct ifmediareq *);
int	vio_ctrleof(struct virtqueue *);
int	vio_ctrl_start(struct vio_softc *, uint8_t, uint8_t, int, int *);
int	vio_ctrl_submit(struct vio_softc *, int);
void	vio_ctrl_finish(struct vio_softc *);
void	vio_ctrl_wakeup(struct vio_softc *, enum vio_ctrl_state);
int	vio_alloc_mem(struct vio_softc *, int);
int	vio_alloc_dmamem(struct vio_softc *);
void	vio_free_dmamem(struct vio_softc *);

#if VIRTIO_DEBUG
void	vio_dump(struct vio_softc *);
#endif

int
vio_match(struct device *parent, void *match, void *aux)
{
	struct virtio_attach_args *va = aux;

	if (va->va_devid == PCI_PRODUCT_VIRTIO_NETWORK)
		return 1;

	return 0;
}

const struct cfattach vio_ca = {
	sizeof(struct vio_softc), vio_match, vio_attach, NULL
};

struct cfdriver vio_cd = {
	NULL, "vio", DV_IFNET
};

int
vio_alloc_dmamem(struct vio_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	int nsegs;

	if (bus_dmamap_create(vsc->sc_dmat, sc->sc_dma_size, 1,
	    sc->sc_dma_size, 0, BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW,
	    &sc->sc_dma_map) != 0)
		goto err;
	if (bus_dmamem_alloc(vsc->sc_dmat, sc->sc_dma_size, 16, 0,
	    &sc->sc_dma_seg, 1, &nsegs, BUS_DMA_NOWAIT | BUS_DMA_ZERO) != 0)
		goto destroy;
	if (bus_dmamem_map(vsc->sc_dmat, &sc->sc_dma_seg, nsegs,
	    sc->sc_dma_size, &sc->sc_dma_kva, BUS_DMA_NOWAIT) != 0)
		goto free;
	if (bus_dmamap_load(vsc->sc_dmat, sc->sc_dma_map, sc->sc_dma_kva,
	    sc->sc_dma_size, NULL, BUS_DMA_NOWAIT) != 0)
		goto unmap;
	return (0);

unmap:
	bus_dmamem_unmap(vsc->sc_dmat, sc->sc_dma_kva, sc->sc_dma_size);
free:
	bus_dmamem_free(vsc->sc_dmat, &sc->sc_dma_seg, 1);
destroy:
	bus_dmamap_destroy(vsc->sc_dmat, sc->sc_dma_map);
err:
	return (1);
}

void
vio_free_dmamem(struct vio_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;

	bus_dmamap_unload(vsc->sc_dmat, sc->sc_dma_map);
	bus_dmamem_unmap(vsc->sc_dmat, sc->sc_dma_kva, sc->sc_dma_size);
	bus_dmamem_free(vsc->sc_dmat, &sc->sc_dma_seg, 1);
	bus_dmamap_destroy(vsc->sc_dmat, sc->sc_dma_map);
}

/* allocate memory */
/*
 * dma memory is used for:
 *   viq_txhdrs[slot]:	 metadata array for frames to be sent (WRITE)
 *   sc_ctrl_cmd:	 command to be sent via ctrl vq (WRITE)
 *   sc_ctrl_status:	 return value for a command via ctrl vq (READ)
 *   sc_ctrl_rx:	 parameter for a VIRTIO_NET_CTRL_RX class command
 *			 (WRITE)
 *   sc_ctrl_mac_tbl_uc: unicast MAC address filter for a VIRTIO_NET_CTRL_MAC
 *			 class command (WRITE)
 *   sc_ctrl_mac_tbl_mc: multicast MAC address filter for a VIRTIO_NET_CTRL_MAC
 *			 class command (WRITE)
 * sc_ctrl_* structures are allocated only one each; they are protected by
 * sc_ctrl_inuse, which must only be accessed at splnet
 *
 * metadata headers for received frames are stored at the start of the
 * rx mbufs.
 */
/*
 * dynamically allocated memory is used for:
 *   viq_rxdmamaps[slot]:		bus_dmamap_t array for received payload
 *   viq_txdmamaps[slot]:		bus_dmamap_t array for sent payload
 *   viq_rxmbufs[slot]:		mbuf pointer array for received frames
 *   viq_txmbufs[slot]:		mbuf pointer array for sent frames
 */
int
vio_alloc_mem(struct vio_softc *sc, int tx_max_segments)
{
	struct virtio_softc	*vsc = sc->sc_virtio;
	struct ifnet		*ifp = &sc->sc_ac.ac_if;
	size_t			 allocsize, rxqsize, txqsize, offset = 0;
	bus_size_t		 txsize;
	caddr_t			 kva;
	int			 i, qidx, r;

	rxqsize = sc->sc_q[0].viq_rxvq->vq_num;
	txqsize = sc->sc_q[0].viq_txvq->vq_num;

	/*
	 * For simplicity, we always allocate the full virtio_net_hdr size
	 * even if VIRTIO_NET_F_MRG_RXBUF is not negotiated and
	 * only a part of the memory is ever used.
	 */
	allocsize = sizeof(struct virtio_net_hdr) * txqsize * sc->sc_nqueues;

	if (virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_VQ)) {
		allocsize += sizeof(struct virtio_net_ctrl_cmd) * 1;
		allocsize += sizeof(struct virtio_net_ctrl_status) * 1;
		allocsize += sizeof(struct virtio_net_ctrl_rx) * 1;
		allocsize += sizeof(struct virtio_net_ctrl_guest_offloads) * 1;
		allocsize += VIO_CTRL_MAC_INFO_SIZE;
	}
	sc->sc_dma_size = allocsize;

	if (vio_alloc_dmamem(sc) != 0) {
		printf("unable to allocate dma region\n");
		return -1;
	}

	kva = sc->sc_dma_kva;

	for (qidx = 0; qidx < sc->sc_nqueues; qidx++) {
		sc->sc_q[qidx].viq_txhdrs =
		    (struct virtio_net_hdr *)(kva + offset);
		offset += sizeof(struct virtio_net_hdr) * txqsize;
	}

	if (virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_VQ)) {
		sc->sc_ctrl_cmd = (void *)(kva + offset);
		offset += sizeof(*sc->sc_ctrl_cmd);
		sc->sc_ctrl_status = (void *)(kva + offset);
		offset += sizeof(*sc->sc_ctrl_status);
		sc->sc_ctrl_rx = (void *)(kva + offset);
		offset += sizeof(*sc->sc_ctrl_rx);
		sc->sc_ctrl_guest_offloads = (void *)(kva + offset);
		offset += sizeof(*sc->sc_ctrl_guest_offloads);
		sc->sc_ctrl_mac_tbl_uc = (void *)(kva + offset);
		offset += sizeof(*sc->sc_ctrl_mac_tbl_uc) +
		    ETHER_ADDR_LEN * VIRTIO_NET_CTRL_MAC_UC_ENTRIES;
		sc->sc_ctrl_mac_tbl_mc = (void *)(kva + offset);
		offset += sizeof(*sc->sc_ctrl_mac_tbl_mc) +
		    ETHER_ADDR_LEN * VIRTIO_NET_CTRL_MAC_MC_ENTRIES;
	}
	KASSERT(offset == allocsize);

	txsize = ifp->if_hardmtu + sc->sc_hdr_size + ETHER_HDR_LEN;

	for (qidx = 0; qidx < sc->sc_nqueues; qidx++) {
		struct vio_queue *vioq = &sc->sc_q[qidx];

		vioq->viq_arrays = mallocarray(rxqsize + txqsize,
		    sizeof(bus_dmamap_t) + sizeof(struct mbuf *), M_DEVBUF,
		    M_WAITOK|M_ZERO);
		if (vioq->viq_arrays == NULL) {
			printf("unable to allocate mem for dmamaps\n");
			goto free;
		}

		vioq->viq_txdmamaps = vioq->viq_arrays + rxqsize;
		vioq->viq_rxmbufs = (void *)(vioq->viq_txdmamaps + txqsize);
		vioq->viq_txmbufs = vioq->viq_rxmbufs + rxqsize;

		for (i = 0; i < rxqsize; i++) {
			r = bus_dmamap_create(vsc->sc_dmat, MAXMCLBYTES,
			    MAXMCLBYTES/PAGE_SIZE + 1, MCLBYTES, 0,
			    BUS_DMA_NOWAIT|BUS_DMA_ALLOCNOW,
			    &vioq->viq_rxdmamaps[i]);
			if (r != 0)
				goto destroy;
		}

		for (i = 0; i < txqsize; i++) {
			r = bus_dmamap_create(vsc->sc_dmat, txsize,
			    tx_max_segments, txsize, 0,
			    BUS_DMA_NOWAIT|BUS_DMA_ALLOCNOW,
			    &vioq->viq_txdmamaps[i]);
			if (r != 0)
				goto destroy;
		}
	}

	return 0;

 destroy:
	printf("dmamap creation failed, error %d\n", r);
	for (qidx = 0; qidx < sc->sc_nqueues; qidx++) {
		struct vio_queue *vioq = &sc->sc_q[qidx];

		for (i = 0; i < txqsize; i++) {
			if (vioq->viq_txdmamaps[i] == NULL)
				break;
			bus_dmamap_destroy(vsc->sc_dmat,
			    vioq->viq_txdmamaps[i]);
		}
		for (i = 0; i < rxqsize; i++) {
			if (vioq->viq_rxdmamaps[i] == NULL)
				break;
			bus_dmamap_destroy(vsc->sc_dmat,
			    vioq->viq_rxdmamaps[i]);
		}
		free(vioq->viq_arrays, M_DEVBUF, (rxqsize + txqsize) *
		    (sizeof(bus_dmamap_t) + sizeof(struct mbuf *)));
		vioq->viq_arrays = NULL;
	}
 free:
	vio_free_dmamem(sc);
	return -1;
}

static void
vio_dmamem_enqueue(struct virtio_softc *vsc, struct vio_softc *sc,
    struct virtqueue *vq, int slot, void *p, size_t size, int write)
{
	VIO_DMAMEM_SYNC(vsc, sc, p, size, write ? BUS_DMASYNC_PREWRITE :
	    BUS_DMASYNC_PREREAD);
	virtio_enqueue_p(vq, slot, sc->sc_dma_map, VIO_DMAMEM_OFFSET(sc, p),
	    size, write);
}

void
vio_get_lladdr(struct arpcom *ac, struct virtio_softc *vsc)
{
	int i;
	for (i = 0; i < ETHER_ADDR_LEN; i++) {
		ac->ac_enaddr[i] = virtio_read_device_config_1(vsc,
		    VIRTIO_NET_CONFIG_MAC + i);
	}
}

void
vio_put_lladdr(struct arpcom *ac, struct virtio_softc *vsc)
{
	int i;
	for (i = 0; i < ETHER_ADDR_LEN; i++) {
		virtio_write_device_config_1(vsc, VIRTIO_NET_CONFIG_MAC + i,
		     ac->ac_enaddr[i]);
	}
}

static int
vio_needs_reset(struct vio_softc *sc)
{
	if (virtio_get_status(sc->sc_virtio) &
	    VIRTIO_CONFIG_DEVICE_STATUS_DEVICE_NEEDS_RESET) {
		printf("%s: device needs reset\n", sc->sc_dev.dv_xname);
		vio_ctrl_wakeup(sc, RESET);
		return 1;
	}
	return 0;
}

void
vio_attach(struct device *parent, struct device *self, void *aux)
{
	struct vio_softc *sc = (struct vio_softc *)self;
	struct virtio_softc *vsc = (struct virtio_softc *)parent;
	int i, tx_max_segments;
	struct ifnet *ifp = &sc->sc_ac.ac_if;

	if (vsc->sc_child != NULL) {
		printf(": child already attached for %s; something wrong...\n",
		    parent->dv_xname);
		return;
	}

	sc->sc_virtio = vsc;

	vsc->sc_child = self;
	vsc->sc_ipl = IPL_NET;
	vsc->sc_config_change = NULL;
	vsc->sc_driver_features = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS |
	    VIRTIO_NET_F_CTRL_VQ | VIRTIO_NET_F_CTRL_RX |
	    VIRTIO_NET_F_MRG_RXBUF | VIRTIO_NET_F_CSUM |
	    VIRTIO_F_RING_EVENT_IDX | VIRTIO_NET_F_GUEST_CSUM;

	vsc->sc_driver_features |= VIRTIO_NET_F_HOST_TSO4;
	vsc->sc_driver_features |= VIRTIO_NET_F_HOST_TSO6;

	vsc->sc_driver_features |= VIRTIO_NET_F_CTRL_GUEST_OFFLOADS;
	vsc->sc_driver_features |= VIRTIO_NET_F_GUEST_TSO4;
	vsc->sc_driver_features |= VIRTIO_NET_F_GUEST_TSO6;

	virtio_negotiate_features(vsc, virtio_net_feature_names);

	sc->sc_nqueues = 1;
	vsc->sc_nvqs = 2 * sc->sc_nqueues;
	if (virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_VQ))
		vsc->sc_nvqs++;

	vsc->sc_vqs = mallocarray(vsc->sc_nvqs, sizeof(*vsc->sc_vqs), M_DEVBUF,
	    M_WAITOK|M_ZERO);
	if (vsc->sc_vqs == NULL) {
		vsc->sc_nvqs = 0;
		goto err;
	}

	sc->sc_q = mallocarray(sc->sc_nqueues, sizeof(*sc->sc_q), M_DEVBUF,
	    M_WAITOK|M_ZERO);
	if (sc->sc_q == NULL)
		goto err;

	if (virtio_has_feature(vsc, VIRTIO_NET_F_MAC)) {
		vio_get_lladdr(&sc->sc_ac, vsc);
	} else {
		ether_fakeaddr(ifp);
		vio_put_lladdr(&sc->sc_ac, vsc);
	}
	printf(", address %s\n", ether_sprintf(sc->sc_ac.ac_enaddr));

	if (virtio_has_feature(vsc, VIRTIO_NET_F_MRG_RXBUF) ||
	    vsc->sc_version_1) {
		sc->sc_hdr_size = sizeof(struct virtio_net_hdr);
	} else {
		sc->sc_hdr_size = offsetof(struct virtio_net_hdr, num_buffers);
	}
	if (virtio_has_feature(vsc, VIRTIO_NET_F_MRG_RXBUF))
		ifp->if_hardmtu = MAXMCLBYTES;
	else
		ifp->if_hardmtu = MCLBYTES - sc->sc_hdr_size - ETHER_HDR_LEN;

	/* defrag for longer mbuf chains */
	tx_max_segments = 16;
	if (virtio_has_feature(vsc, VIRTIO_NET_F_HOST_TSO4) ||
	    virtio_has_feature(vsc, VIRTIO_NET_F_HOST_TSO6)) {
		/*
		 * With TSO, we may get 64K packets and want to be able to
		 * send longer chains without defragmenting
		 */
		tx_max_segments = 32;
	}

	for (i = 0; i < sc->sc_nqueues; i++) {
		int vqidx = 2 * i;
		struct vio_queue *vioq = &sc->sc_q[i];

		vioq->viq_rxvq = &vsc->sc_vqs[vqidx];
		vioq->viq_sc = sc;
		if (virtio_alloc_vq(vsc, vioq->viq_rxvq, vqidx, 2, "rx") != 0)
			goto err;
		vioq->viq_rxvq->vq_done = vio_rx_intr;
		virtio_start_vq_intr(vsc, vioq->viq_rxvq);

		vqidx++;
		vioq->viq_txvq = &vsc->sc_vqs[vqidx];
		if (virtio_alloc_vq(vsc, vioq->viq_txvq, vqidx,
		    tx_max_segments + 1, "tx") != 0) {
			goto err;
		}
		vioq->viq_txvq->vq_done = vio_tx_intr;
		if (virtio_has_feature(vsc, VIRTIO_F_RING_EVENT_IDX))
			virtio_postpone_intr_far(vioq->viq_txvq);
		else
			virtio_stop_vq_intr(vsc, vioq->viq_txvq);
	}

	/* control queue */
	if (virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_VQ)) {
		sc->sc_ctl_vq = &vsc->sc_vqs[2];
		if (virtio_alloc_vq(vsc, sc->sc_ctl_vq, 2, 1,
		    "control") != 0)
			goto err;
		sc->sc_ctl_vq->vq_done = vio_ctrleof;
		virtio_start_vq_intr(vsc, sc->sc_ctl_vq);
	}

	if (vio_alloc_mem(sc, tx_max_segments) < 0)
		goto err;

	strlcpy(ifp->if_xname, self->dv_xname, IFNAMSIZ);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_start = vio_start;
	ifp->if_ioctl = vio_ioctl;
	ifp->if_capabilities = 0;
#if NVLAN > 0
	ifp->if_capabilities |= IFCAP_VLAN_MTU;
	ifp->if_capabilities |= IFCAP_VLAN_HWOFFLOAD;
#endif
	if (virtio_has_feature(vsc, VIRTIO_NET_F_CSUM))
		ifp->if_capabilities |= IFCAP_CSUM_TCPv4|IFCAP_CSUM_UDPv4|
		    IFCAP_CSUM_TCPv6|IFCAP_CSUM_UDPv6;
	if (virtio_has_feature(vsc, VIRTIO_NET_F_HOST_TSO4))
		ifp->if_capabilities |= IFCAP_TSOv4;
	if (virtio_has_feature(vsc, VIRTIO_NET_F_HOST_TSO6))
		ifp->if_capabilities |= IFCAP_TSOv6;

	if (virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_GUEST_OFFLOADS) &&
	    (virtio_has_feature(vsc, VIRTIO_NET_F_GUEST_TSO4) ||
	     virtio_has_feature(vsc, VIRTIO_NET_F_GUEST_TSO6))) {
		ifp->if_xflags |= IFXF_LRO;
		ifp->if_capabilities |= IFCAP_LRO;
	}

	ifq_init_maxlen(&ifp->if_snd, vsc->sc_vqs[1].vq_num - 1);
	ifmedia_init(&sc->sc_media, 0, vio_media_change, vio_media_status);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->sc_media, IFM_ETHER | IFM_AUTO);
	vsc->sc_config_change = vio_config_change;
	timeout_set(&sc->sc_txtick, vio_txtick, sc->sc_q[0].viq_txvq);
	timeout_set(&sc->sc_rxtick, vio_rxtick, sc->sc_q[0].viq_rxvq);

	virtio_set_status(vsc, VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK);
	if_attach(ifp);
	ether_ifattach(ifp);

	return;

err:
	for (i = 0; i < vsc->sc_nvqs; i++)
		virtio_free_vq(vsc, &vsc->sc_vqs[i]);
	free(vsc->sc_vqs, M_DEVBUF, vsc->sc_nvqs * sizeof(*vsc->sc_vqs));
	free(sc->sc_q, M_DEVBUF, sc->sc_nqueues * sizeof(*sc->sc_q));
	vsc->sc_nvqs = 0;
	vsc->sc_child = VIRTIO_CHILD_ERROR;
	return;
}

/* check link status */
void
vio_link_state(struct ifnet *ifp)
{
	struct vio_softc *sc = ifp->if_softc;
	struct virtio_softc *vsc = sc->sc_virtio;
	int link_state = LINK_STATE_FULL_DUPLEX;

	if (virtio_has_feature(vsc, VIRTIO_NET_F_STATUS)) {
		int status = virtio_read_device_config_2(vsc,
		    VIRTIO_NET_CONFIG_STATUS);
		if (!(status & VIRTIO_NET_S_LINK_UP))
			link_state = LINK_STATE_DOWN;
	}
	if (ifp->if_link_state != link_state) {
		ifp->if_link_state = link_state;
		if_link_state_change(ifp);
	}
}

int
vio_config_change(struct virtio_softc *vsc)
{
	struct vio_softc *sc = (struct vio_softc *)vsc->sc_child;
	vio_link_state(&sc->sc_ac.ac_if);
	vio_needs_reset(sc);
	return 1;
}

int
vio_media_change(struct ifnet *ifp)
{
	/* Ignore */
	return (0);
}

void
vio_media_status(struct ifnet *ifp, struct ifmediareq *imr)
{
	imr->ifm_active = IFM_ETHER | IFM_AUTO;
	imr->ifm_status = IFM_AVALID;

	vio_link_state(ifp);
	if (LINK_STATE_IS_UP(ifp->if_link_state) && ifp->if_flags & IFF_UP)
		imr->ifm_status |= IFM_ACTIVE|IFM_FDX;
}

/*
 * Interface functions for ifnet
 */
int
vio_init(struct ifnet *ifp)
{
	struct vio_softc *sc = ifp->if_softc;
	struct virtio_softc *vsc = sc->sc_virtio;
	int qidx;

	vio_stop(ifp, 0);
	for (qidx = 0; qidx < sc->sc_nqueues; qidx++) {
		struct vio_queue *vioq = &sc->sc_q[qidx];

		if_rxr_init(&vioq->viq_rxring,
		    2 * ((ifp->if_hardmtu / MCLBYTES) + 1),
		    vioq->viq_rxvq->vq_num);
		vio_populate_rx_mbufs(sc, vioq);
	}
	ifq_clr_oactive(&ifp->if_snd);
	vio_iff(sc);
	vio_link_state(ifp);

	if (virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_GUEST_OFFLOADS)) {
		uint64_t features = 0;

		if (virtio_has_feature(vsc, VIRTIO_NET_F_GUEST_CSUM))
			SET(features, VIRTIO_NET_F_GUEST_CSUM);

		if (ISSET(ifp->if_xflags, IFXF_LRO)) {
			if (virtio_has_feature(vsc, VIRTIO_NET_F_GUEST_TSO4))
				SET(features, VIRTIO_NET_F_GUEST_TSO4);
			if (virtio_has_feature(vsc, VIRTIO_NET_F_GUEST_TSO6))
				SET(features, VIRTIO_NET_F_GUEST_TSO6);
		}

		vio_ctrl_guest_offloads(sc, features);
	}

	SET(ifp->if_flags, IFF_RUNNING);

	return 0;
}

void
vio_stop(struct ifnet *ifp, int disable)
{
	struct vio_softc *sc = ifp->if_softc;
	struct virtio_softc *vsc = sc->sc_virtio;
	int i;

	CLR(ifp->if_flags, IFF_RUNNING);
	timeout_del(&sc->sc_txtick);
	timeout_del(&sc->sc_rxtick);
	ifq_clr_oactive(&ifp->if_snd);
	/* only way to stop I/O and DMA is resetting... */
	virtio_reset(vsc);
	for (i = 0; i < sc->sc_nqueues; i++)
		vio_rxeof(&sc->sc_q[i]);

	if (virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_VQ))
		vio_ctrl_wakeup(sc, RESET);
	vio_tx_drain(sc);
	if (disable)
		vio_rx_drain(sc);

	virtio_reinit_start(vsc);
	for (i = 0; i < sc->sc_nqueues; i++) {
		virtio_start_vq_intr(vsc, sc->sc_q[i].viq_rxvq);
		virtio_stop_vq_intr(vsc, sc->sc_q[i].viq_txvq);
	}
	if (virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_VQ))
		virtio_start_vq_intr(vsc, sc->sc_ctl_vq);
	virtio_reinit_end(vsc);
	if (virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_VQ))
		vio_ctrl_wakeup(sc, FREE);
}

static inline uint16_t
vio_cksum_update(uint32_t cksum, uint16_t paylen)
{
	/* Add payload length */
	cksum += paylen;

	/* Fold back to 16 bit */
	cksum += cksum >> 16;

	return (uint16_t)(cksum);
}

void
vio_tx_offload(struct virtio_net_hdr *hdr, struct mbuf *m)
{
	struct ether_extracted ext;

	/*
	 * Checksum Offload
	 */

	if (!ISSET(m->m_pkthdr.csum_flags, M_TCP_CSUM_OUT) &&
	    !ISSET(m->m_pkthdr.csum_flags, M_UDP_CSUM_OUT))
		return;

	ether_extract_headers(m, &ext);

	/* Consistency Checks */
	if ((!ext.ip4 && !ext.ip6) || (!ext.tcp && !ext.udp))
		return;

	if ((ext.tcp && !ISSET(m->m_pkthdr.csum_flags, M_TCP_CSUM_OUT)) ||
	    (ext.udp && !ISSET(m->m_pkthdr.csum_flags, M_UDP_CSUM_OUT)))
		return;

	hdr->csum_start = sizeof(*ext.eh);
#if NVLAN > 0
	if (ext.evh)
		hdr->csum_start = sizeof(*ext.evh);
#endif
	hdr->csum_start += ext.iphlen;

	if (ext.tcp)
		hdr->csum_offset = offsetof(struct tcphdr, th_sum);
	else if (ext.udp)
		hdr->csum_offset = offsetof(struct udphdr, uh_sum);

	hdr->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;

	/*
	 * TCP Segmentation Offload
	 */

	if (!ISSET(m->m_pkthdr.csum_flags, M_TCP_TSO))
		return;

	if (!ext.tcp || m->m_pkthdr.ph_mss == 0) {
		tcpstat_inc(tcps_outbadtso);
		return;
	}

	hdr->hdr_len = hdr->csum_start + ext.tcphlen;
	hdr->gso_size = m->m_pkthdr.ph_mss;

	if (ext.ip4)
		hdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
#ifdef INET6
	else if (ext.ip6)
		hdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV6;
#endif

	/*
	 * VirtIO-Net needs pseudo header cksum with IP-payload length for TSO
	 */
	ext.tcp->th_sum = vio_cksum_update(ext.tcp->th_sum,
	    htons(ext.iplen - ext.iphlen));

	tcpstat_add(tcps_outpkttso,
	    (ext.paylen + m->m_pkthdr.ph_mss - 1) / m->m_pkthdr.ph_mss);
}

void
vio_start(struct ifnet *ifp)
{
	struct vio_softc *sc = ifp->if_softc;
	struct virtio_softc *vsc = sc->sc_virtio;
	struct vio_queue *vioq = &sc->sc_q[0];
	struct virtqueue *vq = vioq->viq_txvq;
	struct mbuf *m;
	int queued = 0;

	vio_txeof(vq);

	if (!(ifp->if_flags & IFF_RUNNING) || ifq_is_oactive(&ifp->if_snd))
		return;
	if (ifq_empty(&ifp->if_snd))
		return;

again:
	for (;;) {
		int slot, r;
		struct virtio_net_hdr *hdr;

		m = ifq_deq_begin(&ifp->if_snd);
		if (m == NULL)
			break;

		r = virtio_enqueue_prep(vq, &slot);
		if (r == EAGAIN) {
			ifq_deq_rollback(&ifp->if_snd, m);
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		if (r != 0)
			panic("%s: enqueue_prep for tx buffer: %d",
			    sc->sc_dev.dv_xname, r);

		hdr = &vioq->viq_txhdrs[slot];
		memset(hdr, 0, sc->sc_hdr_size);
		vio_tx_offload(hdr, m);

		r = vio_encap(vioq, slot, m);
		if (r != 0) {
			virtio_enqueue_abort(vq, slot);
			ifq_deq_commit(&ifp->if_snd, m);
			m_freem(m);
			ifp->if_oerrors++;
			continue;
		}
		r = virtio_enqueue_reserve(vq, slot,
		    vioq->viq_txdmamaps[slot]->dm_nsegs + 1);
		if (r != 0) {
			bus_dmamap_unload(vsc->sc_dmat,
			    vioq->viq_txdmamaps[slot]);
			ifq_deq_rollback(&ifp->if_snd, m);
			vioq->viq_txmbufs[slot] = NULL;
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		ifq_deq_commit(&ifp->if_snd, m);

		bus_dmamap_sync(vsc->sc_dmat, vioq->viq_txdmamaps[slot], 0,
		    vioq->viq_txdmamaps[slot]->dm_mapsize,
		    BUS_DMASYNC_PREWRITE);
		vio_dmamem_enqueue(vsc, sc, vq, slot, hdr, sc->sc_hdr_size, 1);
		virtio_enqueue(vq, slot, vioq->viq_txdmamaps[slot], 1);
		virtio_enqueue_commit(vsc, vq, slot, 0);
		queued++;
#if NBPFILTER > 0
		if (ifp->if_bpf)
			bpf_mtap(ifp->if_bpf, m, BPF_DIRECTION_OUT);
#endif
	}
	if (ifq_is_oactive(&ifp->if_snd)) {
		int r;
		if (virtio_has_feature(vsc, VIRTIO_F_RING_EVENT_IDX))
			r = virtio_postpone_intr_smart(vioq->viq_txvq);
		else
			r = virtio_start_vq_intr(vsc, vioq->viq_txvq);
		if (r) {
			vio_txeof(vq);
			goto again;
		}
	}

	if (queued > 0) {
		virtio_notify(vsc, vq);
		timeout_add_sec(&sc->sc_txtick, 1);
	}
}

#if VIRTIO_DEBUG
void
vio_dump(struct vio_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	struct virtio_softc *vsc = sc->sc_virtio;
	int i;

	printf("%s status dump:\n", ifp->if_xname);
	printf("tx tick active: %d\n", !timeout_triggered(&sc->sc_txtick));
	printf("rx tick active: %d\n", !timeout_triggered(&sc->sc_rxtick));
	for (i = 0; i < sc->sc_nqueues; i++) {
		printf("%d: TX virtqueue:\n", i);
		virtio_vq_dump(sc->sc_q[i].viq_txvq);
		printf("%d: RX virtqueue:\n", i);
		virtio_vq_dump(sc->sc_q[i].viq_rxvq);
	}
	if (virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_VQ)) {
		printf("CTL virtqueue:\n");
		virtio_vq_dump(sc->sc_ctl_vq);
		printf("ctrl_inuse: %d\n", sc->sc_ctrl_inuse);
	}
}
#endif

int
vio_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct vio_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	int s, r = 0;

	s = splnet();
	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		if (!(ifp->if_flags & IFF_RUNNING))
			vio_init(ifp);
		break;
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
#if VIRTIO_DEBUG
			if (ifp->if_flags & IFF_DEBUG)
				vio_dump(sc);
#endif
			if (ifp->if_flags & IFF_RUNNING)
				r = ENETRESET;
			else
				vio_init(ifp);
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				vio_stop(ifp, 1);
		}
		break;
	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		r = ifmedia_ioctl(ifp, ifr, &sc->sc_media, cmd);
		break;
	case SIOCGIFRXR:
		r = if_rxr_ioctl((struct if_rxrinfo *)ifr->ifr_data,
		    NULL, MCLBYTES, &sc->sc_q[0].viq_rxring);
		break;
	default:
		r = ether_ioctl(ifp, &sc->sc_ac, cmd, data);
	}

	if (r == ENETRESET) {
		if (ifp->if_flags & IFF_RUNNING)
			vio_iff(sc);
		r = 0;
	}
	splx(s);
	return r;
}

/*
 * Receive implementation
 */
/* allocate and initialize a mbuf for receive */
int
vio_add_rx_mbuf(struct vio_softc *sc, struct vio_queue *vioq, int i)
{
	struct mbuf *m;
	int r;

	m = MCLGETL(NULL, M_DONTWAIT, MCLBYTES);
	if (m == NULL)
		return ENOBUFS;
	vioq->viq_rxmbufs[i] = m;
	m->m_len = m->m_pkthdr.len = m->m_ext.ext_size;
	/* XXX m_adj ETHER_ALIGN ? */
	r = bus_dmamap_load_mbuf(sc->sc_virtio->sc_dmat,
	    vioq->viq_rxdmamaps[i], m, BUS_DMA_READ|BUS_DMA_NOWAIT);
	if (r) {
		m_freem(m);
		vioq->viq_rxmbufs[i] = NULL;
		return r;
	}

	return 0;
}

/* free a mbuf for receive */
void
vio_free_rx_mbuf(struct vio_softc *sc, struct vio_queue *vioq, int i)
{
	bus_dmamap_unload(sc->sc_virtio->sc_dmat, vioq->viq_rxdmamaps[i]);
	m_freem(vioq->viq_rxmbufs[i]);
	vioq->viq_rxmbufs[i] = NULL;
}

/* add mbufs for all the empty receive slots */
void
vio_populate_rx_mbufs(struct vio_softc *sc, struct vio_queue *vioq)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	int r, done = 0;
	u_int slots;
	struct virtqueue *vq = vioq->viq_rxvq;
	int mrg_rxbuf = VIO_HAVE_MRG_RXBUF(sc);

	for (slots = if_rxr_get(&vioq->viq_rxring, vq->vq_num);
	    slots > 0; slots--) {
		int slot;
		r = virtio_enqueue_prep(vq, &slot);
		if (r == EAGAIN)
			break;
		if (r != 0)
			panic("%s: enqueue_prep for rx buffer: %d",
			    sc->sc_dev.dv_xname, r);
		if (vioq->viq_rxmbufs[slot] == NULL) {
			r = vio_add_rx_mbuf(sc, vioq, slot);
			if (r != 0) {
				virtio_enqueue_abort(vq, slot);
				break;
			}
		}
		r = virtio_enqueue_reserve(vq, slot,
		    vioq->viq_rxdmamaps[slot]->dm_nsegs + (mrg_rxbuf ? 0 : 1));
		if (r != 0) {
			vio_free_rx_mbuf(sc, vioq, slot);
			break;
		}
		bus_dmamap_sync(vsc->sc_dmat, vioq->viq_rxdmamaps[slot], 0,
		    vioq->viq_rxdmamaps[slot]->dm_mapsize,
		    BUS_DMASYNC_PREREAD);
		if (mrg_rxbuf) {
			virtio_enqueue(vq, slot, vioq->viq_rxdmamaps[slot], 0);
		} else {
			/*
			 * Buggy kvm wants a buffer of exactly the size of
			 * the header in this case, so we have to split in
			 * two.
			 */
			virtio_enqueue_p(vq, slot, vioq->viq_rxdmamaps[slot],
			    0, sc->sc_hdr_size, 0);
			virtio_enqueue_p(vq, slot, vioq->viq_rxdmamaps[slot],
			    sc->sc_hdr_size, MCLBYTES - sc->sc_hdr_size, 0);
		}
		virtio_enqueue_commit(vsc, vq, slot, 0);
		done = 1;
	}
	if_rxr_put(&vioq->viq_rxring, slots);

	if (done)
		virtio_notify(vsc, vq);
	timeout_add_sec(&sc->sc_rxtick, 1);
}

void
vio_rx_offload(struct mbuf *m, struct virtio_net_hdr *hdr)
{
	struct ether_extracted ext;

	if (!ISSET(hdr->flags, VIRTIO_NET_HDR_F_DATA_VALID) &&
	    !ISSET(hdr->flags, VIRTIO_NET_HDR_F_NEEDS_CSUM))
		return;

	ether_extract_headers(m, &ext);

	if (ext.tcp) {
		SET(m->m_pkthdr.csum_flags, M_TCP_CSUM_IN_OK);
		if (ISSET(hdr->flags, VIRTIO_NET_HDR_F_NEEDS_CSUM))
			SET(m->m_pkthdr.csum_flags, M_TCP_CSUM_OUT);
	} else if (ext.udp) {
		SET(m->m_pkthdr.csum_flags, M_UDP_CSUM_IN_OK);
		if (ISSET(hdr->flags, VIRTIO_NET_HDR_F_NEEDS_CSUM))
			SET(m->m_pkthdr.csum_flags, M_UDP_CSUM_OUT);
	}

	if (hdr->gso_type == VIRTIO_NET_HDR_GSO_TCPV4 ||
	    hdr->gso_type == VIRTIO_NET_HDR_GSO_TCPV6) {
		uint16_t mss = hdr->gso_size;

		if (!ext.tcp || mss == 0) {
			tcpstat_inc(tcps_inbadlro);
			return;
		}

		if ((ext.paylen + mss - 1) / mss <= 1)
			return;

		tcpstat_inc(tcps_inhwlro);
		tcpstat_add(tcps_inpktlro, (ext.paylen + mss - 1) / mss);
		SET(m->m_pkthdr.csum_flags, M_TCP_TSO);
		m->m_pkthdr.ph_mss = mss;
	}
}

/* dequeue received packets */
int
vio_rxeof(struct vio_queue *vioq)
{
	struct vio_softc *sc = vioq->viq_sc;
	struct virtio_softc *vsc = sc->sc_virtio;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct mbuf *m, *m0 = NULL, *mlast;
	int r = 0;
	int slot, len, bufs_left;
	struct virtio_net_hdr *hdr;

	while (virtio_dequeue(vsc, vioq->viq_rxvq, &slot, &len) == 0) {
		r = 1;
		bus_dmamap_sync(vsc->sc_dmat, vioq->viq_rxdmamaps[slot], 0,
		    vioq->viq_rxdmamaps[slot]->dm_mapsize,
		    BUS_DMASYNC_POSTREAD);
		m = vioq->viq_rxmbufs[slot];
		KASSERT(m != NULL);
		bus_dmamap_unload(vsc->sc_dmat, vioq->viq_rxdmamaps[slot]);
		vioq->viq_rxmbufs[slot] = NULL;
		virtio_dequeue_commit(vioq->viq_rxvq, slot);
		if_rxr_put(&vioq->viq_rxring, 1);
		m->m_len = m->m_pkthdr.len = len;
		m->m_pkthdr.csum_flags = 0;
		if (m0 == NULL) {
			hdr = mtod(m, struct virtio_net_hdr *);
			m_adj(m, sc->sc_hdr_size);
			m0 = mlast = m;
			if (VIO_HAVE_MRG_RXBUF(sc))
				bufs_left = hdr->num_buffers - 1;
			else
				bufs_left = 0;
		} else {
			m->m_flags &= ~M_PKTHDR;
			m0->m_pkthdr.len += m->m_len;
			mlast->m_next = m;
			mlast = m;
			bufs_left--;
		}

		if (bufs_left == 0) {
			if (virtio_has_feature(vsc, VIRTIO_NET_F_GUEST_CSUM))
				vio_rx_offload(m0, hdr);
			ml_enqueue(&ml, m0);
			m0 = NULL;
		}
	}
	if (m0 != NULL) {
		DPRINTF("%s: expected %u buffers, got %u\n", __func__,
		    hdr->num_buffers, hdr->num_buffers - bufs_left);
		ifp->if_ierrors++;
		m_freem(m0);
	}

	if (ifiq_input(&ifp->if_rcv, &ml))
		if_rxr_livelocked(&vioq->viq_rxring);

	return r;
}

int
vio_rx_intr(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct vio_softc *sc = (struct vio_softc *)vsc->sc_child;
	/* vioq N uses the rx/tx vq pair 2*N and 2*N + 1 */
	struct vio_queue *vioq = &sc->sc_q[vq->vq_index/2];
	int r, sum = 0;

again:
	r = vio_rxeof(vioq);
	sum += r;
	if (r) {
		vio_populate_rx_mbufs(sc, vioq);
		/* set used event index to the next slot */
		if (virtio_has_feature(vsc, VIRTIO_F_RING_EVENT_IDX)) {
			if (virtio_start_vq_intr(vq->vq_owner, vq))
				goto again;
		}
	}

	return sum;
}

void
vio_rxtick(void *arg)
{
	struct virtqueue *vq = arg;
	struct virtio_softc *vsc = vq->vq_owner;
	struct vio_softc *sc = (struct vio_softc *)vsc->sc_child;
	struct vio_queue *vioq;
	int s, qidx;

	s = splnet();
	for (qidx = 0; qidx < sc->sc_nqueues; qidx++) {
		vioq = &sc->sc_q[qidx];
		vio_populate_rx_mbufs(sc, vioq);
	}
	splx(s);
}

/* free all the mbufs; called from if_stop(disable) */
void
vio_rx_drain(struct vio_softc *sc)
{
	struct vio_queue *vioq;
	int i, qidx;

	for (qidx = 0; qidx < sc->sc_nqueues; qidx++) {
		vioq = &sc->sc_q[qidx];
		for (i = 0; i < vioq->viq_rxvq->vq_num; i++) {
			if (vioq->viq_rxmbufs[i] == NULL)
				continue;
			vio_free_rx_mbuf(sc, vioq, i);
		}
	}
}

/*
 * Transmission implementation
 */
/* actual transmission is done in if_start */
/* tx interrupt; dequeue and free mbufs */
/*
 * tx interrupt is actually disabled unless the tx queue is full, i.e.
 * IFF_OACTIVE is set. vio_txtick is used to make sure that mbufs
 * are dequeued and freed even if no further transfer happens.
 */
int
vio_tx_intr(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct vio_softc *sc = (struct vio_softc *)vsc->sc_child;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	int r;

	r = vio_txeof(vq);
	vio_start(ifp);
	return r;
}

void
vio_txtick(void *arg)
{
	struct virtqueue *vq = arg;
	int s = splnet();
	virtio_check_vq(vq->vq_owner, vq);
	splx(s);
}

int
vio_txeof(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct vio_softc *sc = (struct vio_softc *)vsc->sc_child;
	/* vioq N uses the rx/tx vq pair 2*N and 2*N + 1 */
	struct vio_queue *vioq = &sc->sc_q[vq->vq_index/2];
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	struct mbuf *m;
	int r = 0;
	int slot, len;

	if (!ISSET(ifp->if_flags, IFF_RUNNING))
		return 0;

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		struct virtio_net_hdr *hdr = &vioq->viq_txhdrs[slot];
		r++;
		VIO_DMAMEM_SYNC(vsc, sc, hdr, sc->sc_hdr_size,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_sync(vsc->sc_dmat, vioq->viq_txdmamaps[slot], 0,
		    vioq->viq_txdmamaps[slot]->dm_mapsize,
		    BUS_DMASYNC_POSTWRITE);
		m = vioq->viq_txmbufs[slot];
		bus_dmamap_unload(vsc->sc_dmat, vioq->viq_txdmamaps[slot]);
		vioq->viq_txmbufs[slot] = NULL;
		virtio_dequeue_commit(vq, slot);
		m_freem(m);
	}

	if (r) {
		ifq_clr_oactive(&ifp->if_snd);
		virtio_stop_vq_intr(vsc, vioq->viq_txvq);
	}
	if (vq->vq_used_idx == vq->vq_avail_idx)
		timeout_del(&sc->sc_txtick);
	else if (r)
		timeout_add_sec(&sc->sc_txtick, 1);
	return r;
}

int
vio_encap(struct vio_queue *vioq, int slot, struct mbuf *m)
{
	struct virtio_softc	*vsc = vioq->viq_sc->sc_virtio;
	bus_dmamap_t		 dmap = vioq->viq_txdmamaps[slot];
	int			 r;

	r = bus_dmamap_load_mbuf(vsc->sc_dmat, dmap, m,
	    BUS_DMA_WRITE|BUS_DMA_NOWAIT);
	switch (r) {
	case 0:
		break;
	case EFBIG:
		if (m_defrag(m, M_DONTWAIT) == 0 &&
		    bus_dmamap_load_mbuf(vsc->sc_dmat, dmap, m,
		    BUS_DMA_WRITE|BUS_DMA_NOWAIT) == 0)
			break;

		/* FALLTHROUGH */
	default:
		return ENOBUFS;
	}
	vioq->viq_txmbufs[slot] = m;
	return 0;
}

/* free all the mbufs already put on vq; called from if_stop(disable) */
void
vio_tx_drain(struct vio_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct vio_queue *vioq;
	int i, q;

	for (q = 0; q < sc->sc_nqueues; q++) {
		vioq = &sc->sc_q[q];
		for (i = 0; i < vioq->viq_txvq->vq_num; i++) {
			if (vioq->viq_txmbufs[i] == NULL)
				continue;
			bus_dmamap_unload(vsc->sc_dmat,
			    vioq->viq_txdmamaps[i]);
			m_freem(vioq->viq_txmbufs[i]);
			vioq->viq_txmbufs[i] = NULL;
		}
	}
}

/*
 * Control vq
 */

/*
 * Lock the control queue and the sc_ctrl_* structs and prepare a request.
 *
 * If this function succeeds, the caller must also call either
 * vio_ctrl_submit() or virtio_enqueue_abort(), in both cases followed by
 * vio_ctrl_finish().
 */
int
vio_ctrl_start(struct vio_softc *sc, uint8_t class, uint8_t cmd, int nslots,
    int *slotp)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = sc->sc_ctl_vq;
	int r;

	splassert(IPL_NET);

	while (sc->sc_ctrl_inuse != FREE) {
		if (sc->sc_ctrl_inuse == RESET || vio_needs_reset(sc))
			return ENXIO;
		r = tsleep_nsec(&sc->sc_ctrl_inuse, PRIBIO, "viowait", INFSLP);
		if (r != 0)
			return r;
	}
	sc->sc_ctrl_inuse = INUSE;

	sc->sc_ctrl_cmd->class = class;
	sc->sc_ctrl_cmd->command = cmd;

	r = virtio_enqueue_prep(vq, slotp);
	if (r != 0)
		panic("%s: %s virtio_enqueue_prep: control vq busy",
		    sc->sc_dev.dv_xname, __func__);
	r = virtio_enqueue_reserve(vq, *slotp, nslots + 2);
	if (r != 0)
		panic("%s: %s virtio_enqueue_reserve: control vq busy",
		    sc->sc_dev.dv_xname, __func__);

	vio_dmamem_enqueue(vsc, sc, vq, *slotp, sc->sc_ctrl_cmd,
	    sizeof(*sc->sc_ctrl_cmd), 1);

	return 0;
}

/*
 * Submit a control queue request and wait for the result.
 *
 * vio_ctrl_start() must have been called successfully.
 * After vio_ctrl_submit(), the caller may inspect the
 * data returned from the hypervisor. Afterwards, the caller
 * must always call vio_ctrl_finish().
 */
int
vio_ctrl_submit(struct vio_softc *sc, int slot)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = sc->sc_ctl_vq;
	int r;

	vio_dmamem_enqueue(vsc, sc, vq, slot, sc->sc_ctrl_status,
	    sizeof(*sc->sc_ctrl_status), 0);

	virtio_enqueue_commit(vsc, vq, slot, 1);

	while (sc->sc_ctrl_inuse != DONE) {
		if (sc->sc_ctrl_inuse == RESET || vio_needs_reset(sc))
			return ENXIO;
		r = tsleep_nsec(&sc->sc_ctrl_inuse, PRIBIO, "viodone",
		    VIRTIO_NET_CTRL_TIMEOUT);
		if (r != 0) {
			if (r == EWOULDBLOCK)
				printf("%s: ctrl queue timeout\n",
				    sc->sc_dev.dv_xname);
			vio_ctrl_wakeup(sc, RESET);
			return ENXIO;
		}
	}

	VIO_DMAMEM_SYNC(vsc, sc, sc->sc_ctrl_cmd,
	    sizeof(*sc->sc_ctrl_cmd), BUS_DMASYNC_POSTWRITE);
	VIO_DMAMEM_SYNC(vsc, sc, sc->sc_ctrl_status,
	    sizeof(*sc->sc_ctrl_status), BUS_DMASYNC_POSTREAD);

	if (sc->sc_ctrl_status->ack != VIRTIO_NET_OK)
		return EIO;

	return 0;
}

/*
 * Unlock the control queue and the sc_ctrl_* structs.
 *
 * It is ok to call this function if the control queue is marked dead
 * due to a fatal error.
 */
void
vio_ctrl_finish(struct vio_softc *sc)
{
	if (sc->sc_ctrl_inuse == RESET)
		return;

	vio_ctrl_wakeup(sc, FREE);
}

/* issue a VIRTIO_NET_CTRL_RX class command and wait for completion */
int
vio_ctrl_rx(struct vio_softc *sc, int cmd, int onoff)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = sc->sc_ctl_vq;
	int r, slot;

	r = vio_ctrl_start(sc, VIRTIO_NET_CTRL_RX, cmd, 1, &slot);
	if (r != 0)
		return r;

	sc->sc_ctrl_rx->onoff = onoff;

	vio_dmamem_enqueue(vsc, sc, vq, slot, sc->sc_ctrl_rx,
	    sizeof(*sc->sc_ctrl_rx), 1);

	r = vio_ctrl_submit(sc, slot);
	VIO_DMAMEM_SYNC(vsc, sc, sc->sc_ctrl_rx,
	    sizeof(*sc->sc_ctrl_rx), BUS_DMASYNC_POSTWRITE);
	if (r != 0)
		printf("%s: ctrl cmd %d failed\n", sc->sc_dev.dv_xname, cmd);

	DPRINTF("%s: cmd %d %d: %d\n", __func__, cmd, onoff, r);

	vio_ctrl_finish(sc);
	return r;
}

int
vio_ctrl_guest_offloads(struct vio_softc *sc, uint64_t features)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = sc->sc_ctl_vq;
	int r, slot;

	r = vio_ctrl_start(sc, VIRTIO_NET_CTRL_GUEST_OFFLOADS,
	    VIRTIO_NET_CTRL_GUEST_OFFLOADS_SET, 1, &slot);
	if (r != 0)
		return r;

	sc->sc_ctrl_guest_offloads->offloads = features;

	vio_dmamem_enqueue(vsc, sc, vq, slot, sc->sc_ctrl_guest_offloads,
	    sizeof(*sc->sc_ctrl_guest_offloads), 1);

	r = vio_ctrl_submit(sc, slot);

	VIO_DMAMEM_SYNC(vsc, sc, sc->sc_ctrl_guest_offloads,
	    sizeof(*sc->sc_ctrl_guest_offloads), BUS_DMASYNC_POSTWRITE);

	if (r != 0) {
		printf("%s: offload features 0x%llx failed\n",
		    sc->sc_dev.dv_xname, features);
	}

	DPRINTF("%s: offload features 0x%llx: %d\n", __func__, features, r);

	vio_ctrl_finish(sc);
	return r;
}

void
vio_ctrl_wakeup(struct vio_softc *sc, enum vio_ctrl_state new)
{
	sc->sc_ctrl_inuse = new;
	wakeup(&sc->sc_ctrl_inuse);
}

int
vio_ctrleof(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct vio_softc *sc = (struct vio_softc *)vsc->sc_child;
	int r = 0, ret, slot;

again:
	ret = virtio_dequeue(vsc, vq, &slot, NULL);
	if (ret == ENOENT)
		return r;
	virtio_dequeue_commit(vq, slot);
	r++;
	vio_ctrl_wakeup(sc, DONE);
	if (virtio_start_vq_intr(vsc, vq))
		goto again;

	return r;
}

/* issue VIRTIO_NET_CTRL_MAC_TABLE_SET command and wait for completion */
int
vio_set_rx_filter(struct vio_softc *sc)
{
	/* filter already set in sc_ctrl_mac_tbl */
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = sc->sc_ctl_vq;
	int r, slot;
	size_t len_uc, len_mc;


	r = vio_ctrl_start(sc, VIRTIO_NET_CTRL_MAC,
	    VIRTIO_NET_CTRL_MAC_TABLE_SET, 2, &slot);
	if (r != 0)
		return r;

	len_uc = sizeof(*sc->sc_ctrl_mac_tbl_uc) +
	    sc->sc_ctrl_mac_tbl_uc->nentries * ETHER_ADDR_LEN;
	len_mc = sizeof(*sc->sc_ctrl_mac_tbl_mc) +
	    sc->sc_ctrl_mac_tbl_mc->nentries * ETHER_ADDR_LEN;
	vio_dmamem_enqueue(vsc, sc, vq, slot, sc->sc_ctrl_mac_tbl_uc, len_uc,
	    1);
	vio_dmamem_enqueue(vsc, sc, vq, slot, sc->sc_ctrl_mac_tbl_mc, len_mc,
	    1);

	r = vio_ctrl_submit(sc, slot);
	VIO_DMAMEM_SYNC(vsc, sc, sc->sc_ctrl_mac_tbl_uc, len_uc,
	    BUS_DMASYNC_POSTWRITE);
	VIO_DMAMEM_SYNC(vsc, sc, sc->sc_ctrl_mac_tbl_mc, len_mc,
	    BUS_DMASYNC_POSTWRITE);

	if (r != 0) {
		/* The host's filter table is not large enough */
		printf("%s: failed setting rx filter\n", sc->sc_dev.dv_xname);
	}

	vio_ctrl_finish(sc);
	return r;
}

void
vio_iff(struct vio_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct ifnet *ifp = &sc->sc_ac.ac_if;
	struct arpcom *ac = &sc->sc_ac;
	struct ether_multi *enm;
	struct ether_multistep step;
	int nentries = 0;
	int promisc = 0, allmulti = 0, rxfilter = 0;
	int r;

	splassert(IPL_NET);

	ifp->if_flags &= ~IFF_ALLMULTI;

	if (!virtio_has_feature(vsc, VIRTIO_NET_F_CTRL_RX)) {
		/* no ctrl vq; always promisc */
		ifp->if_flags |= IFF_ALLMULTI | IFF_PROMISC;
		return;
	}

	if (sc->sc_dev.dv_cfdata->cf_flags & CONFFLAG_QEMU_VLAN_BUG)
		ifp->if_flags |= IFF_PROMISC;

	if (ifp->if_flags & IFF_PROMISC || ac->ac_multirangecnt > 0 ||
	    ac->ac_multicnt >= VIRTIO_NET_CTRL_MAC_MC_ENTRIES) {
		ifp->if_flags |= IFF_ALLMULTI;
		if (ifp->if_flags & IFF_PROMISC)
			promisc = 1;
		else
			allmulti = 1;
	} else {
		rxfilter = 1;

		ETHER_FIRST_MULTI(step, ac, enm);
		while (enm != NULL) {
			memcpy(sc->sc_ctrl_mac_tbl_mc->macs[nentries++],
			    enm->enm_addrlo, ETHER_ADDR_LEN);

			ETHER_NEXT_MULTI(step, enm);
		}
	}

	/* set unicast address, VirtualBox wants that */
	memcpy(sc->sc_ctrl_mac_tbl_uc->macs[0], ac->ac_enaddr, ETHER_ADDR_LEN);
	sc->sc_ctrl_mac_tbl_uc->nentries = 1;

	sc->sc_ctrl_mac_tbl_mc->nentries = rxfilter ? nentries : 0;

	r = vio_set_rx_filter(sc);
	if (r == EIO)
		allmulti = 1; /* fallback */
	else if (r != 0)
		return;

	r = vio_ctrl_rx(sc, VIRTIO_NET_CTRL_RX_ALLMULTI, allmulti);
	if (r == EIO)
		promisc = 1; /* fallback */
	else if (r != 0)
		return;

	vio_ctrl_rx(sc, VIRTIO_NET_CTRL_RX_PROMISC, promisc);
}
