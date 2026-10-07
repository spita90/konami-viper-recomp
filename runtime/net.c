/*
 * Network link: the K056230 LANC ring of linked cabinets, carried over UDP.
 *
 * The games' link driver (GTI Club 2: task 0x3db54) is a shared-memory broadcast: the LANC RAM
 * holds one 1 KB slot per cabinet (slot n = NETWORK ID n+1), the node writes its own slot, and
 * each completed ring cycle hands the CPU all four. The game never waits for the network: every
 * frame it sends its packet ("NWK", a sequence number, its mode and its car) and reads the
 * newest one of each peer, accepting a sequence that repeats for up to 4 frames or jumps by 1-4.
 * So a constant latency is harmless; what must be smoothed is jitter.
 *
 * Each LANC cycle (hw.c) calls net_cycle(): it sends the own slot and fills the peers' slots from
 * a small playout queue per peer (one packet per cycle, as the sender produces them, after the
 * queue has filled to the playout buffer). Polled on the guest thread, non-blocking: no thread.
 *
 * A star: the host is NETWORK ID 1 and hands out IDs 2-4; the others talk to the host only, and
 * the host relays each packet to its other members, so only the host needs a reachable port.
 *  - JOIN (token 0): a node asks for a free ID; the host reserves one and answers WELCOME (ID,
 *    token) or FULL. The node takes that ID live, in the attract mode, the way the game's own
 *    TEST MODE item does (profile network.hot, enhanced.c): no restart, joining or leaving.
 *  - JOIN (token): the node, now with its ID, takes its place; the host knows it by the token
 *    and from then on takes its data. A reserved ID waits 60 s for its node.
 *  - START (host to all, from the lobby): the members' games press START, so they join the race
 *    the host starts. The host repeats it every 0.5 s for 10 s to each member that was ready when
 *    it started and whose data says it is still waiting in its lobby (flags bit 0; one that joins
 *    later waits for the next race), so a lost datagram or a START the game ignored
 *    is simply repeated; a member takes at most one every 2 s, and keeps one pending for 10 s.
 *  - FIND (broadcast on the local network, with the public address of the code) / HERE (the host's
 *    answer, with its public address if it knows it): a node in the host's own home cannot always
 *    reach it through the public address (routers without hairpin NAT), so while it asks to join
 *    it also looks for the host next to it, and talks to its local address if it answers.
 *  - LEAVE frees the ID; CLOSE (host to all) ends the session. A member silent for 30 s loses its
 *    ID; a host silent for 30 s ends the session (a dropped connection may come back before).
 *    When the session is over (left, closed, lost) the node takes ID 1 back, live, once it is in
 *    the attract mode.
 *
 * The host's port is opened on its router automatically (portmap.c), which also tells the public
 * address; the session code is that address and port (net_code_encode: 7 characters for the
 * default port), so joining takes one code and no server.
 *
 * The games stop a linked race with NETWORK ERROR as soon as a node is missing, which on a
 * cable was a fault but on the Internet is a dropped Wi-Fi packet burst. So a drop is concealed:
 * the game is given the node's last packet again, with its sequence byte (+3, which we rewrite
 * for every node) advanced every 2 game frames, so the node looks alive and its car stands still;
 * when its packets come back the sequence just goes on. A node that is gone for good stays such a
 * frozen ghost until the game is back in the attract mode, where a node may leave. If the game
 * stops the link anyway (NETWORK ERROR: the error mode in its own packet, profile
 * network.ghost.error_mode, or no LANC cycle for 5 s), the enhanced mode restarts it, the one
 * restart left in link play. RT_NET_NOCONCEAL=1 (tests)
 * turns the concealment off: a node is dropped as soon as its packets stop.
 *
 * Datagrams: data "VPL1", sender ID, flags (bit 0: the game is in the attract mode, ready for a
 * START from the lobby), 16-bit length, 32-bit sequence, then the slot with zero
 * runs coded as 0x00 count (1-255); control "VPLC", type, ID, 0, 0, 32-bit token. Big-endian.
 *
 * Tests: RT_NET_DELAY="ms,jitter_ms,loss%" holds the outgoing datagrams for ms plus a uniform
 * 0..jitter_ms (so they can also arrive out of order) and drops loss% of them;
 * RT_NET_OUTAGE="t,s[,t,s...]" drops every datagram, both ways, for s seconds from emulated time
 * t (a network drop); RT_NET_WATCH=addr logs every change of the two 32-bit guest words at addr
 * (e.g. a game's link counters: GTI Club 2 0x859070).
 * The default UDP port, NET_DEFAULT_PORT 24700, is in the IANA-unassigned block 24681-24726.
 */
#include "runtime.h"
#include "game_config.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
#define close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define SLOT 0x400
#define NODES 4
#define QUEUE 16
#define SILENT_CYCLES 1800          /* ~30 s without packets: a member or the host is gone (a LEAVE is at once) */
#define RESERVE_CYCLES 3600         /* ~60 s for a welcomed node to restart and come back */
#define JOIN_EVERY 30               /* JOIN repeated every ~0.5 s until WELCOME */
#define JOIN_TIMEOUT 600            /* ~10 s without any answer to a first JOIN: no host there */
#define CONCEAL_EVERY 4             /* no new packet for 4 cycles (2 game frames): advance the sequence */
#define LOST_CYCLES 150             /* ~2.5 s without packets: the node is lost (a ghost, or dropped) */
#define RACE_OVER_CYCLES 600        /* ~10 s: a ghost in the race is marked as having finished */
#define GHOST_CYCLES (180 * 60)     /* a ghost is dropped in the attract mode, or after ~3 minutes */
#define HALT_FRAMES 300             /* ~5 s of frames with no LANC cycle: the game stopped the link */

