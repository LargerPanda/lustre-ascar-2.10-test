/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lnet/klnds/o2iblnd/o2iblnd.h
 *
 * Author: Eric Barton <eric@bartonsoftware.com>
 */

#ifdef HAVE_COMPAT_RDMA
#include <linux/compat-2.6.h>

#ifdef LINUX_3_17_COMPAT_H
#undef NEED_KTIME_GET_REAL_NS
#endif

#endif

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/uio.h>

#include <asm/uaccess.h>
#include <asm/io.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <linux/kmod.h>
#include <linux/sysctl.h>
#include <linux/pci.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,32)
#include <linux/pci-dma.h>
#endif

#include <net/sock.h>
#include <linux/in.h>

#include <rdma/rdma_cm.h>
#include <rdma/ib_cm.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_fmr_pool.h>

#define DEBUG_SUBSYSTEM S_LND

#include <libcfs/libcfs.h>
#include <lnet/lnet.h>
#include <lnet/lib-lnet.h>

#define IBLND_PEER_HASH_SIZE		101	/* # peer_ni lists */
/* # scheduler loops before reschedule */
#define IBLND_RESCHED			100

#define IBLND_N_SCHED			2
#define IBLND_N_SCHED_HIGH		4

typedef struct
{
	int              *kib_dev_failover;     /* HCA failover */
	unsigned int     *kib_service;          /* IB service number */
	int              *kib_min_reconnect_interval; /* first failed connection retry... */
	int              *kib_max_reconnect_interval; /* ...exponentially increasing to this */
	int              *kib_cksum;            /* checksum kib_msg_t? */
	int              *kib_timeout;          /* comms timeout (seconds) */
	int              *kib_keepalive;        /* keepalive timeout (seconds) */
	int              *kib_ntx;              /* # tx descs */
	char            **kib_default_ipif;     /* default IPoIB interface */
	int              *kib_retry_count;
	int              *kib_rnr_retry_count;
	int		 *kib_ib_mtu;		/* IB MTU */
	int              *kib_require_priv_port;/* accept only privileged ports */
	int              *kib_use_priv_port;    /* use privileged port for active connect */
	/* # threads on each CPT */
	int		 *kib_nscheds;
	int		 *kib_wrq_sge;		/* # sg elements per wrq */
} kib_tunables_t;

extern kib_tunables_t  kiblnd_tunables;

#define IBLND_MSG_QUEUE_SIZE_V1      8          /* V1 only : # messages/RDMAs in-flight */
#define IBLND_CREDIT_HIGHWATER_V1    7          /* V1 only : when eagerly to return credits */

#define IBLND_CREDITS_DEFAULT        8          /* default # of peer_ni credits */
#define IBLND_CREDITS_MAX          ((typeof(((kib_msg_t*) 0)->ibm_credits)) - 1)  /* Max # of peer_ni credits */

/* when eagerly to return credits */
#define IBLND_CREDITS_HIGHWATER(t, v) ((v) == IBLND_MSG_VERSION_1 ? \
					IBLND_CREDIT_HIGHWATER_V1 : \
					t->lnd_peercredits_hiw)

#ifdef HAVE_RDMA_CREATE_ID_5ARG
# define kiblnd_rdma_create_id(cb, dev, ps, qpt) rdma_create_id(current->nsproxy->net_ns, \
								cb, dev, \
								ps, qpt)
#else
# ifdef HAVE_RDMA_CREATE_ID_4ARG
#  define kiblnd_rdma_create_id(cb, dev, ps, qpt) rdma_create_id(cb, dev, \
								 ps, qpt)
# else
#  define kiblnd_rdma_create_id(cb, dev, ps, qpt) rdma_create_id(cb, dev, ps)
# endif
#endif

/* 2 OOB shall suffice for 1 keepalive and 1 returning credits */
#define IBLND_OOB_CAPABLE(v)       ((v) != IBLND_MSG_VERSION_1)
#define IBLND_OOB_MSGS(v)           (IBLND_OOB_CAPABLE(v) ? 2 : 0)

#define IBLND_MSG_SIZE              (4<<10)                 /* max size of queued messages (inc hdr) */
#define IBLND_MAX_RDMA_FRAGS         LNET_MAX_IOV           /* max # of fragments supported */

/************************/
/* derived constants... */
/* Pools (shared by connections on each CPT) */
/* These pools can grow at runtime, so don't need give a very large value */
#define IBLND_TX_POOL			256
#define IBLND_FMR_POOL			256
#define IBLND_FMR_POOL_FLUSH		192

/* RX messages (per connection) */
#define IBLND_RX_MSGS(c)	\
	((c->ibc_queue_depth) * 2 + IBLND_OOB_MSGS(c->ibc_version))
#define IBLND_RX_MSG_BYTES(c)       (IBLND_RX_MSGS(c) * IBLND_MSG_SIZE)
#define IBLND_RX_MSG_PAGES(c)	\
	((IBLND_RX_MSG_BYTES(c) + PAGE_SIZE - 1) / PAGE_SIZE)

/* WRs and CQEs (per connection) */
#define IBLND_RECV_WRS(c)            IBLND_RX_MSGS(c)
#define IBLND_SEND_WRS(c)	\
	((c->ibc_max_frags + 1) * kiblnd_concurrent_sends(c->ibc_version, \
							  c->ibc_peer->ibp_ni))
#define IBLND_CQ_ENTRIES(c)         (IBLND_RECV_WRS(c) + IBLND_SEND_WRS(c))

struct kib_hca_dev;

/* o2iblnd can run over aliased interface */
#ifdef IFALIASZ
#define KIB_IFNAME_SIZE              IFALIASZ
#else
#define KIB_IFNAME_SIZE              256
#endif

