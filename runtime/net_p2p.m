/*
 * The link's transport through Network.framework, peer to peer (VIPER_P2P: the iOS app; make P2P=1
 * on macOS to test it). Phones next to each other link with no router and no access point: the
 * host listens for UDP and advertises its game with Bonjour, the others look for it and connect,
 * over the Wi-Fi network they share or, with none, directly (AWDL, as AirDrop). It needs Wi-Fi
 * switched on and, on iOS, the local network access (NSLocalNetworkUsageDescription, and the
 * service in NSBonjourServices), and no entitlement.
 *
 * net.c keeps its datagrams, its timers and its addresses: each connection stands as an address of
 * its own (fd00::, the connection's number as the port), compared as any address; the host gets a
 * connection for each node that writes to it, a member has one, to its host. Network.framework
 * runs the connections on a queue of its own: what arrives waits in an inbox, taken by net.c on
 * the guest thread (p2p_recv, never blocking); sending is from that thread.
 */
#import <Foundation/Foundation.h>
#import <Network/Network.h>
#include <dns_sd.h>
#include <pthread.h>
#include <string.h>
#include <arpa/inet.h>
#include "net_p2p.h"

void rt_log(const char *fmt, ...);

#define MAX_PEERS 8
#define INBOX 128
#define DATAGRAM 2200               /* net.c's longest (one frame alone), with room */
#define MAX_GAMES 16

static dispatch_queue_t s_queue;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static nw_listener_t s_listener;
static nw_browser_t s_browser;
static struct { int id, ready; nw_connection_t conn; } s_peer[MAX_PEERS];
static int s_next_id = 1, s_host_peer;
static volatile int s_problem;
static struct { int peer, len; uint8_t data[DATAGRAM]; } s_in[INBOX];
static unsigned s_in_r, s_in_w;
static struct { char name[64]; nw_endpoint_t ep; } s_game[MAX_GAMES];
static int s_ngames;

/* the Bonjour service: P2P_SERVICE, or RT_P2P_SERVICE (tests on a Mac, apart from the phones nearby) */
static const char *service(void) {
    const char *e = getenv("RT_P2P_SERVICE");
    return e && *e ? e : P2P_SERVICE;
}

static void init(void) {
    if (!s_queue)
        s_queue = dispatch_queue_create("viper.link", dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
}

/* UDP, peer to peer included, marked as interactive voice (small datagrams at a steady rate) */
static nw_parameters_t params(void) {
    nw_parameters_t p = nw_parameters_create_secure_udp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_parameters_set_include_peer_to_peer(p, true);
    nw_parameters_set_service_class(p, nw_service_class_interactive_voice);
    return p;
}

/* the local network access refused (Settings), or another failure: for the menu */
static void note_error(const char *what, nw_error_t err) {
    int code = err ? nw_error_get_error_code(err) : 0;
    nw_error_domain_t dom = err ? nw_error_get_error_domain(err) : nw_error_domain_invalid;
    s_problem = dom == nw_error_domain_dns && code == kDNSServiceErr_PolicyDenied ? 1 : 2;
    rt_log("net: %s: error %d (domain %d)%s\n", what, code, (int)dom, s_problem == 1 ? ", local network access denied" : "");
}

/* ------------------------------------------------------------------ connections */
static void peer_addr(int id, struct sockaddr_storage *a, socklen_t *len) {
    struct sockaddr_in6 *s = (struct sockaddr_in6 *)a;
    memset(a, 0, sizeof *a);
    s->sin6_family = AF_INET6;
    s->sin6_len = sizeof *s;
    s->sin6_port = htons((uint16_t)id);
    s->sin6_addr.s6_addr[0] = 0xfd;
    *len = sizeof *s;
}

static int peer_of(const struct sockaddr_storage *a) {
    const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)a;
    return a->ss_family == AF_INET6 && s->sin6_addr.s6_addr[0] == 0xfd ? ntohs(s->sin6_port) : 0;
}