enum { ROLE_OFF, ROLE_HOST, ROLE_JOINING, ROLE_MEMBER };
enum { C_JOIN = 1, C_WELCOME, C_FULL, C_LEAVE, C_CLOSE, C_START, C_FIND, C_HERE };
enum { M_FREE, M_RESERVED, M_ACTIVE };

typedef struct { struct sockaddr_storage a; socklen_t len; } Addr;

int g_net_id;                       /* NETWORK ID 1-4 of this node, 0 = no link (the game uses 1) */
static int g_role, g_sock = -1, g_buffer = 2, g_over, g_welcomed, g_cycles, g_port;
static int g_start_signal;                   /* START received (member) */
static int g_hot_id;                         /* the NETWORK ID to take live (network.hot), 0 none */
static double g_start_time = -100;           /* when (wall clock: the host's joining period is) */
static double g_start_until;                 /* host: sending START until then */
static int g_start_mask;                     /* host: to the members that were ready at that START */
static int g_menu_port = NET_DEFAULT_PORT;   /* the port the menu hosts at (--net-port) */
static uint32_t g_seq, g_token;
static Addr g_hostaddr;             /* member: the host */
static int g_host_silent;
static char g_hostname[300];

static struct {                     /* host: the members, by ID */
    int state, silent, ready;
    uint32_t token;
    Addr addr;
} g_mem[NODES + 1];

typedef struct {                    /* received slots of a node, by ID */
    uint8_t q[QUEUE][SLOT];         /* playout queue */
    int head, count, primed, idle, seen;
    uint32_t last_seq;
    uint8_t last[SLOT];             /* the slot given to the game last, and its own sequence byte */
    int have_last, stale, lost;
    uint8_t real_seq, out_seq;      /* the game's sequence in the packets, and the one we present */
} Node;
static Node g_node[NODES + 1];

/* ------------------------------------------------------------------ sending (RT_NET_DELAY) */
#define DELAYQ 512
static struct { double due; Addr to; int len; uint8_t data[12 + 2 * SLOT]; } g_dq[DELAYQ];
static int g_ndq;
static double g_delay = -1, g_jitter, g_loss;
static uint32_t g_watch;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* RT_NET_OUTAGE: 1 inside one of its windows */
static int outage(void) {
    static double win[16][2];
    static int nwin = -1;
    if (nwin < 0) {
        nwin = 0;
        const char *e = getenv("RT_NET_OUTAGE");
        while (e && *e && nwin < 16) {
            char *p;
            win[nwin][0] = strtod(e, &p);
            if (*p != ',') break;
            win[nwin][1] = strtod(p + 1, &p);
            nwin++;
            e = *p == ',' ? p + 1 : p;
        }
    }
    double t = (double)rt_now() / CPU_HZ;
    for (int i = 0; i < nwin; i++)
        if (t >= win[i][0] && t < win[i][0] + win[i][1]) return 1;
    return 0;
}

static void send_to(const Addr *to, const uint8_t *pkt, int len) {
    if (g_delay < 0) {
        const char *d = getenv("RT_NET_DELAY"), *w = getenv("RT_NET_WATCH");
        g_delay = 0;
        if (d) sscanf(d, "%lf,%lf,%lf", &g_delay, &g_jitter, &g_loss);
        if (w) g_watch = (uint32_t)strtoul(w, NULL, 16);
        if (d) rt_log("net: test delay %.0f ms, jitter %.0f ms, loss %.1f%%\n", g_delay, g_jitter, g_loss);
    }
    if (outage()) return;
    if (!g_delay && !g_jitter && !g_loss) {
        sendto(g_sock, pkt, (size_t)len, 0, (const struct sockaddr *)&to->a, to->len);
        return;
    }
    if (g_loss > 0 && rand() % 10000 < g_loss * 100) return;
    if (g_ndq == DELAYQ) return;
    g_dq[g_ndq].due = now_s() + (g_delay + g_jitter * (rand() / (double)RAND_MAX)) / 1000.0;
    g_dq[g_ndq].to = *to;
    g_dq[g_ndq].len = len;
    memcpy(g_dq[g_ndq].data, pkt, (size_t)len);
    g_ndq++;
}

