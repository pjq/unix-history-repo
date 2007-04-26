/**************************************************************************

Copyright (c) 2007, Chelsio Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

 3. Neither the name of the Chelsio Corporation nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

***************************************************************************/

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/bus_dma.h>
#include <sys/rman.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>


#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/systm.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/cxgb/common/cxgb_common.h>
#include <dev/cxgb/common/cxgb_regs.h>
#include <dev/cxgb/common/cxgb_sge_defs.h>
#include <dev/cxgb/common/cxgb_t3_cpl.h>
#include <dev/cxgb/common/cxgb_firmware_exports.h>

#include <dev/cxgb/sys/mvec.h>

uint32_t collapse_free = 0;
uint32_t mb_free_vec_free = 0;
int      collapse_mbufs = 0;

#define USE_GTS 0

#define SGE_RX_SM_BUF_SIZE	1536
#define SGE_RX_DROP_THRES	16

/*
 * Period of the Tx buffer reclaim timer.  This timer does not need to run
 * frequently as Tx buffers are usually reclaimed by new Tx packets.
 */
#define TX_RECLAIM_PERIOD       (hz >> 2)

/* 
 * work request size in bytes
 */
#define WR_LEN (WR_FLITS * 8)

/* 
 * Values for sge_txq.flags
 */
enum {
	TXQ_RUNNING	= 1 << 0,  /* fetch engine is running */
	TXQ_LAST_PKT_DB = 1 << 1,  /* last packet rang the doorbell */
};

struct tx_desc {
	uint64_t	flit[TX_DESC_FLITS];
} __packed;

struct rx_desc {
	uint32_t	addr_lo;
	uint32_t	len_gen;
	uint32_t	gen2;
	uint32_t	addr_hi;
} __packed;;

struct rsp_desc {               /* response queue descriptor */
	struct rss_header	rss_hdr;
	uint32_t		flags;
	uint32_t		len_cq;
	uint8_t			imm_data[47];
	uint8_t			intr_gen;
} __packed;

#define RX_SW_DESC_MAP_CREATED	(1 << 0)
#define TX_SW_DESC_MAP_CREATED	(1 << 1)
#define RX_SW_DESC_INUSE        (1 << 3)
#define TX_SW_DESC_MAPPED       (1 << 4)

#define RSPQ_NSOP_NEOP           G_RSPD_SOP_EOP(0)
#define RSPQ_EOP                 G_RSPD_SOP_EOP(F_RSPD_EOP)
#define RSPQ_SOP                 G_RSPD_SOP_EOP(F_RSPD_SOP)
#define RSPQ_SOP_EOP             G_RSPD_SOP_EOP(F_RSPD_SOP|F_RSPD_EOP)

struct tx_sw_desc {                /* SW state per Tx descriptor */
	struct mbuf	*m;        
	bus_dmamap_t	map;
	int		flags;
};

struct rx_sw_desc {                /* SW state per Rx descriptor */
	void	        *cl;
	bus_dmamap_t	map;
	int		flags;
};

struct txq_state {
	unsigned int compl;
	unsigned int gen;
	unsigned int pidx;
};

struct refill_fl_cb_arg {
	int               error;
	bus_dma_segment_t seg;
	int               nseg;
};

/*
 * Maps a number of flits to the number of Tx descriptors that can hold them.
 * The formula is
 *
 * desc = 1 + (flits - 2) / (WR_FLITS - 1).
 *
 * HW allows up to 4 descriptors to be combined into a WR.
 */
static uint8_t flit_desc_map[] = {
	0,
#if SGE_NUM_GENBITS == 1
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4
#elif SGE_NUM_GENBITS == 2
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
#else
# error "SGE_NUM_GENBITS must be 1 or 2"
#endif
};


static int lro_default = 0;
int cxgb_debug = 0;

static void t3_free_qset(adapter_t *sc, struct sge_qset *q);
static void sge_timer_cb(void *arg);
static void sge_timer_reclaim(void *arg, int ncount);
static int free_tx_desc(adapter_t *sc, struct sge_txq *q, int n, struct mbuf **m_vec);

/**
 *	reclaim_completed_tx - reclaims completed Tx descriptors
 *	@adapter: the adapter
 *	@q: the Tx queue to reclaim completed descriptors from
 *
 *	Reclaims Tx descriptors that the SGE has indicated it has processed,
 *	and frees the associated buffers if possible.  Called with the Tx
 *	queue's lock held.
 */
static __inline int
reclaim_completed_tx(adapter_t *adapter, struct sge_txq *q, int nbufs, struct mbuf **mvec)
{
	int reclaimed, reclaim = desc_reclaimable(q);
	int n = 0;

	mtx_assert(&q->lock, MA_OWNED);
	
	if (reclaim > 0) {
		n = free_tx_desc(adapter, q, min(reclaim, nbufs), mvec);
		reclaimed = min(reclaim, nbufs);
		q->cleaned += reclaimed;
		q->in_use -= reclaimed;
	} 

	return (n);
}

/**
 *	t3_sge_init - initialize SGE
 *	@adap: the adapter
 *	@p: the SGE parameters
 *
 *	Performs SGE initialization needed every time after a chip reset.
 *	We do not initialize any of the queue sets here, instead the driver
 *	top-level must request those individually.  We also do not enable DMA
 *	here, that should be done after the queues have been set up.
 */
void
t3_sge_init(adapter_t *adap, struct sge_params *p)
{
	u_int ctrl, ups;

	ups = 0; /* = ffs(pci_resource_len(adap->pdev, 2) >> 12); */

	ctrl = F_DROPPKT | V_PKTSHIFT(2) | F_FLMODE | F_AVOIDCQOVFL |
	       F_CQCRDTCTRL |
	       V_HOSTPAGESIZE(PAGE_SHIFT - 11) | F_BIGENDIANINGRESS |
	       V_USERSPACESIZE(ups ? ups - 1 : 0) | F_ISCSICOALESCING;
#if SGE_NUM_GENBITS == 1
	ctrl |= F_EGRGENCTRL;
#endif
	if (adap->params.rev > 0) {
		if (!(adap->flags & (USING_MSIX | USING_MSI)))
			ctrl |= F_ONEINTMULTQ | F_OPTONEINTMULTQ;
		ctrl |= F_CQCRDTCTRL | F_AVOIDCQOVFL;
	}
	t3_write_reg(adap, A_SG_CONTROL, ctrl);
	t3_write_reg(adap, A_SG_EGR_RCQ_DRB_THRSH, V_HIRCQDRBTHRSH(512) |
		     V_LORCQDRBTHRSH(512));
	t3_write_reg(adap, A_SG_TIMER_TICK, core_ticks_per_usec(adap) / 10);
	t3_write_reg(adap, A_SG_CMDQ_CREDIT_TH, V_THRESHOLD(32) |
		     V_TIMEOUT(200 * core_ticks_per_usec(adap)));
	t3_write_reg(adap, A_SG_HI_DRB_HI_THRSH, 1000);
	t3_write_reg(adap, A_SG_HI_DRB_LO_THRSH, 256);
	t3_write_reg(adap, A_SG_LO_DRB_HI_THRSH, 1000);
	t3_write_reg(adap, A_SG_LO_DRB_LO_THRSH, 256);
	t3_write_reg(adap, A_SG_OCO_BASE, V_BASE1(0xfff));
	t3_write_reg(adap, A_SG_DRB_PRI_THRESH, 63 * 1024);
}


/**
 *	sgl_len - calculates the size of an SGL of the given capacity
 *	@n: the number of SGL entries
 *
 *	Calculates the number of flits needed for a scatter/gather list that
 *	can hold the given number of entries.
 */
static __inline unsigned int
sgl_len(unsigned int n)
{
	return ((3 * n) / 2 + (n & 1));
}

/**
 *	get_imm_packet - return the next ingress packet buffer from a response
 *	@resp: the response descriptor containing the packet data
 *
 *	Return a packet containing the immediate data of the given response.
 */
static __inline void
get_imm_packet(adapter_t *sc, const struct rsp_desc *resp, struct mbuf *m, void *cl)
{
	int len;
	uint32_t flags = ntohl(resp->flags);       	
	uint8_t sopeop = G_RSPD_SOP_EOP(flags);

	/*
	 * would be a firmware bug
	 */
	if (sopeop == RSPQ_NSOP_NEOP || sopeop == RSPQ_SOP)
		return;
	
	len = G_RSPD_LEN(ntohl(resp->len_cq));	
	switch (sopeop) {
	case RSPQ_SOP_EOP:
		m->m_len = m->m_pkthdr.len = len; 
		memcpy(m->m_data, resp->imm_data, len); 
		break;
	case RSPQ_EOP:
		memcpy(cl, resp->imm_data, len); 
		m_iovappend(m, cl, MSIZE, len, 0); 
		break;
	}
}


static __inline u_int
flits_to_desc(u_int n)
{
	return (flit_desc_map[n]);
}

void
t3_sge_err_intr_handler(adapter_t *adapter)
{
	unsigned int v, status;

	
	status = t3_read_reg(adapter, A_SG_INT_CAUSE);
	
	if (status & F_RSPQCREDITOVERFOW)
		CH_ALERT(adapter, "SGE response queue credit overflow\n");

	if (status & F_RSPQDISABLED) {
		v = t3_read_reg(adapter, A_SG_RSPQ_FL_STATUS);

		CH_ALERT(adapter,
			 "packet delivered to disabled response queue (0x%x)\n",
			 (v >> S_RSPQ0DISABLED) & 0xff);
	}

	t3_write_reg(adapter, A_SG_INT_CAUSE, status);
	if (status & (F_RSPQCREDITOVERFOW | F_RSPQDISABLED))
		t3_fatal_err(adapter);
}

