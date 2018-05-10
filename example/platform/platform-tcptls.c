#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>

#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include "platform-tcptls.h"
#include "zhe-assert.h"
#include "zhe-tracing.h"
#include "zhe-config-deriv.h"
#include "zhe.h"
#include "zhe-bitset.h"
#include "zhe-mscout.h"

#define MAX_PINGADDRS MAX_PEERS_1
#define PINGADDRIDX_INVALID PEERIDX_INVALID
typedef peeridx_t pingaddridx_t;

struct buf {
    zhe_msgsize_t pos;
    zhe_msgsize_t lim;
    uint8_t buf[TRANSPORT_MTU + 2]; /* 2 bytes extra for framing */
};

enum conn_state {
    CS_CLOSED,
    CS_TCPCONNECT,
    CS_WAITDATA,
    CS_LIVE
};
static inline bool state_allows_receive(enum conn_state s) { return s >= CS_WAITDATA; }

struct conn {
    enum conn_state state;
    connid_t id;
    int s;
    bool inframed, outframed;
    bool datawaiting; /* for framed input */
    struct buf in, out;
    zhe_time_t ttent; /* FIXME: only when TENTATIVE; but in that case we don't need out I think, so a union might be nicer */
    pingaddridx_t pingprogeny; /* index in tcp->pingaddrs */
};

struct tcp {
    int servsock;
    uint16_t port;
    connidx_t cursor;
    struct conn conns[MAX_CONNECTIONS];

    DECL_BITSET(pingmask, MAX_PINGADDRS);
    DECL_BITSET(pingthrottle, MAX_PINGADDRS);
    pingaddridx_t npingaddrs;
    struct sockaddr_in pingaddrs[MAX_PINGADDRS];
    zhe_time_t pingtime[MAX_PINGADDRS];
};

static int zhe_platform_string2addr1(struct zhe_address * restrict addr, const char * restrict str, size_t len);

static struct tcp gtcp;
static struct timespec toffset;

zhe_time_t zhe_platform_time(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (zhe_time_t)((t.tv_sec - toffset.tv_sec) * (1000000000 / ZHE_TIMEBASE) + t.tv_nsec / ZHE_TIMEBASE);
}

void zhe_platform_trace(struct zhe_platform *pf, const char *fmt, ...)
{
    uint32_t t = (uint32_t)zhe_platform_time();
    va_list ap;
    va_start(ap, fmt);
    flockfile(stdout);
    printf("%4"PRIu32".%03"PRIu32" ", ZTIME_TO_SECu32(t), ZTIME_TO_MSECu32(t));
    (void)vprintf(fmt, ap);
    printf("\n");
    funlockfile(stdout);
    va_end(ap);
}

static void set_nonblock(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    flags |= O_NONBLOCK;
    (void)fcntl(sock, F_SETFL, flags);
}

static void set_nosigpipe(int sock)
{
    int set = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));
}

struct zhe_platform *zhe_platform_new(uint16_t port, const char *pingaddrs)
{
    struct tcp * const tcp = &gtcp;

    (void)clock_gettime(CLOCK_MONOTONIC, &toffset);
    toffset.tv_sec -= toffset.tv_sec % 10000;

    /* if port is set, listen on port; else only initiate connections */
    for (connidx_t i = 0; i < MAX_CONNECTIONS; i++) {
        zhe_address_t id;
        id.u.s.idx = i;
        id.u.s.serial = 0;
        tcp->conns[i].state = CS_CLOSED;
        tcp->conns[i].id = id.u.id;
    }

    tcp->cursor = 0;
    tcp->port = htons(port);

