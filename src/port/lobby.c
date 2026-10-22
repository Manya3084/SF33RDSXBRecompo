/* Games waiting on this network (the NETWORK page's LOBBY row).
 *
 * A host that is waiting for a player announces itself a few times a
 * second, on UDP port 33134. The packet names the one address the host
 * screen shows (the way out toward the internet). A machine with two
 * addresses on the same network still appears once, and a join calls
 * that address. The match itself is unchanged (src/port/net.c).
 *
 * This reaches machines that share a local network. A guest network that
 * keeps its machines from hearing each other will not. The typed address
 * remains for a machine that is somewhere else. */

#include "port/port.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_NONE INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_NONE (-1)
#define sock_close close
#endif

#define LOBBY_PORT 33134
#define LOBBY_MAX 8
#define LOBBY_EXPIRE_MS 3000
#define LOBBY_EVERY_MS 500

/* "S3LB", then version 1, then the match port, little endian. */
static const uint8_t k_magic[4] = { 0x53, 0x33, 0x4C, 0x42 };

static struct {
    sock_t s;
    int failed;
    uint64_t sent;
    char self[64];
    struct {
        char addr[32];
        uint64_t heard;
    } game[LOBBY_MAX];
    int n;
} s_lb = { .s = SOCK_NONE };

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* The address another machine on this network would call. A beacon that
 * comes from it is this machine's own announcement. */
static void detect_self(void)
{
    struct sockaddr_in to;
    struct sockaddr_in me;
    socklen_t me_n = sizeof me;
    sock_t s;
    s_lb.self[0] = 0;
#ifdef _WIN32
    {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
    }
#endif
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_NONE) {
        return;
    }
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808u);
    if (connect(s, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(s, (struct sockaddr *)&me, &me_n) == 0) {
        inet_ntop(AF_INET, &me.sin_addr, s_lb.self, sizeof s_lb.self);
    }
    sock_close(s);
}

static int lobby_open(void)
{
    struct sockaddr_in a;
    int on = 1;
#ifdef _WIN32
    u_long nb = 1;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
        return -1;
    }