void
t3_sge_prep(adapter_t *adap, struct sge_params *p)
{
	int i;

	/* XXX Does ETHER_ALIGN need to be accounted for here? */
	p->max_pkt_size = MJUM16BYTES - sizeof(struct cpl_rx_data);

	for (i = 0; i < SGE_QSETS; ++i) {
		struct qset_params *q = p->qset + i;

		q->polling = adap->params.rev > 0;

		if (adap->flags & USING_MSIX)
			q->coalesce_nsecs = 6000;
		else
			q->coalesce_nsecs = 3500;
		
		q->rspq_size = RSPQ_Q_SIZE;
		q->fl_size = FL_Q_SIZE;
		q->jumbo_size = JUMBO_Q_SIZE;
		q->txq_size[TXQ_ETH] = TX_ETH_Q_SIZE;
		q->txq_size[TXQ_OFLD] = 1024;
		q->txq_size[TXQ_CTRL] = 256;
		q->cong_thres = 0;
	}
}

int
t3_sge_alloc(adapter_t *sc)
{

	/* The parent tag. */
	if (bus_dma_tag_create( NULL,			/* parent */
				1, 0,			/* algnmnt, boundary */
				BUS_SPACE_MAXADDR,	/* lowaddr */
				BUS_SPACE_MAXADDR,	/* highaddr */
				NULL, NULL,		/* filter, filterarg */
				BUS_SPACE_MAXSIZE_32BIT,/* maxsize */
				BUS_SPACE_UNRESTRICTED, /* nsegments */
				BUS_SPACE_MAXSIZE_32BIT,/* maxsegsize */
				0,			/* flags */
				NULL, NULL,		/* lock, lockarg */
				&sc->parent_dmat)) {
		device_printf(sc->dev, "Cannot allocate parent DMA tag\n");
		return (ENOMEM);
	}

	/*
	 * DMA tag for normal sized RX frames
	 */
	if (bus_dma_tag_create(sc->parent_dmat, MCLBYTES, 0, BUS_SPACE_MAXADDR,
		BUS_SPACE_MAXADDR, NULL, NULL, MCLBYTES, 1,
		MCLBYTES, BUS_DMA_ALLOCNOW, NULL, NULL, &sc->rx_dmat)) {
		device_printf(sc->dev, "Cannot allocate RX DMA tag\n");
		return (ENOMEM);
	}

	/* 
	 * DMA tag for jumbo sized RX frames.
	 */
	if (bus_dma_tag_create(sc->parent_dmat, MJUMPAGESIZE, 0, BUS_SPACE_MAXADDR,
		BUS_SPACE_MAXADDR, NULL, NULL, MJUMPAGESIZE, 1, MJUMPAGESIZE,
		BUS_DMA_ALLOCNOW, NULL, NULL, &sc->rx_jumbo_dmat)) {
		device_printf(sc->dev, "Cannot allocate RX jumbo DMA tag\n");
		return (ENOMEM);
	}

	/* 
	 * DMA tag for TX frames.
	 */
	if (bus_dma_tag_create(sc->parent_dmat, 1, 0, BUS_SPACE_MAXADDR,
		BUS_SPACE_MAXADDR, NULL, NULL, TX_MAX_SIZE, TX_MAX_SEGS,
		TX_MAX_SIZE, BUS_DMA_ALLOCNOW,
		NULL, NULL, &sc->tx_dmat)) {
		device_printf(sc->dev, "Cannot allocate TX DMA tag\n");
		return (ENOMEM);
	}

	return (0);
}

int
t3_sge_free(struct adapter * sc)
{

	if (sc->tx_dmat != NULL)
		bus_dma_tag_destroy(sc->tx_dmat);

	if (sc->rx_jumbo_dmat != NULL)
		bus_dma_tag_destroy(sc->rx_jumbo_dmat);

	if (sc->rx_dmat != NULL)
		bus_dma_tag_destroy(sc->rx_dmat);

	if (sc->parent_dmat != NULL)
		bus_dma_tag_destroy(sc->parent_dmat);

	return (0);
}

void
t3_update_qset_coalesce(struct sge_qset *qs, const struct qset_params *p)
{

	qs->rspq.holdoff_tmr = max(p->coalesce_nsecs/100, 1U);
	qs->rspq.polling = 0 /* p->polling */;
}

static void
refill_fl_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	struct refill_fl_cb_arg *cb_arg = arg;
	
	cb_arg->error = error;
	cb_arg->seg = segs[0];
	cb_arg->nseg = nseg;

}

/**
 *	refill_fl - refill an SGE free-buffer list
 *	@sc: the controller softc
 *	@q: the free-list to refill
 *	@n: the number of new buffers to allocate
 *
 *	(Re)populate an SGE free-buffer list with up to @n new packet buffers.
 *	The caller must assure that @n does not exceed the queue's capacity.
 */
static void
refill_fl(adapter_t *sc, struct sge_fl *q, int n)
{
	struct rx_sw_desc *sd = &q->sdesc[q->pidx];
	struct rx_desc *d = &q->desc[q->pidx];
	struct refill_fl_cb_arg cb_arg;
	void *cl;
	int err;

	cb_arg.error = 0;
	while (n--) {
		/*
		 * We only allocate a cluster, mbuf allocation happens after rx
		 */
		if ((cl = m_cljget(NULL, M_DONTWAIT, q->buf_size)) == NULL) {
			log(LOG_WARNING, "Failed to allocate cluster\n");
			goto done;
		}
		if ((sd->flags & RX_SW_DESC_MAP_CREATED) == 0) {
			if ((err = bus_dmamap_create(q->entry_tag, 0, &sd->map))) {
				log(LOG_WARNING, "bus_dmamap_create failed %d\n", err);
				uma_zfree(q->zone, cl);
				goto done;
			}
			sd->flags |= RX_SW_DESC_MAP_CREATED;
		}
		err = bus_dmamap_load(q->entry_tag, sd->map, cl, q->buf_size,
		    refill_fl_cb, &cb_arg, 0);
		
		if (err != 0 || cb_arg.error) {
			log(LOG_WARNING, "failure in refill_fl %d\n", cb_arg.error);
			/*
			 * XXX free cluster
			 */
			return;
		}
		
		sd->flags |= RX_SW_DESC_INUSE;
		sd->cl = cl;
		d->addr_lo = htobe32(cb_arg.seg.ds_addr & 0xffffffff);
		d->addr_hi = htobe32(((uint64_t)cb_arg.seg.ds_addr >>32) & 0xffffffff);
		d->len_gen = htobe32(V_FLD_GEN1(q->gen));
		d->gen2 = htobe32(V_FLD_GEN2(q->gen));

		d++;
		sd++;

		if (++q->pidx == q->size) {
			q->pidx = 0;
			q->gen ^= 1;
			sd = q->sdesc;
			d = q->desc;
		}
		q->credits++;
	}

done:
	t3_write_reg(sc, A_SG_KDOORBELL, V_EGRCNTX(q->cntxt_id));
}


/**
 *	free_rx_bufs - free the Rx buffers on an SGE free list
 *	@sc: the controle softc
 *	@q: the SGE free list to clean up
 *
 *	Release the buffers on an SGE free-buffer Rx queue.  HW fetching from
 *	this queue should be stopped before calling this function.
 */
static void
free_rx_bufs(adapter_t *sc, struct sge_fl *q)
{
	u_int cidx = q->cidx;

	while (q->credits--) {
		struct rx_sw_desc *d = &q->sdesc[cidx];

		if (d->flags & RX_SW_DESC_INUSE) {
			bus_dmamap_unload(q->entry_tag, d->map);
			bus_dmamap_destroy(q->entry_tag, d->map);
			uma_zfree(q->zone, d->cl);
		}
		d->cl = NULL;
		if (++cidx == q->size)
			cidx = 0;
	}
}

static __inline void
__refill_fl(adapter_t *adap, struct sge_fl *fl)
{
	refill_fl(adap, fl, min(16U, fl->size - fl->credits));
}

static void
alloc_ring_cb(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	uint32_t *addr;

	addr = arg;
	*addr = segs[0].ds_addr;
}

static int
alloc_ring(adapter_t *sc, size_t nelem, size_t elem_size, size_t sw_size,
    bus_addr_t *phys, void *desc, void *sdesc, bus_dma_tag_t *tag,
    bus_dmamap_t *map, bus_dma_tag_t parent_entry_tag, bus_dma_tag_t *entry_tag)
{
	size_t len = nelem * elem_size;
	void *s = NULL;
	void *p = NULL;
	int err;

	if ((err = bus_dma_tag_create(sc->parent_dmat, PAGE_SIZE, 0,
				      BUS_SPACE_MAXADDR_32BIT,
				      BUS_SPACE_MAXADDR, NULL, NULL, len, 1,
				      len, 0, NULL, NULL, tag)) != 0) {
		device_printf(sc->dev, "Cannot allocate descriptor tag\n");
		return (ENOMEM);
	}

	if ((err = bus_dmamem_alloc(*tag, (void **)&p, BUS_DMA_NOWAIT,
				    map)) != 0) {
		device_printf(sc->dev, "Cannot allocate descriptor memory\n");
		return (ENOMEM);
	}

	bus_dmamap_load(*tag, *map, p, len, alloc_ring_cb, phys, 0);
	bzero(p, len);
	*(void **)desc = p;

	if (sw_size) {
		len = nelem * sw_size;
		s = malloc(len, M_DEVBUF, M_WAITOK);
		bzero(s, len);
		*(void **)sdesc = s;
	}
	if (parent_entry_tag == NULL)
		return (0);
	    
	if ((err = bus_dma_tag_create(parent_entry_tag, 1, 0,
				      BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
		                      NULL, NULL, TX_MAX_SIZE, TX_MAX_SEGS,
				      TX_MAX_SIZE, BUS_DMA_ALLOCNOW,
		                      NULL, NULL, entry_tag)) != 0) {
		device_printf(sc->dev, "Cannot allocate descriptor entry tag\n");
		return (ENOMEM);
	}
	return (0);
}

static void
sge_slow_intr_handler(void *arg, int ncount)
{
	adapter_t *sc = arg;

	t3_slow_intr_handler(sc);
}