    memset(tcp->pingmask, 0, sizeof(tcp->pingmask));
    memset(tcp->pingthrottle, 0, sizeof(tcp->pingthrottle));
    memset(tcp->pingaddrs, 0, sizeof(tcp->pingaddrs));
    tcp->npingaddrs = 0;
    if (pingaddrs) {
        while (tcp->npingaddrs < MAX_PINGADDRS && *pingaddrs) {
            const char *comma = strchr(pingaddrs, ',');
            zhe_address_t tmp;
            size_t len, adv;
            int ok;
            if (comma == NULL) {
                len = adv = strlen(pingaddrs);
                ok = zhe_platform_string2addr1(&tmp, pingaddrs, len);
            } else {
                len = (size_t)(comma - pingaddrs);
                adv = len+1;
                ok = zhe_platform_string2addr1(&tmp, pingaddrs, len);
            }
            if (!ok) {
                ZT(ERROR, "zhe_platform_new: invalid ping address %*.*s", (int)len, (int)len, pingaddrs);
                return NULL;
            }
            pingaddrs += adv;
            tcp->pingaddrs[tcp->npingaddrs] = tmp.u.a;
            zhe_bitset_set(tcp->pingmask, tcp->npingaddrs);
            tcp->npingaddrs++;
        }
    }