static int peer_add(nw_connection_t c) {
    int id = 0;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_PEERS && !id; i++)
        if (!s_peer[i].conn) {
            id = s_next_id;
            s_next_id = s_next_id % 60000 + 1;      /* never the number of a connection just gone */
            s_peer[i].id = id;
            s_peer[i].ready = 0;
            s_peer[i].conn = c;
        }
    pthread_mutex_unlock(&s_lock);
    return id;
}

static void peer_set(int id, int ready, int gone) {
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].id == id && s_peer[i].conn) {
            s_peer[i].ready = ready;
            if (gone) { s_peer[i].conn = nil; s_peer[i].id = 0; }
        }
    pthread_mutex_unlock(&s_lock);
}

static int peer_alive(int id) {
    int alive = 0;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_PEERS; i++) alive |= s_peer[i].id == id && s_peer[i].conn != nil;
    pthread_mutex_unlock(&s_lock);
    return alive;
}

static void inbox_push(int id, dispatch_data_t d) {
    size_t n = dispatch_data_get_size(d);
    if (n == 0 || n > DATAGRAM) return;
    pthread_mutex_lock(&s_lock);
    if (s_in_w - s_in_r < INBOX) {                  /* full: dropped, as a socket's buffer would */
        uint8_t *dst = s_in[s_in_w % INBOX].data;
        s_in[s_in_w % INBOX].peer = id;
        s_in[s_in_w % INBOX].len = (int)n;
        dispatch_data_apply(d, ^bool(dispatch_data_t region, size_t offset, const void *buf, size_t size) {
            (void)region;
            memcpy(dst + offset, buf, size);
            return true;
        });
        s_in_w++;
    }
    pthread_mutex_unlock(&s_lock);
}

/* one datagram at a time, for as long as the connection lives */
static void receive_loop(nw_connection_t c, int id) {
    nw_connection_receive_message(c, ^(dispatch_data_t content, nw_content_context_t ctx, bool complete, nw_error_t err) {
        (void)ctx; (void)complete;
        if (content) inbox_push(id, content);
        if (!peer_alive(id)) return;
        if (!err) receive_loop(c, id);
        else dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), s_queue, ^{ if (peer_alive(id)) receive_loop(c, id); });
    });
}

/* where a connection goes: the interface (en0 a Wi-Fi network, awdl0 phone to phone) and the address */
static void log_path(nw_connection_t c, int id, const char *what) {
    nw_path_t path = nw_connection_copy_current_path(c);
    __block const char *ifname = NULL;
    if (path) nw_path_enumerate_interfaces(path, ^bool(nw_interface_t i) { ifname = nw_interface_get_name(i); return false; });
    nw_endpoint_t remote = path ? nw_path_copy_effective_remote_endpoint(path) : nil;
    if (!remote) remote = nw_connection_copy_endpoint(c);
    char *addr = remote && nw_endpoint_get_type(remote) == nw_endpoint_type_address ? nw_endpoint_copy_address_string(remote) : NULL;
    rt_log("net: connection %d %s, via %s to %s\n", id, what, ifname ? ifname : "?", addr ? addr : "?");
    free(addr);
}

static void start_conn(nw_connection_t c, int id) {
    nw_connection_set_queue(c, s_queue);
    nw_connection_set_state_changed_handler(c, ^(nw_connection_state_t st, nw_error_t err) {
        if (st == nw_connection_state_ready) {
            peer_set(id, 1, 0);
            pthread_mutex_lock(&s_lock);
            nw_connection_t own = nil;
            for (int i = 0; i < MAX_PEERS; i++) if (s_peer[i].id == id) own = s_peer[i].conn;
            pthread_mutex_unlock(&s_lock);
            if (own) log_path(own, id, "ready");
        } else if (st == nw_connection_state_preparing) rt_log("net: connection %d preparing\n", id);
        else if (st == nw_connection_state_waiting) { peer_set(id, 0, 0); note_error("connection waiting", err); }
        else if (st == nw_connection_state_failed) {
            note_error("connection", err);
            pthread_mutex_lock(&s_lock);
            nw_connection_t own = nil;
            for (int i = 0; i < MAX_PEERS; i++) if (s_peer[i].id == id) own = s_peer[i].conn;
            pthread_mutex_unlock(&s_lock);
            if (own) nw_connection_cancel(own);
        } else if (st == nw_connection_state_cancelled) peer_set(id, 0, 1);
    });
    nw_connection_start(c);
    receive_loop(c, id);
}