typedef struct
{
	struct list_head	ibd_list;	/* chain on kib_devs */
	struct list_head	ibd_fail_list;	/* chain on kib_failed_devs */
	__u32			ibd_ifip;	/* IPoIB interface IP */
	/** IPoIB interface name */
	char			ibd_ifname[KIB_IFNAME_SIZE];
	int			ibd_nnets;	/* # nets extant */

	cfs_time_t		ibd_next_failover;
	/* # failover failures */
	int			ibd_failed_failover;
	/* failover in progress */
	unsigned int		ibd_failover;
	/* IPoIB interface is a bonding master */
	unsigned int		ibd_can_failover;
	struct list_head	ibd_nets;
	struct kib_hca_dev	*ibd_hdev;
} kib_dev_t;

typedef struct kib_hca_dev
{
	struct rdma_cm_id   *ibh_cmid;          /* listener cmid */
	struct ib_device    *ibh_ibdev;         /* IB device */
	int                  ibh_page_shift;    /* page shift of current HCA */
	int                  ibh_page_size;     /* page size of current HCA */
	__u64                ibh_page_mask;     /* page mask of current HCA */
	int                  ibh_mr_shift;      /* bits shift of max MR size */
	__u64                ibh_mr_size;       /* size of MR */
#ifdef HAVE_IB_GET_DMA_MR
	struct ib_mr        *ibh_mrs;           /* global MR */
#endif
	struct ib_pd        *ibh_pd;            /* PD */
	kib_dev_t           *ibh_dev;           /* owner */
	atomic_t             ibh_ref;           /* refcount */
} kib_hca_dev_t;

/** # of seconds to keep pool alive */
#define IBLND_POOL_DEADLINE     300
/** # of seconds to retry if allocation failed */
#define IBLND_POOL_RETRY        1

typedef struct
{
        int                     ibp_npages;             /* # pages */
        struct page            *ibp_pages[0];           /* page array */
} kib_pages_t;

struct kib_pool;
struct kib_poolset;

typedef int  (*kib_ps_pool_create_t)(struct kib_poolset *ps,
				     int inc, struct kib_pool **pp_po);
typedef void (*kib_ps_pool_destroy_t)(struct kib_pool *po);
typedef void (*kib_ps_node_init_t)(struct kib_pool *po, struct list_head *node);
typedef void (*kib_ps_node_fini_t)(struct kib_pool *po, struct list_head *node);

struct kib_net;

#define IBLND_POOL_NAME_LEN     32

typedef struct kib_poolset
{
	/* serialize */
	spinlock_t		ps_lock;
	/* network it belongs to */
	struct kib_net		*ps_net;
	/* pool set name */
	char			ps_name[IBLND_POOL_NAME_LEN];
	/* list of pools */
	struct list_head	ps_pool_list;
	/* failed pool list */
	struct list_head	ps_failed_pool_list;
	/* time stamp for retry if failed to allocate */
	cfs_time_t		ps_next_retry;
	/* is allocating new pool */
	int			ps_increasing;
	/* new pool size */
	int			ps_pool_size;
	/* CPT id */
	int			ps_cpt;

	/* create a new pool */
	kib_ps_pool_create_t	ps_pool_create;
	/* destroy a pool */
	kib_ps_pool_destroy_t	ps_pool_destroy;
	/* initialize new allocated node */
	kib_ps_node_init_t	ps_node_init;
	/* finalize node */
	kib_ps_node_fini_t	ps_node_fini;
} kib_poolset_t;

typedef struct kib_pool
{
	/* chain on pool list */
	struct list_head	po_list;
	/* pre-allocated node */
	struct list_head	po_free_list;
	/* pool_set of this pool */
	kib_poolset_t	       *po_owner;
	/* deadline of this pool */
	cfs_time_t		po_deadline;
	/* # of elements in use */
	int			po_allocated;
	/* pool is created on failed HCA */
	int			po_failed;
	/* # of pre-allocated elements */
	int			po_size;
} kib_pool_t;

typedef struct {
        kib_poolset_t           tps_poolset;            /* pool-set */
        __u64                   tps_next_tx_cookie;     /* cookie of TX */
} kib_tx_poolset_t;

typedef struct {
        kib_pool_t              tpo_pool;               /* pool */
        struct kib_hca_dev     *tpo_hdev;               /* device for this pool */
        struct kib_tx          *tpo_tx_descs;           /* all the tx descriptors */
        kib_pages_t            *tpo_tx_pages;           /* premapped tx msg pages */
} kib_tx_pool_t;

typedef struct
{
	spinlock_t		fps_lock;		/* serialize */
	struct kib_net	       *fps_net;		/* IB network */
	struct list_head	fps_pool_list;		/* FMR pool list */
	struct list_head	fps_failed_pool_list;	/* FMR pool list */
	__u64			fps_version;		/* validity stamp */
	int			fps_cpt;		/* CPT id */
	int			fps_pool_size;
	int			fps_flush_trigger;
	int			fps_cache;
	/* is allocating new pool */
	int			fps_increasing;
	/* time stamp for retry if failed to allocate */
	cfs_time_t		fps_next_retry;
} kib_fmr_poolset_t;

#ifndef HAVE_IB_RDMA_WR
struct ib_rdma_wr {
	struct ib_send_wr wr;
};
#endif

struct kib_fast_reg_descriptor { /* For fast registration */
	struct list_head		 frd_list;
	struct ib_rdma_wr		 frd_inv_wr;
#ifdef HAVE_IB_MAP_MR_SG
	struct ib_reg_wr		 frd_fastreg_wr;
#else
	struct ib_rdma_wr		 frd_fastreg_wr;
	struct ib_fast_reg_page_list    *frd_frpl;
#endif
	struct ib_mr			*frd_mr;
	bool				 frd_valid;
};