static void
sge_timer_cb(void *arg)
{
	adapter_t *sc = arg;
	struct sge_qset *qs;
	struct sge_txq  *txq;
	int i, j;
	int reclaim_eth, reclaim_ofl, refill_rx;
	
	for (i = 0; i < sc->params.nports; i++) 
		for (j = 0; j < sc->port[i].nqsets; j++) {
			qs = &sc->sge.qs[i + j];
			txq = &qs->txq[0];
			reclaim_eth = txq[TXQ_ETH].processed - txq[TXQ_ETH].cleaned;
			reclaim_ofl = txq[TXQ_OFLD].processed - txq[TXQ_OFLD].cleaned;
			refill_rx = ((qs->fl[0].credits < qs->fl[0].size) || 
			    (qs->fl[1].credits < qs->fl[1].size));
			if (reclaim_eth || reclaim_ofl || refill_rx) {
				taskqueue_enqueue(sc->tq, &sc->timer_reclaim_task);
				goto done;
			}
		}
done:
	callout_reset(&sc->sge_timer_ch, TX_RECLAIM_PERIOD, sge_timer_cb, sc);
}

/*
 * This is meant to be a catch-all function to keep sge state private
 * to sge.c
 *
 */
int
t3_sge_init_sw(adapter_t *sc)
{

	callout_init(&sc->sge_timer_ch, CALLOUT_MPSAFE);
	callout_reset(&sc->sge_timer_ch, TX_RECLAIM_PERIOD, sge_timer_cb, sc);
	TASK_INIT(&sc->timer_reclaim_task, 0, sge_timer_reclaim, sc);
	TASK_INIT(&sc->slow_intr_task, 0, sge_slow_intr_handler, sc);
	return (0);
}

void
t3_sge_deinit_sw(adapter_t *sc)
{
	callout_drain(&sc->sge_timer_ch);
	if (sc->tq) {
		taskqueue_drain(sc->tq, &sc->timer_reclaim_task);
		taskqueue_drain(sc->tq, &sc->slow_intr_task);
	}
}

/**
 *	refill_rspq - replenish an SGE response queue
 *	@adapter: the adapter
 *	@q: the response queue to replenish
 *	@credits: how many new responses to make available
 *
 *	Replenishes a response queue by making the supplied number of responses
 *	available to HW.
 */
static __inline void
refill_rspq(adapter_t *sc, const struct sge_rspq *q, u_int credits)
{

	/* mbufs are allocated on demand when a rspq entry is processed. */
	t3_write_reg(sc, A_SG_RSPQ_CREDIT_RETURN,
		     V_RSPQ(q->cntxt_id) | V_CREDITS(credits));
}


static void
sge_timer_reclaim(void *arg, int ncount)
{
	adapter_t *sc = arg;
	int i, nqsets = 0;
	struct sge_qset *qs;
	struct sge_txq *txq;
	struct mtx *lock;
	struct mbuf *m_vec[TX_CLEAN_MAX_DESC];
	int n, reclaimable;
	/* 
	 * XXX assuming these quantities are allowed to change during operation
	 */
	for (i = 0; i < sc->params.nports; i++) 
		nqsets += sc->port[i].nqsets;

	for (i = 0; i < nqsets; i++) {
		qs = &sc->sge.qs[i];
		txq = &qs->txq[TXQ_ETH];
		reclaimable = desc_reclaimable(txq);
		if (reclaimable > 0) {
			mtx_lock(&txq->lock);			
			n = reclaim_completed_tx(sc, txq, TX_CLEAN_MAX_DESC, m_vec);
			mtx_unlock(&txq->lock);
			
			for (i = 0; i < n; i++) {
				m_freem_vec(m_vec[i]);
			}
		} 
		    
		txq = &qs->txq[TXQ_OFLD];
		reclaimable = desc_reclaimable(txq);
		if (reclaimable > 0) {
			mtx_lock(&txq->lock);
			n = reclaim_completed_tx(sc, txq, TX_CLEAN_MAX_DESC, m_vec);
			mtx_unlock(&txq->lock);

			for (i = 0; i < n; i++) {
				m_freem_vec(m_vec[i]);
			}
		}

		lock = (sc->flags & USING_MSIX) ? &qs->rspq.lock :
			    &sc->sge.qs[0].rspq.lock;

		if (mtx_trylock(lock)) {
			/* XXX currently assume that we are *NOT* polling */
			uint32_t status = t3_read_reg(sc, A_SG_RSPQ_FL_STATUS);

			if (qs->fl[0].credits < qs->fl[0].size - 16)
				__refill_fl(sc, &qs->fl[0]);
			if (qs->fl[1].credits < qs->fl[1].size - 16)
				__refill_fl(sc, &qs->fl[1]);
			
			if (status & (1 << qs->rspq.cntxt_id)) {
				if (qs->rspq.credits) {
					refill_rspq(sc, &qs->rspq, 1);
					qs->rspq.credits--;
					t3_write_reg(sc, A_SG_RSPQ_FL_STATUS, 
					    1 << qs->rspq.cntxt_id);
				}
			}
			mtx_unlock(lock);
		}
	}
}

/**
 *	init_qset_cntxt - initialize an SGE queue set context info
 *	@qs: the queue set
 *	@id: the queue set id
 *
 *	Initializes the TIDs and context ids for the queues of a queue set.
 */
static void
init_qset_cntxt(struct sge_qset *qs, u_int id)
{

	qs->rspq.cntxt_id = id;
	qs->fl[0].cntxt_id = 2 * id;
	qs->fl[1].cntxt_id = 2 * id + 1;
	qs->txq[TXQ_ETH].cntxt_id = FW_TUNNEL_SGEEC_START + id;
	qs->txq[TXQ_ETH].token = FW_TUNNEL_TID_START + id;
	qs->txq[TXQ_OFLD].cntxt_id = FW_OFLD_SGEEC_START + id;
	qs->txq[TXQ_CTRL].cntxt_id = FW_CTRL_SGEEC_START + id;
	qs->txq[TXQ_CTRL].token = FW_CTRL_TID_START + id;
}


static void
txq_prod(struct sge_txq *txq, unsigned int ndesc, struct txq_state *txqs)
{
	txq->in_use += ndesc;
	/*
	 * XXX we don't handle stopping of queue
	 * presumably start handles this when we bump against the end
	 */
	txqs->gen = txq->gen;
	txq->unacked += ndesc;
	txqs->compl = (txq->unacked & 8) << (S_WR_COMPL - 3);
	txq->unacked &= 7;
	txqs->pidx = txq->pidx;
	txq->pidx += ndesc;
	
	if (txq->pidx >= txq->size) {
		txq->pidx -= txq->size;
		txq->gen ^= 1;
	}

}

/**
 *	calc_tx_descs - calculate the number of Tx descriptors for a packet
 *	@m: the packet mbufs
 *      @nsegs: the number of segments 
 *
 * 	Returns the number of Tx descriptors needed for the given Ethernet
 * 	packet.  Ethernet packets require addition of WR and CPL headers.
 */
static __inline unsigned int
calc_tx_descs(const struct mbuf *m, int nsegs)
{
	unsigned int flits;

	if (m->m_pkthdr.len <= WR_LEN - sizeof(struct cpl_tx_pkt))
		return 1;

	flits = sgl_len(nsegs) + 2;
#ifdef TSO_SUPPORTED
	if  (m->m_pkthdr.csum_flags & (CSUM_TSO))
		flits++;
#endif	
	return flits_to_desc(flits);
}

static unsigned int
busdma_map_mbufs(struct mbuf **m, struct sge_txq *txq,
    struct tx_sw_desc *stx, bus_dma_segment_t *segs, int *nsegs)
{
	struct mbuf *m0;
	int err, pktlen;
	
	m0 = *m;
	pktlen = m0->m_pkthdr.len;

	err = bus_dmamap_load_mvec_sg(txq->entry_tag, stx->map, m0, segs, nsegs, 0);
#ifdef DEBUG		
	if (err) {
		int n = 0;
		struct mbuf *mtmp = m0;
		while(mtmp) {
			n++;
			mtmp = mtmp->m_next;
		}
		printf("map_mbufs: bus_dmamap_load_mbuf_sg failed with %d - pkthdr.len==%d nmbufs=%d\n",
		    err, m0->m_pkthdr.len, n);
	}
#endif
	if (err == EFBIG) {
		/* Too many segments, try to defrag */
		m0 = m_defrag(m0, M_NOWAIT);
		if (m0 == NULL) {
			m_freem(*m);
			*m = NULL;
			return (ENOBUFS);
		}
		*m = m0;
		err = bus_dmamap_load_mbuf_sg(txq->entry_tag, stx->map, m0, segs, nsegs, 0);
	}

	if (err == ENOMEM) {
		return (err);
	}

	if (err) {
		if (cxgb_debug)
			printf("map failure err=%d pktlen=%d\n", err, pktlen);
		m_freem_vec(m0);
		*m = NULL;
		return (err);
	}

	bus_dmamap_sync(txq->entry_tag, stx->map, BUS_DMASYNC_PREWRITE);
	stx->flags |= TX_SW_DESC_MAPPED;

	return (0);
}

/**
 *	make_sgl - populate a scatter/gather list for a packet
 *	@sgp: the SGL to populate
 *	@segs: the packet dma segments
 *	@nsegs: the number of segments
 *
 *	Generates a scatter/gather list for the buffers that make up a packet
 *	and returns the SGL size in 8-byte words.  The caller must size the SGL
 *	appropriately.
 */
static __inline void
make_sgl(struct sg_ent *sgp, bus_dma_segment_t *segs, int nsegs)
{
	int i, idx;
	
	for (idx = 0, i = 0; i < nsegs; i++, idx ^= 1) {
		if (i && idx == 0) 
			++sgp;

		sgp->len[idx] = htobe32(segs[i].ds_len);
		sgp->addr[idx] = htobe64(segs[i].ds_addr);
	}
	
	if (idx)
		sgp->len[idx] = 0;
}
	
/**
 *	check_ring_tx_db - check and potentially ring a Tx queue's doorbell
 *	@adap: the adapter
 *	@q: the Tx queue
 *
 *	Ring the doorbel if a Tx queue is asleep.  There is a natural race,
 *	where the HW is going to sleep just after we checked, however,
 *	then the interrupt handler will detect the outstanding TX packet
 *	and ring the doorbell for us.
 *
 *	When GTS is disabled we unconditionally ring the doorbell.
 */
