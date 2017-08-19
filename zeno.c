/* -*- mode: c; c-basic-offset: 4; fill-column: 95; -*- */
#include <stdio.h>
#include <inttypes.h>

#include <string.h>
#include <limits.h>
#include <assert.h>

#include "zeno.h"
#include "zeno-config-deriv.h"
#include "zeno-msg.h"
#include "zeno-int.h"
#include "zeno-tracing.h"
#include "zeno-time.h"
#include "pack.h"
#include "unpack.h"
#include "bitset.h"
#include "binheap.h"

#define WC_DRESULT_SIZE     8 /* worst-case result size: header, commitid, status, rid */
#define WC_DCOMMIT_SIZE     2 /* commit: header, commitid */
#define WC_DPUB_SIZE        6 /* pub: header, rid (not using properties) */
#define WC_DSUB_SIZE        7 /* sub: header, rid, mode (neither properties nor periodic modes) */

#define PEERST_UNKNOWN       0
#define PEERST_OPENING_MIN   1
#define PEERST_OPENING_MAX   5
#define PEERST_ESTABLISHED 255

union {
    const struct peerid v;
    struct peerid v_nonconst;
} ownid_union;
#define ownid (ownid_union.v)
const ztimediff_t lease_dur = 5000;

struct in_conduit {
    seq_t seq;                    /* next seq to be delivered */
    seq_t lseqpU;                 /* latest seq known to exist, plus UNIT */
    seq_t useq;                   /* next unreliable seq to be delivered */
    uint8_t synched: 1;           /* whether a synch was received since (re)establishing the connection */
    uint8_t usynched: 1;          /* whether some unreliable data was received since (re)establishing the connection */
    ztime_t tack;
};

struct out_conduit {
    zeno_address_t addr;          /* destination address */
    seq_t    seq;                 /* next seq to send */
    seq_t    seqbase;             /* latest seq ack'd + UNIT = first available */
    seq_t    useq;                /* next unreliable seq to send */
    uint16_t pos;                 /* next byte goes into rbuf[pos] */
    uint16_t spos;                /* starting pos of current sample for patching in size */
    uint16_t firstpos;            /* starting pos (actually, size) of oldest sample in window */
    uint16_t xmitw_bytes;         /* size of transmit window pointed to by rbuf */
    ztime_t  tsynch;              /* next time to send out a SYNCH because of unack'd messages */
    cid_t    cid;                 /* conduit id */
    ztime_t  last_rexmit;
    seq_t    last_rexmit_seq;
    unsigned draining_window: 1;
    uint8_t  *rbuf;               /* reliable samples (or declarations); prepended by size (of type zmsize_t) */
};

/* FIXME: add a mask of mconduits that the peer listens to, with the set determined from the multicast locators in the discovery phase */
struct peer {
    uint8_t state;                /* connection state for this peer */
    ztime_t tlease;               /* peer must send something before tlease or we'll close the session | next time for scout/open msg */
    ztimediff_t lease_dur;        /* lease duration in ms */
#if HAVE_UNICAST_CONDUIT
    struct out_conduit oc;        /* unicast to this peer */
#else
    struct { zeno_address_t addr; } oc;
#endif
    struct in_conduit ic[N_IN_CONDUITS]; /* one slot for each out conduit from this peer */
    struct peerid id;             /* peer id */
#if N_OUT_MCONDUITS > 0
    uint8_t mc_member[(N_OUT_MCONDUITS+7)/8]; /* FIXME: what if I want to change the underlying type of a bitset ... should have a better way of declaring this */
#endif
};

#if N_OUT_MCONDUITS > 0
#  if MAX_PEERS == 0
#    error "N_OUT_CONDUITS > 1 requires MAX_PEERS > 0"
#  endif
struct out_mconduit {
    struct out_conduit oc;        /* same transmit window management as unicast */
    struct minseqheap seqbase;    /* tracks ACKs from peers for computing oc.seqbase as min of them all */
};

struct out_mconduit out_mconduits[N_OUT_MCONDUITS];
uint8_t out_mconduits_oc_rbuf[N_OUT_MCONDUITS][XMITW_BYTES];
#endif

#if N_OUT_MCONDUITS == 0
#  define DO_FOR_UNICAST_OR_MULTICAST(cid_, unicast_, multicast_) do { \
          unicast_; \
      } while (0)
#elif ! HAVE_UNICAST_CONDUIT
#  define DO_FOR_UNICAST_OR_MULTICAST(cid_, unicast_, multicast_) do { \
          multicast_; \
      } while (0)
#else
#  define DO_FOR_UNICAST_OR_MULTICAST(cid_, unicast_, multicast_) do { \
          if (cid_ == UNICAST_CID) { \
              unicast_; \
          } else { \
              multicast_; \
          } \
      } while (0)
#endif

/* we send SCOUT messages to a separately configurable address (not so much because it really seems
   necessary to have a separate address for scouting, as that we need a statically available address
   to use for the destination of the outgoing packet) */
zeno_address_t scoutaddr;
struct zeno_transport *transport;

#if MAX_MULTICAST_GROUPS > 0
uint16_t n_multicast_locators;
zeno_address_t multicast_locators[MAX_MULTICAST_GROUPS];
#endif

/* FIXME: for packet-based we can do with a single input buffer; for stream-based we will probably need an input buffer per peer */
#if TRANSPORT_MODE == TRANSPORT_STREAM && MAX_PEERS > 0
#error "haven't worked out the details of peer-to-peer with stream-based transports"
#endif
uint8_t inbuf[TRANSPORT_MTU];     /* where we buffer incoming packets */
#if TRANSPORT_MODE == TRANSPORT_STREAM
zmsize_t inp;                     /* current position in inbuf while collecting a message for processing */
#endif

/* output buffer is a single packet; a single packet has a single destination and carries reliable data for at most one conduit */
uint8_t outbuf[TRANSPORT_MTU];    /* where we buffer next outgoing packet */
zmsize_t outp;                    /* current position in outbuf */
#define OUTSPOS_UNSET ((zmsize_t) -1)
zmsize_t outspos;                 /* OUTSPOS_UNSET or pos of last reliable SData/Declare header (OUTSPOS_UNSET <=> outc == NULL) */
struct out_conduit *outc;         /* conduit over which reliable messages are carried in this packet, or NULL */
zeno_address_t *outdst;           /* destination address: &scoutaddr, &peer.oc.addr, &out_mconduits[cid].addr */
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
ztime_t outdeadline;              /* pack until destination change, packet full, or this time passed */
#endif

/* In client mode, we pretend the broker is peer 0 (and the only peer at that). It isn't really a peer,
   but the data structures we need are identical, only the discovery behaviour and (perhaps) session
   handling is a bit different. */
peeridx_t npeers;
struct peer peers[MAX_PEERS_1];
#if HAVE_UNICAST_CONDUIT
uint8_t peers_oc_rbuf[MAX_PEERS_1][XMITW_BYTES_UNICAST];
#endif

#if MAX_PEERS > 0
/* In peer mode, always send scouts periodically, with tnextscout giving the time for the next scout 
   message to go out. In client mode, scouting is conditional upon the state of the broker, in that
   case scouts only go out if peers[0].state = UNKNOWN, and we use peers[0].tlease to time them. */
ztime_t tnextscout;
#endif

static void remove_acked_messages(struct out_conduit * const c, seq_t seq);

static void oc_reset_transmit_window(struct out_conduit * const oc)
{
    oc->seqbase = oc->seq;
    oc->firstpos = oc->spos;
    oc->draining_window = 0;
}

static void oc_setup1(struct out_conduit * const oc, cid_t cid, uint16_t xmitw_bytes, uint8_t *rbuf)
{
    memset(&oc->addr, 0, sizeof(oc->addr));
    oc->cid = cid;
    oc->seq = 0; /* FIXME: reset seq, or send SYNCH? the latter, I think */
    oc->useq = 0; /* FIXME: reset or SYNCH? -- SYNCH doesn't even cover unreliable at the moment */
    oc->pos = sizeof(zmsize_t);
    oc->spos = 0;
    oc->xmitw_bytes = xmitw_bytes;
    oc->rbuf = rbuf;
    oc_reset_transmit_window(oc);
}

static void reset_outbuf(void)
{
    /* FIXME: keep outgoing packet builder as is? */
    outspos = OUTSPOS_UNSET;
    outp = 0;
    outc = NULL;
    outdst = NULL;
}

static void reset_peer(peeridx_t peeridx, ztime_t tnow)
{
    /* FIXME: remote declares */

    struct peer * const p = &peers[peeridx];
    /* If data destined for this peer, drop it it */
#if HAVE_UNICAST_CONDUIT
    if (outdst == &p->oc.addr) {
        reset_outbuf();
    }
#endif
#if N_OUT_MCONDUITS > 0
    /* For those multicast conduits where this peer is among the ones that need to ACK,
       update the administration */
    for (cid_t i = 0; i < N_OUT_MCONDUITS; i++) {
        struct out_mconduit * const mc = &out_mconduits[i];
        if (minseqheap_delete(peeridx, &mc->seqbase)) {
            assert(bitset_test(p->mc_member, (unsigned)i));
            if (minseqheap_isempty(&mc->seqbase)) {
                remove_acked_messages(&mc->oc, mc->oc.seq);
            } else {
                remove_acked_messages(&mc->oc, minseqheap_get_min(&mc->seqbase));
            }
        } else {
            assert(!bitset_test(p->mc_member, (unsigned)i));
        }
    }
#endif
#ifndef NDEBUG
    /* State of most fields shouldn't matter if peer state is UNKNOWN, sequence numbers
       and transmit windows in conduits do matter (so we don't need to clear them upon
       accepting the peer) */
    memset(p, 0xee, sizeof(*p));
#endif
    if (p->state == PEERST_ESTABLISHED) {
        npeers--;
    }
    p->state = PEERST_UNKNOWN;
#if HAVE_UNICAST_CONDUIT
    oc_setup1(&p->oc, UNICAST_CID, XMITW_BYTES_UNICAST, peers_oc_rbuf[peeridx]);
#endif
    for (cid_t i = 0; i < N_IN_CONDUITS; i++) {
        p->ic[i].seq = 0;
        p->ic[i].lseqpU = 0;
        p->ic[i].useq = 0;
        p->ic[i].synched = 0;
        p->ic[i].usynched = 0;
    }
#if N_OUT_MCONDUITS > 0
    memset(p->mc_member, 0, sizeof(p->mc_member));
#endif
}

