/*-
 * Copyright (c) 2001-2007, by Cisco Systems, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * a) Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * b) Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the distribution.
 *
 * c) Neither the name of Cisco Systems, Inc. nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/* $KAME: sctp_var.h,v 1.24 2005/03/06 16:04:19 itojun Exp $	 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#ifndef _NETINET_SCTP_VAR_H_
#define _NETINET_SCTP_VAR_H_

#include <netinet/sctp_uio.h>

#if defined(_KERNEL)

extern struct pr_usrreqs sctp_usrreqs;


#define sctp_feature_on(inp, feature)  (inp->sctp_features |= feature)
#define sctp_feature_off(inp, feature) (inp->sctp_features &= ~feature)
#define sctp_is_feature_on(inp, feature) (inp->sctp_features & feature)
#define sctp_is_feature_off(inp, feature) ((inp->sctp_features & feature) == 0)

#define	sctp_sbspace(asoc, sb) ((long) (((sb)->sb_hiwat > (asoc)->sb_cc) ? ((sb)->sb_hiwat - (asoc)->sb_cc) : 0))

#define	sctp_sbspace_failedmsgs(sb) ((long) (((sb)->sb_hiwat > (sb)->sb_cc) ? ((sb)->sb_hiwat - (sb)->sb_cc) : 0))

#define sctp_sbspace_sub(a,b) ((a > b) ? (a - b) : 0)

/*
 * I tried to cache the readq entries at one point. But the reality
 * is that it did not add any performance since this meant we had to
 * lock the STCB on read. And at that point once you have to do an
 * extra lock, it really does not matter if the lock is in the ZONE
 * stuff or in our code. Note that this same problem would occur with
 * an mbuf cache as well so it is not really worth doing, at least
 * right now :-D
 */

#define sctp_free_a_readq(_stcb, _readq) { \
	SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_readq, (_readq)); \
	SCTP_DECR_READQ_COUNT(); \
}

#define sctp_alloc_a_readq(_stcb, _readq) { \
	(_readq) = SCTP_ZONE_GET(sctppcbinfo.ipi_zone_readq, struct sctp_queued_to_read); \
	if ((_readq)) { \
 	     SCTP_INCR_READQ_COUNT(); \
	} \
}

#define sctp_free_a_strmoq(_stcb, _strmoq) { \
	SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_strmoq, (_strmoq)); \
	SCTP_DECR_STRMOQ_COUNT(); \
}

#define sctp_alloc_a_strmoq(_stcb, _strmoq) { \
	(_strmoq) = SCTP_ZONE_GET(sctppcbinfo.ipi_zone_strmoq, struct sctp_stream_queue_pending); \
	if ((_strmoq)) { \
		SCTP_INCR_STRMOQ_COUNT(); \
 	} \
}


#define sctp_free_a_chunk(_stcb, _chk) { \
	if (((_stcb)->asoc.free_chunk_cnt > sctp_asoc_free_resc_limit) || \
	    (sctppcbinfo.ipi_free_chunks > sctp_system_free_resc_limit)) { \
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, (_chk)); \
		SCTP_DECR_CHK_COUNT(); \
	} else { \
		TAILQ_INSERT_TAIL(&(_stcb)->asoc.free_chunks, (_chk), sctp_next); \
		(_stcb)->asoc.free_chunk_cnt++; \
		atomic_add_int(&sctppcbinfo.ipi_free_chunks, 1); \
	} \
}

#define sctp_alloc_a_chunk(_stcb, _chk) { \
	if (TAILQ_EMPTY(&(_stcb)->asoc.free_chunks))  { \
		(_chk) = SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk, struct sctp_tmit_chunk); \
		if ((_chk)) { \
			SCTP_INCR_CHK_COUNT(); \
		} \
	} else { \
		(_chk) = TAILQ_FIRST(&(_stcb)->asoc.free_chunks); \
		TAILQ_REMOVE(&(_stcb)->asoc.free_chunks, (_chk), sctp_next); \
		atomic_subtract_int(&sctppcbinfo.ipi_free_chunks, 1); \
                SCTP_STAT_INCR(sctps_cached_chk); \
		(_stcb)->asoc.free_chunk_cnt--; \
	} \
}