static __inline void
check_ring_tx_db(adapter_t *adap, struct sge_txq *q)
{
#if USE_GTS
	clear_bit(TXQ_LAST_PKT_DB, &q->flags);
	if (test_and_set_bit(TXQ_RUNNING, &q->flags) == 0) {
		set_bit(TXQ_LAST_PKT_DB, &q->flags);
#ifdef T3_TRACE
		T3_TRACE1(adap->tb[q->cntxt_id & 7], "doorbell Tx, cntxt %d",
			  q->cntxt_id);
#endif
		t3_write_reg(adap, A_SG_KDOORBELL,
			     F_SELEGRCNTX | V_EGRCNTX(q->cntxt_id));
	}
#else
	wmb();            /* write descriptors before telling HW */
	t3_write_reg(adap, A_SG_KDOORBELL,
		     F_SELEGRCNTX | V_EGRCNTX(q->cntxt_id));
#endif
}

static __inline void
wr_gen2(struct tx_desc *d, unsigned int gen)
{
#if SGE_NUM_GENBITS == 2
	d->flit[TX_DESC_FLITS - 1] = htobe64(gen);
#endif
}

/* sizeof(*eh) + sizeof(*vhdr) + sizeof(*ip) + sizeof(*tcp) */
#define TCPPKTHDRSIZE (ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN + 20 + 20)

int
t3_encap(struct port_info *p, struct mbuf **m)
{
	adapter_t *sc;
	struct mbuf *m0;
	struct sge_qset *qs;
	struct sge_txq *txq;
	struct tx_sw_desc *stx;
	struct txq_state txqs;
	unsigned int nsegs, ndesc, flits, cntrl, mlen;
	int err, tso_info = 0;

	struct work_request_hdr *wrp;
	struct tx_sw_desc *txsd;
	struct sg_ent *sgp, sgl[TX_MAX_SEGS / 2 + 1];
	bus_dma_segment_t segs[TX_MAX_SEGS];
	uint32_t wr_hi, wr_lo, sgl_flits; 

	struct tx_desc *txd;
	struct cpl_tx_pkt *cpl;
	
	DPRINTF("t3_encap ");
	m0 = *m;	
	sc = p->adapter;
	qs = &sc->sge.qs[p->first_qset];
	txq = &qs->txq[TXQ_ETH];
	stx = &txq->sdesc[txq->pidx];
	txd = &txq->desc[txq->pidx];
	cpl = (struct cpl_tx_pkt *)txd;
	mlen = m0->m_pkthdr.len;
	cpl->len = htonl(mlen | 0x80000000);
	
	DPRINTF("mlen=%d\n", mlen);
	/*
	 * XXX handle checksum, TSO, and VLAN here
	 *	 
	 */
	cntrl = V_TXPKT_INTF(p->port);

	/*
	 * XXX need to add VLAN support for 6.x
	 */
#ifdef VLAN_SUPPORTED
	if (m0->m_flags & M_VLANTAG) 
		cntrl |= F_TXPKT_VLAN_VLD | V_TXPKT_VLAN(m0->m_pkthdr.ether_vtag);
	if  (m0->m_pkthdr.csum_flags & (CSUM_TSO))
		tso_info = V_LSO_MSS(m0->m_pkthdr.tso_segsz);
#endif		
	if (tso_info) {
		int eth_type;
		struct cpl_tx_pkt_lso *hdr = (struct cpl_tx_pkt_lso *) cpl;
		struct ip *ip;
		struct tcphdr *tcp;
		uint8_t *pkthdr, tmp[TCPPKTHDRSIZE]; /* is this too large for the stack? */
		
		txd->flit[2] = 0;
		cntrl |= V_TXPKT_OPCODE(CPL_TX_PKT_LSO);
		hdr->cntrl = htonl(cntrl);
		
		if (__predict_false(m0->m_len < TCPPKTHDRSIZE)) {
			pkthdr = &tmp[0];
			m_copydata(m0, 0, TCPPKTHDRSIZE, pkthdr);
		} else {
			pkthdr = m0->m_data;
		}

		if (__predict_false(m0->m_flags & M_VLANTAG)) {
			eth_type = CPL_ETH_II_VLAN;
			ip = (struct ip *)(pkthdr + ETHER_HDR_LEN +
			    ETHER_VLAN_ENCAP_LEN);
		} else {
			eth_type = CPL_ETH_II;
			ip = (struct ip *)(pkthdr + ETHER_HDR_LEN);
		}
		tcp = (struct tcphdr *)((uint8_t *)ip +
		    sizeof(*ip)); 

		tso_info |= V_LSO_ETH_TYPE(eth_type) |
			    V_LSO_IPHDR_WORDS(ip->ip_hl) |
			    V_LSO_TCPHDR_WORDS(tcp->th_off);
		hdr->lso_info = htonl(tso_info);
		flits = 3;	
	} else {
		cntrl |= V_TXPKT_OPCODE(CPL_TX_PKT);
		cpl->cntrl = htonl(cntrl);
		
		if (mlen <= WR_LEN - sizeof(*cpl)) {
			txq_prod(txq, 1, &txqs);
			txq->sdesc[txqs.pidx].m = m0;
			
			if (m0->m_len == m0->m_pkthdr.len)
				memcpy(&txd->flit[2], m0->m_data, mlen);
			else
				m_copydata(m0, 0, mlen, (caddr_t)&txd->flit[2]);

			flits = (mlen + 7) / 8 + 2;
			cpl->wr.wr_hi = htonl(V_WR_BCNTLFLT(mlen & 7) |
					  V_WR_OP(FW_WROPCODE_TUNNEL_TX_PKT) |
					  F_WR_SOP | F_WR_EOP | txqs.compl);
			wmb();
			cpl->wr.wr_lo = htonl(V_WR_LEN(flits) |
			    V_WR_GEN(txqs.gen) | V_WR_TID(txq->token));

			wr_gen2(txd, txqs.gen);
			check_ring_tx_db(sc, txq);
			return (0);
		}
		flits = 2;
	}

	wrp = (struct work_request_hdr *)txd;
	
	if ((err = busdma_map_mbufs(m, txq, stx, segs, &nsegs)) != 0) {
		return (err);
	}
	m0 = *m;
	ndesc = calc_tx_descs(m0, nsegs);
	
	sgp = (ndesc == 1) ? (struct sg_ent *)&txd->flit[flits] : &sgl[0];
	make_sgl(sgp, segs, nsegs);

	sgl_flits = sgl_len(nsegs);

	DPRINTF("make_sgl success nsegs==%d ndesc==%d\n", nsegs, ndesc);
	txq_prod(txq, ndesc, &txqs);
	txsd = &txq->sdesc[txqs.pidx];
	wr_hi = htonl(V_WR_OP(FW_WROPCODE_TUNNEL_TX_PKT) | txqs.compl);
	wr_lo = htonl(V_WR_TID(txq->token));
	txsd->m = m0;
	
	if (__predict_true(ndesc == 1)) {
		wrp->wr_hi = htonl(F_WR_SOP | F_WR_EOP | V_WR_DATATYPE(1) |
		    V_WR_SGLSFLT(flits)) | wr_hi;
		wmb();
		wrp->wr_lo = htonl(V_WR_LEN(flits + sgl_flits) |
		    V_WR_GEN(txqs.gen)) | wr_lo;
		/* XXX gen? */
		wr_gen2(txd, txqs.gen);
	} else {
		unsigned int ogen = txqs.gen;
		const uint64_t *fp = (const uint64_t *)sgl;
		struct work_request_hdr *wp = wrp;
		
		/* XXX - CHECK ME */
		wrp->wr_hi = htonl(F_WR_SOP | V_WR_DATATYPE(1) |
		    V_WR_SGLSFLT(flits)) | wr_hi;
		
		while (sgl_flits) {
			unsigned int avail = WR_FLITS - flits;

			if (avail > sgl_flits)
				avail = sgl_flits;
			memcpy(&txd->flit[flits], fp, avail * sizeof(*fp));
			sgl_flits -= avail;
			ndesc--;
			if (!sgl_flits)
				break;
			
			fp += avail;
			txd++;
			txsd++;
			if (++txqs.pidx == txq->size) {
				txqs.pidx = 0;
				txqs.gen ^= 1;
				txd = txq->desc;
				txsd = txq->sdesc;
			}
			
			/*
			 * when the head of the mbuf chain
			 * is freed all clusters will be freed
			 * with it
			 */
			txsd->m = NULL;
			wrp = (struct work_request_hdr *)txd;
			wrp->wr_hi = htonl(V_WR_DATATYPE(1) |
			    V_WR_SGLSFLT(1)) | wr_hi;
			wrp->wr_lo = htonl(V_WR_LEN(min(WR_FLITS,
				    sgl_flits + 1)) |
			    V_WR_GEN(txqs.gen)) | wr_lo;
			wr_gen2(txd, txqs.gen);
			flits = 1;
		}
#ifdef WHY			
		skb->priority = pidx;
#endif
		wrp->wr_hi |= htonl(F_WR_EOP);
		wmb();
		wp->wr_lo = htonl(V_WR_LEN(WR_FLITS) | V_WR_GEN(ogen)) | wr_lo;
		wr_gen2((struct tx_desc *)wp, ogen);
	}
	check_ring_tx_db(p->adapter, txq);

	return (0);
}


/**
 *	write_imm - write a packet into a Tx descriptor as immediate data
 *	@d: the Tx descriptor to write
 *	@m: the packet
 *	@len: the length of packet data to write as immediate data
 *	@gen: the generation bit value to write
 *
 *	Writes a packet as immediate data into a Tx descriptor.  The packet
 *	contains a work request at its beginning.  We must write the packet
 *	carefully so the SGE doesn't read accidentally before it's written in
 *	its entirety.
 */