static void init_globals(void)
{
    ztime_t tnow = zeno_time();
    
#if N_OUT_MCONDUITS > 0
    /* Need to reset out_mconduits[.].seqbase.ix[i] before reset_peer(i) may be called */
    for (cid_t i = 0; i < N_OUT_MCONDUITS; i++) {
        struct out_mconduit * const mc = &out_mconduits[i];
        oc_setup1(&mc->oc, i, XMITW_BYTES, out_mconduits_oc_rbuf[i]);
        mc->seqbase.n = 0;
        /* FIXME: seqbase.hx[peeridx] is not a good idea */
        for (peeridx_t j = 0; j < MAX_PEERS; j++) {
            mc->seqbase.hx[j] = PEERIDX_INVALID;
            mc->seqbase.ix[j].i = PEERIDX_INVALID;
        }
    }
#endif
    for (peeridx_t i = 0; i < MAX_PEERS_1; i++) {
        reset_peer(i, tnow);
    }
    npeers = 0;
    reset_outbuf();
    /* FIXME: keep incoming packet buffer? I guess in packet mode that's ok, for streaming would probably need MAX_PEERS_1 */
#if TRANSPORT_MODE == TRANSPORT_STREAM
    inp = 0;
#endif
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
    outdeadline = tnow;
#endif
#if MAX_PEERS > 0
    tnextscout = tnow;
#endif
}

static uint16_t xmitw_pos_add(const struct out_conduit *c, uint16_t p, uint16_t a)
{
    if ((p += a) >= c->xmitw_bytes) {
        p -= c->xmitw_bytes;
    }
    return p;
}

uint16_t xmitw_bytesavail(const struct out_conduit *c)
{
    uint16_t res;
    assert(c->pos < c->xmitw_bytes);
    assert(c->pos == xmitw_pos_add(c, c->spos, sizeof(zmsize_t)));
    assert(c->firstpos < c->xmitw_bytes);
    res = c->firstpos + (c->firstpos < c->pos ? c->xmitw_bytes : 0) - c->pos;
    assert(res <= c->xmitw_bytes);
    return res;
}

static seq_t oc_get_nsamples(struct out_conduit const * const c)
{
    return (seq_t)(c->seq - c->seqbase) >> SEQNUM_SHIFT;
}

static void pack_msend(void)
{
    assert ((outspos == OUTSPOS_UNSET) == (outc == NULL));
    assert (outdst != NULL);
    if (outspos != OUTSPOS_UNSET) {
        /* FIXME: not-so-great proxy for transition past 3/4 of window size */
        uint16_t cnt = xmitw_bytesavail(outc);
        if (cnt < outc->xmitw_bytes / 4 && cnt + outspos >= outc->xmitw_bytes / 4) {
            outbuf[outspos] |= MSFLAG;
        }
    }
    if (transport_ops.send(transport, outbuf, outp, outdst) < 0) {
        assert(0);
    }
    outp = 0;
    outspos = OUTSPOS_UNSET;
    outc = NULL;
    outdst = NULL;
}

static void pack_check_avail(uint16_t n)
{
    assert(sizeof (outbuf) - outp >= n);
}

void pack_reserve(zeno_address_t *dst, struct out_conduit *oc, zpsize_t cnt)
{
    /* oc != NULL <=> reserving for reliable data */
    /* make room by sending out current packet if requested number of bytes is no longer
       available, and also send out current packet if the destination changes */
    if (TRANSPORT_MTU - outp < cnt || (outdst != NULL && dst != outdst) || (outc && outc != oc)) {
        /* we should never even try to generate a message that is too large for a packet */
        assert(outp != 0);
        pack_msend();
    }
    if (oc) {
        outc = oc;
    }
    outdst = dst;
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
    if (outp == 0) {
        /* packing deadline: note that no incomplete messages will ever be in the buffer when
           we check, because it is single-threaded and we always complete whatever message we
           start constructing */
        outdeadline = zeno_time() + LATENCY_BUDGET;
        ZT(DEBUG, ("deadline at %"PRIu32".%0"PRIu32, (uint32_t)outdeadline / 1000u, (uint32_t)outdeadline % 1000u));
    }
#endif
}

void pack1(uint8_t x)
{
    pack_check_avail(1);
    outbuf[outp++] = x;
}

void pack2(uint8_t x, uint8_t y)
{
    pack_check_avail(2);
    outbuf[outp++] = x;
    outbuf[outp++] = y;
}

void pack_u16(uint16_t x)
{
    pack2(x & 0xff, x >> 8);
}

void pack_vec(zpsize_t n, const void *vbuf)
{
    const uint8_t *buf = vbuf;
    pack_vle16(n);
    pack_check_avail(n);
    while (n--) {
        outbuf[outp++] = *buf++;
    }
}

uint16_t pack_locs_calcsize(void)
{
#if MAX_MULTICAST_GROUPS > 0
    size_t n = pack_vle16req(n_multicast_locators);
    char tmp[TRANSPORT_ADDRSTRLEN];
    for (uint16_t i = 0; i < n_multicast_locators; i++) {
        size_t n1 = transport_ops.addr2string(tmp, sizeof(tmp), &multicast_locators[i]);
        assert(n1 < UINT16_MAX);
        n += pack_vle16req((uint16_t)n1) + n1;
    }
    assert(n < UINT16_MAX);
    return (uint16_t)n;
#else
    return 1;
#endif
}

void pack_locs(void)
{
#if MAX_MULTICAST_GROUPS > 0
    pack_vle16(n_multicast_locators);
    for (uint16_t i = 0; i < n_multicast_locators; i++) {
        char tmp[TRANSPORT_ADDRSTRLEN];
        uint16_t n1 = (uint16_t)transport_ops.addr2string(tmp, sizeof(tmp), &multicast_locators[i]);
        pack_vec(n1, tmp);
    }
#else
    pack_vle16(0);
#endif
}

cid_t oc_get_cid(struct out_conduit *c)
{
    return c->cid;
}

void oc_hit_full_window(struct out_conduit *c)
{
    c->draining_window = 1;
    if (outp > 0) {
        pack_msynch(outdst, MSFLAG, c->cid, c->seqbase, oc_get_nsamples(c));
        pack_msend();
    }
}

#if N_OUT_MCONDUITS > 0
int ocm_have_peers(const struct out_mconduit *mc)
{
    return !minseqheap_isempty(&mc->seqbase);
}
#endif

void oc_pack_copyrel(struct out_conduit *c, zmsize_t from)
{
    /* only for non-empty sequence of initial bytes of message (i.e., starts with header */
    assert(c->pos == xmitw_pos_add(c, c->spos, sizeof(zmsize_t)));
    assert(from < outp);
    assert(!(outbuf[from] & MSFLAG));
    while (from < outp) {
        assert(c->pos != c->firstpos || c->seq == c->seqbase);
        c->rbuf[c->pos] = outbuf[from++];
        c->pos = xmitw_pos_add(c, c->pos, 1);
    }
}

zmsize_t oc_pack_payload_msgprep(seq_t *s, struct out_conduit *c, int relflag, zpsize_t sz)
{
    assert(c->pos == xmitw_pos_add(c, c->spos, sizeof(zmsize_t)));
    if (!relflag) {
        pack_reserve_mconduit(&c->addr, NULL, c->cid, sz);
        *s = c->useq;
    } else {
        pack_reserve_mconduit(&c->addr, c, c->cid, sz);
        *s = c->seq;
        outspos = outp;
    }
    //ZT(RELIABLE, ("oc_pack_payload_msgprep cid %u %p seq %u sz %u", c->cid, (void*)c, *s >> SEQNUM_SHIFT, (unsigned)sz));
    return outp;
}

void oc_pack_payload(struct out_conduit *c, int relflag, zpsize_t sz, const void *vdata)
{
    /* c->spos points to size byte, header byte immediately follows it, so reliability flag is
     easily located in the buffer */
    const uint8_t *data = (const uint8_t *)vdata;
    while (sz--) {
        outbuf[outp++] = *data;
        if (relflag) {
            assert(c->pos != c->firstpos);
            c->rbuf[c->pos] = *data;
            c->pos = xmitw_pos_add(c, c->pos, 1);
        }
        data++;
    }
}