#define sctp_free_remote_addr(__net) { \
	if ((__net)) {  \
		if (atomic_fetchadd_int(&(__net)->ref_count, -1) == 1) { \
			(void)SCTP_OS_TIMER_STOP(&(__net)->rxt_timer.timer); \
			(void)SCTP_OS_TIMER_STOP(&(__net)->pmtu_timer.timer); \
			(void)SCTP_OS_TIMER_STOP(&(__net)->fr_timer.timer); \
                        if ((__net)->ro.ro_rt) { \
				RTFREE((__net)->ro.ro_rt); \
				(__net)->ro.ro_rt = NULL; \
                        } \
			if ((__net)->src_addr_selected) { \
				sctp_free_ifa((__net)->ro._s_addr); \
				(__net)->ro._s_addr = NULL; \
			} \
                        (__net)->src_addr_selected = 0; \
			(__net)->dest_state = SCTP_ADDR_NOT_REACHABLE; \
			SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_net, (__net)); \
			SCTP_DECR_RADDR_COUNT(); \
		} \
	} \
}

#define sctp_sbfree(ctl, stcb, sb, m) { \
	uint32_t val; \
	val = atomic_fetchadd_int(&(sb)->sb_cc,-(SCTP_BUF_LEN((m)))); \
	if (val < SCTP_BUF_LEN((m))) { \
	   panic("sb_cc goes negative"); \
	} \
	val = atomic_fetchadd_int(&(sb)->sb_mbcnt,-(MSIZE)); \
	if (val < MSIZE) { \
	    panic("sb_mbcnt goes negative"); \
	} \
	if (SCTP_BUF_IS_EXTENDED(m)) { \
		val = atomic_fetchadd_int(&(sb)->sb_mbcnt,-(SCTP_BUF_EXTEND_SIZE(m))); \
		if (val < SCTP_BUF_EXTEND_SIZE(m)) { \
		    panic("sb_mbcnt goes negative2"); \
		} \
	} \
	if (((ctl)->do_not_ref_stcb == 0) && stcb) {\
	  val = atomic_fetchadd_int(&(stcb)->asoc.sb_cc,-(SCTP_BUF_LEN((m)))); \
	  if (val < SCTP_BUF_LEN((m))) {\
	     panic("stcb->sb_cc goes negative"); \
	  } \
	  val = atomic_fetchadd_int(&(stcb)->asoc.sb_mbcnt,-(MSIZE)); \
	  if (val < MSIZE) { \
	     panic("asoc->mbcnt goes negative"); \
	  } \
	  if (SCTP_BUF_IS_EXTENDED(m)) { \
		val = atomic_fetchadd_int(&(stcb)->asoc.sb_mbcnt,-(SCTP_BUF_EXTEND_SIZE(m))); \
		if (val < SCTP_BUF_EXTEND_SIZE(m)) { \
		   panic("assoc stcb->mbcnt would go negative"); \
		} \
	  } \
	} \
	if (SCTP_BUF_TYPE(m) != MT_DATA && SCTP_BUF_TYPE(m) != MT_HEADER && \
	    SCTP_BUF_TYPE(m) != MT_OOBDATA) \
		atomic_subtract_int(&(sb)->sb_ctl,SCTP_BUF_LEN((m))); \
}


#define sctp_sballoc(stcb, sb, m) { \
	atomic_add_int(&(sb)->sb_cc,SCTP_BUF_LEN((m))); \
	atomic_add_int(&(sb)->sb_mbcnt, MSIZE); \
	if (SCTP_BUF_IS_EXTENDED(m)) \
		atomic_add_int(&(sb)->sb_mbcnt,SCTP_BUF_EXTEND_SIZE(m)); \
	if (stcb) { \
		atomic_add_int(&(stcb)->asoc.sb_cc,SCTP_BUF_LEN((m))); \
		atomic_add_int(&(stcb)->asoc.sb_mbcnt, MSIZE); \
		if (SCTP_BUF_IS_EXTENDED(m)) \
			atomic_add_int(&(stcb)->asoc.sb_mbcnt,SCTP_BUF_EXTEND_SIZE(m)); \
	} \
	if (SCTP_BUF_TYPE(m) != MT_DATA && SCTP_BUF_TYPE(m) != MT_HEADER && \
	    SCTP_BUF_TYPE(m) != MT_OOBDATA) \
		atomic_add_int(&(sb)->sb_ctl,SCTP_BUF_LEN((m))); \
}