    if (port == 0) {
        tcp->servsock = -1;
        if (tcp->npingaddrs == 0) {
            ZT(ERROR, "zhe_platform_new: no addresses to ping and no port to listen on");
            return NULL;
        }
    } else {
        struct sockaddr_in addr;
        if ((tcp->servsock = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
            return NULL;
        }
        set_nonblock(tcp->servsock);
        int val = 1;
        setsockopt(tcp->servsock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = tcp->port;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(tcp->servsock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
            perror("bind");
            close(tcp->servsock);
            return NULL;
        }
        if (listen(tcp->servsock, 5) == -1) {
            perror("listen");
            close(tcp->servsock);
            return NULL;
        }
    }
    return (struct zhe_platform *)tcp;
}

static char *uint16_to_string(char * restrict str, uint16_t val)
{
    if (val == 0) {
        *str++ = '0';
        *str = 0;
        return str;
    } else {
        char *end;
        if (val >= 10000)     { str += 5; }
        else if (val >= 1000) { str += 4; }
        else if (val >= 100)  { str += 3; }
        else if (val >= 10)   { str += 2; }
        else                  { str += 1; }
        end = str;
        *str-- = 0;
        while (val != 0) {
            *str-- = '0' + (val % 10);
            val /= 10;
        }
        return end;
    }
}

static size_t addr2string1(const struct tcp *tcp, char * restrict str, const zhe_address_t * restrict addr)
{
    struct sockaddr_in stip;
    const struct sockaddr_in *ip = NULL;
    char *p;
    strcpy(str, "tcp/");
    p = str + 4;
    switch (addr->kind) {
        case ZHE_AK_IP:
            ip = &addr->u.a;
            break;
        case ZHE_AK_CONN: {
            struct conn const * const conn = &tcp->conns[addr->u.s.idx];
            if (conn->state == CS_LIVE && conn->id == addr->u.id) {
                socklen_t len = sizeof(stip);
                getpeername(conn->s, (struct sockaddr *)&stip, &len);
                ip = &stip;
            }
            break;
        }
    }
    if (ip != NULL) {
        (void)inet_ntop(AF_INET, &ip->sin_addr.s_addr, p, INET_ADDRSTRLEN);
        p += strlen(p);
        *p++ = ':';
        p = uint16_to_string(p, ntohs(ip->sin_port));
    }
    return (size_t)(p - str);
}

size_t zhe_platform_addr2string(const struct zhe_platform *pf, char * restrict str, size_t size, const zhe_address_t * restrict addr)
{
    zhe_assert(size > 0);
    if (size >= TRANSPORT_ADDRSTRLEN) {
        return addr2string1((const struct tcp *)pf, str, addr);
    } else {
        char tmp[TRANSPORT_ADDRSTRLEN];
        size_t n = addr2string1((const struct tcp *)pf, tmp, addr);
        if (n >= size) {
            n = size - 1;
        }
        memcpy(str, tmp, n);
        str[n] = 0;
        return n;
    }
}

static int zhe_platform_string2addr1(struct zhe_address * restrict addr, const char * restrict str, size_t len)
{
    char copy[4*(4+1) + 1+5 + 1]; /* IP:PORT, hex/octal components in IP require potentially 4 characters */
    char *portstr, *portend;
    unsigned long port;
    memset(addr, 0, sizeof(*addr));
    if (len >= sizeof(copy)) {
        return 0;
    }
    if (strncmp(str, "tcp/", 4) == 0) {
        str += 4;
    }
    memcpy(copy, str, len);
    copy[len] = 0;
    if ((portstr = strchr(copy, ':')) != NULL) {
        *portstr++ = 0;
    }
    addr->kind = ZHE_AK_IP;
    addr->u.a.sin_family = AF_INET;
    if (inet_pton(AF_INET, copy, &addr->u.a.sin_addr.s_addr) != 1) {
        return 0;
    }
    if (portstr != NULL) {
        port = strtoul(portstr, &portend, 10);
        if (*portstr == 0 || *portend != 0 || port > 65535) {
            return 0;
        }
        addr->u.a.sin_port = htons((uint16_t)port);
    } else {
        return 0;
    }
    return 1;
}

int zhe_platform_string2addr(const struct zhe_platform *pf, struct zhe_address * restrict addr, const char * restrict str)
{
    return zhe_platform_string2addr1(addr, str, strlen(str));
}

int zhe_platform_join(const struct zhe_platform *pf, const struct zhe_address *addr)
{
    return 1;
}

bool zhe_platform_needs_keepalive(struct zhe_platform *pf)
{
    return false;
}

static void make_closed(struct conn *conn)
{
    close(conn->s);
    conn->s = -1;
    conn->state = CS_CLOSED;
}

static void make_live(struct conn *conn)
{
    zhe_address_t newid;
    conn->state = CS_LIVE;
    newid.u.id = conn->id;
    newid.u.s.serial++;
    conn->id = newid.u.id;
}

static void make_waitdata(struct conn *conn)
{
    conn->state = CS_WAITDATA;
    conn->inframed = true;
    conn->outframed = true;
    conn->datawaiting = false;
    conn->in.lim = conn->in.pos = 0;
    conn->out.lim = conn->out.pos = 0;
}

static size_t encvle14(uint8_t *dst, size_t val)
{
    zhe_assert(val < 16384); /* max 2 bytes */
    if (val <= 0x7f) {
        dst[0] = (uint8_t)val;
        return 1;
    } else {
        dst[0] = (uint8_t)(0x80 | (val & 0x7f));
        dst[1] = (uint8_t)(val >> 7);
        return 2;
    }
}

static int decvle14(const uint8_t *src, size_t sz, size_t *val)
{
    if (sz < 1) {
        return -1;
    } else if (src[0] <= 0x7f) {
        *val = src[0];
        return 1;
    } else if (sz == 1) {
        return 0;
    } else if (src[1] <= 0x7f) {
        *val = (src[1] << 7) | (src[0] & 0x7f);
        return 2;
    } else {
        return -1;
    }
}

static int zhe_platform_send_conn(struct tcp * const tcp, const void * restrict buf, size_t size, const zhe_address_t * restrict dst)
{
    struct conn * const conn = &tcp->conns[dst->u.s.idx];
    ssize_t ret;
    zhe_assert(size <= TRANSPORT_MTU);
    if (conn->state != CS_LIVE || dst->u.id != conn->id) {
        return SENDRECV_HANGUP;
    }
    zhe_assert(conn->s != -1);
    zhe_assert(conn->out.pos < conn->out.lim || (conn->out.pos == 0 && conn->out.lim == 0));
    if (conn->out.pos < conn->out.lim) {
        ret = write(conn->s, conn->out.buf + conn->out.pos, conn->out.lim - conn->out.pos);
        if (ret > 0) {
            conn->out.pos += (zhe_msgsize_t)ret;
            if (conn->out.pos == conn->out.lim) {
                conn->out.pos = conn->out.lim = 0;
            }
        }
    }
    if (conn->out.lim == 0) {
        uint8_t vlelen[2];
        struct iovec iov[2];
        int iovcnt = 0;
        size_t xsize;
        if (!conn->outframed) {
            xsize = 0;
        } else {
            iov[iovcnt].iov_base = vlelen;
            iov[iovcnt++].iov_len = xsize = encvle14(vlelen, size);
            ZT(TRANSPORT, "send %d frame %u", conn->s, (unsigned)size);
        }
        iov[iovcnt].iov_base = (void *)buf;
        iov[iovcnt++].iov_len = size;
        xsize += size;
        /* write length + messages and retain any leftovers */
        ret = writev(conn->s, iov, iovcnt);
        if (ret > 0 && (size_t)ret < xsize) {
            size_t nwr = (size_t)ret;
            int i = 0;
            while (nwr >= iov[i].iov_len) {
                nwr -= iov[i++].iov_len;
                zhe_assert(i < iovcnt);
            }
            zhe_msgsize_t rem = (zhe_msgsize_t)(iov[i].iov_len - nwr);
            memcpy(conn->out.buf + conn->out.lim, (const uint8_t *)iov[i].iov_base + nwr, rem);
            conn->out.lim += rem;
            for (++i; i < iovcnt; i++) {
                memcpy(conn->out.buf + conn->out.lim, iov[i].iov_base, iov[i].iov_len);
                conn->out.lim += iov[i].iov_len;
            }
        }
    } else {
        return 0;
    }
    if (ret > 0) {
        return (int)ret;
    } else if (ret == 0 || errno == EPIPE) {
        ZT(TRANSPORT, "sock %d eof on write", conn->s);
        make_closed(conn);
        return SENDRECV_HANGUP;
    } else if (errno == EAGAIN || errno == ENOBUFS || errno == EWOULDBLOCK) {
        return 0;
    } else {
        return SENDRECV_ERROR;
    }
}

int zhe_platform_send(struct zhe_platform *pf, const void * restrict buf, size_t size, const zhe_address_t * restrict dst)
{
    /* FIXME: generic code currently sends scouts & keepalives using IP addresses - we fake it by sending them over all live connections */
    struct tcp *tcp = (struct tcp *)pf;
    if (dst->kind == ZHE_AK_CONN) {
        return zhe_platform_send_conn(tcp, buf, size, dst);
    } else {
        zhe_address_t dst1;
        dst1.kind = ZHE_AK_CONN;
        for (connidx_t i = 0; i < MAX_CONNECTIONS; i++) {
            struct conn * const conn = &tcp->conns[i];
            if (conn->state == CS_LIVE) {
                dst1.u.id = conn->id;
                (void)zhe_platform_send_conn(tcp, buf, size, &dst1);
            }
        }
        return 0;
    }
}

int zhe_platform_recv(struct zhe_platform *pf, zhe_recvbuf_t *buf, zhe_address_t * restrict src)
{
    struct tcp *tcp = (struct tcp *)pf;
    int news;

    /* FIXME: should not poll like this ... */
    for (connidx_t i = tcp->cursor; i < MAX_CONNECTIONS; i++) {
        struct conn * const conn = &tcp->conns[i];
        ssize_t ret;
        zhe_assert(conn->in.pos < conn->in.lim || (conn->in.pos == 0 && conn->in.lim == 0));
        if (!state_allows_receive(conn->state)) {
            continue;
        }
        ret = read(conn->s, conn->in.buf + conn->in.lim, sizeof(conn->in.buf) - conn->in.lim);
        if (ret > 0 || (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK) && conn->datawaiting)) {
            if (conn->state == CS_WAITDATA) {
                ZT(TRANSPORT, "sock %d now live", conn->s);
                make_live(conn);
            }
            if (ret > 0) {
                conn->in.lim += (zhe_msgsize_t)ret;
            }
            src->kind = ZHE_AK_CONN;
            src->u.id = conn->id;
            tcp->cursor = i+1;
            if (!conn->inframed) {
                buf->buf = conn->in.buf + conn->in.pos;
                return (int)(conn->in.lim - conn->in.pos);
            } else {
                size_t len;
                int lenlen = decvle14(conn->in.buf + conn->in.pos, conn->in.lim - conn->in.pos, &len);
                if (lenlen < 0) {
                    ZT(TRANSPORT, "sock %d framing error", conn->s);
                    make_closed(conn);
                    return (conn->state == CS_LIVE ? SENDRECV_HANGUP : 0);
                } else if (lenlen == 0) {
                    ZT(TRANSPORT, "sock %d short read of frame length", conn->s);
                    /* short read of length */
                    conn->datawaiting = false;
                    return 0;
                } else if (len == 0) {
                    ZT(TRANSPORT, "sock %d switch input to non-framed", conn->s);
                    /* "infinite" frame switches from framed to non-framed */
                    conn->inframed = false;
                    conn->datawaiting = false;
                    conn->in.pos += lenlen;
                    buf->buf = conn->in.buf + conn->in.pos;
                    return (int)(conn->in.lim - conn->in.pos);
                } else if (conn->in.lim - conn->in.pos < lenlen + len) {
                    ZT(TRANSPORT, "sock %d incomplete frame (%u of %u)", conn->s, (unsigned)len, conn->in.lim - conn->in.pos - lenlen);
                    conn->datawaiting = false;
                    /* incomplete frame - try again later */
                    return 0;
                } else {
                    conn->datawaiting = true; /* there may be, we aren't quite sure! */
                    conn->in.pos += lenlen;
                    buf->buf = conn->in.buf + conn->in.pos;
                    ZT(TRANSPORT, "sock %d frame (%u of %u)", conn->s, (unsigned)len, conn->in.lim - conn->in.pos);
                    return (int)len;
                }
            }
        } else if (ret == 0) {
            ZT(TRANSPORT, "sock %d eof", conn->s);
            src->kind = ZHE_AK_CONN;
            src->u.id = conn->id;
            make_closed(conn);
            tcp->cursor = i+1;
            /* only inform the core code of the closing of the connection if we previously informed it of its existence */
            return (conn->state == CS_LIVE ? SENDRECV_HANGUP : 0);
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return SENDRECV_ERROR;
        }
    }

    /* Round-robin with an attempt at accepting a connection as the last step in the cycle */
    tcp->cursor = 0;
    if (tcp->servsock != -1) {
        struct sockaddr_in *addr;
        socklen_t addrlen;
        connidx_t idx;
        for (idx = 0; idx < MAX_CONNECTIONS; idx++) {
            if (tcp->conns[idx].state == CS_CLOSED) {
                break;
            }
        }
        if (idx < MAX_CONNECTIONS && (news = accept(tcp->servsock, (struct sockaddr *)&addr, &addrlen)) >= 0) {
            struct conn * const conn = &tcp->conns[idx];
            ZT(TRANSPORT, "sock %d accepted connection", news);
            conn->s = news;
            conn->ttent = zhe_platform_time(); /* FIXME: I kinda try to avoid calling time(), but then, this is platform code */
            conn->pingprogeny = PINGADDRIDX_INVALID;
            make_waitdata(conn);
            if (!conn->outframed) {
                const uint8_t noframe = 0;
                if (write(conn->s, &noframe, sizeof(noframe)) != sizeof(noframe)) {
                    ZT(TRANSPORT, "sock %d failed to send byte that disables framing", conn->s);
                    make_closed(conn);
                }
            }
        }
    }
    return 0;
}

int zhe_platform_advance(struct zhe_platform *pf, const zhe_address_t * restrict src, int cnt)
{
    struct tcp *tcp = (struct tcp *)pf;
    zhe_assert(src->kind == ZHE_AK_CONN);
    struct conn * const conn = &tcp->conns[src->u.s.idx];
    if (conn->state == CS_LIVE && conn->id == src->u.id && cnt > 0) {
        zhe_assert(cnt <= conn->in.lim - conn->in.pos);
        const zhe_msgsize_t pos1 = conn->in.pos + (zhe_msgsize_t)cnt;
        const zhe_msgsize_t rem = conn->in.lim - pos1;
        memmove(conn->in.buf, conn->in.buf + pos1, rem);
        conn->in.lim = rem;
        conn->in.pos = 0;
        conn->datawaiting = (conn->inframed && rem > 0);
    }
    return 0;
}

static void try_connect(struct tcp *tcp, connidx_t clidx, pingaddridx_t pidx, zhe_time_t tnow)
{
    struct conn * const conn = &tcp->conns[clidx];
    if ((conn->s = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        return;
    }
    set_nonblock(conn->s);
    set_nosigpipe(conn->s);
    if (connect(conn->s, (struct sockaddr *)&tcp->pingaddrs[pidx], sizeof(tcp->pingaddrs[pidx])) == -1) {
        if (errno != EINPROGRESS) {
            close(conn->s);
            return;
        }
        ZT(TRANSPORT, "sock %d connecting", conn->s);
        conn->state = CS_TCPCONNECT;
        conn->ttent = tnow;
        conn->pingprogeny = pidx;
    } else {
        ZT(TRANSPORT, "sock %d connected instantaneously", conn->s);
        conn->state = CS_TCPCONNECT;
        conn->ttent = tnow;
        conn->pingprogeny = pidx;
        make_waitdata(conn);
    }
}

void zhe_platform_housekeeping(struct zhe_platform *pf, zhe_time_t tnow)
{
    struct tcp *tcp = (struct tcp *)pf;
    connidx_t clidx = MAX_CONNECTIONS;
    DECL_BITSET(pingmask, MAX_PINGADDRS);

    {
        bitset_iter_t it;
        unsigned idx;
        if (zhe_bitset_iter_first(&it, tcp->pingthrottle, MAX_PINGADDRS, &idx)) {
            do {
                if ((zhe_timediff_t)(tnow - tcp->pingtime[idx]) > ZHE_TCPOPEN_THROTTLE) {
                    zhe_bitset_clear(tcp->pingthrottle, idx);
                }
            } while (zhe_bitset_iter_next(&it, &idx));
        }
    }

    memcpy(pingmask, tcp->pingmask, sizeof(pingmask));
    zhe_bitset_andnot(pingmask, tcp->pingthrottle, MAX_PINGADDRS);
    for (connidx_t i = 0; i < MAX_CONNECTIONS; i++) {
        struct conn * const conn = &tcp->conns[i];
        if (conn->state == CS_CLOSED) {
            clidx = i;
        } else {
            if (conn->pingprogeny != PINGADDRIDX_INVALID) {
                zhe_bitset_clear(pingmask, conn->pingprogeny);
            }
            if (conn->state < CS_LIVE) {
                if ((zhe_timediff_t)(tnow - tcp->conns[i].ttent) > ZHE_TCPOPEN_MAXWAIT) {
                    ZT(TRANSPORT, "sock %d did not make it to live", conn->s);
                    make_closed(conn);
                }
                switch (conn->state) {
                    case CS_CLOSED:
                        /* it may have been closed just now */
                        break;
                    case CS_LIVE:
                        zhe_assert(0);
                    case CS_TCPCONNECT: {
                        fd_set ws; FD_ZERO(&ws); FD_SET(conn->s, &ws);
                        if (select(conn->s+1, NULL, &ws, NULL, NULL) >= 0 && FD_ISSET(conn->s, &ws)) {
                            int err;
                            socklen_t errlen = sizeof(err);
                            getsockopt(conn->s, SOL_SOCKET, SO_ERROR, &err, &errlen);
                            if (err == 0) {
                                ZT(TRANSPORT, "sock %d connection established", conn->s);
                                make_waitdata(conn);
                                /* FIXME: there needs to be a bit more cooperation on the sending of SCOUT messages */
                                uint8_t scoutmagic[MSCOUT_MAX_SIZE + 1];
                                zhe_msgsize_t sz = zhe_make_mscout(scoutmagic + 1, sizeof(scoutmagic) - 1);
                                scoutmagic[0] = conn->outframed ? (uint8_t)sz : 0;
                                if (write(conn->s, scoutmagic, sz+1) != sz+1) {
                                    ZT(TRANSPORT, "sock %d initial write failed", conn->s);
                                    make_closed(conn);
                                }
                            } else {
                                ZT(TRANSPORT, "sock %d connect failed: %d %s", conn->s, err, strerror(err));
                                zhe_bitset_set(tcp->pingthrottle, conn->pingprogeny);
                                tcp->pingtime[conn->pingprogeny] = tnow;
                                make_closed(conn);
                            }
                        }
                        break;
                    }
                    case CS_WAITDATA:
                        break;
                }
            }
        }
    }
    int idx;
    if (clidx != MAX_CONNECTIONS && (idx = zhe_bitset_findfirst(pingmask, MAX_PINGADDRS)) >= 0) {
        try_connect(tcp, clidx, (pingaddridx_t)idx, tnow);
    }
}

void zhe_platform_close_session(struct zhe_platform *pf, const struct zhe_address * restrict addr)
{
    struct tcp *tcp = (struct tcp *)pf;
    assert(addr->kind == ZHE_AK_CONN);
    struct conn * const conn = &tcp->conns[addr->u.s.idx];
    if (conn->state != CS_LIVE || addr->u.id != conn->id) {
        /* FIXME: should check state in all cases where the id gets checked */
        return;
    }
    make_closed(conn);
}

int zhe_platform_addr_eq(const struct zhe_address *a, const struct zhe_address *b)
{
    if (a->kind != b->kind) {
        return 0;
    } else {
        int eq = 0;
        switch (a->kind) {
            case ZHE_AK_IP:
                eq = a->u.a.sin_addr.s_addr == b->u.a.sin_addr.s_addr && a->u.a.sin_port == b->u.a.sin_port;
                break;
            case ZHE_AK_CONN:
                eq = a->u.id == b->u.id;
                break;
        }
        return eq;
    }
}

int zhe_platform_wait(const struct zhe_platform *pf, zhe_timediff_t timeout)
{
    struct tcp * const tcp = (struct tcp *)pf;
    fd_set rs, ws;
    struct timeval tv;
    int maxfd;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    if (tcp->servsock != -1) {
        FD_SET(tcp->servsock, &rs);
        maxfd = tcp->servsock;
    } else {
        maxfd = -1;
    }
    for (connidx_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (state_allows_receive(tcp->conns[i].state)) {
            if (tcp->conns[i].datawaiting) {
                /* if we have reason to believe there is data ready for processing, there is nothing to be gained from calling select() */
                return 1;
            }
            const int s = tcp->conns[i].s;
            FD_SET(s, &rs);
            if (s > maxfd) { maxfd = s; }
        } else if (tcp->conns[i].state == CS_TCPCONNECT) {
            const int s = tcp->conns[i].s;
            FD_SET(s, &ws);
            if (s > maxfd) { maxfd = s; }
        }
    }
    if (timeout < 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
    } else {
        tv.tv_sec = ZTIME_TO_SECu32(timeout);
        tv.tv_usec = 1000 * ZTIME_TO_MSECu32(timeout);
    }
    return select(maxfd+1, &rs, &ws, NULL, &tv) > 0;
}