void oc_pack_payload_done(struct out_conduit *c, int relflag)
{
    if (!relflag) {
        c->useq += SEQNUM_UNIT;
    } else {
        zmsize_t len = (zmsize_t) (c->pos - c->spos + (c->pos < c->spos ? c->xmitw_bytes : 0) - sizeof(zmsize_t));
        memcpy(&c->rbuf[c->spos], &len, sizeof(len));
        c->spos = c->pos;
        c->pos = xmitw_pos_add(c, c->pos, sizeof(zmsize_t));
        if (c->seq == c->seqbase) {
            /* first unack'd sample, schedule SYNCH */
            c->tsynch = zeno_time() + MSYNCH_INTERVAL;
        }
        c->seq += SEQNUM_UNIT;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////

#define MAX_SUBS 10
#define MAX_PUBS 10

struct subtable {
    /* ID of the resource subscribed to (could also be a SID, actually) */
    rid_t rid;

    /* Minimum number of bytes that must be available in transmit window in the given conduit
       before calling, must include message overhead (for writing SDATA -- that is, no PRID
       present -- worst case is 9 bytes with a payload limit of 127 bytes and 32-bit RIDs) */
    struct out_conduit *oc;
    zpsize_t xmitneed;

    /* */
    void *arg;
    subhandler_t handler;
};
struct subtable subs[MAX_SUBS];

struct pubtable {
    struct out_conduit *oc;
    rid_t rid;
};
struct pubtable pubs[MAX_PUBS];

/* FIXME: should switch from publisher determines reliability to subscriber determines
   reliability, i.e., publisher reliability bit gets set to 
   (foldr or False $ map isReliableSub subs).  Keeping the reliability information
   separate from pubs has the advantage of saving quite a few bytes. */
uint8_t pubs_isrel[(MAX_PUBS + 7) / 8];
uint8_t rsubs[(MAX_PUBS + 7) / 8];

uint8_t precommit_rsubs[(MAX_PUBS + 7) / 8];
uint8_t precommit_result;
rid_t precommit_invalid_rid;

uint8_t precommit_curpkt_rsubs[(MAX_PUBS + 7) / 8];
uint8_t precommit_curpkt_result;
rid_t precommit_curpkt_invalid_rid;

void rsub_register(rid_t rid, uint8_t submode)
{
    uint8_t pubidx;
    assert(rid != 0);
    for (pubidx = 0; pubidx < MAX_PUBS; pubidx++) {
        if (pubs[pubidx].rid == rid) {
            break;
        }
    }
    if (submode == SUBMODE_PUSH && pubidx < MAX_PUBS) {
        bitset_set(precommit_curpkt_rsubs, pubidx);
    } else {
        if (precommit_curpkt_result == 0) {
            precommit_curpkt_invalid_rid = rid;
        }
        if (submode != SUBMODE_PUSH) {
            precommit_curpkt_result |= 1;
        }
        if (pubidx >= MAX_PUBS) {
            precommit_curpkt_result |= 2;
        }
    }
}

uint8_t rsub_precommit(rid_t *err_rid)
{
    assert (precommit_curpkt_result == 0);
    if (precommit_result == 0) {
        return 0;
    } else {
        size_t i;
        *err_rid = precommit_invalid_rid;
        for (i = 0; i < sizeof (precommit_rsubs); i++) {
            precommit_rsubs[i] = 0;
        }
        precommit_result = 0;
        precommit_invalid_rid = 0;
        return precommit_result;
    }
}

void rsub_commit(void)
{
    size_t i;
    assert (precommit_result == 0);
    for (i = 0; i < sizeof (rsubs); i++)
        rsubs[i] |= precommit_rsubs[i];
    for (i = 0; i < sizeof (precommit_rsubs); i++) {
        precommit_rsubs[i] = 0;
    }
    precommit_result = 0;
    precommit_invalid_rid = 0;
}

int rsub_exists(uint8_t pubidx)
{
    assert(pubidx < MAX_PUBS);
    return bitset_test(rsubs, pubidx);
}

void rsub_precommit_curpkt_abort(void)
{
    size_t i;
    for (i = 0; i < sizeof (precommit_rsubs); i++) {
        precommit_curpkt_rsubs[i] = 0;
    }
    precommit_curpkt_invalid_rid = 0;
    precommit_curpkt_result = 0;
}

void rsub_precommit_curpkt_done(void)
{
    size_t i;
    for (i = 0; i < sizeof (precommit_rsubs); i++) {
        precommit_rsubs[i] |= precommit_curpkt_rsubs[i];
    }
    if (precommit_invalid_rid == 0) {
        precommit_invalid_rid = precommit_curpkt_invalid_rid;
    }
    precommit_result |= precommit_curpkt_result;
    rsub_precommit_curpkt_abort();
}

void rsub_clear(void)
{
    size_t i;
    for (i = 0; i < sizeof (rsubs); i++) {
        rsubs[i] = 0;
    }
    for (i = 0; i < sizeof (precommit_rsubs); i++) {
        precommit_rsubs[i] = 0;
    }
    precommit_result = 0;
    precommit_invalid_rid = 0;
    rsub_precommit_curpkt_abort();
}

///////////////////////////////////////////////////////////////////////////////////////////

static zmsize_t handle_mscout(peeridx_t peeridx, zmsize_t sz, const uint8_t *data)
{
#if MAX_PEERS > 0
    const uint32_t lookfor = MSCOUT_PEER;
    const int state_ok = (peers[peeridx].state == PEERST_UNKNOWN);
#else
    const uint32_t lookfor = MSCOUT_CLIENT;
    const int state_ok = 1;
#endif
    const uint8_t *data0 = data;
    uint8_t hdr;
    uint32_t mask;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_vle32(&sz, &data, &mask)) {
        return 0;
    }
    /* For a client all activity is really client-initiated, so we can get away
       with not responding to a SCOUT; for a peer it is different */
    if ((mask & lookfor) && state_ok) {
        ZT(PEERDISC, ("got a scout! sending a hello"));
        pack_mhello(&peers[peeridx].oc.addr);
        pack_msend();
    }
    return (zmsize_t)(data - data0);
}

static int set_peer_mcast_locs(peeridx_t peeridx, struct unpack_locs_iter *it)
{
    zpsize_t sz;
    const uint8_t *loc;
    while (unpack_locs_iter(it, &sz, &loc)) {
        zeno_address_t addr;
        if (!transport_ops.octseq2addr(&addr, sz, loc)) {
            return 0;
        }
#if N_OUT_MCONDUITS > 0
        for (cid_t cid = 0; cid < N_OUT_MCONDUITS; cid++) {
            if (transport_ops.addr_eq(&addr, &out_mconduits[cid].oc.addr)) {
                bitset_set(peers[peeridx].mc_member, (unsigned)cid);

                char tmp[TRANSPORT_ADDRSTRLEN];
                transport_ops.addr2string(tmp, sizeof(tmp), &addr);
                ZT(PEERDISC, ("loc %s cid %u", tmp, (unsigned)cid));
            }
        }
#endif
    }
    return 1;
}

static zmsize_t handle_mhello(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, ztime_t tnow)
{
#if MAX_PEERS > 0
    const uint32_t lookfor = MSCOUT_PEER | MSCOUT_BROKER;
    const int state_ok = (peers[peeridx].state == PEERST_UNKNOWN || peers[peeridx].state == PEERST_ESTABLISHED);
#else
    const uint32_t lookfor = MSCOUT_BROKER;
    const int state_ok = (peers[peeridx].state == PEERST_UNKNOWN);
#endif
    const uint8_t *data0 = data;
    struct unpack_locs_iter locs_it;
    uint8_t hdr;
    uint32_t mask;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_vle32(&sz, &data, &mask) ||
        !unpack_locs(&sz, &data, &locs_it) ||
        !unpack_props(&sz, &data)) {
        return 0;
    }
    if ((mask & lookfor) && state_ok) {
        int send_open = 1;

        ZT(PEERDISC, ("got a hello! sending an open"));
        if (peers[peeridx].state != PEERST_ESTABLISHED) {
            if (!set_peer_mcast_locs(peeridx, &locs_it)) {
                ZT(PEERDISC, ("'twas but a hello with an invalid locator list ..."));
                send_open = 0;
            } else {
                peers[peeridx].state = PEERST_OPENING_MIN;
                peers[peeridx].tlease = tnow;
            }
        }
        /* FIXME: what parts of peers[peeridx] need to be initialised here? */
        /* FIXME: a hello when established indicates a reconnect for the other one => should clear ic[.].synched, usynched, I think */
        if (send_open) {
            pack_mopen(&peers[peeridx].oc.addr, SEQNUM_LEN, &ownid, lease_dur);
            pack_msend();
        }
    }
    return (zmsize_t)(data - data0);
}

static peeridx_t find_peeridx_by_id(peeridx_t peeridx, zpsize_t idlen, const uint8_t * restrict id)
{
    /* FIXME: peeridx can now change while processing a message ... so should pass peeridx by reference in all message handling */

    /* keepalive, open, accept and close contain a peer id, and can be used to switch source address;
       peeridx on input is the peeridx determined using the source address, on return it will be the
       peeridx of the known peer with this id (if any) and otherwise just the same idx */

    if (peers[peeridx].state == PEERST_ESTABLISHED) {
        /* assume there is no foul play and this is the same peer */
        return peeridx;
    }

    for (peeridx_t i = 0; i < MAX_PEERS_1; i++) {
        if (peers[i].state != PEERST_ESTABLISHED) {
            continue;
        }
        if (peers[i].id.len == idlen && memcmp(peers[i].id.id, id, idlen) == 0) {
            char olda[TRANSPORT_ADDRSTRLEN], newa[TRANSPORT_ADDRSTRLEN];
            transport_ops.addr2string(olda, sizeof(olda), &peers[i].oc.addr);
            transport_ops.addr2string(newa, sizeof(newa), &peers[peeridx].oc.addr);
            ZT(PEERDISC, ("peer %u changed address from %s to %s", (unsigned)i, olda, newa));
            peers[i].oc.addr = peers[peeridx].oc.addr;
            return i;
        }
    }
    return peeridx;
}