static __inline void write_imm(struct tx_desc *d, struct mbuf *m,
			     unsigned int len, unsigned int gen)
{
	struct work_request_hdr *from = (struct work_request_hdr *)m->m_data;
	struct work_request_hdr *to = (struct work_request_hdr *)d;

	memcpy(&to[1], &from[1], len - sizeof(*from));
	to->wr_hi = from->wr_hi | htonl(F_WR_SOP | F_WR_EOP |
					V_WR_BCNTLFLT(len & 7));
	wmb();
	to->wr_lo = from->wr_lo | htonl(V_WR_GEN(gen) |
					V_WR_LEN((len + 7) / 8));
	wr_gen2(d, gen);
	m_freem(m);
}

/**
 *	check_desc_avail - check descriptor availability on a send queue
 *	@adap: the adapter
 *	@q: the TX queue
 *	@m: the packet needing the descriptors
 *	@ndesc: the number of Tx descriptors needed
 *	@qid: the Tx queue number in its queue set (TXQ_OFLD or TXQ_CTRL)
 *
 *	Checks if the requested number of Tx descriptors is available on an
 *	SGE send queue.  If the queue is already suspended or not enough
 *	descriptors are available the packet is queued for later transmission.
 *	Must be called with the Tx queue locked.
 *
 *	Returns 0 if enough descriptors are available, 1 if there aren't
 *	enough descriptors and the packet has been queued, and 2 if the caller
 *	needs to retry because there weren't enough descriptors at the
 *	beginning of the call but some freed up in the mean time.
 */
static __inline int
check_desc_avail(adapter_t *adap, struct sge_txq *q,
				   struct mbuf *m, unsigned int ndesc,
				   unsigned int qid)
{
	/* 
	 * XXX We currently only use this for checking the control queue
	 * the control queue is only used for binding qsets which happens
	 * at init time so we are guaranteed enough descriptors
	 */
#if 0	
	if (__predict_false(!skb_queue_empty(&q->sendq))) {
addq_exit:	__skb_queue_tail(&q->sendq, skb);
		return 1;
	}
	if (__predict_false(q->size - q->in_use < ndesc)) {

		struct sge_qset *qs = txq_to_qset(q, qid);

		set_bit(qid, &qs->txq_stopped);
		smp_mb__after_clear_bit();

		if (should_restart_tx(q) &&
		    test_and_clear_bit(qid, &qs->txq_stopped))
			return 2;

		q->stops++;
		goto addq_exit;
	}
#endif	
	return 0;
}


/**
 *	reclaim_completed_tx_imm - reclaim completed control-queue Tx descs
 *	@q: the SGE control Tx queue
 *
 *	This is a variant of reclaim_completed_tx() that is used for Tx queues
 *	that send only immediate data (presently just the control queues) and
 *	thus do not have any sk_buffs to release.
 */
static __inline void
reclaim_completed_tx_imm(struct sge_txq *q)
{
	unsigned int reclaim = q->processed - q->cleaned;

	mtx_assert(&q->lock, MA_OWNED);
	
	q->in_use -= reclaim;
	q->cleaned += reclaim;
}

static __inline int
immediate(const struct mbuf *m)
{
	return m->m_len <= WR_LEN  && m->m_pkthdr.len <= WR_LEN ;
}

/**
 *	ctrl_xmit - send a packet through an SGE control Tx queue
 *	@adap: the adapter
 *	@q: the control queue
 *	@m: the packet
 *
 *	Send a packet through an SGE control Tx queue.  Packets sent through
 *	a control queue must fit entirely as immediate data in a single Tx
 *	descriptor and have no page fragments.
 */
static int
ctrl_xmit(adapter_t *adap, struct sge_txq *q, struct mbuf *m)
{
	int ret;
	struct work_request_hdr *wrp = (struct work_request_hdr *)m->m_data;

	if (__predict_false(!immediate(m))) {
		m_freem(m);
		return 0;
	}

	wrp->wr_hi |= htonl(F_WR_SOP | F_WR_EOP);
	wrp->wr_lo = htonl(V_WR_TID(q->token));

	mtx_lock(&q->lock);
again:	reclaim_completed_tx_imm(q);

	ret = check_desc_avail(adap, q, m, 1, TXQ_CTRL);
	if (__predict_false(ret)) {
		if (ret == 1) {
			mtx_unlock(&q->lock);
			return (-1);
		}
		goto again;
	}

	write_imm(&q->desc[q->pidx], m, m->m_len, q->gen);

	q->in_use++;
	if (++q->pidx >= q->size) {
		q->pidx = 0;
		q->gen ^= 1;
	}
	mtx_unlock(&q->lock);
	wmb();
	t3_write_reg(adap, A_SG_KDOORBELL,
		     F_SELEGRCNTX | V_EGRCNTX(q->cntxt_id));
	return (0);
}

#ifdef RESTART_CTRLQ
/**
 *	restart_ctrlq - restart a suspended control queue
 *	@qs: the queue set cotaining the control queue
 *
 *	Resumes transmission on a suspended Tx control queue.
 */
static void
restart_ctrlq(unsigned long data)
{
	struct mbuf *m;
	struct sge_qset *qs = (struct sge_qset *)data;
	struct sge_txq *q = &qs->txq[TXQ_CTRL];
	adapter_t *adap = qs->port->adapter;

	mtx_lock(&q->lock);
again:	reclaim_completed_tx_imm(q);
	
	while (q->in_use < q->size &&
	       (skb = __skb_dequeue(&q->sendq)) != NULL) {

		write_imm(&q->desc[q->pidx], skb, skb->len, q->gen);

		if (++q->pidx >= q->size) {
			q->pidx = 0;
			q->gen ^= 1;
		}
		q->in_use++;
	}
	if (!skb_queue_empty(&q->sendq)) {
		set_bit(TXQ_CTRL, &qs->txq_stopped);
		smp_mb__after_clear_bit();

		if (should_restart_tx(q) &&
		    test_and_clear_bit(TXQ_CTRL, &qs->txq_stopped))
			goto again;
		q->stops++;
	}

	mtx_unlock(&q->lock);
	t3_write_reg(adap, A_SG_KDOORBELL,
		     F_SELEGRCNTX | V_EGRCNTX(q->cntxt_id));
}
#endif

/*
 * Send a management message through control queue 0
 */
int
t3_mgmt_tx(struct adapter *adap, struct mbuf *m)
{
	return ctrl_xmit(adap, &adap->sge.qs[0].txq[TXQ_CTRL], m);
}

/**
 *	t3_sge_alloc_qset - initialize an SGE queue set
 *	@sc: the controller softc
 *	@id: the queue set id
 *	@nports: how many Ethernet ports will be using this queue set
 *	@irq_vec_idx: the IRQ vector index for response queue interrupts
 *	@p: configuration parameters for this queue set
 *	@ntxq: number of Tx queues for the queue set
 *	@pi: port info for queue set
 *
 *	Allocate resources and initialize an SGE queue set.  A queue set
 *	comprises a response queue, two Rx free-buffer queues, and up to 3
 *	Tx queues.  The Tx queues are assigned roles in the order Ethernet
 *	queue, offload queue, and control queue.
 */