#define sctp_ucount_incr(val) { \
	val++; \
}

#define sctp_ucount_decr(val) { \
	if (val > 0) { \
		val--; \
	} else { \
		val = 0; \
	} \
}

#define sctp_mbuf_crush(data) do { \
	struct mbuf *_m; \
	_m = (data); \
	while(_m && (SCTP_BUF_LEN(_m) == 0)) { \
		(data)  = SCTP_BUF_NEXT(_m); \
		SCTP_BUF_NEXT(_m) = NULL; \
		sctp_m_free(_m); \
		_m = (data); \
	} \
} while (0)

#ifdef RANDY_WILL_USE_LATER	/* this will be the non-invarant version */
#define sctp_flight_size_decrease(tp1) do { \
	if (tp1->whoTo->flight_size >= tp1->book_size) \
		tp1->whoTo->flight_size -= tp1->book_size; \
	else \
		tp1->whoTo->flight_size = 0; \
} while (0)


#define sctp_total_flight_decrease(stcb, tp1) do { \
	if (stcb->asoc.total_flight >= tp1->book_size) { \
		stcb->asoc.total_flight -= tp1->book_size; \
		if (stcb->asoc.total_flight_count > 0) \
			stcb->asoc.total_flight_count--; \
	} else { \
		stcb->asoc.total_flight = 0; \
		stcb->asoc.total_flight_count = 0; \
	} \
} while (0)

#else

#define sctp_flight_size_decrease(tp1) do { \
	if (tp1->whoTo->flight_size >= tp1->book_size) \
		tp1->whoTo->flight_size -= tp1->book_size; \
	else \
		panic("flight size corruption"); \
} while (0)


#define sctp_total_flight_decrease(stcb, tp1) do { \
	if (stcb->asoc.total_flight >= tp1->book_size) { \
		stcb->asoc.total_flight -= tp1->book_size; \
		if (stcb->asoc.total_flight_count > 0) \
			stcb->asoc.total_flight_count--; \
	} else { \
		panic("total flight size corruption"); \
	} \
} while (0)

#endif

#define sctp_flight_size_increase(tp1) do { \
       (tp1)->whoTo->flight_size += (tp1)->book_size; \
} while (0)


#define sctp_total_flight_increase(stcb, tp1) do { \
       (stcb)->asoc.total_flight_count++; \
       (stcb)->asoc.total_flight += (tp1)->book_size; \
} while (0)

struct sctp_nets;
struct sctp_inpcb;
struct sctp_tcb;
struct sctphdr;

void sctp_ctlinput __P((int, struct sockaddr *, void *));
int sctp_ctloutput __P((struct socket *, struct sockopt *));
void sctp_input __P((struct mbuf *, int));
void sctp_drain __P((void));
void sctp_init __P((void));


void sctp_pcbinfo_cleanup(void);

int sctp_shutdown __P((struct socket *));
void sctp_notify 
__P((struct sctp_inpcb *, int, struct sctphdr *,
    struct sockaddr *, struct sctp_tcb *,
    struct sctp_nets *));

	int sctp_bindx(struct socket *, int, struct sockaddr_storage *,
        int, int, struct proc *);

/* can't use sctp_assoc_t here */
	int sctp_peeloff(struct socket *, struct socket *, int, caddr_t, int *);

	int sctp_ingetaddr(struct socket *,
        struct sockaddr **
);

	int sctp_peeraddr(struct socket *,
        struct sockaddr **
);

	int sctp_listen(struct socket *, int, struct thread *);

	int sctp_accept(struct socket *, struct sockaddr **);

#endif				/* _KERNEL */

#endif				/* !_NETINET_SCTP_VAR_H_ */