static void close_peers(void) {
    nw_connection_t all[MAX_PEERS];
    int n = 0;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].conn) { all[n++] = s_peer[i].conn; s_peer[i].conn = nil; s_peer[i].id = 0; }
    s_in_r = s_in_w;
    s_host_peer = 0;
    pthread_mutex_unlock(&s_lock);
    for (int i = 0; i < n; i++) { nw_connection_cancel(all[i]); all[i] = nil; }
}

/* ------------------------------------------------------------------ net.c's side */
int p2p_host(const char *name) {
    @autoreleasepool {
        init();
        p2p_close();
        s_problem = 0;
        nw_listener_t l = nw_listener_create(params());
        if (!l) { rt_log("net: no listener\n"); return -1; }
        nw_listener_set_advertise_descriptor(l, nw_advertise_descriptor_create_bonjour_service(name, service(), NULL));
        nw_listener_set_queue(l, s_queue);
        nw_listener_set_new_connection_handler(l, ^(nw_connection_t c) {
            int id = peer_add(c);
            if (!id) { nw_connection_cancel(c); return; }   /* more than the cabinets: no room */
            start_conn(c, id);
        });
        static char s_name[96];
        snprintf(s_name, sizeof s_name, "%s", name);
        nw_listener_set_state_changed_handler(l, ^(nw_listener_state_t st, nw_error_t err) {
            if (st == nw_listener_state_ready) rt_log("net: advertised nearby as \"%s\"\n", s_name);
            else if (st == nw_listener_state_failed || (st == nw_listener_state_waiting && err)) note_error("listener", err);
        });
        nw_listener_start(l);
        s_listener = l;
        return 0;
    }
}

int p2p_join(const char *name) {
    @autoreleasepool {
        init();
        nw_endpoint_t ep = nil;
        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < s_ngames; i++)
            if (!strcmp(s_game[i].name, name)) ep = s_game[i].ep;
        pthread_mutex_unlock(&s_lock);
        if (!ep) { rt_log("net: \"%s\" is not nearby\n", name); return -1; }
        close_peers();
        s_problem = 0;
        nw_connection_t c = nw_connection_create(ep, params());
        int id = peer_add(c);
        if (!c || !id) return -1;
        pthread_mutex_lock(&s_lock);
        s_host_peer = id;
        pthread_mutex_unlock(&s_lock);
        start_conn(c, id);
        rt_log("net: connecting to \"%s\" nearby\n", name);
        return 0;
    }
}

void p2p_close(void) {
    @autoreleasepool {
        if (s_listener) { nw_listener_cancel(s_listener); s_listener = nil; }
        close_peers();
    }
}

int p2p_send(const struct sockaddr_storage *to, const void *p, int len) {
    @autoreleasepool {
        int id = peer_of(to);
        nw_connection_t c = nil;
        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < MAX_PEERS; i++)
            if (s_peer[i].id == id && s_peer[i].ready) c = s_peer[i].conn;
        pthread_mutex_unlock(&s_lock);
        if (!c || len <= 0) {                       /* not connected (yet): net.c sends again */
            static int told;
            if (told++ < 3) rt_log("net: connection %d not ready, a datagram held back\n", id);
            return -1;
        }
        dispatch_data_t d = dispatch_data_create(p, (size_t)len, s_queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        nw_connection_send(c, d, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true, ^(nw_error_t err) { (void)err; });
        return 0;
    }
}