#endif
    if (s_lb.s != SOCK_NONE) {
        return 0;
    }
    s_lb.s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_lb.s == SOCK_NONE) {
        return -1;
    }
    setsockopt(s_lb.s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
#ifdef SO_REUSEPORT
    /* every machine here listens on the same port, including two copies
     * on one computer */
    setsockopt(s_lb.s, SOL_SOCKET, SO_REUSEPORT, (const char *)&on, sizeof on);
#endif
    setsockopt(s_lb.s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(LOBBY_PORT);
    if (bind(s_lb.s, (struct sockaddr *)&a, sizeof a) != 0) {
        sock_close(s_lb.s);
        s_lb.s = SOCK_NONE;
        return -1;
    }
#ifdef _WIN32
    ioctlsocket(s_lb.s, FIONBIO, &nb);
#else
    fcntl(s_lb.s, F_SETFL, fcntl(s_lb.s, F_GETFL, 0) | O_NONBLOCK);
#endif
    if (!s_lb.self[0]) {
        detect_self();
    }
    return 0;
}

static void drop_expired(uint64_t now)
{
    int i = 0;
    while (i < s_lb.n) {
        if (now - s_lb.game[i].heard > LOBBY_EXPIRE_MS) {
            memmove(&s_lb.game[i], &s_lb.game[i + 1], (size_t)(s_lb.n - i - 1) * sizeof s_lb.game[0]);
            s_lb.n--;
        } else {
            i++;
        }
    }
}

/* An address of this machine, other than loopback. Loopback is how the
 * test on this computer pretends to be someone else. */
static int our_address(const char *ip)
{
#ifndef _WIN32
    struct ifaddrs *ifs = NULL;
    struct ifaddrs *p;
#endif
    if (!ip || !ip[0] || strcmp(ip, "127.0.0.1") == 0) {
        return 0;
    }
    if (s_lb.self[0] && strcmp(ip, s_lb.self) == 0) {
        return 1;
    }
#ifndef _WIN32
    if (getifaddrs(&ifs) != 0) {
        return 0;
    }
    for (p = ifs; p; p = p->ifa_next) {
        char buf[INET_ADDRSTRLEN];
        struct sockaddr_in *a;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        a = (struct sockaddr_in *)p->ifa_addr;
        if ((ntohl(a->sin_addr.s_addr) >> 24) == 127) {
            continue;
        }
        if (inet_ntop(AF_INET, &a->sin_addr, buf, sizeof buf) && strcmp(buf, ip) == 0) {
            freeifaddrs(ifs);
            return 1;
        }
    }
    freeifaddrs(ifs);
#endif
    return 0;
}

static void remember(const char *addr, uint64_t now)
{
    int i;
    for (i = 0; i < s_lb.n; i++) {
        if (strcmp(s_lb.game[i].addr, addr) == 0) {
            s_lb.game[i].heard = now;
            return;
        }
    }
    if (s_lb.n >= LOBBY_MAX) {
        return;
    }
    snprintf(s_lb.game[s_lb.n].addr, sizeof s_lb.game[0].addr, "%s", addr);
    s_lb.game[s_lb.n].heard = now;
    s_lb.n++;
}

static void receive(uint64_t now)
{
    uint8_t b[32];
    struct sockaddr_in from;
    socklen_t from_n;
    char ip[INET_ADDRSTRLEN];
    char addr[32];
    int port;
    int n;
    for (;;) {
        from_n = sizeof from;
        n = (int)recvfrom(s_lb.s, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &from_n);
        if (n < 0) {
            return;
        }
        if (n < 7 || memcmp(b, k_magic, 4) != 0 || b[4] != 1) {
            continue;
        }
        if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip)) {
            continue;
        }
        /* The loopback copy of our own beacon. Another program on this
         * machine sends from some other port, and that one counts. */
        if (strcmp(ip, "127.0.0.1") == 0 && ntohs(from.sin_port) == LOBBY_PORT) {
            continue;
        }
        /* The address to call, when the packet carries one. Otherwise the
         * address it came from. Two interfaces of one host then stay one row. */
        if (n >= 11) {
            struct in_addr announced;
            memcpy(&announced, b + 7, 4);
            if (announced.s_addr != 0 && announced.s_addr != htonl(INADDR_BROADCAST)) {
                if (!inet_ntop(AF_INET, &announced, ip, sizeof ip)) {
                    continue;
                }
            }
        }
        if (our_address(ip)) {
            continue;
        }
        port = b[5] | (b[6] << 8);
        if (port <= 0 || port > 65535) {
            continue;
        }
        if (port == SF_NET_PORT) {
            snprintf(addr, sizeof addr, "%s", ip);
        } else {
            snprintf(addr, sizeof addr, "%s:%d", ip, port);
        }
        remember(addr, now);
    }
}

static void send_to(const uint8_t *b, int n, uint32_t dest)
{
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(LOBBY_PORT);
    to.sin_addr.s_addr = dest;
    sendto(s_lb.s, (const char *)b, n, 0, (struct sockaddr *)&to, sizeof to);
}

#ifndef _WIN32
/* Out of one interface, with that interface's address as the source.
 * Sending to 255.255.255.255 leaves a copy on every interface, and a
 * machine with two addresses shows up twice. */
static void send_via(const uint8_t *b, int n, uint32_t dest, uint32_t src, unsigned ifindex)
{
    struct msghdr msg;
    struct iovec iov;
    struct sockaddr_in to;
    char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
    struct cmsghdr *cmsg;
    struct in_pktinfo *pkt;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(LOBBY_PORT);
    to.sin_addr.s_addr = dest;
    iov.iov_base = (void *)b;
    iov.iov_len = (size_t)n;
    memset(&msg, 0, sizeof msg);
    memset(cbuf, 0, sizeof cbuf);
    msg.msg_name = &to;
    msg.msg_namelen = sizeof to;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof cbuf;
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = IPPROTO_IP;
    cmsg->cmsg_type = IP_PKTINFO;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
    pkt = (struct in_pktinfo *)CMSG_DATA(cmsg);
    memset(pkt, 0, sizeof *pkt);
    pkt->ipi_ifindex = ifindex;
    pkt->ipi_spec_dst.s_addr = src;
    if (sendmsg(s_lb.s, &msg, 0) < 0) {
        send_to(b, n, dest);
    }
}

