/* The link's transport through Network.framework, peer to peer (runtime/net_p2p.m): VIPER_P2P,
 * the iOS app (and a macOS build to test it, make P2P=1). net.c keeps its datagrams and its
 * addresses: each connection stands as an address of its own (see p2p_send). */
#pragma once
#include <stddef.h>
#include <sys/socket.h>

#define P2P_SERVICE "_viperlink._udp"       /* Bonjour (the iOS app's NSBonjourServices) */

int p2p_host(const char *name);             /* listen, advertised nearby as `name`; 0 if started */
int p2p_join(const char *name);             /* connect to the game advertised as `name`; 0 if found */
void p2p_close(void);                       /* every connection, the listener */
/* datagrams with the connection they came from or go to, as an address; -1 if none / not sent */
int p2p_send(const struct sockaddr_storage *to, const void *p, int len);
int p2p_recv(void *buf, int max, struct sockaddr_storage *from, socklen_t *len);
void p2p_host_addr(struct sockaddr_storage *a, socklen_t *len);     /* a member: the host's */
/* looking for games nearby (the JOIN screen): their Bonjour names, as found so far */
void p2p_browse(int on);
int p2p_games(char names[][64], int max);
int p2p_problem(void);                      /* 0 none, 1 local network access denied, 2 failed */