static void send_due(void) {
    double t = now_s();
    for (int i = 0; i < g_ndq;)
        if (g_dq[i].due <= t) {
            sendto(g_sock, g_dq[i].data, (size_t)g_dq[i].len, 0, (struct sockaddr *)&g_dq[i].to.a, g_dq[i].to.len);
            g_dq[i] = g_dq[--g_ndq];
        } else i++;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void control(const Addr *to, int type, int id, uint32_t token) {
    uint8_t p[12] = { 'V', 'P', 'L', 'C', (uint8_t)type, (uint8_t)id, 0, 0 };
    put32(p + 8, token);
    send_to(to, p, 12);
}

/* ------------------------------------------------------------------ slots */
static int encode(const uint8_t *src, uint8_t *dst) {
    int n = 0;
    for (int i = 0; i < SLOT;) {
        if (src[i]) { dst[n++] = src[i++]; continue; }
        int run = 0;
        while (i < SLOT && !src[i] && run < 255) { i++; run++; }
        dst[n++] = 0;
        dst[n++] = (uint8_t)run;
    }
    return n;
}

static int decode(const uint8_t *src, int len, uint8_t *dst) {
    int o = 0;
    for (int i = 0; i < len;) {
        if (src[i]) { if (o == SLOT) return -1; dst[o++] = src[i++]; continue; }
        if (i + 1 >= len) return -1;
        int run = src[i + 1];
        if (o + run > SLOT) return -1;
        memset(dst + o, 0, (size_t)run);
        o += run;
        i += 2;
    }
    return o == SLOT ? 0 : -1;
}

static void node_reset(int id, uint8_t *ram) {
    if (ram) memset(ram + (id - 1) * SLOT, 0, SLOT);     /* as a cabinet that left the ring */
    memset(&g_node[id], 0, sizeof g_node[id]);
}

static void node_push(int id, uint32_t seq, const uint8_t *slot) {
    Node *nd = &g_node[id];
    if (nd->seen && (int32_t)(seq - nd->last_seq) <= 0) return;     /* late or duplicate */
    if (!nd->seen) rt_log("net: NETWORK ID %d is linked\n", id);
    nd->seen = 1;
    nd->last_seq = seq;
    nd->idle = 0;
    if (nd->count == QUEUE) { nd->head = (nd->head + 1) % QUEUE; nd->count--; }   /* overrun: drop the oldest */
    memcpy(nd->q[(nd->head + nd->count) % QUEUE], slot, SLOT);
    nd->count++;
}

/* ------------------------------------------------------------------ setup */
static void list_broadcasts(void);

static int same_addr(const Addr *x, const Addr *y) {
    const struct sockaddr_storage *a = &x->a, *b = &y->a;
    if (a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *p = (const void *)a, *q = (const void *)b;
        return p->sin_port == q->sin_port && p->sin_addr.s_addr == q->sin_addr.s_addr;
    }
    const struct sockaddr_in6 *p = (const void *)a, *q = (const void *)b;
    return p->sin6_port == q->sin6_port && !memcmp(&p->sin6_addr, &q->sin6_addr, sizeof p->sin6_addr);
}

/* Windows: Winsock must be started before any socket or name lookup */
static void sockets_init(void) {
#ifdef _WIN32
    static int wsa;
    WSADATA wd;
    if (!wsa) wsa = !WSAStartup(MAKEWORD(2, 2), &wd);
#endif
}

static int open_socket(int port) {
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { rt_log("net: socket: %s\n", strerror(errno)); return -1; }
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(g_sock, (struct sockaddr *)&me, sizeof me) < 0) {
        rt_log("net: bind port %d: %s\n", port, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return -1;
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    BOOL no = FALSE;                                /* no recvfrom errors for ICMP port unreachable */
    DWORD got;
    WSAIoctl(g_sock, SIO_UDP_CONNRESET, &no, sizeof no, NULL, 0, &got, NULL, NULL);
#else
    fcntl(g_sock, F_SETFL, fcntl(g_sock, F_GETFL) | O_NONBLOCK);
#endif
    int on = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);     /* FIND on the local network */
    return 0;
}

/* the IPv4 address of an Addr (host order), 0 if none */
static uint32_t addr_ip(const Addr *x) {
    return x->a.ss_family == AF_INET ? ntohl(((const struct sockaddr_in *)&x->a)->sin_addr.s_addr) : 0;
}

/* ------------------------------------------------------------------ session codes */
/* The host's IPv4 address and port in Crockford's base32 (no I, L, O, U), with a check against
 * typing errors: 7 characters (32 bits of address, 3 of check) for the default port, 10 for any
 * other (48 bits, 2 of check); shown as XXX XXXX / XXXXX XXXXX. Reading folds lowercase, O to 0,
 * I and L to 1, and skips spaces and dashes. */
static const char k_b32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static unsigned code_check(unsigned a, unsigned b, unsigned c, unsigned d, unsigned port) {
    return (a * 3 + b * 5 + c * 7 + d * 11 + (port >> 8) * 13 + (port & 255) * 17) % 251;
}

int net_code_encode(const char *ip, int port, char *out, size_t n) {
    unsigned a, b, c, d;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255 || port < 1 || port > 65535 || n < 12) return -1;
    uint64_t v = (uint64_t)a << 24 | b << 16 | c << 8 | d;
    int k = port == NET_DEFAULT_PORT ? 7 : 10;
    if (k == 7) v = v << 3 | (code_check(a, b, c, d, (unsigned)port) & 7);
    else v = (v << 16 | (unsigned)port) << 2 | (code_check(a, b, c, d, (unsigned)port) & 3);
    for (int i = 0, o = 0; i < k; i++) {
        if (i == k - (k == 7 ? 4 : 5)) out[o++] = ' ';
        out[o++] = k_b32[(v >> (5 * (k - 1 - i))) & 31];
        out[o] = 0;
    }
    return 0;
}