typedef struct
{
	struct list_head	fpo_list;	/* chain on pool list */
	struct kib_hca_dev     *fpo_hdev;	/* device for this pool */
	kib_fmr_poolset_t      *fpo_owner;	/* owner of this pool */
	union {
		struct {
			struct ib_fmr_pool *fpo_fmr_pool; /* IB FMR pool */
		} fmr;
		struct { /* For fast registration */
			struct list_head  fpo_pool_list;
			int		  fpo_pool_size;
		} fast_reg;
	};
	cfs_time_t		fpo_deadline;	/* deadline of this pool */
	int			fpo_failed;	/* fmr pool is failed */
	int			fpo_map_count;	/* # of mapped FMR */
	int			fpo_is_fmr;
} kib_fmr_pool_t;

typedef struct {
	kib_fmr_pool_t			*fmr_pool;	/* pool of FMR */
	struct ib_pool_fmr		*fmr_pfmr;	/* IB pool fmr */
	struct kib_fast_reg_descriptor	*fmr_frd;
	u32				 fmr_key;
} kib_fmr_t;

typedef struct kib_net
{
	/* chain on kib_dev_t::ibd_nets */
	struct list_head	ibn_list;
	__u64			ibn_incarnation;/* my epoch */
	int			ibn_init;	/* initialisation state */
	int			ibn_shutdown;	/* shutting down? */

	atomic_t		ibn_npeers;	/* # peers extant */
	atomic_t		ibn_nconns;	/* # connections extant */

	kib_tx_poolset_t	**ibn_tx_ps;	/* tx pool-set */
	kib_fmr_poolset_t	**ibn_fmr_ps;	/* fmr pool-set */

	kib_dev_t		*ibn_dev;	/* underlying IB device */
} kib_net_t;

#define KIB_THREAD_SHIFT		16
#define KIB_THREAD_ID(cpt, tid)		((cpt) << KIB_THREAD_SHIFT | (tid))
#define KIB_THREAD_CPT(id)		((id) >> KIB_THREAD_SHIFT)
#define KIB_THREAD_TID(id)		((id) & ((1UL << KIB_THREAD_SHIFT) - 1))

struct kib_sched_info {
	/* serialise */
	spinlock_t		ibs_lock;
	/* schedulers sleep here */
	wait_queue_head_t	ibs_waitq;
	/* conns to check for rx completions */
	struct list_head	ibs_conns;
	/* number of scheduler threads */
	int			ibs_nthreads;
	/* max allowed scheduler threads */
	int			ibs_nthreads_max;
	int			ibs_cpt;	/* CPT id */
};

typedef struct
{
	int			kib_init;	/* initialisation state */
	int			kib_shutdown;	/* shut down? */
	struct list_head	kib_devs;	/* IB devices extant */
	/* list head of failed devices */
	struct list_head	kib_failed_devs;
	/* schedulers sleep here */
	wait_queue_head_t	kib_failover_waitq;
	atomic_t		kib_nthreads;	/* # live threads */
	/* stabilize net/dev/peer_ni/conn ops */
	rwlock_t		kib_global_lock;
	/* hash table of all my known peers */
	struct list_head	*kib_peers;
	/* size of kib_peers */
	int			kib_peer_hash_size;
	/* the connd task (serialisation assertions) */
	void			*kib_connd;
	/* connections to setup/teardown */
	struct list_head	kib_connd_conns;
	/* connections with zero refcount */
	struct list_head	kib_connd_zombies;
	/* connections to reconnect */
	struct list_head	kib_reconn_list;
	/* peers wait for reconnection */
	struct list_head	kib_reconn_wait;
	/*
	 * The second that peers are pulled out from \a kib_reconn_wait
	 * for reconnection.
	 */
	unsigned int		kib_reconn_sec;
	/* connection daemon sleeps here */
	wait_queue_head_t	kib_connd_waitq;
	spinlock_t		kib_connd_lock;	/* serialise */
	struct ib_qp_attr	kib_error_qpa;	/* QP->ERROR */
	/* percpt data for schedulers */
	struct kib_sched_info	**kib_scheds;
} kib_data_t;

#define IBLND_INIT_NOTHING         0
#define IBLND_INIT_DATA            1
#define IBLND_INIT_ALL             2

/************************************************************************
 * IB Wire message format.
 * These are sent in sender's byte order (i.e. receiver flips).
 */

typedef struct kib_connparams
{
        __u16             ibcp_queue_depth;
        __u16             ibcp_max_frags;
        __u32             ibcp_max_msg_size;
} WIRE_ATTR kib_connparams_t;

typedef struct
{
	struct lnet_hdr		ibim_hdr;	/* portals header */
	char			ibim_payload[0];/* piggy-backed payload */
} WIRE_ATTR kib_immediate_msg_t;

typedef struct
{
        __u32             rf_nob;               /* # bytes this frag */
        __u64             rf_addr;              /* CAVEAT EMPTOR: misaligned!! */
} WIRE_ATTR kib_rdma_frag_t;

typedef struct
{
        __u32             rd_key;               /* local/remote key */
        __u32             rd_nfrags;            /* # fragments */
        kib_rdma_frag_t   rd_frags[0];          /* buffer frags */
} WIRE_ATTR kib_rdma_desc_t;

typedef struct
{
	struct lnet_hdr		ibprm_hdr;	/* portals header */
	__u64			ibprm_cookie;	/* opaque completion cookie */
} WIRE_ATTR kib_putreq_msg_t;

typedef struct
{
        __u64             ibpam_src_cookie;     /* reflected completion cookie */
        __u64             ibpam_dst_cookie;     /* opaque completion cookie */
        kib_rdma_desc_t   ibpam_rd;             /* sender's sink buffer */
} WIRE_ATTR kib_putack_msg_t;

typedef struct
{
	struct lnet_hdr		ibgm_hdr;	/* portals header */
	__u64			ibgm_cookie;	/* opaque completion cookie */
	kib_rdma_desc_t		ibgm_rd;	/* rdma descriptor */
} WIRE_ATTR kib_get_msg_t;