int
t3_sge_alloc_qset(adapter_t *sc, u_int id, int nports, int irq_vec_idx,
		  const struct qset_params *p, int ntxq, struct port_info *pi)
{
	struct sge_qset *q = &sc->sge.qs[id];
	int i, ret = 0;

	init_qset_cntxt(q, id);
	
	if ((ret = alloc_ring(sc, p->fl_size, sizeof(struct rx_desc),
		    sizeof(struct rx_sw_desc), &q->fl[0].phys_addr,
		    &q->fl[0].desc, &q->fl[0].sdesc,
		    &q->fl[0].desc_tag, &q->fl[0].desc_map,
		    sc->rx_dmat, &q->fl[0].entry_tag)) != 0) {
		printf("error %d from alloc ring fl0\n", ret);
		goto err;
	}

	if ((ret = alloc_ring(sc, p->jumbo_size, sizeof(struct rx_desc),
		    sizeof(struct rx_sw_desc), &q->fl[1].phys_addr,
		    &q->fl[1].desc, &q->fl[1].sdesc,
		    &q->fl[1].desc_tag, &q->fl[1].desc_map,
		    sc->rx_jumbo_dmat, &q->fl[1].entry_tag)) != 0) {
		printf("error %d from alloc ring fl1\n", ret);
		goto err;
	}

	if ((ret = alloc_ring(sc, p->rspq_size, sizeof(struct rsp_desc), 0,
		    &q->rspq.phys_addr, &q->rspq.desc, NULL,
		    &q->rspq.desc_tag, &q->rspq.desc_map,
		    NULL, NULL)) != 0) {
		printf("error %d from alloc ring rspq\n", ret);
		goto err;
	}

	for (i = 0; i < ntxq; ++i) {
		/*
		 * The control queue always uses immediate data so does not
		 * need to keep track of any mbufs.
		 * XXX Placeholder for future TOE support.
		 */
		size_t sz = i == TXQ_CTRL ? 0 : sizeof(struct tx_sw_desc);

		if ((ret = alloc_ring(sc, p->txq_size[i],
			    sizeof(struct tx_desc), sz,
			    &q->txq[i].phys_addr, &q->txq[i].desc,
			    &q->txq[i].sdesc, &q->txq[i].desc_tag,
			    &q->txq[i].desc_map,
			    sc->tx_dmat, &q->txq[i].entry_tag)) != 0) {
			printf("error %d from alloc ring tx %i\n", ret, i);
			goto err;
		}
		q->txq[i].gen = 1;
		q->txq[i].size = p->txq_size[i];
		mtx_init(&q->txq[i].lock, "t3 txq lock", NULL, MTX_DEF);
	}

	q->fl[0].gen = q->fl[1].gen = 1;
	q->fl[0].size = p->fl_size;
	q->fl[1].size = p->jumbo_size;

	q->rspq.gen = 1;
	q->rspq.size = p->rspq_size;
	mtx_init(&q->rspq.lock, "t3 rspq lock", NULL, MTX_DEF);
	
	q->txq[TXQ_ETH].stop_thres = nports *
	    flits_to_desc(sgl_len(TX_MAX_SEGS + 1) + 3);

	q->fl[0].buf_size = MCLBYTES;
	q->fl[0].zone = zone_clust;
	q->fl[0].type = EXT_CLUSTER;
	q->fl[1].buf_size = MJUMPAGESIZE;
	q->fl[1].zone = zone_jumbop;
	q->fl[1].type = EXT_JUMBOP;
	
	q->lro.enabled = lro_default;
	
	mtx_lock(&sc->sge.reg_lock);
	ret = -t3_sge_init_rspcntxt(sc, q->rspq.cntxt_id, irq_vec_idx,
				   q->rspq.phys_addr, q->rspq.size,
				   q->fl[0].buf_size, 1, 0);
	if (ret) {
		printf("error %d from t3_sge_init_rspcntxt\n", ret);
		goto err_unlock;
	}

	for (i = 0; i < SGE_RXQ_PER_SET; ++i) {
		ret = -t3_sge_init_flcntxt(sc, q->fl[i].cntxt_id, 0,
					  q->fl[i].phys_addr, q->fl[i].size,
					  q->fl[i].buf_size, p->cong_thres, 1,
					  0);
		if (ret) {
			printf("error %d from t3_sge_init_flcntxt for index i=%d\n", ret, i);
			goto err_unlock;
		}
	}

	ret = -t3_sge_init_ecntxt(sc, q->txq[TXQ_ETH].cntxt_id, USE_GTS,
				 SGE_CNTXT_ETH, id, q->txq[TXQ_ETH].phys_addr,
				 q->txq[TXQ_ETH].size, q->txq[TXQ_ETH].token,
				 1, 0);
	if (ret) {
		printf("error %d from t3_sge_init_ecntxt\n", ret);
		goto err_unlock;
	}

	if (ntxq > 1) {
		ret = -t3_sge_init_ecntxt(sc, q->txq[TXQ_OFLD].cntxt_id,
					 USE_GTS, SGE_CNTXT_OFLD, id,
					 q->txq[TXQ_OFLD].phys_addr,
					 q->txq[TXQ_OFLD].size, 0, 1, 0);
		if (ret) {
			printf("error %d from t3_sge_init_ecntxt\n", ret);
			goto err_unlock;
		}
	}

	if (ntxq > 2) {
		ret = -t3_sge_init_ecntxt(sc, q->txq[TXQ_CTRL].cntxt_id, 0,
					 SGE_CNTXT_CTRL, id,
					 q->txq[TXQ_CTRL].phys_addr,
					 q->txq[TXQ_CTRL].size,
					 q->txq[TXQ_CTRL].token, 1, 0);
		if (ret) {
			printf("error %d from t3_sge_init_ecntxt\n", ret);
			goto err_unlock;
		}
	}
	
	mtx_unlock(&sc->sge.reg_lock);
	t3_update_qset_coalesce(q, p);
	q->port = pi;
	
	refill_fl(sc, &q->fl[0], q->fl[0].size);
	refill_fl(sc, &q->fl[1], q->fl[1].size);
	refill_rspq(sc, &q->rspq, q->rspq.size - 1);

	t3_write_reg(sc, A_SG_GTS, V_RSPQ(q->rspq.cntxt_id) |
		     V_NEWTIMER(q->rspq.holdoff_tmr));

	return (0);

err_unlock:
	mtx_unlock(&sc->sge.reg_lock);
err:	
	t3_free_qset(sc, q);

	return (ret);
}


/**
 *	free_qset - free the resources of an SGE queue set
 *	@sc: the controller owning the queue set
 *	@q: the queue set
 *
 *	Release the HW and SW resources associated with an SGE queue set, such
 *	as HW contexts, packet buffers, and descriptor rings.  Traffic to the
 *	queue set must be quiesced prior to calling this.
 */
static void
t3_free_qset(adapter_t *sc, struct sge_qset *q)
{
	int i;

	for (i = 0; i < SGE_RXQ_PER_SET; ++i) {
		if (q->fl[i].desc) {
			mtx_lock(&sc->sge.reg_lock);
			t3_sge_disable_fl(sc, q->fl[i].cntxt_id);
			mtx_unlock(&sc->sge.reg_lock);
			bus_dmamap_unload(q->fl[i].desc_tag, q->fl[i].desc_map);
			bus_dmamem_free(q->fl[i].desc_tag, q->fl[i].desc,
					q->fl[i].desc_map);
			bus_dma_tag_destroy(q->fl[i].desc_tag);
			bus_dma_tag_destroy(q->fl[i].entry_tag);
		}
		if (q->fl[i].sdesc) {
			free_rx_bufs(sc, &q->fl[i]);
			free(q->fl[i].sdesc, M_DEVBUF);
		}
	}

	for (i = 0; i < SGE_TXQ_PER_SET; ++i) {
		if (q->txq[i].desc) {
			mtx_lock(&sc->sge.reg_lock);
			t3_sge_enable_ecntxt(sc, q->txq[i].cntxt_id, 0);
			mtx_unlock(&sc->sge.reg_lock);
			bus_dmamap_unload(q->txq[i].desc_tag,
					q->txq[i].desc_map);
			bus_dmamem_free(q->txq[i].desc_tag, q->txq[i].desc,
					q->txq[i].desc_map);
			bus_dma_tag_destroy(q->txq[i].desc_tag);
			bus_dma_tag_destroy(q->txq[i].entry_tag);
		}
		if (q->txq[i].sdesc) {
			free(q->txq[i].sdesc, M_DEVBUF);
		}
		if (mtx_initialized(&q->txq[i].lock)) {
			mtx_destroy(&q->txq[i].lock);
		}
	}

	if (q->rspq.desc) {
		mtx_lock(&sc->sge.reg_lock);
		t3_sge_disable_rspcntxt(sc, q->rspq.cntxt_id);
		mtx_unlock(&sc->sge.reg_lock);
		
		bus_dmamap_unload(q->rspq.desc_tag, q->rspq.desc_map);
		bus_dmamem_free(q->rspq.desc_tag, q->rspq.desc,
			        q->rspq.desc_map);
		bus_dma_tag_destroy(q->rspq.desc_tag);
	}

	if (mtx_initialized(&q->rspq.lock))
		mtx_destroy(&q->rspq.lock); 
	
	bzero(q, sizeof(*q));
}

/**
 *	t3_free_sge_resources - free SGE resources
 *	@sc: the adapter softc
 *
 *	Frees resources used by the SGE queue sets.
 */
void
t3_free_sge_resources(adapter_t *sc)
{
	int i;

	for (i = 0; i < SGE_QSETS; ++i)
		t3_free_qset(sc, &sc->sge.qs[i]);
}

/**
 *	t3_sge_start - enable SGE
 *	@sc: the controller softc
 *
 *	Enables the SGE for DMAs.  This is the last step in starting packet
 *	transfers.
 */
void
t3_sge_start(adapter_t *sc)
{
	t3_set_reg_field(sc, A_SG_CONTROL, F_GLOBALENABLE, F_GLOBALENABLE);
}


/**
 *	free_tx_desc - reclaims Tx descriptors and their buffers
 *	@adapter: the adapter
 *	@q: the Tx queue to reclaim descriptors from
 *	@n: the number of descriptors to reclaim
 *
 *	Reclaims Tx descriptors from an SGE Tx queue and frees the associated
 *	Tx buffers.  Called with the Tx queue lock held.
 */
int
free_tx_desc(adapter_t *sc, struct sge_txq *q, int n, struct mbuf **m_vec)
{
	struct tx_sw_desc *d;
	unsigned int cidx = q->cidx;
	int nbufs = 0;
	
#ifdef T3_TRACE
	T3_TRACE2(sc->tb[q->cntxt_id & 7],
		  "reclaiming %u Tx descriptors at cidx %u", n, cidx);
#endif
	d = &q->sdesc[cidx];
	
	while (n-- > 0) {
		DPRINTF("cidx=%d d=%p\n", cidx, d);
		if (d->m) {
			if (d->flags & TX_SW_DESC_MAPPED) {
				bus_dmamap_unload(q->entry_tag, d->map);
				bus_dmamap_destroy(q->entry_tag, d->map);
				d->flags &= ~TX_SW_DESC_MAPPED;
			}
			m_vec[nbufs] = d->m;
			d->m = NULL;
			nbufs++;
		}
		++d;
		if (++cidx == q->size) {
			cidx = 0;
			d = q->sdesc;
		}
	}
	q->cidx = cidx;

	return (nbufs);
}

/**
 *	is_new_response - check if a response is newly written
 *	@r: the response descriptor
 *	@q: the response queue
 *
 *	Returns true if a response descriptor contains a yet unprocessed
 *	response.
 */
static __inline int
is_new_response(const struct rsp_desc *r,
    const struct sge_rspq *q)
{
	return (r->intr_gen & F_RSPD_GEN2) == q->gen;
}

#define RSPD_GTS_MASK  (F_RSPD_TXQ0_GTS | F_RSPD_TXQ1_GTS)
#define RSPD_CTRL_MASK (RSPD_GTS_MASK | \
			V_RSPD_TXQ0_CR(M_RSPD_TXQ0_CR) | \
			V_RSPD_TXQ1_CR(M_RSPD_TXQ1_CR) | \
			V_RSPD_TXQ2_CR(M_RSPD_TXQ2_CR))

/* How long to delay the next interrupt in case of memory shortage, in 0.1us. */
#define NOMEM_INTR_DELAY 2500

static __inline void
deliver_partial_bundle(struct t3cdev *tdev, struct sge_rspq *q)
{
	;
}

static __inline void
rx_offload(struct t3cdev *tdev, struct sge_rspq *rq,
    struct mbuf *m)
{
#ifdef notyet
	if (rq->polling) {
		rq->offload_skbs[rq->offload_skbs_idx++] = skb;
		if (rq->offload_skbs_idx == RX_BUNDLE_SIZE) {
			cxgb_ofld_recv(tdev, rq->offload_skbs, RX_BUNDLE_SIZE);
			rq->offload_skbs_idx = 0;
			rq->offload_bundles++;
		}
	} else
#endif
	{
		/* XXX */
		panic("implement offload enqueue\n");
	}

}