/* a code into "a.b.c.d:port"; -1 if it is not one */
int net_code_decode(const char *code, char *hostport, size_t n) {
    uint64_t v = 0;
    int k = 0;
    for (const char *p = code; *p; p++) {
        int ch = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
        if (ch == '-' || ch == ' ') continue;
        if (ch == 'O') ch = '0';
        if (ch == 'I' || ch == 'L') ch = '1';
        const char *q = ch ? strchr(k_b32, ch) : NULL;
        if (!q || ++k > 10) return -1;
        v = v << 5 | (uint64_t)(q - k_b32);
    }
    unsigned port, chk;
    if (k == 7) { port = NET_DEFAULT_PORT; chk = (unsigned)v & 7; v >>= 3; }
    else if (k == 10) { chk = (unsigned)v & 3; v >>= 2; port = (unsigned)v & 0xffff; v >>= 16; }
    else return -1;
    unsigned a = (unsigned)(v >> 24) & 255, b = (unsigned)(v >> 16) & 255, c = (unsigned)(v >> 8) & 255, d = (unsigned)v & 255;
    if ((code_check(a, b, c, d, port) & (k == 7 ? 7u : 3u)) != chk || !port) return -1;
    snprintf(hostport, n, "%u.%u.%u.%u:%u", a, b, c, d, port);
    return 0;
}