static void accept_peer(peeridx_t peeridx, zpsize_t idlen, const uint8_t * restrict id, ztimediff_t lease_dur, ztime_t tnow)
{
    struct peer * const p = &peers[peeridx];
    char astr[TRANSPORT_ADDRSTRLEN];
    char idstr[3*PEERID_SIZE];
    assert(p->state != PEERST_ESTABLISHED);
    assert(idlen > 0 && idlen <= PEERID_SIZE);
    transport_ops.addr2string(astr, sizeof(astr), &p->oc.addr);

    assert(idlen <= PEERID_SIZE);
    assert(lease_dur >= 0);
    for (int i = 0; i < idlen; i++) {
        sprintf(idstr+3*i-(i>0), "%s%02hhx", i == 0 ? "" : ":", id[i]);
    }
    ZT(PEERDISC, ("accept peer %s %s @ %u; lease = %" PRId32, idstr, astr, peeridx, (int32_t)lease_dur));

    p->state = PEERST_ESTABLISHED;
    p->id.len = idlen;
    memcpy(p->id.id, id, idlen);
    p->lease_dur = lease_dur;
    p->tlease = tnow + (ztime_t)p->lease_dur;
#if N_OUT_MCONDUITS > 0
    for (cid_t cid = 0; cid < N_OUT_MCONDUITS; cid++) {
        if (bitset_test(p->mc_member, (unsigned)cid)) {
            struct out_mconduit * const mc = &out_mconduits[cid];
            minseqheap_insert(peeridx, mc->oc.seq, &mc->seqbase);
        }
    }
#endif
    npeers++;
}

static zmsize_t handle_mopen(peeridx_t * restrict peeridx, zmsize_t sz, const uint8_t * restrict data, ztime_t tnow)
{
    const uint8_t *data0 = data;
    uint8_t hdr, version;
    uint16_t seqsize;
    zpsize_t idlen;
    uint8_t id[PEERID_SIZE];
    zpsize_t dummy;
    uint8_t reason;
    uint32_t ld100;
    struct unpack_locs_iter locs_it;
    struct peer *p;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_byte(&sz, &data, &version) /* version */ ||
        !unpack_vec(&sz, &data, sizeof(id), &idlen, id) /* peer id */ ||
        !unpack_vle32(&sz, &data, &ld100) /* lease duration */ ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL) /* auth */ ||
        !unpack_locs(&sz, &data, &locs_it)) {
        return 0;
    }
    if (!(hdr & MLFLAG)) {
        seqsize = 14;
    } else if (!unpack_vle16(&sz, &data, &seqsize)) {
        return 0;
    } else if (seqsize != SEQNUM_LEN) {
        ZT(PEERDISC, ("got an open with an unsupported sequence number size (%hu)", seqsize));
        reason = CLR_UNSUPP_SEQLEN;
        goto reject;
    }
    if (version != ZENO_VERSION) {
        ZT(PEERDISC, ("got an open with an unsupported version (%hhu)", version));
        reason = CLR_UNSUPP_PROTO;
        goto reject;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, ("got an open with an under- or oversized id (%hu)", idlen));
        reason = CLR_ERROR;
        goto reject;
    }
    if (ld100 > ZTIMEDIFF_MAX / 100) {
        ZT(PEERDISC, ("got an open with a lease duration that is not representable here"));
        reason = CLR_ERROR;
        goto reject;
    }
    if (idlen == ownid.len && memcmp(ownid.id, id, idlen) == 0) {
        ZT(PEERDISC, ("got an open with my own peer id"));
        goto reject_no_close;
    }

    *peeridx = find_peeridx_by_id(*peeridx, idlen, id);

    p = &peers[*peeridx];
    if (p->state != PEERST_ESTABLISHED) {
        if (!set_peer_mcast_locs(*peeridx, &locs_it)) {
            ZT(PEERDISC, ("'twas but an open with an invalid locator list ..."));
            reason = CLR_ERROR;
            goto reject;
        }
        accept_peer(*peeridx, idlen, id, 100 * (ztimediff_t)ld100, tnow);
    }
    pack_maccept(&p->oc.addr, &ownid, &p->id, lease_dur);
    pack_msend();

    return (zmsize_t)(data - data0);

reject:
    pack_mclose(&peers[*peeridx].oc.addr, reason, &ownid);
    /* don't want anything to do with the other anymore; calling reset on one that is already in UNKNOWN is harmless */
    reset_peer(*peeridx, tnow);
    /* no point in interpreting following messages in packet */
reject_no_close:
    return 0;
}

static zmsize_t handle_maccept(peeridx_t * restrict peeridx, zmsize_t sz, const uint8_t * restrict data, ztime_t tnow)
{
    const uint8_t *data0 = data;
    zpsize_t idlen;
    uint8_t id[PEERID_SIZE];
    uint32_t ld100;
    zpsize_t dummy;
    int forme;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_vec(&sz, &data, sizeof(id), &idlen, id)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, ("got an open with an under- or oversized id (%hu)", idlen));
        goto reject_no_close;
    }
    forme = (idlen == ownid.len && memcmp(id, ownid.id, idlen) == 0);
    if (!unpack_vec(&sz, &data, sizeof (id), &idlen, id) ||
        !unpack_vle32(&sz, &data, &ld100) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, ("got an open with an under- or oversized id (%hu)", idlen));
        goto reject_no_close;
    }
    if (forme) {
        if (ld100 > ZTIMEDIFF_MAX / 100) {
            ZT(PEERDISC, ("got an open with a lease duration that is not representable here"));
            goto reject;
        }
        *peeridx = find_peeridx_by_id(*peeridx, idlen, id);
        if (peers[*peeridx].state >= PEERST_OPENING_MIN && peers[*peeridx].state <= PEERST_OPENING_MAX) {
            accept_peer(*peeridx, idlen, id, 100 * (ztimediff_t)ld100, tnow);
        }
    }
    return (zmsize_t)(data - data0);

reject:
    pack_mclose(&peers[*peeridx].oc.addr, CLR_ERROR, &ownid);
    /* don't want anything to do with the other anymore; calling reset on one that is already in UNKNOWN is harmless */
    reset_peer(*peeridx, tnow);
    /* no point in interpreting following messages in packet */
reject_no_close:
    return 0;
}

static zmsize_t handle_mclose(peeridx_t * restrict peeridx, zmsize_t sz, const uint8_t * restrict data, ztime_t tnow)
{
    /* FIXME: should check id of sender */
    zpsize_t idlen;
    uint8_t id[PEERID_SIZE];
    uint8_t reason;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_vec(&sz, &data, sizeof(id), &idlen, id) ||
        !unpack_byte(&sz, &data, &reason)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        ZT(PEERDISC, ("got a close with an under- or oversized id (%hu)", idlen));
        reset_peer(*peeridx, tnow);
        return 0;
    }
    *peeridx = find_peeridx_by_id(*peeridx, idlen, id);
    if (peers[*peeridx].state != PEERST_UNKNOWN) {
        reset_peer(*peeridx, tnow);
    }
    return 0;
}

static zmsize_t handle_dresource(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    /* No use for a broker declaring its resources, but we don't bug out over it */
    const uint8_t *data0 = data;
    uint8_t hdr;
    zpsize_t dummy;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_rid(&sz, &data, NULL) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !unpack_props(&sz, &data)) {
        return 0;
    }
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_dpub(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    /* No use for a broker declaring its publications, but we don't bug out over it */
    const uint8_t *data0 = data;
    uint8_t hdr;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_rid(&sz, &data, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !unpack_props(&sz, &data)) {
        return 0;
    }
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_dsub(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    const uint8_t *data0 = data;
    rid_t rid;
    uint8_t hdr, mode;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_rid(&sz, &data, &rid) ||
        !unpack_byte(&sz, &data, &mode)) {
        return 0;
    }
    if (mode == 0 || mode > SUBMODE_MAX) {
        return 0;
    }
    if (mode == SUBMODE_PERIODPULL || mode == SUBMODE_PERIODPUSH) {
        if (!unpack_vle32(&sz, &data, NULL) ||
            !unpack_vle32(&sz, &data, NULL))
            return 0;
    }
    if ((hdr & DPFLAG) && !unpack_props(&sz, &data)) {
        return 0;
    }
    if (interpret) {
        rsub_register(rid, mode);
    }
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_dselection(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    /* FIXME: support selections? */
    const uint8_t *data0 = data;
    rid_t sid;
    uint8_t hdr;
    zpsize_t dummy;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_rid(&sz, &data, &sid) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !unpack_props(&sz, &data)) {
        return 0;
    }
    if (interpret) {
        if (precommit_curpkt_result == 0) {
            precommit_curpkt_invalid_rid = sid;
        }
        precommit_curpkt_result |= 4;
    }
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_dbindid(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    /* FIXME: support bindings?  I don't think there's a need. */
    const uint8_t *data0 = data;
    rid_t sid;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_rid(&sz, &data, &sid) ||
        !unpack_rid(&sz, &data, NULL)) {
        return 0;
    }
    if (interpret) {
        if (precommit_curpkt_result == 0) {
            precommit_curpkt_invalid_rid = sid;
        }
        precommit_curpkt_result |= 8;
    }
    return (zmsize_t)(data - data0);
}

static struct out_conduit *out_conduit_from_cid(peeridx_t peeridx, cid_t cid)
{
    struct out_conduit *c;
    DO_FOR_UNICAST_OR_MULTICAST(cid, c = &peers[peeridx].oc, c = &out_mconduits[cid].oc);
    return c;
}