typedef struct
{
        __u64             ibcm_cookie;          /* opaque completion cookie */
        __s32             ibcm_status;          /* < 0 failure: >= 0 length */
} WIRE_ATTR kib_completion_msg_t;

typedef struct
{
        /* First 2 fields fixed FOR ALL TIME */
        __u32             ibm_magic;            /* I'm an ibnal message */
        __u16             ibm_version;          /* this is my version number */

        __u8              ibm_type;             /* msg type */
        __u8              ibm_credits;          /* returned credits */
        __u32             ibm_nob;              /* # bytes in whole message */
        __u32             ibm_cksum;            /* checksum (0 == no checksum) */
        __u64             ibm_srcnid;           /* sender's NID */
        __u64             ibm_srcstamp;         /* sender's incarnation */
        __u64             ibm_dstnid;           /* destination's NID */
        __u64             ibm_dststamp;         /* destination's incarnation */

        union {
                kib_connparams_t      connparams;
                kib_immediate_msg_t   immediate;
                kib_putreq_msg_t      putreq;
                kib_putack_msg_t      putack;
                kib_get_msg_t         get;
                kib_completion_msg_t  completion;
        } WIRE_ATTR ibm_u;
} WIRE_ATTR kib_msg_t;

#define IBLND_MSG_MAGIC LNET_PROTO_IB_MAGIC	/* unique magic */

#define IBLND_MSG_VERSION_1         0x11
#define IBLND_MSG_VERSION_2         0x12
#define IBLND_MSG_VERSION           IBLND_MSG_VERSION_2

#define IBLND_MSG_CONNREQ           0xc0        /* connection request */
#define IBLND_MSG_CONNACK           0xc1        /* connection acknowledge */
#define IBLND_MSG_NOOP              0xd0        /* nothing (just credits) */
#define IBLND_MSG_IMMEDIATE         0xd1        /* immediate */
#define IBLND_MSG_PUT_REQ           0xd2        /* putreq (src->sink) */
#define IBLND_MSG_PUT_NAK           0xd3        /* completion (sink->src) */
#define IBLND_MSG_PUT_ACK           0xd4        /* putack (sink->src) */
#define IBLND_MSG_PUT_DONE          0xd5        /* completion (src->sink) */
#define IBLND_MSG_GET_REQ           0xd6        /* getreq (sink->src) */
#define IBLND_MSG_GET_DONE          0xd7        /* completion (src->sink: all OK) */

typedef struct {
        __u32            ibr_magic;             /* sender's magic */
        __u16            ibr_version;           /* sender's version */
        __u8             ibr_why;               /* reject reason */
        __u8             ibr_padding;           /* padding */
        __u64            ibr_incarnation;       /* incarnation of peer_ni */
        kib_connparams_t ibr_cp;                /* connection parameters */
} WIRE_ATTR kib_rej_t;

/* connection rejection reasons */
#define IBLND_REJECT_CONN_RACE       1          /* You lost connection race */
#define IBLND_REJECT_NO_RESOURCES    2          /* Out of memory/conns etc */
#define IBLND_REJECT_FATAL           3          /* Anything else */

#define IBLND_REJECT_CONN_UNCOMPAT   4          /* incompatible version peer_ni */
#define IBLND_REJECT_CONN_STALE      5          /* stale peer_ni */

/* peer_ni's rdma frags doesn't match mine */
#define IBLND_REJECT_RDMA_FRAGS      6
/* peer_ni's msg queue size doesn't match mine */
#define IBLND_REJECT_MSG_QUEUE_SIZE  7
#define IBLND_REJECT_INVALID_SRV_ID  8

/***********************************************************************/

typedef struct kib_rx                           /* receive message */
{
	/* queue for attention */
	struct list_head	rx_list;
	/* owning conn */
	struct kib_conn	       *rx_conn;
	/* # bytes received (-1 while posted) */
	int			rx_nob;
	/* completion status */
	enum ib_wc_status	rx_status;
	/* message buffer (host vaddr) */
	kib_msg_t	       *rx_msg;
	/* message buffer (I/O addr) */
	__u64			rx_msgaddr;
	/* for dma_unmap_single() */
	DECLARE_PCI_UNMAP_ADDR(rx_msgunmap);
	/* receive work item... */
	struct ib_recv_wr	rx_wrq;
	/* ...and its memory */
	struct ib_sge		rx_sge;
} kib_rx_t;

#define IBLND_POSTRX_DONT_POST    0             /* don't post */
#define IBLND_POSTRX_NO_CREDIT    1             /* post: no credits */
#define IBLND_POSTRX_PEER_CREDIT  2             /* post: give peer_ni back 1 credit */
#define IBLND_POSTRX_RSRVD_CREDIT 3             /* post: give myself back 1 reserved credit */

typedef struct kib_tx                           /* transmit message */
{
	/* queue on idle_txs ibc_tx_queue etc. */
	struct list_head	tx_list;
	/* pool I'm from */
	kib_tx_pool_t		*tx_pool;
	/* owning conn */
	struct kib_conn		*tx_conn;
	/* # tx callbacks outstanding */
	short			tx_sending;
	/* queued for sending */
	short			tx_queued;
	/* waiting for peer_ni */
	short			tx_waiting;
	/* LNET completion status */
	int			tx_status;
	/* completion deadline */
	unsigned long		tx_deadline;
	/* completion cookie */
	__u64			tx_cookie;
	/* lnet msgs to finalize on completion */
	struct lnet_msg		*tx_lntmsg[2];
	/* message buffer (host vaddr) */
	kib_msg_t		*tx_msg;
	/* message buffer (I/O addr) */
	__u64			tx_msgaddr;
	/* for dma_unmap_single() */
	DECLARE_PCI_UNMAP_ADDR(tx_msgunmap);
	/** sge for tx_msgaddr */
	struct ib_sge		tx_msgsge;
	/* # send work items */
	int			tx_nwrq;
	/* # used scatter/gather elements */
	int			tx_nsge;
	/* send work items... */
	struct ib_rdma_wr	*tx_wrq;
	/* ...and their memory */
	struct ib_sge		*tx_sge;
	/* rdma descriptor */
	kib_rdma_desc_t		*tx_rd;
	/* # entries in... */
	int			tx_nfrags;
	/* dma_map_sg descriptor */
	struct scatterlist	*tx_frags;
	/* rdma phys page addrs */
	__u64			*tx_pages;
	/* FMR */
	kib_fmr_t		fmr;
				/* dma direction */
	int			tx_dmadir;
} kib_tx_t;