static int resolve(const char *peer, Addr *out) {
    char host[256], fromcode[32];
    if (!strchr(peer, ':') && !net_code_decode(peer, fromcode, sizeof fromcode)) peer = fromcode;
    const char *colon = strrchr(peer, ':');
    if (!colon || colon == peer || colon - peer >= (int)sizeof host) { rt_log("net: %s is not host:port\n", peer); return -1; }
    snprintf(host, sizeof host, "%.*s", (int)(colon - peer), peer);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res;
    if (getaddrinfo(host, colon + 1, &hints, &res) || !res) { rt_log("net: cannot resolve %s\n", peer); return -1; }
    memcpy(&out->a, res->ai_addr, res->ai_addrlen);
    out->len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

static void set_buffer(int buffer) { if (buffer > 0 && buffer < QUEUE) g_buffer = buffer; }

/* a new session (host or member) starts from a clean state: an earlier one may have ended */
static int g_found_local;                    /* a joining node found its host on the local network */
static int g_halted;                         /* the game stopped its link (NETWORK ERROR) */

static void session_reset(void) {
    g_over = g_welcomed = g_host_silent = g_start_signal = g_hot_id = g_found_local = g_halted = 0;
    g_start_until = 0;
    memset(g_mem, 0, sizeof g_mem);
    memset(g_node, 0, sizeof g_node);
}

/* the host: NETWORK ID 1, at UDP port `port` */
int net_host(int port, int buffer) {
    set_buffer(buffer);
    session_reset();
    sockets_init();
    /* a port already taken on this machine (another host): the next ones of the free block */
    int last = port >= NET_DEFAULT_PORT && port < 24726 ? 24726 : port;
    while (open_socket(port)) if (++port > last) return -1;
    g_role = ROLE_HOST;
    g_net_id = 1;
    g_port = port;
    portmap_start(port);
    rt_log("net: host, NETWORK ID 1, port %d, playout buffer %d\n", port, g_buffer);
    return 0;
}

/* a member of the host at `peer` (session code or host:port): id 0 asks for a free ID (taken
 * live in the attract mode, enhanced mode), id 2-4 is a node that has its ID already (tests:
 * --net-id with --net-peer, set in the NVRAM at boot) */
int net_join(const char *peer, int port, int id, uint32_t token, int buffer) {
    set_buffer(buffer);
    session_reset();
    sockets_init();
    if (resolve(peer, &g_hostaddr) || open_socket(port)) return -1;
    snprintf(g_hostname, sizeof g_hostname, "%s", peer);
    if (!id) list_broadcasts();
    g_role = id ? ROLE_MEMBER : ROLE_JOINING;
    g_net_id = id;
    g_token = token;
    rt_log("net: joining %s%s, playout buffer %d\n", peer, id ? "" : " for a free NETWORK ID", g_buffer);
    if (id) rt_log("net: NETWORK ID %d\n", id);
    return 0;
}

int net_active(void) { return g_role != ROLE_OFF; }

/* the live NETWORK ID change (network.hot): the ID the game should take now (0: none), and the
 * report that it has (enhanced.c, from the net_set_id hook, in the attract mode) */
int net_hot_target(void) { return g_hot_id; }
void net_hot_request(int id) { g_hot_id = id; }

void net_hot_switched(int id) {
    g_hot_id = 0;
    if (id > 1 && g_role == ROLE_JOINING) {         /* joined: a member with that ID, no restart */
        g_role = ROLE_MEMBER;
        g_net_id = id;
        g_welcomed = 0;                             /* JOIN with the token: the host makes it active */
        g_host_silent = 0;
        memset(g_node, 0, sizeof g_node);
        rt_log("net: NETWORK ID %d taken\n", id);
    } else if (id == 1) {                           /* left: a single cabinet again */
        rt_log("net: NETWORK ID 1 taken, link play over\n");
        net_off();
    }
}
int net_session_over(void) { return g_over; }          /* left, closed or lost: back to ID 1 */

/* how the last session ended, for the menu (kept after net_off) */
static volatile int g_result;

static void end_session_r(const char *why, int result) {
    if (g_over) return;
    g_over = 1;
    g_result = result;
    rt_log("net: session over (%s)\n", why);
}

/* the player leaves (menu); also at exit */
void net_leave(void) {
    if (g_role == ROLE_HOST)
        for (int id = 2; id <= NODES; id++) { if (g_mem[id].state) control(&g_mem[id].addr, C_CLOSE, id, 0); }
    else if (g_role != ROLE_OFF) control(&g_hostaddr, C_LEAVE, g_net_id, g_token);
    send_due();
    end_session_r("left", NET_RES_LEFT);
}

void net_shutdown(void) {
    if (g_sock >= 0 && !g_over) net_leave();
    portmap_stop_wait(1);                           /* at exit: let the job remove the mapping */
}

/* the host after its session: no link any more (it keeps NETWORK ID 1, no restart needed) */
void net_off(void) {
    portmap_stop();
    if (g_sock >= 0) close(g_sock);
    g_sock = -1;
    g_role = ROLE_OFF;
    g_net_id = 0;
    memset(g_mem, 0, sizeof g_mem);
}

/* ------------------------------------------------------------------ receiving */
/* the host's public IPv4 (host order) as its router told, 0 if unknown */
static uint32_t public_ip(void) {
    char pub[64];
    unsigned a, b, c, d;
    portmap_status(pub, sizeof pub, NULL, NULL, 0);
    return sscanf(pub, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 ? a << 24 | b << 16 | c << 8 | d : 0;
}

/* a joining node: the host answered our FIND from the local network: talk to it there */
static void found_here(const Addr *from, uint32_t ip) {
    if (g_role != ROLE_JOINING || g_hot_id || g_found_local || (ip && ip != addr_ip(&g_hostaddr))) return;
    g_found_local = 1;
    g_hostaddr = *from;
    g_host_silent = 0;
    char s[64];
    inet_ntop(AF_INET, &((const struct sockaddr_in *)&from->a)->sin_addr, s, sizeof s);
    rt_log("net: the host is on the local network, at %s\n", s);
}

static void host_control(const Addr *from, int type, int id, uint32_t token) {
    if (type == C_FIND) {                           /* a node next to us looks for its host */
        uint32_t mine = public_ip();
        if (!mine || mine == token) {
            static Addr last;                       /* logged once per node, not at every FIND */
            if (!same_addr(&last, from)) { last = *from; rt_log("net: a node on the local network looks for this host\n"); }
            control(from, C_HERE, 1, mine);
        }
        return;
    }
    if (type == C_JOIN) {
        int got = 0;
        for (int i = 2; i <= NODES && !got; i++)              /* known: its token, or (resent JOIN) its address */
            if (g_mem[i].state && ((token && g_mem[i].token == token) || (!token && same_addr(&g_mem[i].addr, from)))) got = i;
        if (!got && id >= 2 && id <= NODES && !g_mem[id].state) got = id;      /* a node with its ID (tests) */
        for (int i = 2; i <= NODES && !got; i++)
            if (!g_mem[i].state) got = i;
        if (!got) { control(from, C_FULL, 0, 0); return; }
        if (!g_mem[got].state) {
            g_mem[got].token = token ? token : ((uint32_t)rand() << 16 ^ (uint32_t)rand() ^ (uint32_t)now_s()) | 1;
            rt_log("net: NETWORK ID %d %s\n", got, token || id ? "joined" : "reserved");
        }
        if ((token || id) && g_mem[got].state == M_RESERVED) rt_log("net: NETWORK ID %d is back\n", got);
        if (token || id) g_mem[got].state = M_ACTIVE;
        else if (g_mem[got].state != M_ACTIVE) g_mem[got].state = M_RESERVED;
        g_mem[got].addr = *from;
        g_mem[got].silent = 0;
        control(from, C_WELCOME, got, g_mem[got].token);
    } else if (type == C_LEAVE && id >= 2 && id <= NODES && g_mem[id].state && g_mem[id].token == token) {
        rt_log("net: NETWORK ID %d left\n", id);
        g_mem[id].state = M_FREE;
    }
}

static void member_control(int type, int id, uint32_t token) {
    if (type == C_WELCOME && g_role == ROLE_JOINING) {
        if (!g_hot_id) rt_log("net: given NETWORK ID %d, taking it in the attract mode\n", id);
        g_hot_id = id;
        g_token = token;
    } else if (type == C_WELCOME && g_role == ROLE_MEMBER && id == g_net_id) {
        if (!g_welcomed) rt_log("net: in the session as NETWORK ID %d\n", id);
        g_welcomed = 1;
        g_token = token;
    } else if (type == C_FULL) {
        rt_log("net: the session is full\n");
        end_session_r("full", NET_RES_FULL);
    } else if (type == C_CLOSE) end_session_r("closed by the host", NET_RES_CLOSED);
    else if (type == C_START && g_role == ROLE_MEMBER && now_s() - g_start_time > 2) {
        g_start_time = now_s();
        g_start_signal = 1;
        rt_log("net: the host starts the race\n");
    }
}

static void receive(uint8_t *ram) {
    uint8_t buf[2048], slot[SLOT];
    for (;;) {
        Addr from;
        from.len = sizeof from.a;
        ssize_t n = recvfrom(g_sock, buf, sizeof buf, 0, (struct sockaddr *)&from.a, &from.len);
        if (n < 0) return;                          /* EAGAIN: drained */
        if (n < 12 || outage()) continue;
        int type = buf[4], id = buf[5];
        if (!memcmp(buf, "VPLC", 4)) {
            if (type == C_HERE) { found_here(&from, get32(buf + 8)); continue; }
            if (g_role == ROLE_HOST) host_control(&from, type, id, get32(buf + 8));
            else if (same_addr(&from, &g_hostaddr)) { g_host_silent = 0; member_control(type, id, get32(buf + 8)); }
            continue;
        }
        int len = buf[6] << 8 | buf[7];
        id = buf[4];                                /* data: the sender's ID */
        if (memcmp(buf, "VPL1", 4) || id < 1 || id > NODES || id == g_net_id || len != n - 12) continue;
        if (g_role == ROLE_HOST) {                  /* only from the member holding that ID */
            if (g_mem[id].state != M_ACTIVE || !same_addr(&g_mem[id].addr, &from)) continue;
            g_mem[id].silent = 0;
            g_mem[id].ready = buf[5] & 1;
            for (int i = 2; i <= NODES; i++)        /* relay */
                if (i != id && g_mem[i].state == M_ACTIVE) send_to(&g_mem[i].addr, buf, (int)n);
        } else {
            if (!same_addr(&from, &g_hostaddr)) continue;
            g_host_silent = 0;
            if (g_role != ROLE_MEMBER) continue;    /* not in the session yet */
        }
        if (!decode(buf + 12, len, slot)) node_push(id, get32(buf + 8), slot);
    }
    (void)ram;
}

/* ------------------------------------------------------------------ timers */

/* The broadcast addresses (network order) to look for a host on the local network: the limited
 * one, which Windows sends out of one interface only, and each interface's own; listed once */
#define MAX_BCAST 8
static uint32_t g_bcast[MAX_BCAST];
static int g_nbcast;

static void list_broadcasts(void) {
    g_nbcast = 0;
    g_bcast[g_nbcast++] = htonl(INADDR_BROADCAST);
#ifdef _WIN32
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *list = malloc(size);
    if (list && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                     NULL, list, &size) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && g_nbcast < MAX_BCAST; u = u->Next) {
                ULONG len = u->OnLinkPrefixLength;
                if (len == 0 || len >= 31) continue;
                uint32_t ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
                g_bcast[g_nbcast++] = htonl(ip | (0xffffffffu >> len));
            }
        }
    }
    free(list);