static zmsize_t handle_dcommit(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    const uint8_t *data0 = data;
    uint8_t commitid;
    uint8_t res;
    rid_t err_rid;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_byte(&sz, &data, &commitid)) {
        return 0;
    }
    if (interpret) {
        /* If we can't reserve space in the transmit window, pretend we never received the
           DECLARE message and abandon the rest of the packet.  Eventually we'll get a
           retransmit and retry.  Use worst-case size for result */
        struct out_conduit * const oc = out_conduit_from_cid(0, 0);
        /* FIXME: need to make sure no peer present is ok, too */
        if (!oc_pack_mdeclare(oc, 1, WC_DRESULT_SIZE)) {
            return 0;
        } else {
            rsub_precommit_curpkt_done();
            if ((res = rsub_precommit(&err_rid)) == 0) {
                rsub_commit();
            }
            pack_dresult(commitid, res, err_rid);
            oc_pack_mdeclare_done(oc);
            pack_msend();
        }
    }
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_dresult(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    const uint8_t *data0 = data;
    uint8_t commitid, status;
    rid_t rid;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_byte(&sz, &data, &commitid) ||
        !unpack_byte(&sz, &data, &status)) {
        return 0;
    }
    if (status && !unpack_rid(&sz, &data, &rid)) {
        return 0;
    }
    if (interpret && status != 0) {
        /* Don't know what to do when the broker refuses my declarations - although I guess it
           would make some sense to close the connection and try again.  But even if that is
           the right thing to do, don't do that just yet, because it shouldn't fail.

           Also note that we're not looking at the commit id at all, I am not sure yet what
           problems that may cause ... */
        assert(0);
    }
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_ddeleteres(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, int interpret)
{
    const uint8_t *data0 = data;
    uint8_t hdr;
    zpsize_t dummy;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_vec(&sz, &data, 0, &dummy, NULL)) {
        return 0;
    }
    if ((hdr & DPFLAG) && !unpack_props(&sz, &data)) {
        return 0;
    }
    if (interpret) {
        precommit_curpkt_result |= 16;
    }
    return (zmsize_t)(data - data0);
}

int seq_lt(seq_t a, seq_t b)
{
    return (sseq_t) (a - b) < 0;
}

int seq_le(seq_t a, seq_t b)
{
    return (sseq_t) (a - b) <= 0;
}

static int ic_may_deliver_seq(const struct in_conduit *ic, uint8_t hdr, seq_t seq)
{
    if (hdr & MRFLAG) {
        return (ic->seq == seq);
    } else if (ic->usynched) {
        return seq_le(ic->useq, seq);
    } else {
        return 1;
    }
}

static void ic_update_seq (struct in_conduit *ic, uint8_t hdr, seq_t seq)
{
    assert(ic_may_deliver_seq(ic, hdr, seq));
    if (hdr & MRFLAG) {
        assert(seq_lt(ic->seq, ic->lseqpU));
        ic->seq = seq + SEQNUM_UNIT;
    } else {
        assert(seq_le(ic->seq, ic->lseqpU));
        ic->useq = seq + SEQNUM_UNIT;
        ic->usynched = 1;
    }
}

static void acknack_if_needed(peeridx_t peeridx, cid_t cid, int wantsack, ztime_t tnow)
{
    seq_t cnt = (peers[peeridx].ic[cid].lseqpU - peers[peeridx].ic[cid].seq) >> SEQNUM_SHIFT;
    uint32_t mask;
    assert(seq_le(peers[peeridx].ic[cid].seq, peers[peeridx].ic[cid].lseqpU));
    if (cnt == 0) {
        mask = 0;
    } else {
        mask = ~(uint32_t)0;
        if (cnt < 32) { /* avoid undefined behaviour */
            mask >>= 32 - cnt;
        }
    }
    if (wantsack || (mask != 0 && (ztimediff_t)(tnow - peers[peeridx].ic[cid].tack) > ROUNDTRIP_TIME_ESTIMATE)) {
        /* ACK goes out over unicast path; the conduit used for sending it doesn't have
           much to do with it other than administrative stuff */
        ZT(RELIABLE, ("acknack_if_needed peeridx %u cid %u wantsack %d mask %u seq %u", peeridx, cid, wantsack, mask, peers[peeridx].ic[cid].seq >> SEQNUM_SHIFT));
        pack_macknack(&peers[peeridx].oc.addr, cid, peers[peeridx].ic[cid].seq, mask);
        pack_msend();
        peers[peeridx].ic[cid].tack = tnow;
    }
}

static zmsize_t handle_mdeclare(peeridx_t peeridx, zmsize_t sz, const uint8_t * restrict data, cid_t cid, ztime_t tnow)
{
    /* Note 1: not buffering data received out-of-order, so but need to decode everything to
       find next message, which we may "have to" interpret - we don't really "have to", but to
       simply pretend we never received it is a bit rough.

       Note 2: a commit requires us to send something in reply, but we may be unable to because
       of a full transmit window in the reliable channel.  The elegant option is to suspend
       further input processing, until space is available again, the inelegant one is to verify
       we have space beforehand, and pretend we never received the DECLARE if we don't. */
    const uint8_t *data0 = data;
    uint8_t hdr;
    zmsize_t ncons;
    uint16_t ndecls;
    seq_t seq;
    int intp;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_seq(&sz, &data, &seq) ||
        !unpack_vle16(&sz, &data, &ndecls)) {
        return 0;
    }
    if (seq_le(peers[peeridx].ic[cid].lseqpU, seq)) {
        peers[peeridx].ic[cid].lseqpU = seq + SEQNUM_UNIT;
    }
    intp = (peers[peeridx].state == PEERST_ESTABLISHED && ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq));
    if (ndecls > 0) {
        do {
            uint8_t kind = *data & DKIND;
            switch (kind) {
                case DRESOURCE:  ncons = handle_dresource(peeridx, sz, data, intp); break;
                case DPUB:       ncons = handle_dpub(peeridx, sz, data, intp); break;
                case DSUB:       ncons = handle_dsub(peeridx, sz, data, intp); break;
                case DSELECTION: ncons = handle_dselection(peeridx, sz, data, intp); break;
                case DBINDID:    ncons = handle_dbindid(peeridx, sz, data, intp); break;
                case DCOMMIT:    ncons = handle_dcommit(peeridx, sz, data, intp); break;
                case DRESULT:    ncons = handle_dresult(peeridx, sz, data, intp); break;
                case DDELETERES: ncons = handle_ddeleteres(peeridx, sz, data, intp); break;
                default:         ncons = 0; break;
            }
            sz -= ncons;
            data += ncons;
            if (ncons > 0) {
                --ndecls;
            }
        } while (sz > 0 && ncons > 0);
        if (ndecls != 0) {
            rsub_precommit_curpkt_abort();
            return 0;
        }
    }
    if (intp) {
        /* Merge uncommitted declaration state resulting from this DECLARE message into
           uncommitted state accumulator, as we have now completely and successfully processed
           this message.  */
        rsub_precommit_curpkt_done();
        (void)ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
    }
    /* FIXME: or should it only do this if S flag set? */
    acknack_if_needed(peeridx, cid, hdr & MSFLAG, tnow);
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_msynch(peeridx_t peeridx, zmsize_t sz, const uint8_t * restrict data, cid_t cid, ztime_t tnow)
{
    const uint8_t *data0 = data;
    uint8_t hdr;
    seq_t cnt_shifted;
    seq_t seqbase;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_seq(&sz, &data, &seqbase) ||
        !unpack_seq(&sz, &data, &cnt_shifted)) {
        return 0;
    }
    if (peers[peeridx].state == PEERST_ESTABLISHED) {
        ZT(RELIABLE, ("handle_msynch peeridx %u cid %u seq %u cnt %u", peeridx, cid, seqbase >> SEQNUM_SHIFT, cnt_shifted >> SEQNUM_SHIFT));
        if (seq_le(peers[peeridx].ic[cid].seq, seqbase) || !peers[peeridx].ic[cid].synched) {
            if (!peers[peeridx].ic[cid].synched) {
                ZT(PEERDISC, ("handle_msynch peeridx %u cid %u seq %u cnt %u", peeridx, cid, seqbase >> SEQNUM_SHIFT, cnt_shifted >> SEQNUM_SHIFT));
            }
            peers[peeridx].ic[cid].seq = seqbase;
            peers[peeridx].ic[cid].lseqpU = seqbase + cnt_shifted;
            peers[peeridx].ic[cid].synched = 1;
        }
        acknack_if_needed(peeridx, cid, hdr & MSFLAG, tnow);
    }
    return (zmsize_t)(data - data0);
}

unsigned zeno_delivered, zeno_discarded;

static int handle_msdata_deliver(rid_t prid, zpsize_t paysz, const uint8_t *pay)
{
    uint8_t k;
    for (k = 0; k < MAX_SUBS; k++) {
        if (subs[k].rid == prid) {
            break;
        }
    }
    if (k == MAX_SUBS) {
        return 1;
    } else if (subs[k].xmitneed == 0 || subs[k].xmitneed <= xmitw_bytesavail(subs[k].oc)) {
        /* Do note that "xmitneed" had better include overhead! */
        subs[k].handler(prid, paysz, pay, subs[k].arg);
        return 1;
    } else {
        return 0;
    }
}

static zmsize_t handle_msdata(peeridx_t peeridx, zmsize_t sz, const uint8_t * restrict data, cid_t cid, ztime_t tnow)
{
    const uint8_t *data0 = data;
    uint8_t hdr;
    zpsize_t paysz;
    seq_t seq;
    rid_t rid, prid;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_seq(&sz, &data, &seq) ||
        !unpack_rid(&sz, &data, &rid)) {
        return 0;
    }
    if (!(hdr & MPFLAG)) {
        prid = rid;
    } else if (!unpack_rid(&sz, &data, &prid)) {
        return 0;
    }

    /* Attempt to "extract" payload -- we don't actually extract it but leave it in place to save memory
       and time.  If it is fully present, pay will still point to the payload size and all
       we need to redo is skip the VLE encoded length in what we know to be a valid buffer */
    const uint8_t * const pay = data;
    if (!unpack_vec(&sz, &data, 0, &paysz, NULL)) {
        return 0;
    }

    if (peers[peeridx].state != PEERST_ESTABLISHED) {
        /* Not accepting data from peers that we haven't (yet) established a connection with */
        return (zmsize_t)(data - data0);
    }

    if (!(hdr & MRFLAG)) {
        if (ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq)) {
            (void)handle_msdata_deliver(prid, paysz, skip_validated_vle(pay));
            ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
        }
    } else if (peers[peeridx].ic[cid].synched) {
        if (seq_le(peers[peeridx].ic[cid].lseqpU, seq)) {
            peers[peeridx].ic[cid].lseqpU = seq + SEQNUM_UNIT;
        }
        if (ic_may_deliver_seq(&peers[peeridx].ic[cid], hdr, seq)) {
            ZT(RELIABLE, ("handle_msdata peeridx %u cid %u seq %u deliver", peeridx, cid, seq >> SEQNUM_SHIFT));
            if (handle_msdata_deliver(prid, paysz, skip_validated_vle(pay))) {
                /* if failed to deliver, we must retry, which necessitates a retransmit and not updating the conduit state */
                ic_update_seq(&peers[peeridx].ic[cid], hdr, seq);
            }
            zeno_delivered++;
        } else {
            ZT(RELIABLE, ("handle_msdata peeridx %u cid %u seq %u != %u", peeridx, cid, seq >> SEQNUM_SHIFT, peers[peeridx].ic[cid].seq >> SEQNUM_SHIFT));
            zeno_discarded++;
        }
        acknack_if_needed(peeridx, cid, hdr & MSFLAG, tnow);
    }

    return (zmsize_t)(data - data0);
}

