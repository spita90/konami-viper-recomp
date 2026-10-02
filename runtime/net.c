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
 * Here each LANC cycle (hw.c) calls net_cycle(): it sends the own slot and fills the peers' slots
 * from a small playout queue per peer (one packet per cycle, as the sender produces them, after
 * the queue has filled to --net-buffer packets). The topology is a star: a node started with
 * --net-peer talks to that host only; the host answers whoever sends to it and relays each
 * packet to its other peers, so only the host needs a reachable port (NAT on the other side is
 * fine). Polled on the guest thread, non-blocking: no extra thread.
 *
 * Datagram: "VPL1", sender NETWORK ID, 0, 16-bit length, 32-bit sequence (big-endian), then the
 * slot, zero runs coded as 0x00 count (count 1-255).
 */
#include "runtime.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SLOT 0x400
#define NODES 4
#define QUEUE 16
#define MAXPEERS 8
#define TIMEOUT_CYCLES 120          /* ~2 s without packets: the node is gone, its slot cleared */

int g_net_id;                       /* NETWORK ID 1-4, 0 = no link */
static int g_sock = -1, g_buffer = 2;
static struct sockaddr_storage g_peer[MAXPEERS];
static socklen_t g_peer_len[MAXPEERS];
static int g_npeers, g_host;        /* g_host: no --net-peer, so the peers are learned */
static uint32_t g_seq;

typedef struct {
    uint8_t q[QUEUE][SLOT];         /* playout queue of slots */
    uint32_t qseq[QUEUE];
    int head, count, primed, idle;
    uint32_t last_seq;
    int seen;
} Node;
static Node g_node[NODES + 1];

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
    for (int i = 0; i < len && o <= SLOT;) {
        if (src[i]) { dst[o++] = src[i++]; continue; }
        if (i + 1 >= len) return -1;
        int run = src[i + 1];
        if (o + run > SLOT) return -1;
        memset(dst + o, 0, (size_t)run);
        o += run;
        i += 2;
    }
    return o == SLOT ? 0 : -1;
}

static int same_addr(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;
    return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr);
}

static int peer_index(const struct sockaddr_storage *from, socklen_t len) {
    for (int i = 0; i < g_npeers; i++)
        if (same_addr(&g_peer[i], from)) return i;
    if (!g_host || g_npeers >= MAXPEERS) return -1;
    g_peer[g_npeers] = *from;               /* the host learns its peers from their packets */
    g_peer_len[g_npeers] = len;
    rt_log("net: peer %d joined\n", g_npeers);
    return g_npeers++;
}

int net_init(int id, int port, const char *peer, int buffer) {
    g_net_id = id;
    if (buffer > 0 && buffer < QUEUE) g_buffer = buffer;
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { rt_log("net: socket: %s\n", strerror(errno)); return -1; }
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(g_sock, (struct sockaddr *)&me, sizeof me) < 0) { rt_log("net: bind port %d: %s\n", port, strerror(errno)); return -1; }
    fcntl(g_sock, F_SETFL, fcntl(g_sock, F_GETFL) | O_NONBLOCK);
    g_host = peer == NULL;
    if (peer) {
        char host[256];
        const char *colon = strrchr(peer, ':');
        if (!colon || colon - peer >= (int)sizeof host) { rt_log("net: --net-peer wants host:port\n"); return -1; }
        snprintf(host, sizeof host, "%.*s", (int)(colon - peer), peer);
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res;
        if (getaddrinfo(host, colon + 1, &hints, &res) || !res) { rt_log("net: cannot resolve %s\n", peer); return -1; }
        memcpy(&g_peer[0], res->ai_addr, res->ai_addrlen);
        g_peer_len[0] = (socklen_t)res->ai_addrlen;
        g_npeers = 1;
        freeaddrinfo(res);
    }
    rt_log("net: NETWORK ID %d, port %d, %s, playout buffer %d\n", id, port, peer ? peer : "host", g_buffer);
    return 0;
}

static void receive(void) {
    uint8_t buf[2048], slot[SLOT];
    for (;;) {
        struct sockaddr_storage from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(g_sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        if (n < 0) return;                          /* EAGAIN: drained */
        if (n < 12 || memcmp(buf, "VPL1", 4)) continue;
        int id = buf[4], len = buf[6] << 8 | buf[7];
        uint32_t seq = (uint32_t)buf[8] << 24 | buf[9] << 16 | buf[10] << 8 | buf[11];
        if (id < 1 || id > NODES || id == g_net_id || len != n - 12) continue;
        int pi = peer_index(&from, flen);
        if (pi < 0) continue;
        if (g_host)                                 /* relay to the other peers */
            for (int i = 0; i < g_npeers; i++)
                if (i != pi) sendto(g_sock, buf, (size_t)n, 0, (struct sockaddr *)&g_peer[i], g_peer_len[i]);
        if (decode(buf + 12, len, slot)) continue;
        Node *nd = &g_node[id];
        if (nd->seen && (int32_t)(seq - nd->last_seq) <= 0) continue;   /* late or duplicate */
        if (!nd->seen) rt_log("net: NETWORK ID %d is linked\n", id);
        nd->seen = 1;
        nd->last_seq = seq;
        nd->idle = 0;
        if (nd->count == QUEUE) { nd->head = (nd->head + 1) % QUEUE; nd->count--; }   /* overrun: drop the oldest */
        int tail = (nd->head + nd->count) % QUEUE;
        memcpy(nd->q[tail], slot, SLOT);
        nd->qseq[tail] = seq;
        nd->count++;
    }
}

/* one LANC ring cycle: own slot out, peers' slots in (ram = the LANC RAM, 4 slots) */
void net_cycle(uint8_t *ram) {
    if (g_sock < 0) return;
    uint8_t pkt[12 + 2 * SLOT];
    int len = encode(ram + (g_net_id - 1) * SLOT, pkt + 12);
    memcpy(pkt, "VPL1", 4);
    pkt[4] = (uint8_t)g_net_id; pkt[5] = 0;
    pkt[6] = (uint8_t)(len >> 8); pkt[7] = (uint8_t)len;
    g_seq++;
    pkt[8] = (uint8_t)(g_seq >> 24); pkt[9] = (uint8_t)(g_seq >> 16); pkt[10] = (uint8_t)(g_seq >> 8); pkt[11] = (uint8_t)g_seq;
    for (int i = 0; i < g_npeers; i++) sendto(g_sock, pkt, (size_t)(12 + len), 0, (struct sockaddr *)&g_peer[i], g_peer_len[i]);
    receive();
    for (int id = 1; id <= NODES; id++) {
        if (id == g_net_id) continue;
        Node *nd = &g_node[id];
        if (!nd->seen) continue;
        if (++nd->idle > TIMEOUT_CYCLES) {         /* gone: as a cabinet that left the ring */
            rt_log("net: NETWORK ID %d timed out\n", id);
            memset(ram + (id - 1) * SLOT, 0, SLOT);
            memset(nd, 0, sizeof *nd);
            continue;
        }
        if (!nd->primed && nd->count < g_buffer) continue;     /* filling the playout buffer */
        nd->primed = 1;
        while (nd->count > g_buffer + 2) { nd->head = (nd->head + 1) % QUEUE; nd->count--; }   /* catch up */
        if (!nd->count) continue;                   /* late: the game keeps the previous packet */
        memcpy(ram + (id - 1) * SLOT, nd->q[nd->head], SLOT);
        nd->head = (nd->head + 1) % QUEUE;
        nd->count--;
    }
}