#else
    struct ifaddrs *list;
    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs *i = list; i && g_nbcast < MAX_BCAST; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) &&
                (i->ifa_flags & IFF_BROADCAST) && !(i->ifa_flags & IFF_LOOPBACK) && i->ifa_broadaddr)
                g_bcast[g_nbcast++] = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr.s_addr;
        freeifaddrs(list);
    }
#endif
}

/* FIND on the local network, for a host behind the same router (which would have to send our
 * packets to its own public address back in, and many do not). The host listens at its local
 * port, which is the code's port unless the router gave another one outside: so also the first
 * ports of the block */
static void find_local(void) {
    int code_port = ntohs(((struct sockaddr_in *)&g_hostaddr.a)->sin_port);
    Addr bc = g_hostaddr;
    struct sockaddr_in *sin = (struct sockaddr_in *)&bc.a;
    for (int b = 0; b < g_nbcast; b++) {
        sin->sin_addr.s_addr = g_bcast[b];
        for (int p = -1; p < 4; p++) {
            int port = p < 0 ? code_port : NET_DEFAULT_PORT + p;
            if (p >= 0 && port == code_port) continue;
            sin->sin_port = htons((uint16_t)port);
            control(&bc, C_FIND, 0, addr_ip(&g_hostaddr));
        }
    }
}

static void timers(void) {
    if (g_role == ROLE_HOST) {
        for (int id = 2; id <= NODES; id++)
            if (g_mem[id].state && ++g_mem[id].silent > (g_mem[id].state == M_RESERVED ? RESERVE_CYCLES : SILENT_CYCLES)) {
                rt_log("net: NETWORK ID %d is gone\n", id);
                g_mem[id].state = M_FREE;
            }
    } else if (!g_over && ++g_host_silent > (g_role == ROLE_JOINING ? JOIN_TIMEOUT : SILENT_CYCLES))
        end_session_r("no answer from the host", NET_RES_NOANSWER);
    /* JOIN until WELCOME; a member also asks again when the host goes quiet (it restarted) */
    if ((g_role == ROLE_JOINING || (g_role == ROLE_MEMBER && (!g_welcomed || g_host_silent > JOIN_EVERY * 2)))
        && !g_over && g_cycles % JOIN_EVERY == 1) {
        control(&g_hostaddr, C_JOIN, g_net_id, g_token);
        if (g_role == ROLE_JOINING && !g_hot_id && !g_found_local) find_local();   /* the host next to us? */
    }
}

/* every video frame: the timers and the control traffic go on when the game has stopped the link
 * (NETWORK ERROR stops the LANC cycles), and such a stop is reported (net_game_halted) */
void net_frame(void) {
    static int last_cycles = -1, still;
    if (g_sock < 0 || !g_cycles) return;            /* the game has not started its link yet */
    if (g_cycles != last_cycles) { last_cycles = g_cycles; still = 0; return; }
    timers();
    send_due();
    receive(NULL);
    if (++still == HALT_FRAMES) {
        g_halted = 1;
        rt_log("net: the game stopped its link (NETWORK ERROR)\n");
    }
}

int net_game_halted(void) { return g_halted; }