static void remove_acked_messages(struct out_conduit * restrict c, seq_t seq)
{
    ZT(RELIABLE, ("remove_acked_messages cid %u %p seq %u", c->cid, (void*)c, seq >> SEQNUM_SHIFT));

    if (seq_lt(c->seq, seq)) {
        /* Broker is ACKing samples we haven't even sent yet, use the opportunity to drain the
           transmit window */
        seq = c->seq;
    }

    if(seq_lt(c->seqbase, seq)) {
        /* Acking some samples, drop everything from seqbase up to but not including seq */
#ifndef NDEBUG
        seq_t cnt = (seq_t)(seq - c->seqbase) >> SEQNUM_SHIFT;
#endif
        while (c->seqbase != seq) {
            zmsize_t len;
            assert(cnt > 0);
#ifndef NDEBUG
            cnt--;
#endif
            c->seqbase += SEQNUM_UNIT;
            memcpy(&len, &c->rbuf[c->firstpos], sizeof(len));
            c->firstpos = xmitw_pos_add(c, c->firstpos, len + sizeof(zmsize_t));
        }
        assert(cnt == 0);
        assert(((c->firstpos + sizeof(zmsize_t)) % c->xmitw_bytes == c->pos) == (c->seq == c->seqbase));
    }

    if (oc_get_nsamples(c) == 0) {
        c->draining_window = 0;
    }
}

