/*
 * Port mapping for the link-play host (net.c): the host's UDP port is opened on the home router
 * automatically, with NAT-PMP or, failing that, UPnP IGD (third_party/libnatpmp, miniupnpc;
 * compiled in). The router also tells the public address, which goes into the session code.
 *
 * The mapping has a lease of LEASE seconds, renewed every LEASE / 2 while the host runs, so it
 * expires by itself if the process dies without removing it (killed, crashed); at the end of the
 * session and at exit it is removed. A router whose UPnP takes only permanent mappings gets one,
 * removed at exit; one left over by a crashed run (same port, this machine) is taken over.
 * Discovery takes a couple of seconds, so it all runs on its own thread, which otherwise sleeps.
 */
#include "runtime.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include "natpmp.h"
#include "miniupnpc.h"
#include "upnpcommands.h"
#include "upnperrors.h"

#define LEASE 120
#define DESC "konami-viper-recomp link"

enum { PM_IDLE, PM_WORKING, PM_OK, PM_FAILED };
static pthread_t g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static int g_state, g_stop, g_port, g_running;
static char g_public[64], g_method[16], g_problem[160];

/* ------------------------------------------------------------------ NAT-PMP */
static int natpmp_wait(natpmp_t *n, natpmpresp_t *r) {
    for (;;) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(n->s, &fds);
        getnatpmprequesttimeout(n, &tv);
        if (tv.tv_sec > 1) tv.tv_sec = 1;               /* do not wait for the 9 retries (2 min) */
        select(n->s + 1, &fds, NULL, NULL, &tv);
        int rc = readnatpmpresponseorretry(n, r);
        if (rc != NATPMP_TRYAGAIN) return rc;
        if (n->try_number > 2) return NATPMP_ERR_NOGATEWAYSUPPORT;
    }
}

static int natpmp_map(int port, uint32_t lifetime, char *pub, size_t publen) {
    natpmp_t n;
    natpmpresp_t r;
    if (initnatpmp(&n, 0, 0) < 0) return -1;
    int ok = -1;
    if (pub && sendpublicaddressrequest(&n) >= 0 && natpmp_wait(&n, &r) == 0)
        snprintf(pub, publen, "%s", inet_ntoa(r.pnu.publicaddress.addr));
    if ((!pub || pub[0]) && sendnewportmappingrequest(&n, NATPMP_PROTOCOL_UDP, (uint16_t)port, (uint16_t)port, lifetime) >= 0
        && natpmp_wait(&n, &r) == 0)
        ok = lifetime && r.pnu.newportmapping.mappedpublicport != port ? -2 : 0;   /* another port: not ours */
    closenatpmp(&n);
    return ok;
}

/* ------------------------------------------------------------------ UPnP IGD */
static struct UPNPUrls g_urls;
static struct IGDdatas g_igd;
static char g_lan[64];
static int g_have_igd, g_permanent;

static int upnp_find(char *pub, size_t publen) {
    int err = 0;
    struct UPNPDev *dev = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &err);
    if (!dev) return -1;
    char wan[64] = "";
    int rc = UPNP_GetValidIGD(dev, &g_urls, &g_igd, g_lan, sizeof g_lan, wan, sizeof wan);
    freeUPNPDevlist(dev);
    if (rc != UPNP_CONNECTED_IGD && rc != UPNP_PRIVATEIP_IGD) { if (rc) FreeUPNPUrls(&g_urls); return -1; }
    g_have_igd = 1;
    char ext[40] = "";
    if (UPNP_GetExternalIPAddress(g_urls.controlURL, g_igd.first.servicetype, ext) == UPNPCOMMAND_SUCCESS && ext[0])
        snprintf(pub, publen, "%s", ext);
    else snprintf(pub, publen, "%s", wan);
    return 0;
}

static int upnp_map(int port) {
    char p[8], lease[8];
    snprintf(p, sizeof p, "%d", port);
    snprintf(lease, sizeof lease, "%d", g_permanent ? 0 : LEASE);
    int rc = UPNP_AddPortMapping(g_urls.controlURL, g_igd.first.servicetype, p, p, g_lan, DESC, "UDP", NULL, lease);
    if (rc == 725 && !g_permanent) {                     /* OnlyPermanentLeasesSupported */
        g_permanent = 1;
        rc = UPNP_AddPortMapping(g_urls.controlURL, g_igd.first.servicetype, p, p, g_lan, DESC, "UDP", NULL, "0");
    }
    if (rc == 718) {                                     /* ConflictInMappingEntry: ours from a crashed run? */
        char client[64] = "", iport[8] = "", desc[80] = "", en[8] = "", dur[16] = "";
        if (UPNP_GetSpecificPortMappingEntryExt(g_urls.controlURL, g_igd.first.servicetype, p, "UDP", NULL,
                                                client, iport, desc, sizeof desc, en, dur) == UPNPCOMMAND_SUCCESS
            && !strcmp(client, g_lan) && atoi(iport) == port) return 0;
        snprintf(g_problem, sizeof g_problem, "UDP port %d is already used on the router by another device", port);
    }
    return rc == UPNPCOMMAND_SUCCESS ? 0 : -1;
}