/* ------------------------------------------------------------------ one LANC ring cycle */
void net_cycle(uint8_t *ram) {
    if (g_sock < 0) return;
    g_cycles++;
    timers();
    /* START every 0.5 s for 10 s to every member still waiting in its lobby: a lost datagram, or a
     * START its game ignored, is just repeated; a member that has started says so (ready 0) */
    if (g_role == ROLE_HOST && g_start_until > 0 && g_cycles % 30 == 0) {
        if (now_s() > g_start_until) g_start_until = 0;
        else
            for (int id = 2; id <= NODES; id++)
                if ((g_start_mask >> id & 1) && g_mem[id].state == M_ACTIVE && g_mem[id].ready)
                    control(&g_mem[id].addr, C_START, id, 0);
    }
    if (!g_over && (g_role == ROLE_HOST || (g_role == ROLE_MEMBER && g_welcomed))) {
        uint8_t pkt[12 + 2 * SLOT];
        int len = encode(ram + (g_net_id - 1) * SLOT, pkt + 12);
        memcpy(pkt, "VPL1", 4);
        pkt[4] = (uint8_t)g_net_id; pkt[5] = (uint8_t)(enh_lobby_ready() ? 1 : 0);
        pkt[6] = (uint8_t)(len >> 8); pkt[7] = (uint8_t)len;
        put32(pkt + 8, ++g_seq);
        if (g_role == ROLE_HOST) {
            for (int id = 2; id <= NODES; id++)
                if (g_mem[id].state == M_ACTIVE) send_to(&g_mem[id].addr, pkt, 12 + len);
        } else send_to(&g_hostaddr, pkt, 12 + len);
    }
    send_due();
    receive(ram);
    if (GAME_NET_ERROR_MODE >= 0 && GAME_NET_GHOST_FLAGS != 0 && g_net_id && !g_halted) {   /* our own packet: in error? */
        const uint8_t *own = ram + (g_net_id - 1) * SLOT;
        if (!memcmp(own, "NWK", 3) && (int)(get32(own + GAME_NET_GHOST_FLAGS) >> GAME_NET_GHOST_MODE_SHIFT & 15) == GAME_NET_ERROR_MODE) {
            g_halted = 1;
            rt_log("net: the game stopped on NETWORK ERROR\n");
        }
    }
    if (g_watch) {
        static uint32_t last[2] = { 0xffffffffu, 0xffffffffu };
        uint32_t v0 = LD32(g_watch), v1 = LD32(g_watch + 4);
        if (v0 != last[0] || v1 != last[1]) rt_log("net: watch %08x = %08x %08x\n", g_watch, v0, v1);
        last[0] = v0;
        last[1] = v1;
    }
    if (g_role != ROLE_MEMBER && g_role != ROLE_HOST) return;
    for (int id = 1; id <= NODES; id++) {
        if (id == g_net_id) continue;
        Node *nd = &g_node[id];
        if (!nd->seen) continue;
        int fresh = 0;
        if (nd->primed || nd->count >= g_buffer) {             /* (the playout buffer filled once) */
            nd->primed = 1;
            while (nd->count > g_buffer + 2) { nd->head = (nd->head + 1) % QUEUE; nd->count--; }   /* catch up */
            if (nd->count) {
                memcpy(nd->last, nd->q[nd->head], SLOT);
                nd->head = (nd->head + 1) % QUEUE;
                nd->count--;
                fresh = 1;
            }
        }
        if (!fresh && !nd->have_last) continue;
        int nwk = !memcmp(nd->last, "NWK", 3);
        if (fresh) {
            if (nd->lost) rt_log("net: NETWORK ID %d is back\n", id);
            nd->lost = 0;
            nd->idle = 0;
            if (!nd->have_last) nd->out_seq = nd->last[3];
            else if (nd->last[3] != nd->real_seq) { nd->out_seq++; nd->stale = 0; }
            else if (++nd->stale >= CONCEAL_EVERY) { nd->out_seq++; nd->stale = 0; }
            nd->real_seq = nd->last[3];
            nd->have_last = 1;
        } else {                                            /* late or gone: conceal */
            if (++nd->stale >= CONCEAL_EVERY) { nd->out_seq++; nd->stale = 0; }
            if (++nd->idle > LOST_CYCLES && !nd->lost) {
                nd->lost = 1;
                rt_log("net: NETWORK ID %d lost%s\n", id, GAME_NET_GHOST_KEEP ? ": shown standing still until the attract mode" : "");
            }
        }
        static int noconceal = -1;
        if (noconceal < 0) noconceal = getenv("RT_NET_NOCONCEAL") != NULL;
        if ((nd->lost && (!GAME_NET_GHOST_KEEP || enh_in_attract() || nd->idle > GHOST_CYCLES)) || (noconceal && nd->idle > 2)) {
            rt_log("net: NETWORK ID %d is no longer linked\n", id);
            node_reset(id, ram);
            continue;
        }
        uint8_t *slot = ram + (id - 1) * SLOT;
        memcpy(slot, nd->last, SLOT);
        if (nwk) slot[3] = nd->out_seq;
        if (GAME_NET_GHOST_FLAGS != 0 && nd->lost && nwk) {     /* a ghost: leave the race as the game allows */
            uint32_t w = get32(slot + GAME_NET_GHOST_FLAGS);
            int mode = (int)(w >> GAME_NET_GHOST_MODE_SHIFT) & 15, sub = (int)(w >> GAME_NET_GHOST_SUB_SHIFT) & 15;
            if (mode == GAME_NET_GHOST_MODE_GAME && sub == GAME_NET_GHOST_SUB_RACE && nd->idle > RACE_OVER_CYCLES)
                put32(slot + GAME_NET_GHOST_FLAGS, w | GAME_NET_GHOST_RACE_OVER);
            if (mode == GAME_NET_GHOST_MODE_GAME && sub >= GAME_NET_GHOST_SUB_SETUP_LO && sub <= GAME_NET_GHOST_SUB_SETUP_HI)
                put32(slot + GAME_NET_GHOST_SOLO_OFF, get32(slot + GAME_NET_GHOST_SOLO_OFF) | GAME_NET_GHOST_SOLO_BITS);
        }
    }
}