static void
restart_tx(struct sge_qset *qs)
{
	;
}

void
t3_rx_eth(struct port_info *pi, struct sge_rspq *rq, struct mbuf *m, int ethpad)
{
	struct cpl_rx_pkt *cpl = (struct cpl_rx_pkt *)(m->m_data + ethpad);
	struct ifnet *ifp = pi->ifp;
	
	DPRINTF("rx_eth m=%p m->m_data=%p p->iff=%d\n", m, m->m_data, cpl->iff);
	if (&pi->adapter->port[cpl->iff] != pi)
		panic("bad port index %d m->m_data=%p\n", cpl->iff, m->m_data);

	if ((ifp->if_capenable & IFCAP_RXCSUM) && !cpl->fragment &&
	    cpl->csum_valid && cpl->csum == 0xffff) {
		m->m_pkthdr.csum_flags = (CSUM_IP_CHECKED|CSUM_IP_VALID);
		rspq_to_qset(rq)->port_stats[SGE_PSTAT_RX_CSUM_GOOD]++;
		m->m_pkthdr.csum_flags = (CSUM_IP_CHECKED|CSUM_IP_VALID|CSUM_DATA_VALID|CSUM_PSEUDO_HDR);
		m->m_pkthdr.csum_data = 0xffff;
	}
	/* 
	 * XXX need to add VLAN support for 6.x
	 */
#ifdef VLAN_SUPPORTED
	if (__predict_false(cpl->vlan_valid)) {
		m->m_pkthdr.ether_vtag = ntohs(cpl->vlan);
		m->m_flags |= M_VLANTAG;
	} 
#endif
	
	m->m_pkthdr.rcvif = ifp;
	m->m_pkthdr.header = m->m_data + sizeof(*cpl) + ethpad;
	m_explode(m);
	/*
	 * adjust after conversion to mbuf chain
	 */
	m_adj(m, sizeof(*cpl) + ethpad);

	(*ifp->if_input)(ifp, m);
}

/**
 *	get_packet - return the next ingress packet buffer from a free list
 *	@adap: the adapter that received the packet
 *	@drop_thres: # of remaining buffers before we start dropping packets
 *	@qs: the qset that the SGE free list holding the packet belongs to
 *      @mh: the mbuf header, contains a pointer to the head and tail of the mbuf chain
 *      @r: response descriptor 
 *
 *	Get the next packet from a free list and complete setup of the
 *	sk_buff.  If the packet is small we make a copy and recycle the
 *	original buffer, otherwise we use the original buffer itself.  If a
 *	positive drop threshold is supplied packets are dropped and their
 *	buffers recycled if (a) the number of remaining buffers is under the
 *	threshold and the packet is too big to copy, or (b) the packet should
 *	be copied but there is no memory for the copy.
 */

static int
get_packet(adapter_t *adap, unsigned int drop_thres, struct sge_qset *qs,
    struct mbuf *m, struct rsp_desc *r)
{
	
	unsigned int len_cq =  ntohl(r->len_cq);
	struct sge_fl *fl = (len_cq & F_RSPD_FLQ) ? &qs->fl[1] : &qs->fl[0];
	struct rx_sw_desc *sd = &fl->sdesc[fl->cidx];
	uint32_t len = G_RSPD_LEN(len_cq);
	uint32_t flags = ntohl(r->flags);
	uint8_t sopeop = G_RSPD_SOP_EOP(flags);
	int ret = 0;
	
	prefetch(sd->cl);
	
	fl->credits--;
	bus_dmamap_sync(fl->entry_tag, sd->map, BUS_DMASYNC_POSTREAD);
	bus_dmamap_unload(fl->entry_tag, sd->map);

	
	switch(sopeop) {
	case RSPQ_SOP_EOP:
		DBG(DBG_RX, ("get_packet: SOP-EOP m %p\n", m));
		m_cljset(m, sd->cl, fl->type);
		m->m_len = m->m_pkthdr.len = len;
		ret = 1;
		goto done;
		break;
	case RSPQ_NSOP_NEOP:
		DBG(DBG_RX, ("get_packet: NO_SOP-NO_EOP m %p\n", m));
		ret = 0;
		break;
	case RSPQ_SOP:
		DBG(DBG_RX, ("get_packet: SOP m %p\n", m));
		m_iovinit(m);
		ret = 0;
		break;
	case RSPQ_EOP:
		DBG(DBG_RX, ("get_packet: EOP m %p\n", m));
		ret = 1;
		break;
	}
	m_iovappend(m, sd->cl, fl->buf_size, len, 0);

done:	
	if (++fl->cidx == fl->size)
		fl->cidx = 0;

	return (ret);
}


/**
 *	handle_rsp_cntrl_info - handles control information in a response
 *	@qs: the queue set corresponding to the response
 *	@flags: the response control flags
 *
 *	Handles the control information of an SGE response, such as GTS
 *	indications and completion credits for the queue set's Tx queues.
 *	HW coalesces credits, we don't do any extra SW coalescing.
 */
static __inline void
handle_rsp_cntrl_info(struct sge_qset *qs, uint32_t flags)
{
	unsigned int credits;

#if USE_GTS
	if (flags & F_RSPD_TXQ0_GTS)
		clear_bit(TXQ_RUNNING, &qs->txq[TXQ_ETH].flags);
#endif
	credits = G_RSPD_TXQ0_CR(flags);
	if (credits) {
		qs->txq[TXQ_ETH].processed += credits;
		if (desc_reclaimable(&qs->txq[TXQ_ETH]) > TX_START_MAX_DESC)
			taskqueue_enqueue(qs->port->adapter->tq,
			    &qs->port->adapter->timer_reclaim_task);
	}
	
	credits = G_RSPD_TXQ2_CR(flags);
	if (credits) 
		qs->txq[TXQ_CTRL].processed += credits;

# if USE_GTS
	if (flags & F_RSPD_TXQ1_GTS)
		clear_bit(TXQ_RUNNING, &qs->txq[TXQ_OFLD].flags);
# endif
	credits = G_RSPD_TXQ1_CR(flags);
	if (credits)
		qs->txq[TXQ_OFLD].processed += credits;
}

static void
check_ring_db(adapter_t *adap, struct sge_qset *qs,
    unsigned int sleeping)
{
	;
}

/*
 * This is an awful hack to bind the ithread to CPU 1
 * to work around lack of ithread affinity
 */
static void
bind_ithread(int cpu)
{
#if 0	
	KASSERT(cpu < mp_ncpus, ("invalid cpu identifier"));
	if (mp_ncpus > 1) {
		mtx_lock_spin(&sched_lock);
		sched_bind(curthread, cpu);
		mtx_unlock_spin(&sched_lock);
	}
#endif
}

/**
 *	process_responses - process responses from an SGE response queue
 *	@adap: the adapter
 *	@qs: the queue set to which the response queue belongs
 *	@budget: how many responses can be processed in this round
 *
 *	Process responses from an SGE response queue up to the supplied budget.
 *	Responses include received packets as well as credits and other events
 *	for the queues that belong to the response queue's queue set.
 *	A negative budget is effectively unlimited.
 *
 *	Additionally choose the interrupt holdoff time for the next interrupt
 *	on this queue.  If the system is under memory shortage use a fairly
 *	long delay to help recovery.
 */
static int
process_responses(adapter_t *adap, struct sge_qset *qs, int budget)
{
	struct sge_rspq *rspq = &qs->rspq;
	struct rsp_desc *r = &rspq->desc[rspq->cidx];
	int budget_left = budget;
	unsigned int sleeping = 0;
	int lro = qs->lro.enabled;
		
	static uint8_t pinned[MAXCPU];

#ifdef DEBUG	
	static int last_holdoff = 0;
	if (rspq->holdoff_tmr != last_holdoff) {
		printf("next_holdoff=%d\n", rspq->holdoff_tmr);
		last_holdoff = rspq->holdoff_tmr;
	}
#endif	
	if (pinned[qs->rspq.cntxt_id * adap->params.nports] == 0) {
		/*
		 * Assumes that cntxt_id < mp_ncpus
		 */
		bind_ithread(qs->rspq.cntxt_id);
		pinned[qs->rspq.cntxt_id * adap->params.nports] = 1;
	}
	rspq->next_holdoff = rspq->holdoff_tmr;

	while (__predict_true(budget_left && is_new_response(r, rspq))) {
		int eth, eop = 0, ethpad = 0;
		uint32_t flags = ntohl(r->flags);
		uint32_t rss_csum = *(const uint32_t *)r;
		uint32_t rss_hash = r->rss_hdr.rss_hash_val;
		
		eth = (r->rss_hdr.opcode == CPL_RX_PKT);
		
		if (__predict_false(flags & F_RSPD_ASYNC_NOTIF)) {
			/* XXX */
			printf("async notification\n");

		} else if  (flags & F_RSPD_IMM_DATA_VALID) {
			struct mbuf *m = NULL;
			if (cxgb_debug)
				printf("IMM DATA VALID\n");
			if (rspq->m == NULL)
				rspq->m = m_gethdr(M_NOWAIT, MT_DATA);
                        else
				m = m_gethdr(M_NOWAIT, MT_DATA);

			if (rspq->m == NULL || m == NULL) {
				rspq->next_holdoff = NOMEM_INTR_DELAY;
				budget_left--;
				break;
			}
			get_imm_packet(adap, r, rspq->m, m);
			eop = 1;
			rspq->imm_data++;
		} else if (r->len_cq) {			
			int drop_thresh = eth ? SGE_RX_DROP_THRES : 0;

                        if (rspq->m == NULL)  
				rspq->m = m_gethdr(M_NOWAIT, MT_DATA);
			if (rspq->m == NULL) { 
				log(LOG_WARNING, "failed to get mbuf for packet\n"); 
				break; 
			}

			ethpad = 2;
			eop = get_packet(adap, drop_thresh, qs, rspq->m, r);
		} else {
			DPRINTF("pure response\n");
			rspq->pure_rsps++;
		}

		if (flags & RSPD_CTRL_MASK) {
			sleeping |= flags & RSPD_GTS_MASK;
			handle_rsp_cntrl_info(qs, flags);
		}

		r++;
		if (__predict_false(++rspq->cidx == rspq->size)) {
			rspq->cidx = 0;
			rspq->gen ^= 1;
			r = rspq->desc;
		}
		
		prefetch(r);
		if (++rspq->credits >= (rspq->size / 4)) {
			refill_rspq(adap, rspq, rspq->credits);
			rspq->credits = 0;
		}
		
		if (eop) {
			prefetch(rspq->m->m_data); 
			prefetch(rspq->m->m_data + L1_CACHE_BYTES); 

			if (eth) {				
				t3_rx_eth_lro(adap, rspq, rspq->m, ethpad,
				    rss_hash, rss_csum, lro);

				rspq->m = NULL;
			} else {
#ifdef notyet
				if (__predict_false(r->rss_hdr.opcode == CPL_TRACE_PKT))
					m_adj(m, 2);

				rx_offload(&adap->tdev, rspq, m);
#endif
			}
#ifdef notyet			
			taskqueue_enqueue(adap->tq, &adap->timer_reclaim_task);
#else			
			__refill_fl(adap, &qs->fl[0]);
			__refill_fl(adap, &qs->fl[1]);
#endif
		}
		--budget_left;
	}
	t3_sge_lro_flush_all(adap, qs);
	deliver_partial_bundle(&adap->tdev, rspq);

	if (sleeping)
		check_ring_db(adap, qs, sleeping);

	smp_mb();  /* commit Tx queue processed updates */
	if (__predict_false(qs->txq_stopped != 0))
		restart_tx(qs);

	budget -= budget_left;
	return (budget);
}