typedef struct kib_connvars
{
        /* connection-in-progress variables */
        kib_msg_t                 cv_msg;
} kib_connvars_t;

typedef struct kib_conn
{
	/* scheduler information */
	struct kib_sched_info	*ibc_sched;
	/* owning peer_ni */
	struct kib_peer		*ibc_peer;
	/* HCA bound on */
	kib_hca_dev_t		*ibc_hdev;
	/* stash on peer_ni's conn list */
	struct list_head	ibc_list;
	/* schedule for attention */
	struct list_head	ibc_sched_list;
	/* version of connection */
	__u16			ibc_version;
	/* reconnect later */
	__u16			ibc_reconnect:1;
	/* which instance of the peer */
	__u64			ibc_incarnation;
	/* # users */
	atomic_t		ibc_refcount;
	/* what's happening */
	int			ibc_state;
	/* # uncompleted sends */
	int			ibc_nsends_posted;
	/* # uncompleted NOOPs */
	int			ibc_noops_posted;
	/* # credits I have */
	int			ibc_credits;
	/* # credits to return */
	int			ibc_outstanding_credits;
	/* # ACK/DONE msg credits */
	int			ibc_reserved_credits;
	/* set on comms error */
	int			ibc_comms_error;
	/* connections queue depth */
	__u16			ibc_queue_depth;
	/* connections max frags */
	__u16			ibc_max_frags;
	/* receive buffers owned */
	unsigned int		ibc_nrx:16;
	/* scheduled for attention */
	unsigned int		ibc_scheduled:1;
	/* CQ callback fired */
	unsigned int		ibc_ready:1;
	/* time of last send */
	unsigned long		ibc_last_send;
	/** link chain for kiblnd_check_conns only */
	struct list_head	ibc_connd_list;
	/** rxs completed before ESTABLISHED */
	struct list_head	ibc_early_rxs;
	/** IBLND_MSG_NOOPs for IBLND_MSG_VERSION_1 */
	struct list_head	ibc_tx_noops;
	/* sends that need a credit */
	struct list_head	ibc_tx_queue;
	/* sends that don't need a credit */
	struct list_head	ibc_tx_queue_nocred;
	/* sends that need to reserve an ACK/DONE msg */
	struct list_head	ibc_tx_queue_rsrvd;
	/* active tx awaiting completion */
	struct list_head	ibc_active_txs;
	/* serialise */
	spinlock_t		ibc_lock;
	/* the rx descs */
	kib_rx_t		*ibc_rxs;
	/* premapped rx msg pages */
	kib_pages_t		*ibc_rx_pages;

	/* CM id */
	struct rdma_cm_id	*ibc_cmid;
	/* completion queue */
	struct ib_cq		*ibc_cq;

	/* in-progress connection state */
	kib_connvars_t		*ibc_connvars;
} kib_conn_t;

#define IBLND_CONN_INIT               0         /* being initialised */
#define IBLND_CONN_ACTIVE_CONNECT     1         /* active sending req */
#define IBLND_CONN_PASSIVE_WAIT       2         /* passive waiting for rtu */
#define IBLND_CONN_ESTABLISHED        3         /* connection established */
#define IBLND_CONN_CLOSING            4         /* being closed */
#define IBLND_CONN_DISCONNECTED       5         /* disconnected */

typedef struct kib_peer
{
	/* stash on global peer_ni list */
	struct list_head	ibp_list;
	/* who's on the other end(s) */
	lnet_nid_t		ibp_nid;
	/* LNet interface */
	struct lnet_ni		*ibp_ni;
	/* all active connections */
	struct list_head	ibp_conns;
	/* next connection to send on for round robin */
	struct kib_conn		*ibp_next_conn;
	/* msgs waiting for a conn */
	struct list_head	ibp_tx_queue;
	/* incarnation of peer_ni */
	__u64			ibp_incarnation;
	/* when (in jiffies) I was last alive */
	cfs_time_t		ibp_last_alive;
	/* # users */
	atomic_t		ibp_refcount;
	/* version of peer_ni */
	__u16			ibp_version;
	/* current passive connection attempts */
	unsigned short		ibp_accepting;
	/* current active connection attempts */
	unsigned short		ibp_connecting;
	/* reconnect this peer_ni later */
	unsigned char		ibp_reconnecting;
	/* counter of how many times we triggered a conn race */
	unsigned char		ibp_races;
	/* # consecutive reconnection attempts to this peer */
	unsigned int		ibp_reconnected;
	/* errno on closing this peer_ni */
	int			ibp_error;
	/* max map_on_demand */
	__u16			ibp_max_frags;
	/* max_peer_credits */
	__u16			ibp_queue_depth;
} kib_peer_ni_t;

#ifndef HAVE_IB_INC_RKEY
/**
 * ib_inc_rkey - increments the key portion of the given rkey. Can be used
 * for calculating a new rkey for type 2 memory windows.
 * @rkey - the rkey to increment.
 */
static inline u32 ib_inc_rkey(u32 rkey)
{
	const u32 mask = 0x000000ff;
	return ((rkey + 1) & mask) | (rkey & ~mask);
}
#endif

extern kib_data_t      kiblnd_data;

extern void kiblnd_hdev_destroy(kib_hca_dev_t *hdev);

int kiblnd_msg_queue_size(int version, struct lnet_ni *ni);