int p2p_recv(void *buf, int max, struct sockaddr_storage *from, socklen_t *len) {
    pthread_mutex_lock(&s_lock);
    if (s_in_r == s_in_w) { pthread_mutex_unlock(&s_lock); return -1; }
    int n = s_in[s_in_r % INBOX].len < max ? s_in[s_in_r % INBOX].len : max;
    memcpy(buf, s_in[s_in_r % INBOX].data, (size_t)n);
    peer_addr(s_in[s_in_r % INBOX].peer, from, len);
    s_in_r++;
    pthread_mutex_unlock(&s_lock);
    return n;
}

void p2p_host_addr(struct sockaddr_storage *a, socklen_t *len) {
    pthread_mutex_lock(&s_lock);
    int id = s_host_peer;
    pthread_mutex_unlock(&s_lock);
    peer_addr(id, a, len);
}

int p2p_problem(void) { return s_problem; }

int p2p_owns(const struct sockaddr_storage *a) { return peer_of(a) != 0; }

/* ------------------------------------------------------------------ games nearby */
static void game_change(nw_browse_result_t r, int add) {
    nw_endpoint_t ep = nw_browse_result_copy_endpoint(r);
    const char *name = ep ? nw_endpoint_get_bonjour_service_name(ep) : NULL;
    if (!name) return;
    pthread_mutex_lock(&s_lock);
    int at = -1;
    for (int i = 0; i < s_ngames; i++) if (!strcmp(s_game[i].name, name)) at = i;
    if (add && at < 0 && s_ngames < MAX_GAMES) {
        snprintf(s_game[s_ngames].name, sizeof s_game[s_ngames].name, "%s", name);
        s_game[s_ngames++].ep = ep;
    } else if (!add && at >= 0) {
        s_game[at] = s_game[--s_ngames];
        s_game[s_ngames].ep = nil;
    }
    pthread_mutex_unlock(&s_lock);
    rt_log("net: game nearby %s: \"%s\"\n", add ? "found" : "gone", name);
}

void p2p_browse(int on) {
    init();
    dispatch_async(s_queue, ^{
        if (on && !s_browser) {
            nw_browser_t b = nw_browser_create(nw_browse_descriptor_create_bonjour_service(service(), NULL), params());
            nw_browser_set_queue(b, s_queue);
            nw_browser_set_browse_results_changed_handler(b, ^(nw_browse_result_t old, nw_browse_result_t now, bool complete) {
                (void)complete;
                nw_browse_result_change_t ch = nw_browse_result_get_changes(old, now);
                if (ch & nw_browse_result_change_result_removed) game_change(old, 0);
                if (ch & nw_browse_result_change_result_added) game_change(now, 1);
            });
            nw_browser_set_state_changed_handler(b, ^(nw_browser_state_t st, nw_error_t err) {
                if (st == nw_browser_state_ready) s_problem = 0;
                else if (st == nw_browser_state_failed || (st == nw_browser_state_waiting && err)) note_error("looking for games", err);
            });
            pthread_mutex_lock(&s_lock);
            for (int i = 0; i < s_ngames; i++) s_game[i].ep = nil;
            s_ngames = 0;
            pthread_mutex_unlock(&s_lock);
            s_problem = 0;
            nw_browser_start(b);
            s_browser = b;
        } else if (!on && s_browser) {
            nw_browser_cancel(s_browser);
            s_browser = nil;                        /* the games found stay, to join one of them */
        }
    });
}

int p2p_games(char names[][64], int max) {
    pthread_mutex_lock(&s_lock);
    int n = s_ngames < max ? s_ngames : max;
    for (int i = 0; i < n; i++) memcpy(names[i], s_game[i].name, 64);
    pthread_mutex_unlock(&s_lock);
    return n;
}
