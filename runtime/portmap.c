/*
 * Port mapping for the link-play host (net.c): the host's UDP port is opened on the home router
 * automatically, with NAT-PMP or, failing that, UPnP IGD (third_party/libnatpmp, miniupnpc;
 * compiled in). The router also tells the public address, which goes into the session code.
 *
 * The mapping has a lease of LEASE seconds, renewed every LEASE / 2 while the host runs, so it
 * expires by itself if the process dies without removing it (killed, crashed); at the end of the
 * session and at exit it is removed. A router whose UPnP takes only permanent mappings gets one,
 * removed at exit; one left over by a crashed run (same port, this machine) is taken over.
 *
 * Discovery and the requests block for up to a few seconds, so each session's mapping is a job on
 * its own thread, which otherwise sleeps. Stopping never waits for it (the game must not stall):
 * the job removes the mapping as soon as it can and frees itself; only at exit does
 * portmap_stop() wait for that, briefly. A new session starts a new job at once.
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
#define EXIT_WAIT_MS 1500

enum { PM_IDLE, PM_WORKING, PM_OK, PM_FAILED };

typedef struct {
    int port, stop, done, detached;
    struct UPNPUrls urls;
    struct IGDdatas igd;
    char lan[64];
    int have_igd, permanent;
} Job;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static Job *g_job;                          /* the current session's job, NULL if none */
static pthread_t g_thread;
static int g_state;
static char g_public[64], g_problem[160];

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
static int upnp_find(Job *j, char *pub, size_t publen) {
    int err = 0;
    struct UPNPDev *dev = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &err);
    if (!dev) return -1;
    char wan[64] = "";
    int rc = UPNP_GetValidIGD(dev, &j->urls, &j->igd, j->lan, sizeof j->lan, wan, sizeof wan);
    freeUPNPDevlist(dev);
    if (rc != UPNP_CONNECTED_IGD && rc != UPNP_PRIVATEIP_IGD) { if (rc) FreeUPNPUrls(&j->urls); return -1; }
    j->have_igd = 1;
    char ext[40] = "";
    if (UPNP_GetExternalIPAddress(j->urls.controlURL, j->igd.first.servicetype, ext) == UPNPCOMMAND_SUCCESS && ext[0])
        snprintf(pub, publen, "%s", ext);
    else snprintf(pub, publen, "%s", wan);
    return 0;
}

static int upnp_map(Job *j, char *problem, size_t problemlen) {
    char p[8], lease[8];
    snprintf(p, sizeof p, "%d", j->port);
    snprintf(lease, sizeof lease, "%d", j->permanent ? 0 : LEASE);
    int rc = UPNP_AddPortMapping(j->urls.controlURL, j->igd.first.servicetype, p, p, j->lan, DESC, "UDP", NULL, lease);
    if (rc == 725 && !j->permanent) {                    /* OnlyPermanentLeasesSupported */
        j->permanent = 1;
        rc = UPNP_AddPortMapping(j->urls.controlURL, j->igd.first.servicetype, p, p, j->lan, DESC, "UDP", NULL, "0");
    }
    if (rc == 718) {                                     /* ConflictInMappingEntry: ours from a crashed run? */
        char client[64] = "", iport[8] = "", desc[80] = "", en[8] = "", dur[16] = "";
        if (UPNP_GetSpecificPortMappingEntryExt(j->urls.controlURL, j->igd.first.servicetype, p, "UDP", NULL,
                                                client, iport, desc, sizeof desc, en, dur) == UPNPCOMMAND_SUCCESS
            && !strcmp(client, j->lan) && atoi(iport) == j->port) return 0;
        if (problem) snprintf(problem, problemlen, "UDP port %d is already used on the router by another device", j->port);
    }
    return rc == UPNPCOMMAND_SUCCESS ? 0 : -1;
}

static void upnp_unmap(Job *j) {
    char p[8];
    snprintf(p, sizeof p, "%d", j->port);
    UPNP_DeletePortMapping(j->urls.controlURL, j->igd.first.servicetype, p, "UDP", NULL);
}

/* ------------------------------------------------------------------ the job */
static int is_private(const char *ip) {
    unsigned a, b;
    if (sscanf(ip, "%u.%u", &a, &b) != 2) return 1;
    return a == 10 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b < 128)
           || a == 127 || a == 0;
}

/* the state shown by the menu, if j is still the current job */
static void set_state(Job *j, int s, const char *pub, const char *problem) {
    pthread_mutex_lock(&g_lock);
    if (j == g_job) {
        g_state = s;
        if (pub) snprintf(g_public, sizeof g_public, "%s", pub);
        if (problem) snprintf(g_problem, sizeof g_problem, "%s", problem);
    }
    pthread_mutex_unlock(&g_lock);
}