/* ------------------------------------------------------------------ the menu (enhanced.c) */
/* The menu runs on the frontend thread: it only files requests, which the guest thread carries
 * out (net_requests, every frame), and reads a snapshot of the state. */
static int g_req;                           /* 1 host, 2 join, 3 stop / leave, 4 start (atomic) */
static char g_req_code[32];

void net_request_host(void) { g_result = 0; __atomic_store_n(&g_req, 1, __ATOMIC_RELEASE); }
void net_request_join(const char *code) { snprintf(g_req_code, sizeof g_req_code, "%s", code); g_result = 0; __atomic_store_n(&g_req, 2, __ATOMIC_RELEASE); }
void net_request_stop(void) { __atomic_store_n(&g_req, 3, __ATOMIC_RELEASE); }
void net_request_start(void) { __atomic_store_n(&g_req, 4, __ATOMIC_RELEASE); }

/* a member: 1 while the host's START is pending (for ~10 s, inside the game's joining period);
 * net_start_taken() once the game has pressed START */
int net_start_signal(void) {
    if (g_start_signal && now_s() - g_start_time > 10) g_start_signal = 0;
    return g_start_signal;
}
void net_start_taken(void) { g_start_signal = 0; }

/* the members linked and ready (in the attract mode) now: the host starts with one at least */
static int node_ready(int id) {
    return g_node[id].seen && !g_node[id].lost && (g_role != ROLE_HOST || (g_mem[id].state == M_ACTIVE && g_mem[id].ready));
}
int net_linked_count(void) {
    int n = 0;
    for (int id = 1; id <= NODES; id++)
        if (id != g_net_id && node_ready(id)) n++;
    return n;
}

void net_set_port(int port) { g_menu_port = port; }

void net_requests(int buffer) {
    int r = __atomic_exchange_n(&g_req, 0, __ATOMIC_ACQ_REL);
    if (!r) return;
    if (r == 1 && g_role == ROLE_OFF) { if (net_host(g_menu_port, buffer)) g_result = NET_RES_PORT; }
    else if (r == 2 && g_role == ROLE_OFF) { if (net_join(g_req_code, 0, 0, 0, buffer)) g_result = NET_RES_BADCODE; }
    else if (r == 3 && g_role != ROLE_OFF) net_leave();
    else if (r == 4 && g_role == ROLE_HOST) {         /* the race of the members ready now; later ones wait */
        g_start_mask = 0;
        for (int id = 2; id <= NODES; id++)
            if (node_ready(id)) g_start_mask |= 1 << id;
        g_start_until = now_s() + 10;
        rt_log("net: starting the race for the session (members %s%s%s)\n", g_start_mask & 4 ? "2 " : "",
               g_start_mask & 8 ? "3 " : "", g_start_mask & 16 ? "4" : "");
    }
}

void net_status(NetStatus *st) {
    memset(st, 0, sizeof *st);
    st->role = g_role == ROLE_HOST ? NET_HOST : g_role == ROLE_MEMBER ? NET_MEMBER : g_role == ROLE_JOINING ? NET_JOINING : NET_OFF;
    st->id = g_net_id;
    st->over = g_over;
    st->result = g_result;
    if (g_role == ROLE_HOST) {
        char pub[64];
        int pubport = 0;
        st->portmap = portmap_status(pub, sizeof pub, &pubport, st->problem, sizeof st->problem);
        if (st->portmap == 2 && net_code_encode(pub, pubport, st->code, sizeof st->code)) st->code[0] = 0;
    } else if (g_role != ROLE_OFF) {               /* a member: the host's code, written out */
        char hp[300], ip[64];
        if (net_code_decode(g_hostname, hp, sizeof hp)) snprintf(hp, sizeof hp, "%s", g_hostname);
        const char *colon = strrchr(hp, ':');
        snprintf(ip, sizeof ip, "%.*s", colon ? (int)(colon - hp) : 0, hp);
        if (!colon || net_code_encode(ip, atoi(colon + 1), st->code, sizeof st->code))
            snprintf(st->code, sizeof st->code, "%s", g_hostname);
    }
    for (int id = 1; id <= NODES; id++) {
        if (id == g_net_id) st->node[id] = NET_NODE_SELF;
        else if (g_role == ROLE_HOST && g_mem[id].state == M_RESERVED) st->node[id] = NET_NODE_JOINING;
        else if (g_node[id].seen) st->node[id] = g_node[id].lost ? NET_NODE_LOST : node_ready(id) ? NET_NODE_LINKED : NET_NODE_JOINING;
    }
}