static void upnp_unmap(int port) {
    char p[8];
    snprintf(p, sizeof p, "%d", port);
    UPNP_DeletePortMapping(g_urls.controlURL, g_igd.first.servicetype, p, "UDP", NULL);
}

/* ------------------------------------------------------------------ the thread */
static int is_private(const char *ip) {
    unsigned a, b;
    if (sscanf(ip, "%u.%u", &a, &b) != 2) return 1;
    return a == 10 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b < 128)
           || a == 127 || a == 0;
}

static void set_state(int s, const char *method, const char *pub) {
    pthread_mutex_lock(&g_lock);
    g_state = s;
    if (method) snprintf(g_method, sizeof g_method, "%s", method);
    if (pub) snprintf(g_public, sizeof g_public, "%s", pub);
    pthread_mutex_unlock(&g_lock);
}

/* sleeps up to `s` seconds; 1 when asked to stop */
static int nap(int s) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += s;
    pthread_mutex_lock(&g_lock);
    while (!g_stop && pthread_cond_timedwait(&g_wake, &g_lock, &until) == 0) {}
    int stop = g_stop;
    pthread_mutex_unlock(&g_lock);
    return stop;
}

static void *worker(void *arg) {
    (void)arg;
    char pub[64] = "";
    const char *method = NULL;
    if (natpmp_map(g_port, LEASE, pub, sizeof pub) == 0) method = "NAT-PMP";
    else if (upnp_find(pub, sizeof pub) == 0 && upnp_map(g_port) == 0) method = "UPnP";
    if (!method) {
        if (!g_problem[0]) snprintf(g_problem, sizeof g_problem, "the router does not open UDP port %d (no NAT-PMP or UPnP)", g_port);
        rt_log("net: %s: the others cannot reach this host unless the port is opened by hand\n", g_problem);
        set_state(PM_FAILED, NULL, pub[0] ? pub : NULL);
        if (g_have_igd) { FreeUPNPUrls(&g_urls); g_have_igd = 0; }
        return NULL;
    }
    if (is_private(pub))
        snprintf(g_problem, sizeof g_problem, "the router's public address %s is private: another router or the provider (CGNAT) is in between", pub);
    char code[16] = "";
    net_code_encode(pub, g_port, code, sizeof code);
    rt_log("net: UDP port %d opened on the router with %s, public address %s, session code %s%s%s\n", g_port, method, pub,
           code, g_problem[0] ? "; " : "", g_problem);
    set_state(PM_OK, method, pub);
    while (!nap(g_permanent ? 3600 : LEASE / 2))           /* renew the lease */
        if (method[0] == 'N' ? natpmp_map(g_port, LEASE, NULL, 0) : upnp_map(g_port))
            rt_log("net: the router did not renew the port mapping\n");
    if (method[0] == 'N') natpmp_map(g_port, 0, NULL, 0);
    else upnp_unmap(g_port);
    if (g_have_igd) { FreeUPNPUrls(&g_urls); g_have_igd = 0; }
    rt_log("net: UDP port %d closed on the router\n", g_port);
    return NULL;
}

void portmap_start(int port) {
    if (g_running || getenv("RT_NET_NOPORTMAP")) return;    /* RT_NET_NOPORTMAP: local tests */
    g_port = port;
    g_stop = 0;
    g_problem[0] = 0;
    g_state = PM_WORKING;
    g_running = pthread_create(&g_thread, NULL, worker, NULL) == 0;
}

void portmap_stop(void) {
    if (!g_running) return;
    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);                          /* a removal takes well under a second */
    g_running = 0;
    g_state = PM_IDLE;
}

/* 0 idle, 1 working, 2 open, 3 failed; the public address and the problem, if any */
int portmap_status(char *pub, int publen, char *problem, int problemlen) {
    pthread_mutex_lock(&g_lock);
    int s = g_state;
    if (pub) snprintf(pub, (size_t)publen, "%s", g_public);
    if (problem) snprintf(problem, (size_t)problemlen, "%s", g_problem);
    pthread_mutex_unlock(&g_lock);
    return s;
}