static zmsize_t handle_macknack(peeridx_t peeridx, zmsize_t sz, const uint8_t * restrict data, cid_t cid, ztime_t tnow)
{
    const uint8_t *data0 = data;
    struct out_conduit * const c = out_conduit_from_cid(peeridx, cid);
    seq_t seq, seq_ack;
    uint8_t hdr;
    uint32_t mask;
    if (!unpack_byte(&sz, &data, &hdr) ||
        !unpack_seq(&sz, &data, &seq)) {
        return 0;
    }
    if (!(hdr & MMFLAG)) {
        mask = 0;
    } else if (!unpack_vle32(&sz, &data, &mask)) {
        return 0;
    } else {
        /* Make the retransmit request for message SEQ implied by the use of an ACKNACK
         explicit in the mask (which means we won't retransmit SEQ + 32). */
        mask = (mask << 1) | 1;
    }
    if (peers[peeridx].state != PEERST_ESTABLISHED) {
        return (zmsize_t)(data - data0);
    }

    DO_FOR_UNICAST_OR_MULTICAST(cid, seq_ack = seq, seq_ack = minseqheap_update_seq(peeridx, seq, c->seqbase, &out_mconduits[cid].seqbase));
    remove_acked_messages(c, seq_ack);

    if (mask == 0) {
        /* Pure ACK - no need to do anything else */
        if (seq != c->seq) {
            ZT(RELIABLE, ("handle_macknack peeridx %u cid %u seq %u ACK but we have [%u,%u]", peeridx, cid, seq >> SEQNUM_SHIFT, c->seqbase >> SEQNUM_SHIFT, (c->seq >> SEQNUM_SHIFT)-1));
        } else {
            ZT(RELIABLE, ("handle_macknack peeridx %u cid %u seq %u ACK", peeridx, cid, seq >> SEQNUM_SHIFT));
        }
    } else if (seq_lt(seq, c->seqbase) || seq_le(c->seq, seq)) {
        /* If the broker ACKs stuff we have dropped already, or if it NACKs stuff we have not
           even sent yet, send a SYNCH without the S flag (i.e., let the broker decide what to
           do with it) */
        ZT(RELIABLE, ("handle_macknack peeridx %u cid %u %p seq %u mask %08x - [%u,%u] - send synch", peeridx, cid, (void*)c, seq >> SEQNUM_SHIFT, mask, c->seqbase >> SEQNUM_SHIFT, (c->seq >> SEQNUM_SHIFT)-1));
        pack_msynch(&c->addr, 0, c->cid, c->seqbase, oc_get_nsamples(c));
        pack_msend();
    } else if ((ztimediff_t)(tnow - c->last_rexmit) <= ROUNDTRIP_TIME_ESTIMATE && seq_lt(seq, c->last_rexmit_seq)) {
        ZT(RELIABLE, ("handle_macknack peeridx %u cid %u seq %u mask %08x - suppress", peeridx, cid, seq >> SEQNUM_SHIFT, mask));
    } else {
        /* Retransmits can always be performed because they do not require buffering new
           messages, all we need to do is push out the buffered messages.  We want the S bit
           set on the last of the retransmitted ones, so we "clear" outspos and then set it
           before pushing out that last sample. */
        uint16_t p;
        zmsize_t sz, outspos_tmp = OUTSPOS_UNSET;
#if MAX_PEERS != 0
        seq_t seqbase;
#endif
        ZT(RELIABLE, ("handle_macknack peeridx %u cid %u seq %u mask %08x", peeridx, cid, seq >> SEQNUM_SHIFT, mask));
#if MAX_PEERS == 0
        assert (seq == c->seqbase);
#endif
        /* Do not set the S bit on anything that happens to currently be in the output buffer,
           if that is of the same conduit as the one we are retransmitting on, as we by now know
           that we will retransmit at least one message and therefore will send a message with
           the S flag set and will schedule a SYNCH anyway */
        if (outc == c) {
            outspos = OUTSPOS_UNSET;
            outc = NULL;
        }
        /* Note: transmit window is formatted as SZ1 [MSG2 x SZ1] SZ2 [MSG2 x SZ2], &c,
           wrapping around at c->xmit_bytes.  */
        memcpy(&sz, &c->rbuf[c->firstpos], sizeof(sz));
        p = xmitw_pos_add(c, c->firstpos, sizeof(zmsize_t));
#if MAX_PEERS != 0
        seqbase = c->seqbase;
        while (seq_lt(seqbase, seq)) {
            p = xmitw_pos_add(c, p, sz);
            seqbase += SEQNUM_UNIT;
            memcpy(&sz, &c->rbuf[p], sizeof(sz));
            p = xmitw_pos_add(c, p, sizeof(zmsize_t));
        }
#endif
        while (mask && seq_lt(seq, c->seq)) {
            if ((mask & 1) == 0) {
                p = xmitw_pos_add(c, p, sz);
            } else {
                /* Out conduit is NULL so that the invariant that (outspos == OUTSPOS_UNSET) <=> 
                   (outc == NULL) is maintained, and also in consideration of the fact that keeping
                   track of the conduit and the position of the last reliable message is solely
                   for the purpose of setting the S flag and scheduling SYNCH messages.  Retransmits
                   are require none of that beyond what we do here locally anyway. */
                ZT(RELIABLE, ("handle_macknack   rx %u", seq >> SEQNUM_SHIFT));
                pack_reserve_mconduit(&c->addr, NULL, cid, sz);
                outspos_tmp = outp;
                while (sz--) {
                    pack1(c->rbuf[p]);
                    p = xmitw_pos_add(c, p, 1);
                }
            }
            mask >>= 1;
            seq += SEQNUM_UNIT;
            memcpy(&sz, &c->rbuf[p], sizeof(sz));
            p = xmitw_pos_add(c, p, sizeof(zmsize_t));
        }
        c->last_rexmit = tnow;
        c->last_rexmit_seq = seq;
        /* Asserting that seq <= c->seq is a somewhat nonsensical considering the guards for
           this block and the loop condition, but it clarifies the second assertion: if we got
           all the way to the most recent sample, then P should point to the first free
           position in the transmit window, a.k.a. c->pos.  */
        assert(seq_le(seq, c->seq));
        assert(seq != c->seq || p == c->pos);
        /* Since we must have sent at least one message, outspos_tmp must have been set.  Set
           the S flag in that final message. Also make sure we send a SYNCH not too long after
           (and so do all that pack_msend would otherwise have done for c). */
        assert(outspos_tmp != OUTSPOS_UNSET);
#if 1 /* setting the S bit is not the same as a SYNCH */
        outbuf[outspos_tmp] |= MSFLAG;
#else
        pack_msynch(&c->addr, MSFLAG, cid, c->seqbase, oc_get_nsamples(c));
        c->tsynch = tnow + MSYNCH_INTERVAL;
#endif
        pack_msend();
    }
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_mping(peeridx_t peeridx, zmsize_t sz, const uint8_t * restrict data)
{
    const uint8_t *data0 = data;
    uint16_t hash;
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_u16(&sz, &data, &hash)) {
        return 0;
    }
    pack_mpong(&peers[peeridx].oc.addr, hash);
    pack_msend();
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_mpong(peeridx_t peeridx, zmsize_t sz, const uint8_t * restrict data)
{
    const uint8_t *data0 = data;
    if (!unpack_skip(&sz, &data, 3)) {
        return 0;
    }
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_mkeepalive(peeridx_t * restrict peeridx, zmsize_t sz, const uint8_t * restrict data, ztime_t tnow)
{
    const uint8_t *data0 = data;
    zpsize_t idlen;
    uint8_t id[PEERID_SIZE];
    if (!unpack_skip(&sz, &data, 1) ||
        !unpack_vec(&sz, &data, sizeof(id), &idlen, id)) {
        return 0;
    }
    if (idlen == 0 || idlen > PEERID_SIZE) {
        reset_peer(*peeridx, tnow);
        return 0;
    }
    *peeridx = find_peeridx_by_id(*peeridx, idlen, id);
    return (zmsize_t)(data - data0);
}

static zmsize_t handle_mconduit(peeridx_t peeridx, zmsize_t sz, const uint8_t * restrict data, cid_t * restrict cid, ztime_t tnow)
{
    uint8_t hdr, cid_byte;
    if (!unpack_byte(&sz, &data, &hdr)) {
        return 0;
    } else if (hdr & MZFLAG) {
        *cid = 1 + ((hdr >> 5) & 0x3);
    } else if (!unpack_byte(&sz, &data, &cid_byte)) {
        return 0;
    } else if (cid_byte > MAX_CID_T) {
        reset_peer(peeridx, tnow);
        return 0;
    } else {
        *cid = (cid_t)cid_byte;
    }
    if (*cid >= N_IN_CONDUITS) {
        reset_peer(peeridx, tnow);
        return 0;
    }
    return 1;
}

static uint8_t *handle_packet(peeridx_t peeridx, zmsize_t sz, const uint8_t *data, ztime_t tnow)
{
    zmsize_t ncons;
    cid_t cid = 0;
    do {
        uint8_t kind = *data & MKIND;
        switch (kind) {
            case MSCOUT:     ncons = handle_mscout(peeridx, sz, data); break;
            case MHELLO:     ncons = handle_mhello(peeridx, sz, data, tnow); break;
            case MOPEN:      ncons = handle_mopen(&peeridx, sz, data, tnow); break;
            case MACCEPT:    ncons = handle_maccept(&peeridx, sz, data, tnow); break;
            case MCLOSE:     ncons = handle_mclose(&peeridx, sz, data, tnow); break;
            case MDECLARE:   ncons = handle_mdeclare(peeridx, sz, data, cid, tnow); break;
            case MSDATA:     ncons = handle_msdata(peeridx, sz, data, cid, tnow); break;
            case MPING:      ncons = handle_mping(peeridx, sz, data); break;
            case MPONG:      ncons = handle_mpong(peeridx, sz, data); break;
            case MSYNCH:     ncons = handle_msynch(peeridx, sz, data, cid, tnow); break;
            case MACKNACK:   ncons = handle_macknack(peeridx, sz, data, cid, tnow); break;
            case MKEEPALIVE: ncons = handle_mkeepalive(&peeridx, sz, data, tnow); break;
            case MCONDUIT:   ncons = handle_mconduit(peeridx, sz, data, &cid, tnow); break;
            default:         ncons = 0; break;
        }
        sz -= ncons;
        data += ncons;
    } while (ncons > 0 && sz > 0);
    return (uint8_t *)data;
}

/////////////////////////////////////////////////////////////////////////////////////

/* Not currently implementing cancelling subscriptions or stopping publishing, but once that is
   included, should clear pubs_to_declare if it so happens that the publication hasn't been
   declared yet by the time it is deleted */
uint8_t pubs_to_declare[(MAX_PUBS + 7) / 8];
uint8_t subs_to_declare[(MAX_SUBS + 7) / 8];
uint8_t must_commit;
uint8_t gcommitid;

void reset_pubs_to_declare(void)
{
    uint8_t i;
    for (i = 0; i < sizeof(pubs_to_declare); i++) {
        pubs_to_declare[i] = 0;
    }
    for (i = 0; i < MAX_PUBS; i++) {
        if (pubs[i].rid != 0) {
            bitset_set(pubs_to_declare, i);
        }
    }
}

void reset_subs_to_declare(void)
{
    uint8_t i;
    for (i = 0; i < sizeof(subs_to_declare); i++) {
        subs_to_declare[i] = 0;
    }
    for (i = 0; i < MAX_SUBS; i++) {
        if (subs[i].rid != 0) {
            bitset_set(subs_to_declare, i);
        }
    }
}

pubidx_t publish(rid_t rid, unsigned cid, int reliable)
{
    /* We will be publishing rid, dynamically allocating a "pubidx" for it and scheduling a
       DECLARE message that informs the broker of this.  By scheduling it, we avoid the having
       to send a reliable message when the transmit window is full.  */
    uint8_t i;
    for (i = 0; i < MAX_PUBS; i++) {
        if (pubs[i].rid == 0) {
            break;
        }
    }
    assert(i < MAX_PUBS);
    assert(!bitset_test(pubs_isrel, i));
    assert(cid < N_OUT_CONDUITS);
    pubs[i].rid = rid;
    /* FIXME: horrible hack ... */
    pubs[i].oc = out_conduit_from_cid(0, (cid_t)cid);
    if (reliable) {
        bitset_set(pubs_isrel, i);
    }
    bitset_set(pubs_to_declare, i);
    return (pubidx_t){i};
}

subidx_t subscribe(rid_t rid, zpsize_t xmitneed, subhandler_t handler, void *arg)
{
    uint8_t i;
    for (i = 0; i < MAX_SUBS; i++) {
        if (subs[i].rid == 0) {
            break;
        }
    }
    assert(i < MAX_SUBS);
    subs[i].rid = rid;
    subs[i].xmitneed = xmitneed;
    subs[i].handler = handler;
    subs[i].arg = arg;
    bitset_set(subs_to_declare, i);
    return (subidx_t){i};
}

int zeno_write(pubidx_t pubidx, zpsize_t sz, const void *data)
{
    /* returns 0 on failure and 1 on success; the only defined failure case is a full transmit
     window for reliable pulication while remote subscribers exist */
    struct out_conduit * const oc = pubs[pubidx.idx].oc;
    int relflag;
    assert(pubs[pubidx.idx].rid != 0);
#if 0 /* FIXME */
    if (!bitset_test(rsubs, pubidx.idx)) {
        /* success is assured if there are no subscribers */
        return 1;
    }
#endif

#if 1 /* FIXME: horrible hack alert */
    DO_FOR_UNICAST_OR_MULTICAST(oc->cid, {
        struct peer * const peer = (struct peer *)((char *)oc - offsetof(struct peer, oc));
        if (peer->state != PEERST_ESTABLISHED) {
            return 1;
        }
    }, {
        if (minseqheap_isempty(&out_mconduits[oc->cid].seqbase)) {
            return 1;
        }
    });
#endif

    relflag = bitset_test(pubs_isrel, pubidx.idx);

    if (oc->draining_window) {
        return !relflag;
    }

    if (!oc_pack_msdata(oc, relflag, pubs[pubidx.idx].rid, sz)) {
        /* for reliable, a full window means failure; for unreliable it is a non-issue */
        return !relflag;
    } else {
        oc_pack_msdata_payload(oc, relflag, sz, data);
        oc_pack_msdata_done(oc, relflag);
#if LATENCY_BUDGET == 0
        pack_msend();
#endif
        /* not flushing to allow packing */
        return 1;
    }
}

/////////////////////////////////////////////////////////////////////////////////////

void send_declares(ztime_t tnow)
{
    struct out_conduit * const oc = out_conduit_from_cid(0, 0);
    int first;

    /* Push out any pending declarations.  We keep trying until the transmit window has room.
       It may therefore be a while before the broker is informed of a new publication, and
       conceivably data could be published that will be lost.  */
    if ((first = bitset_findfirst(pubs_to_declare, MAX_PUBS)) >= 0) {
        if (oc_pack_mdeclare(oc, 1, WC_DPUB_SIZE)) {
            assert(pubs[first].rid != 0);
            pack_dpub(pubs[first].rid);
            oc_pack_mdeclare_done(oc);
            bitset_clear(pubs_to_declare, (unsigned)first);
            must_commit = 1;
        }
    } else if ((first = bitset_findfirst(subs_to_declare, MAX_SUBS)) >= 0) {
        if (oc_pack_mdeclare(oc, 1, WC_DSUB_SIZE)) {
            assert(subs[first].rid != 0);
            pack_dsub(subs[first].rid);
            oc_pack_mdeclare_done(oc);
            bitset_clear(subs_to_declare, (unsigned)first);
            must_commit = 1;
        }
    } else if (must_commit && oc_pack_mdeclare(oc, 1, WC_DCOMMIT_SIZE)) {
        pack_dcommit(gcommitid++);
        oc_pack_mdeclare_done(oc);
        pack_msend();
        must_commit = 0;
    }
}

int zeno_init(const struct zeno_config *config)
{
    /* FIXME: is there a way to make the transport pluggable at run-time without dynamic allocation? I don't think so, not with the MTU so important ... */
    if (config->idlen == 0 || config->idlen > PEERID_SIZE) {
        return -1;
    }
    if (config->n_mconduit_dstaddrs != N_OUT_MCONDUITS) {
        /* these must match */
        return -1;
    }
    if (config->n_mcgroups_join > MAX_MULTICAST_GROUPS) {
        /* but you don't have to join MAX groups */
        return -1;
    }

    ownid_union.v_nonconst.len = (zpsize_t)config->idlen;
    memcpy(ownid_union.v_nonconst.id, config->id, config->idlen);

    if (!transport_ops.octseq2addr(&scoutaddr, strlen(config->scoutaddr), config->scoutaddr)) {
        return -1;
    }

    init_globals();
    transport = transport_ops.new(config, &scoutaddr);
    if (transport == NULL) {
        return -1;
    }

#if MAX_MULTICAST_GROUPS > 0
    n_multicast_locators = (uint16_t)config->n_mcgroups_join;
    for (size_t i = 0; i < config->n_mcgroups_join; i++) {
        struct zeno_address *a = &multicast_locators[i];
        ZT(PEERDISC, ("joining %s ...", config->mcgroups_join[i]));
        if (!transport_ops.octseq2addr(a, strlen(config->mcgroups_join[i]), config->mcgroups_join[i])) {
            ZT(PEERDISC, ("invalid address %s", config->mcgroups_join[i]));
            return -1;
        }
        if (!transport_ops.join(transport, a)) {
            ZT(PEERDISC, ("joining %s failed", config->mcgroups_join[i]));
            return -1;
        }
    }
#endif

#if N_OUT_MCONDUITS > 0
    for (cid_t i = 0; i < N_OUT_MCONDUITS; i++) {
        ZT(PEERDISC, ("conduit %u -> %s", (unsigned)i, config->mconduit_dstaddrs[i]));
        if (!transport_ops.octseq2addr(&out_mconduits[i].oc.addr, strlen(config->mconduit_dstaddrs[i]), config->mconduit_dstaddrs[i])) {
            ZT(PEERDISC, ("invalid address %s", config->mconduit_dstaddrs[i]));
            return -1;
        }
    }
#endif
    return 0;
}

void zeno_loop_init(void)
{
    ztime_t tnow = zeno_time();
#if MAX_PEERS == 0
    peers[0].tlease = tnow - SCOUT_INTERVAL;
#else
    tnextscout = tnow - SCOUT_INTERVAL;
#endif
}

static void maybe_send_scout(ztime_t tnow)
{
#if MAX_PEERS == 0
    if (peers[0].state == PEERST_UNKNOWN && (ztimediff_t)(tnow - peers[0].tlease) >= 0) {
        peers[0].tlease = tnow + SCOUT_INTERVAL;
        pack_mscout(&scoutaddr);
        pack_msend();
    }
    /* FIXME: send keepalive if connected to a broker? */
#else
    if ((ztimediff_t)(tnow - tnextscout) >= 0) {
        tnextscout = tnow + SCOUT_INTERVAL;
        pack_mscout(&scoutaddr);
        if (npeers > 0) {
            /* Scout messages are ignored by peers that have established a session with the source
               of the scout message, and then there is also the issue of potentially changing source
               addresses ... so we combine the scout with a keepalive if we know some peers */
            pack_mkeepalive(&scoutaddr, &ownid);
        }
        pack_msend();
    }
#endif
}

#if TRANSPORT_MODE == TRANSPORT_PACKET
static int handle_input_packet(ztime_t tnow)
{
    zeno_address_t insrc;
    ssize_t recvret;
    char addrstr[TRANSPORT_ADDRSTRLEN];
    if ((recvret = transport_ops.recv(transport, inbuf, sizeof(inbuf), &insrc)) > 0) {
        peeridx_t peeridx, free_peeridx = PEERIDX_INVALID;
        for (peeridx = 0; peeridx < MAX_PEERS_1; peeridx++) {
            if (transport_ops.addr_eq(&insrc, &peers[peeridx].oc.addr)) {
                break;
            } else if (peers[peeridx].state == PEERST_UNKNOWN && free_peeridx == PEERIDX_INVALID) {
                free_peeridx = peeridx;
            }
        }
        (void)transport_ops.addr2string(addrstr, sizeof(addrstr), &insrc);
        if (peeridx == MAX_PEERS_1 && free_peeridx != PEERIDX_INVALID) {
            ZT(DEBUG, ("possible new peer %s @ %u", addrstr, free_peeridx));
            peeridx = free_peeridx;
            peers[peeridx].oc.addr = insrc;
        }
        if (peeridx < MAX_PEERS_1) {
            ZT(DEBUG, ("handle message from %s @ %u", addrstr, peeridx));
            if (peers[peeridx].state == PEERST_ESTABLISHED) {
                peers[peeridx].tlease = tnow;
            }
            handle_packet(peeridx, (zmsize_t)recvret, inbuf, tnow);
            /* peeridx need no longer be correct */
        } else {
            ZT(DEBUG, ("message from %s dropped: no available peeridx", addrstr));
        }
        return 1;
    } else if (recvret < 0) {
        assert(0);
        return 0;
    } else {
        return 0;
    }
}
#endif

#if TRANSPORT_MODE == TRANSPORT_STREAM
#if MAX_PEERS != 0
#error "stream currently only implemented for client mode"
#endif
static int handle_input_stream(ztime_t tnow)
{
    static ztime_t t_progress;
    uint8_t read_something = 0;
    zeno_address_t insrc;
    ssize_t recvret;
    recvret = transport_ops.recv(transport, inbuf + inp, sizeof(inbuf) - inp, &insrc);
    if (recvret > 0) {
        inp += recvret;
        read_something = 1;
    }
    if (recvret < 0) {
        assert(0);
        return 0;
    }
    if (inp == 0) {
        t_progress = tnow;
        return 0;
    } else {
        /* No point in repeatedly trying to decode the same incomplete data */
        int ret = 0;
        if (read_something) {
            uint8_t *datap = handle_packet(0, inp, inbuf, tnow);
            /* peeridx need no longer be correct */
            zmsize_t cons = (zmsize_t) (datap - inbuf);
            if (cons > 0) {
                t_progress = tnow;
                if (peers[0].state == PEERST_ESTABLISHED) {
                    /* any packet is considered proof of liveliness of the broker (the
                       state of course doesn't really change ...) */
                    peers[0].tlease = t_progress;
                }
                if (cons < inp) {
                    memmove(inbuf, datap, inp - cons);
                }
                inp -= cons;
                ret = 1;
            }
        }

        if (inp == sizeof(inbuf) || (inp > 0 && (ztimediff_t)(tnow - t_progress) > 300)) {
            /* No progress: discard whatever we have buffered and hope for the best. */
            inp = 0;
        }
        return ret;
    }
}
#endif

static void maybe_send_msync_oc(struct out_conduit * const oc, ztime_t tnow)
{
    if (oc->seq != oc->seqbase && (ztimediff_t)(tnow - oc->tsynch) >= 0) {
        oc->tsynch = tnow + MSYNCH_INTERVAL;
        pack_msynch(&oc->addr, MSFLAG, oc->cid, oc->seqbase, oc_get_nsamples(oc));
        pack_msend();
    }
}

void flush_output(ztime_t tnow)
{
    /* Flush any pending output if the latency budget has been exceeded */
#if LATENCY_BUDGET != 0 && LATENCY_BUDGET != LATENCY_BUDGET_INF
    if (outp > 0 && (ztimediff_t)(tnow - outdeadline) >= 0) {
        pack_msend();
    }
#endif
}

static void housekeeping(ztime_t tnow)
{
    maybe_send_scout(tnow);

    /* FIXME: obviously, this is far too big a waste of CPU time if MAX_PEERS is biggish */
    for (peeridx_t i = 0; i < MAX_PEERS_1; i++) {
        switch(peers[i].state) {
            case PEERST_UNKNOWN:
                break;
            case PEERST_ESTABLISHED:
                if ((ztimediff_t)(tnow - peers[i].tlease) > peers[i].lease_dur) {
                    ZT(PEERDISC, ("lease expired on peer @ %u", i));
                    pack_mclose(&peers[i].oc.addr, 0, &ownid);
                    pack_msend();
                    reset_peer(i, tnow);
                }
#if HAVE_UNICAST_CONDUIT
                maybe_send_msync_oc(&peers[i].oc, tnow);
#endif
                break;
            default:
                assert(peers[i].state >= PEERST_OPENING_MIN && peers[i].state <= PEERST_OPENING_MAX);
                if ((ztimediff_t)(tnow - peers[i].tlease) > OPEN_INTERVAL) {
                    if (peers[i].state == PEERST_OPENING_MAX) {
                        /* maximum number of attempts reached, forget it */
                        ZT(PEERDISC, ("giving up on attempting to establish a session with peer @ %u", i));
                        reset_peer(i, tnow);
                    } else {
                        ZT(PEERDISC, ("retry opening a session with peer @ %u", i));
                        peers[i].state++;
                        peers[i].tlease = tnow;
                        pack_mopen(&peers[i].oc.addr, SEQNUM_LEN, &ownid, lease_dur);
                        pack_msend();
                    }
                }
                break;
        }
    }

#if N_OUT_MCONDUITS > 0
    for (cid_t cid = 0; cid < N_OUT_MCONDUITS; cid++) {
        struct out_mconduit * const mc = &out_mconduits[cid];

#if 0 /* now not storing anything by checking #peers */
        /* FIXME: should not even store the data temporarily if there are no peers */
        if (minseqheap_isempty(&mc->seqbase)) {
            remove_acked_messages(&mc->oc, mc->oc.seq);
        }
#endif

        maybe_send_msync_oc(&mc->oc, tnow);
    }
#endif

    flush_output(tnow);
}

ztime_t zeno_loop(void)
{
    ztime_t tnow = zeno_time();
    int r;

    do {
#if TRANSPORT_MODE == TRANSPORT_PACKET
        r = handle_input_packet(tnow);
#elif TRANSPORT_MODE == TRANSPORT_STREAM
        r = handle_input_stream(tnow);
#else
#error "TRANSPORT_MODE not handled"
#endif
    } while (r);

    housekeeping(tnow);

    return tnow + 1; /* FIXME: need to keep track of next event */
}

void zeno_wait_input(ztimediff_t timeout)
{
    (void)zeno_loop();
    transport_ops.wait(transport, timeout);
}