/*
 * A helper function that processes responses and issues GTS.
 */
static __inline int
process_responses_gts(adapter_t *adap, struct sge_rspq *rq)
{
	int work;
	static int last_holdoff = 0;
	
	work = process_responses(adap, rspq_to_qset(rq), -1);

	if (cxgb_debug && (rq->next_holdoff != last_holdoff)) {
		printf("next_holdoff=%d\n", rq->next_holdoff);
		last_holdoff = rq->next_holdoff;
	}

	t3_write_reg(adap, A_SG_GTS, V_RSPQ(rq->cntxt_id) |
		     V_NEWTIMER(rq->next_holdoff) | V_NEWINDEX(rq->cidx));
	return work;
}


/*
 * Interrupt handler for legacy INTx interrupts for T3B-based cards.
 * Handles data events from SGE response queues as well as error and other
 * async events as they all use the same interrupt pin.  We use one SGE
 * response queue per port in this mode and protect all response queues with
 * queue 0's lock.
 */
void
t3b_intr(void *data)
{
	uint32_t map;
	adapter_t *adap = data;
	struct sge_rspq *q0 = &adap->sge.qs[0].rspq;
	struct sge_rspq *q1 = &adap->sge.qs[1].rspq;
	
	t3_write_reg(adap, A_PL_CLI, 0);
	map = t3_read_reg(adap, A_SG_DATA_INTR);

	if (!map) 
		return;

	if (__predict_false(map & F_ERRINTR))
		taskqueue_enqueue(adap->tq, &adap->slow_intr_task);
	
	mtx_lock(&q0->lock);
	
	if (__predict_true(map & 1))
		process_responses_gts(adap, q0);
	
	if (map & 2)
		process_responses_gts(adap, q1);

	mtx_unlock(&q0->lock);
}

/*
 * The MSI interrupt handler.  This needs to handle data events from SGE
 * response queues as well as error and other async events as they all use
 * the same MSI vector.  We use one SGE response queue per port in this mode
 * and protect all response queues with queue 0's lock.
 */
void
t3_intr_msi(void *data)
{
	adapter_t *adap = data;
	struct sge_rspq *q0 = &adap->sge.qs[0].rspq;
	struct sge_rspq *q1 = &adap->sge.qs[1].rspq;
	int new_packets = 0;

	mtx_lock(&q0->lock);
	if (process_responses_gts(adap, q0)) {
		new_packets = 1;
	}

	if (adap->params.nports == 2 &&
	    process_responses_gts(adap, q1)) {
		new_packets = 1;
	}
	
	mtx_unlock(&q0->lock);
	if (new_packets == 0)
		taskqueue_enqueue(adap->tq, &adap->slow_intr_task);
}

void
t3_intr_msix(void *data)
{
	struct sge_qset *qs = data;
	adapter_t *adap = qs->port->adapter;
	struct sge_rspq *rspq = &qs->rspq;

	mtx_lock(&rspq->lock);
	if (process_responses_gts(adap, rspq) == 0) {
#ifdef notyet
		rspq->unhandled_irqs++;
#endif
	}
	mtx_unlock(&rspq->lock);
}

/* 
 * broken by recent mbuf changes 
 */ 
static int
t3_lro_enable(SYSCTL_HANDLER_ARGS)
{
	adapter_t *sc;
	int i, j, enabled, err, nqsets = 0;

#ifndef LRO_WORKING
	return (0);
#endif	
	
	sc = arg1;
	enabled = sc->sge.qs[0].lro.enabled;
        err = sysctl_handle_int(oidp, &enabled, arg2, req);

	if (err != 0) {
		return (err);
	}
	if (enabled == sc->sge.qs[0].lro.enabled)
		return (0);

	for (i = 0; i < sc->params.nports; i++) 
		for (j = 0; j < sc->port[i].nqsets; j++)
			nqsets++;
	
	for (i = 0; i < nqsets; i++) {
		sc->sge.qs[i].lro.enabled = enabled;
	}
	
	return (0);
}

static int
t3_set_coalesce_nsecs(SYSCTL_HANDLER_ARGS)
{
	adapter_t *sc = arg1;
	struct qset_params *qsp = &sc->params.sge.qset[0]; 
	int coalesce_nsecs;	
	struct sge_qset *qs;
	int i, j, err, nqsets = 0;
	struct mtx *lock;
	
	coalesce_nsecs = qsp->coalesce_nsecs;
        err = sysctl_handle_int(oidp, &coalesce_nsecs, arg2, req);

	if (err != 0) {
		return (err);
	}
	if (coalesce_nsecs == qsp->coalesce_nsecs)
		return (0);

	for (i = 0; i < sc->params.nports; i++) 
		for (j = 0; j < sc->port[i].nqsets; j++)
			nqsets++;

	coalesce_nsecs = max(100, coalesce_nsecs);

	for (i = 0; i < nqsets; i++) {
		qs = &sc->sge.qs[i];
		qsp = &sc->params.sge.qset[i];
		qsp->coalesce_nsecs = coalesce_nsecs;
		
		lock = (sc->flags & USING_MSIX) ? &qs->rspq.lock :
			    &sc->sge.qs[0].rspq.lock;

		mtx_lock(lock);
		t3_update_qset_coalesce(qs, qsp);
		t3_write_reg(sc, A_SG_GTS, V_RSPQ(qs->rspq.cntxt_id) |
		    V_NEWTIMER(qs->rspq.holdoff_tmr));
		mtx_unlock(lock);
	}

	return (0);
}


void
t3_add_sysctls(adapter_t *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *children;
	
	ctx = device_get_sysctl_ctx(sc->dev);
	children = SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev));

	/* random information */
	SYSCTL_ADD_STRING(ctx, children, OID_AUTO, 
	    "firmware_version",
	    CTLFLAG_RD, &sc->fw_version,
	    0, "firmware version");

	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, 
	    "enable_lro",
	    CTLTYPE_INT|CTLFLAG_RW, sc,
	    0, t3_lro_enable,
	    "I", "enable large receive offload");

	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, 
	    "intr_coal",
	    CTLTYPE_INT|CTLFLAG_RW, sc,
	    0, t3_set_coalesce_nsecs,
	    "I", "interrupt coalescing timer (ns)");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, 
	    "enable_debug",
	    CTLFLAG_RW, &cxgb_debug,
	    0, "enable verbose debugging output");

	SYSCTL_ADD_INT(ctx, children, OID_AUTO, 
	    "collapse_free",
	    CTLFLAG_RD, &collapse_free,
	    0, "frees during collapse");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, 
	    "mb_free_vec_free",
	    CTLFLAG_RD, &mb_free_vec_free,
	    0, "frees during mb_free_vec");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, 
	    "collapse_mbufs",
	    CTLFLAG_RW, &collapse_mbufs,
	    0, "collapse mbuf chains into iovecs");
}

/**
 *	t3_get_desc - dump an SGE descriptor for debugging purposes
 *	@qs: the queue set
 *	@qnum: identifies the specific queue (0..2: Tx, 3:response, 4..5: Rx)
 *	@idx: the descriptor index in the queue
 *	@data: where to dump the descriptor contents
 *
 *	Dumps the contents of a HW descriptor of an SGE queue.  Returns the
 *	size of the descriptor.
 */
int
t3_get_desc(const struct sge_qset *qs, unsigned int qnum, unsigned int idx,
		unsigned char *data)
{
	if (qnum >= 6)
		return (EINVAL);

	if (qnum < 3) {
		if (!qs->txq[qnum].desc || idx >= qs->txq[qnum].size)
			return -EINVAL;
		memcpy(data, &qs->txq[qnum].desc[idx], sizeof(struct tx_desc));
		return sizeof(struct tx_desc);
	}

	if (qnum == 3) {
		if (!qs->rspq.desc || idx >= qs->rspq.size)
			return (EINVAL);
		memcpy(data, &qs->rspq.desc[idx], sizeof(struct rsp_desc));
		return sizeof(struct rsp_desc);
	}

	qnum -= 4;
	if (!qs->fl[qnum].desc || idx >= qs->fl[qnum].size)
		return (EINVAL);
	memcpy(data, &qs->fl[qnum].desc[idx], sizeof(struct rx_desc));
	return sizeof(struct rx_desc);
}