static int stopping(Job *j) {
    pthread_mutex_lock(&g_lock);
    int s = j->stop;
    pthread_mutex_unlock(&g_lock);
    return s;
}

/* sleeps up to `s` seconds; 1 when asked to stop */
static int nap(Job *j, int s) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += s;
    pthread_mutex_lock(&g_lock);
    while (!j->stop && pthread_cond_timedwait(&g_wake, &g_lock, &until) == 0) {}
    int stop = j->stop;
    pthread_mutex_unlock(&g_lock);
    return stop;
}

static void finish(Job *j) {
    if (j->have_igd) FreeUPNPUrls(&j->urls);
    pthread_mutex_lock(&g_lock);
    j->done = 1;
    int detached = j->detached;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    if (detached) free(j);                  /* nobody waits for it any more */
}

static void *worker(void *arg) {
    Job *j = arg;
    char pub[64] = "", problem[160] = "";
    int nat = 0;
    if (natpmp_map(j->port, LEASE, pub, sizeof pub) == 0) nat = 1;
    else if (stopping(j) || upnp_find(j, pub, sizeof pub) || stopping(j) || upnp_map(j, problem, sizeof problem)) {
        if (!stopping(j)) {
            if (!problem[0]) snprintf(problem, sizeof problem, "the router does not open UDP port %d (no NAT-PMP or UPnP)", j->port);
            rt_log("net: %s: the others cannot reach this host unless the port is opened by hand\n", problem);
            set_state(j, PM_FAILED, pub[0] ? pub : NULL, problem);
        }
        finish(j);
        return NULL;
    }
    if (is_private(pub))
        snprintf(problem, sizeof problem, "the router's public address %s is private: another router or the provider (CGNAT) is in between", pub);
    char code[16] = "";
    net_code_encode(pub, j->port, code, sizeof code);
    rt_log("net: UDP port %d opened on the router with %s, public address %s, session code %s%s%s\n", j->port,
           nat ? "NAT-PMP" : "UPnP", pub, code, problem[0] ? "; " : "", problem);
    set_state(j, PM_OK, pub, problem);
    while (!nap(j, j->permanent ? 3600 : LEASE / 2))     /* renew the lease */
        if (nat ? natpmp_map(j->port, LEASE, NULL, 0) : upnp_map(j, NULL, 0))
            rt_log("net: the router did not renew the port mapping\n");
    if (nat) natpmp_map(j->port, 0, NULL, 0);
    else upnp_unmap(j);
    rt_log("net: UDP port %d closed on the router\n", j->port);
    finish(j);
    return NULL;
}

void portmap_start(int port) {
    if (g_job || getenv("RT_NET_NOPORTMAP")) return;      /* RT_NET_NOPORTMAP: local tests */
    Job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->port = port;
    pthread_mutex_lock(&g_lock);
    g_job = j;
    g_state = PM_WORKING;
    g_public[0] = g_problem[0] = 0;
    pthread_mutex_unlock(&g_lock);
    if (pthread_create(&g_thread, NULL, worker, j)) {
        pthread_mutex_lock(&g_lock);
        g_job = NULL;
        g_state = PM_IDLE;
        pthread_mutex_unlock(&g_lock);
        free(j);
    }
}

/* ends the current job: the mapping goes as soon as the job can; at exit (wait) for at most
 * EXIT_WAIT_MS, otherwise not at all (the lease expires by itself if the process is gone) */
void portmap_stop_wait(int wait) {
    pthread_mutex_lock(&g_lock);
    Job *j = g_job;
    if (!j) { pthread_mutex_unlock(&g_lock); return; }
    g_job = NULL;
    g_state = PM_IDLE;
    j->stop = 1;
    pthread_cond_broadcast(&g_wake);
    if (wait) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += (long)EXIT_WAIT_MS * 1000000L;
        until.tv_sec += until.tv_nsec / 1000000000L;
        until.tv_nsec %= 1000000000L;
        while (!j->done && pthread_cond_timedwait(&g_wake, &g_lock, &until) == 0) {}
    }
    int done = j->done;
    if (!done) j->detached = 1;             /* the job frees itself when it finishes */
    pthread_t t = g_thread;
    pthread_mutex_unlock(&g_lock);
    if (done) { pthread_join(t, NULL); free(j); }
    else pthread_detach(t);
}

void portmap_stop(void) { portmap_stop_wait(0); }

/* 0 idle, 1 working, 2 open, 3 failed; the public address and the problem, if any */
int portmap_status(char *pub, int publen, char *problem, int problemlen) {
    pthread_mutex_lock(&g_lock);
    int s = g_state;
    if (pub) snprintf(pub, (size_t)publen, "%s", g_public);
    if (problem) snprintf(problem, (size_t)problemlen, "%s", g_problem);
    pthread_mutex_unlock(&g_lock);
    return s;
}