/* max # of fragments configured by user */
static inline int
kiblnd_cfg_rdma_frags(struct lnet_ni *ni)
{
	struct lnet_ioctl_config_o2iblnd_tunables *tunables;
	int mod;

	tunables = &ni->ni_lnd_tunables.lnd_tun_u.lnd_o2ib;
	mod = tunables->lnd_map_on_demand;
	return mod != 0 ? mod : IBLND_MAX_RDMA_FRAGS;
}

static inline int
kiblnd_rdma_frags(int version, struct lnet_ni *ni)
{
	return version == IBLND_MSG_VERSION_1 ?
	  IBLND_MAX_RDMA_FRAGS :
	  kiblnd_cfg_rdma_frags(ni);
}

static inline int
kiblnd_concurrent_sends(int version, struct lnet_ni *ni)
{
	struct lnet_ioctl_config_o2iblnd_tunables *tunables;
	int concurrent_sends;

	tunables = &ni->ni_lnd_tunables.lnd_tun_u.lnd_o2ib;
	concurrent_sends = tunables->lnd_concurrent_sends;

	if (version == IBLND_MSG_VERSION_1) {
		if (concurrent_sends > IBLND_MSG_QUEUE_SIZE_V1 * 2)
			return IBLND_MSG_QUEUE_SIZE_V1 * 2;

		if (concurrent_sends < IBLND_MSG_QUEUE_SIZE_V1 / 2)
			return IBLND_MSG_QUEUE_SIZE_V1 / 2;
	}

	return concurrent_sends;
}

static inline void
kiblnd_hdev_addref_locked(kib_hca_dev_t *hdev)
{
	LASSERT(atomic_read(&hdev->ibh_ref) > 0);
	atomic_inc(&hdev->ibh_ref);
}

static inline void
kiblnd_hdev_decref(kib_hca_dev_t *hdev)
{
	LASSERT(atomic_read(&hdev->ibh_ref) > 0);
	if (atomic_dec_and_test(&hdev->ibh_ref))
		kiblnd_hdev_destroy(hdev);
}

static inline int
kiblnd_dev_can_failover(kib_dev_t *dev)
{
	if (!list_empty(&dev->ibd_fail_list)) /* already scheduled */
                return 0;

        if (*kiblnd_tunables.kib_dev_failover == 0) /* disabled */
                return 0;

        if (*kiblnd_tunables.kib_dev_failover > 1) /* force failover */
                return 1;

        return dev->ibd_can_failover;
}

#define kiblnd_conn_addref(conn)                                \
do {                                                            \
        CDEBUG(D_NET, "conn[%p] (%d)++\n",                      \
	       (conn), atomic_read(&(conn)->ibc_refcount)); \
	atomic_inc(&(conn)->ibc_refcount);                  \
} while (0)

#define kiblnd_conn_decref(conn)					\
do {									\
	unsigned long flags;						\
									\
	CDEBUG(D_NET, "conn[%p] (%d)--\n",				\
	       (conn), atomic_read(&(conn)->ibc_refcount));		\
	LASSERT_ATOMIC_POS(&(conn)->ibc_refcount);			\
	if (atomic_dec_and_test(&(conn)->ibc_refcount)) {		\
		spin_lock_irqsave(&kiblnd_data.kib_connd_lock, flags);	\
		list_add_tail(&(conn)->ibc_list,			\
				  &kiblnd_data.kib_connd_zombies);	\
		wake_up(&kiblnd_data.kib_connd_waitq);		\
		spin_unlock_irqrestore(&kiblnd_data.kib_connd_lock, flags);\
	}								\
} while (0)

#define kiblnd_peer_addref(peer_ni)                                \
do {                                                            \
	CDEBUG(D_NET, "peer_ni[%p] -> %s (%d)++\n",                \
	       (peer_ni), libcfs_nid2str((peer_ni)->ibp_nid),         \
	       atomic_read (&(peer_ni)->ibp_refcount));        	\
	atomic_inc(&(peer_ni)->ibp_refcount);                  	\
} while (0)

#define kiblnd_peer_decref(peer_ni)                                \
do {                                                            \
	CDEBUG(D_NET, "peer_ni[%p] -> %s (%d)--\n",                \
	       (peer_ni), libcfs_nid2str((peer_ni)->ibp_nid),         \
	       atomic_read (&(peer_ni)->ibp_refcount));        	\
	LASSERT_ATOMIC_POS(&(peer_ni)->ibp_refcount);              \
	if (atomic_dec_and_test(&(peer_ni)->ibp_refcount))     	\
		kiblnd_destroy_peer(peer_ni);                      \
} while (0)

static inline bool
kiblnd_peer_connecting(kib_peer_ni_t *peer_ni)
{
	return peer_ni->ibp_connecting != 0 ||
	       peer_ni->ibp_reconnecting != 0 ||
	       peer_ni->ibp_accepting != 0;
}

static inline bool
kiblnd_peer_idle(kib_peer_ni_t *peer_ni)
{
	return !kiblnd_peer_connecting(peer_ni) && list_empty(&peer_ni->ibp_conns);
}

static inline struct list_head *
kiblnd_nid2peerlist (lnet_nid_t nid)
{
	unsigned int hash =
		((unsigned int)nid) % kiblnd_data.kib_peer_hash_size;

	return &kiblnd_data.kib_peers[hash];
}

static inline int
kiblnd_peer_active (kib_peer_ni_t *peer_ni)
{
	/* Am I in the peer_ni hash table? */
	return !list_empty(&peer_ni->ibp_list);
}

static inline struct kib_conn *
kiblnd_get_conn_locked (kib_peer_ni_t *peer_ni)
{
	struct list_head *next;

	LASSERT(!list_empty(&peer_ni->ibp_conns));

	/* Advance to next connection, be sure to skip the head node */
	if (!peer_ni->ibp_next_conn ||
	    peer_ni->ibp_next_conn->ibc_list.next == &peer_ni->ibp_conns)
		next = peer_ni->ibp_conns.next;
	else
		next = peer_ni->ibp_next_conn->ibc_list.next;
	peer_ni->ibp_next_conn = list_entry(next, struct kib_conn, ibc_list);

	return peer_ni->ibp_next_conn;
}

