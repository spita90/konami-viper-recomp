/* <net/route.h> for the iOS SDK, which does not ship it: what third_party/libnatpmp/getgateway.c
 * reads of the routing table (sysctl NET_RT_FLAGS) to find the router, with the layout of Darwin's
 * routing messages (the same kernel as macOS, whose SDK has the header). iOS build only
 * (ios/CMakeLists.txt); if the sandbox refuses the sysctl, portmap.c goes on with UPnP. */
#pragma once
#include <stdint.h>
#include <sys/types.h>

struct rt_metrics {
    uint32_t rmx_locks, rmx_mtu, rmx_hopcount;
    int32_t rmx_expire;
    uint32_t rmx_recvpipe, rmx_sendpipe, rmx_ssthresh, rmx_rtt, rmx_rttvar, rmx_pksent;
    uint32_t rmx_state;
    uint32_t rmx_filler[3];
};

struct rt_msghdr {
    u_short rtm_msglen;
    u_char rtm_version;
    u_char rtm_type;
    u_short rtm_index;
    int rtm_flags;
    int rtm_addrs;
    pid_t rtm_pid;
    int rtm_seq;
    int rtm_errno;
    int rtm_use;
    uint32_t rtm_inits;
    struct rt_metrics rtm_rmx;
};

#define RTM_VERSION 5
#define RTM_GET 0x4
#define RTF_UP 0x1
#define RTF_GATEWAY 0x2
#define RTA_DST 0x1
#define RTA_GATEWAY 0x2
#define RTA_NETMASK 0x4
#define RTAX_DST 0
#define RTAX_GATEWAY 1
#define RTAX_NETMASK 2
#define RTAX_MAX 8
#define NET_RT_DUMP 1
#define NET_RT_FLAGS 2