/* 1 when the broadcast left the interface that owns s_lb.self. */
static int send_on_self(const uint8_t *b, int n)
{
    struct ifaddrs *ifs = NULL;
    struct ifaddrs *p;
    int sent = 0;
    if (!s_lb.self[0] || getifaddrs(&ifs) != 0) {
        return 0;
    }
    for (p = ifs; p; p = p->ifa_next) {
        char buf[INET_ADDRSTRLEN];
        struct sockaddr_in *a;
        struct sockaddr_in *br;
        unsigned idx;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_broadaddr || !p->ifa_name) {
            continue;
        }
        if ((p->ifa_flags & (IFF_UP | IFF_BROADCAST)) != (IFF_UP | IFF_BROADCAST)) {
            continue;
        }
        a = (struct sockaddr_in *)p->ifa_addr;
        if (!inet_ntop(AF_INET, &a->sin_addr, buf, sizeof buf) || strcmp(buf, s_lb.self) != 0) {
            continue;
        }
        br = (struct sockaddr_in *)p->ifa_broadaddr;
        idx = if_nametoindex(p->ifa_name);
        if (idx != 0 && br->sin_addr.s_addr != 0) {
            send_via(b, n, br->sin_addr.s_addr, a->sin_addr.s_addr, idx);
        } else if (br->sin_addr.s_addr != 0) {
            send_to(b, n, br->sin_addr.s_addr);
        }
        sent = 1;
        break;
    }
    freeifaddrs(ifs);
    return sent;
}
#endif

/* The host screen's address, on that interface's broadcast, plus the
 * loopback broadcast so a second copy on this computer hears it. */
static void announce(int match_port, uint64_t now)
{
    uint8_t b[11];
    int len = 7;
#ifndef _WIN32
    struct ifaddrs *ifs = NULL;
    struct ifaddrs *p;
#endif
    if (s_lb.sent && now - s_lb.sent < LOBBY_EVERY_MS) {
        return;
    }
    s_lb.sent = now;
    memcpy(b, k_magic, 4);
    b[4] = 1;
    b[5] = (uint8_t)(match_port & 0xFF);
    b[6] = (uint8_t)((match_port >> 8) & 0xFF);
    if (s_lb.self[0] && inet_pton(AF_INET, s_lb.self, b + 7) == 1) {
        len = 11;
    }
    send_to(b, len, htonl(INADDR_LOOPBACK | 0x00FFFFFFu)); /* 127.255.255.255 */
#ifndef _WIN32
    if (send_on_self(b, len)) {
        return;
    }
    /* No chosen address: every interface, so a machine somewhere hears it. */
    send_to(b, len, htonl(INADDR_BROADCAST));
    if (getifaddrs(&ifs) == 0) {
        for (p = ifs; p; p = p->ifa_next) {
            struct sockaddr_in *br;
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_broadaddr) {
                continue;
            }
            if ((p->ifa_flags & (IFF_UP | IFF_BROADCAST)) != (IFF_UP | IFF_BROADCAST)) {
                continue;
            }
            br = (struct sockaddr_in *)p->ifa_broadaddr;
            if (br->sin_addr.s_addr != 0 && br->sin_addr.s_addr != htonl(INADDR_BROADCAST)) {
                send_to(b, len, br->sin_addr.s_addr);
            }
        }
        freeifaddrs(ifs);
    }
#else
    send_to(b, len, htonl(INADDR_BROADCAST));
#endif
}

void sf_lobby_poll(int announce_on, int match_port)
{
    uint64_t now;
    if (s_lb.s == SOCK_NONE && lobby_open() != 0) {
        if (!s_lb.failed) {
            fprintf(stderr, "lobby: port %d could not be opened\n", LOBBY_PORT);
            s_lb.failed = 1;
        }
        return;
    }
    now = now_ms();
    drop_expired(now);
    receive(now);
    if (announce_on && match_port > 0) {
        announce(match_port, now);
    }
}

void sf_lobby_close(void)
{
    if (s_lb.s != SOCK_NONE) {
        sock_close(s_lb.s);
        s_lb.s = SOCK_NONE;
    }
    s_lb.sent = 0;
    s_lb.n = 0;
    s_lb.failed = 0;
}

int sf_lobby_count(void)
{
    return s_lb.n;
}

int sf_lobby_address(int index, char *out, size_t n)
{
    if (!out || n == 0 || index < 0 || index >= s_lb.n) {
        return -1;
    }
    snprintf(out, n, "%s", s_lb.game[index].addr);
    return 0;
}