static inline int
kiblnd_send_keepalive(kib_conn_t *conn)
{
	return (*kiblnd_tunables.kib_keepalive > 0) &&
		cfs_time_after(jiffies, conn->ibc_last_send +
			       msecs_to_jiffies(*kiblnd_tunables.kib_keepalive *
						MSEC_PER_SEC));
}

static inline int
kiblnd_need_noop(kib_conn_t *conn)
{
	struct lnet_ni *ni = conn->ibc_peer->ibp_ni;
	struct lnet_ioctl_config_o2iblnd_tunables *tunables;

	LASSERT(conn->ibc_state >= IBLND_CONN_ESTABLISHED);
	tunables = &ni->ni_lnd_tunables.lnd_tun_u.lnd_o2ib;

        if (conn->ibc_outstanding_credits <
	    IBLND_CREDITS_HIGHWATER(tunables, conn->ibc_version) &&
            !kiblnd_send_keepalive(conn))
                return 0; /* No need to send NOOP */

        if (IBLND_OOB_CAPABLE(conn->ibc_version)) {
		if (!list_empty(&conn->ibc_tx_queue_nocred))
                        return 0; /* NOOP can be piggybacked */

                /* No tx to piggyback NOOP onto or no credit to send a tx */
		return (list_empty(&conn->ibc_tx_queue) ||
                        conn->ibc_credits == 0);
        }

	if (!list_empty(&conn->ibc_tx_noops) || /* NOOP already queued */
	    !list_empty(&conn->ibc_tx_queue_nocred) || /* piggyback NOOP */
            conn->ibc_credits == 0)                    /* no credit */
                return 0;

        if (conn->ibc_credits == 1 &&      /* last credit reserved for */
            conn->ibc_outstanding_credits == 0) /* giving back credits */
                return 0;

        /* No tx to piggyback NOOP onto or no credit to send a tx */
	return (list_empty(&conn->ibc_tx_queue) || conn->ibc_credits == 1);
}

static inline void
kiblnd_abort_receives(kib_conn_t *conn)
{
        ib_modify_qp(conn->ibc_cmid->qp,
                     &kiblnd_data.kib_error_qpa, IB_QP_STATE);
}

static inline const char *
kiblnd_queue2str(kib_conn_t *conn, struct list_head *q)
{
	if (q == &conn->ibc_tx_queue)
		return "tx_queue";

	if (q == &conn->ibc_tx_queue_rsrvd)
		return "tx_queue_rsrvd";

	if (q == &conn->ibc_tx_queue_nocred)
		return "tx_queue_nocred";

	if (q == &conn->ibc_active_txs)
		return "active_txs";

	LBUG();
	return NULL;
}

/* CAVEAT EMPTOR: We rely on descriptor alignment to allow us to use the
 * lowest bits of the work request id to stash the work item type. */

#define IBLND_WID_INVAL	0
#define IBLND_WID_TX	1
#define IBLND_WID_RX	2
#define IBLND_WID_RDMA	3
#define IBLND_WID_MR	4
#define IBLND_WID_MASK	7UL

static inline __u64
kiblnd_ptr2wreqid (void *ptr, int type)
{
        unsigned long lptr = (unsigned long)ptr;

        LASSERT ((lptr & IBLND_WID_MASK) == 0);
        LASSERT ((type & ~IBLND_WID_MASK) == 0);
        return (__u64)(lptr | type);
}

static inline void *
kiblnd_wreqid2ptr (__u64 wreqid)
{
        return (void *)(((unsigned long)wreqid) & ~IBLND_WID_MASK);
}

static inline int
kiblnd_wreqid2type (__u64 wreqid)
{
        return (wreqid & IBLND_WID_MASK);
}

static inline void
kiblnd_set_conn_state (kib_conn_t *conn, int state)
{
	conn->ibc_state = state;
	smp_mb();
}

static inline void
kiblnd_init_msg (kib_msg_t *msg, int type, int body_nob)
{
        msg->ibm_type = type;
        msg->ibm_nob  = offsetof(kib_msg_t, ibm_u) + body_nob;
}

static inline int
kiblnd_rd_size (kib_rdma_desc_t *rd)
{
        int   i;
        int   size;

        for (i = size = 0; i < rd->rd_nfrags; i++)
                size += rd->rd_frags[i].rf_nob;

        return size;
}

static inline __u64
kiblnd_rd_frag_addr(kib_rdma_desc_t *rd, int index)
{
        return rd->rd_frags[index].rf_addr;
}

static inline __u32
kiblnd_rd_frag_size(kib_rdma_desc_t *rd, int index)
{
        return rd->rd_frags[index].rf_nob;
}

static inline __u32
kiblnd_rd_frag_key(kib_rdma_desc_t *rd, int index)
{
        return rd->rd_key;
}

static inline int
kiblnd_rd_consume_frag(kib_rdma_desc_t *rd, int index, __u32 nob)
{
        if (nob < rd->rd_frags[index].rf_nob) {
                rd->rd_frags[index].rf_addr += nob;
                rd->rd_frags[index].rf_nob  -= nob;
        } else {
                index ++;
        }

        return index;
}

static inline int
kiblnd_rd_msg_size(kib_rdma_desc_t *rd, int msgtype, int n)
{
        LASSERT (msgtype == IBLND_MSG_GET_REQ ||
                 msgtype == IBLND_MSG_PUT_ACK);

        return msgtype == IBLND_MSG_GET_REQ ?
               offsetof(kib_get_msg_t, ibgm_rd.rd_frags[n]) :
               offsetof(kib_putack_msg_t, ibpam_rd.rd_frags[n]);
}

static inline __u64
kiblnd_dma_mapping_error(struct ib_device *dev, u64 dma_addr)
{
        return ib_dma_mapping_error(dev, dma_addr);
}

static inline __u64 kiblnd_dma_map_single(struct ib_device *dev,
                                          void *msg, size_t size,
                                          enum dma_data_direction direction)
{
        return ib_dma_map_single(dev, msg, size, direction);
}

static inline void kiblnd_dma_unmap_single(struct ib_device *dev,
                                           __u64 addr, size_t size,
                                          enum dma_data_direction direction)
{
        ib_dma_unmap_single(dev, addr, size, direction);
}

#define KIBLND_UNMAP_ADDR_SET(p, m, a)  do {} while (0)
#define KIBLND_UNMAP_ADDR(p, m, a)      (a)

static inline int kiblnd_dma_map_sg(struct ib_device *dev,
                                    struct scatterlist *sg, int nents,
                                    enum dma_data_direction direction)
{
        return ib_dma_map_sg(dev, sg, nents, direction);
}

static inline void kiblnd_dma_unmap_sg(struct ib_device *dev,
                                       struct scatterlist *sg, int nents,
                                       enum dma_data_direction direction)
{
        ib_dma_unmap_sg(dev, sg, nents, direction);
}

static inline __u64 kiblnd_sg_dma_address(struct ib_device *dev,
                                          struct scatterlist *sg)
{
        return ib_sg_dma_address(dev, sg);
}

static inline unsigned int kiblnd_sg_dma_len(struct ib_device *dev,
                                             struct scatterlist *sg)
{
        return ib_sg_dma_len(dev, sg);
}

/* XXX We use KIBLND_CONN_PARAM(e) as writable buffer, it's not strictly
 * right because OFED1.2 defines it as const, to use it we have to add
 * (void *) cast to overcome "const" */

#define KIBLND_CONN_PARAM(e)            ((e)->param.conn.private_data)
#define KIBLND_CONN_PARAM_LEN(e)        ((e)->param.conn.private_data_len)

#ifdef HAVE_IB_GET_DMA_MR
struct ib_mr *kiblnd_find_rd_dma_mr(struct lnet_ni *ni, kib_rdma_desc_t *rd,
				    int negotiated_nfrags);
#endif
void kiblnd_map_rx_descs(kib_conn_t *conn);
void kiblnd_unmap_rx_descs(kib_conn_t *conn);
void kiblnd_pool_free_node(kib_pool_t *pool, struct list_head *node);
struct list_head *kiblnd_pool_alloc_node(kib_poolset_t *ps);

int  kiblnd_fmr_pool_map(kib_fmr_poolset_t *fps, kib_tx_t *tx,
			 kib_rdma_desc_t *rd, __u32 nob, __u64 iov,
			 kib_fmr_t *fmr, bool *is_fastreg);
void kiblnd_fmr_pool_unmap(kib_fmr_t *fmr, int status);

int  kiblnd_tunables_setup(struct lnet_ni *ni);
int  kiblnd_tunables_init(void);

int  kiblnd_connd (void *arg);
int  kiblnd_scheduler(void *arg);
int  kiblnd_thread_start(int (*fn)(void *arg), void *arg, char *name);
int  kiblnd_failover_thread (void *arg);

int  kiblnd_alloc_pages(kib_pages_t **pp, int cpt, int npages);

int  kiblnd_cm_callback(struct rdma_cm_id *cmid,
                        struct rdma_cm_event *event);
int  kiblnd_translate_mtu(int value);

int  kiblnd_dev_failover(kib_dev_t *dev);
int kiblnd_create_peer(struct lnet_ni *ni, kib_peer_ni_t **peerp,
		       lnet_nid_t nid);
void kiblnd_destroy_peer (kib_peer_ni_t *peer);
bool kiblnd_reconnect_peer(kib_peer_ni_t *peer);
void kiblnd_destroy_dev (kib_dev_t *dev);
void kiblnd_unlink_peer_locked (kib_peer_ni_t *peer_ni);
kib_peer_ni_t *kiblnd_find_peer_locked(struct lnet_ni *ni, lnet_nid_t nid);
int  kiblnd_close_stale_conns_locked (kib_peer_ni_t *peer_ni,
                                      int version, __u64 incarnation);
int  kiblnd_close_peer_conns_locked (kib_peer_ni_t *peer_ni, int why);

kib_conn_t *kiblnd_create_conn(kib_peer_ni_t *peer_ni, struct rdma_cm_id *cmid,
			       int state, int version);
void kiblnd_destroy_conn(kib_conn_t *conn, bool free_conn);
void kiblnd_close_conn (kib_conn_t *conn, int error);
void kiblnd_close_conn_locked (kib_conn_t *conn, int error);

void kiblnd_launch_tx(struct lnet_ni *ni, kib_tx_t *tx, lnet_nid_t nid);
void kiblnd_txlist_done(struct list_head *txlist, int status);

void kiblnd_qp_event(struct ib_event *event, void *arg);
void kiblnd_cq_event(struct ib_event *event, void *arg);
void kiblnd_cq_completion(struct ib_cq *cq, void *arg);

void kiblnd_pack_msg(struct lnet_ni *ni, kib_msg_t *msg, int version,
		     int credits, lnet_nid_t dstnid, __u64 dststamp);
int  kiblnd_unpack_msg(kib_msg_t *msg, int nob);
int  kiblnd_post_rx (kib_rx_t *rx, int credit);

int  kiblnd_send(struct lnet_ni *ni, void *private, struct lnet_msg *lntmsg);
int kiblnd_recv(struct lnet_ni *ni, void *private, struct lnet_msg *lntmsg,
		int delayed, unsigned int niov, struct kvec *iov,
		lnet_kiov_t *kiov, unsigned int offset, unsigned int mlen,
		unsigned int rlen);

