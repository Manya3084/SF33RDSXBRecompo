/* A beacon from another address shows up in the lobby, a beacon from
 * this machine does not, and a game that stops announcing drops out. */

#include "port/port.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int s_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "fail: %s\n", msg);
        s_fail = 1;
    }
}

static void send_beacon(int match_port)
{
    int s;
    int on = 1;
    uint8_t b[7] = { 0x53, 0x33, 0x4C, 0x42, 1, 0, 0 };
    struct sockaddr_in to;
    b[5] = (uint8_t)(match_port & 0xFF);
    b[6] = (uint8_t)((match_port >> 8) & 0xFF);
    s = socket(AF_INET, SOCK_DGRAM, 0);
    expect(s >= 0, "probe socket");
    if (s < 0) {
        return;
    }
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(33134);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    expect(sendto(s, b, sizeof b, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof b, "send beacon");
    close(s);
}

int main(void)
{
    char addr[64];
    int found;
    int i;
    int r;
    int on = 1;
    uint8_t got[16];
    struct sockaddr_in a;
    struct timeval tv;

    sf_lobby_poll(0, 33133);
    send_beacon(33133);
    sf_lobby_poll(0, 33133);
    expect(sf_lobby_count() == 1, "one game");
    expect(sf_lobby_address(0, addr, sizeof addr) == 0, "address");
    expect(strcmp(addr, "127.0.0.1") == 0, "loopback, default port left off");

    send_beacon(40000);
    sf_lobby_poll(0, 33133);
    found = 0;
    for (i = 0; i < sf_lobby_count(); i++) {
        if (sf_lobby_address(i, addr, sizeof addr) == 0 && strcmp(addr, "127.0.0.1:40000") == 0) {
            found = 1;
        }
    }
    expect(found, "a different match port is kept");

    /* This machine's own announcement is not a game to join. */
    sf_lobby_poll(1, 33133);
    for (i = 0; i < sf_lobby_count(); i++) {
        sf_lobby_address(i, addr, sizeof addr);
        expect(strcmp(addr, "127.0.0.1") == 0 || strcmp(addr, "127.0.0.1:40000") == 0, "own beacon stays out");
    }

    r = socket(AF_INET, SOCK_DGRAM, 0);
    expect(r >= 0, "hear socket");
    setsockopt(r, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
    setsockopt(r, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(33134);
    expect(bind(r, (struct sockaddr *)&a, sizeof a) == 0, "bind next to the lobby");
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(r, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sf_lobby_poll(1, 33133); /* the last one was under a second ago: send again after the gap */
    usleep(600000);
    sf_lobby_poll(1, 33133);
    found = 0;
    for (i = 0; i < 4; i++) {
        ssize_t n = recv(r, got, sizeof got, 0);
        if (n >= 7 && got[0] == 0x53 && got[1] == 0x33 && got[2] == 0x4C && got[3] == 0x42 && got[4] == 1) {
            found = 1;
            break;
        }
    }
    expect(found, "a host's announcement is heard on the network");
    close(r);

    /* The address inside the packet is the one to call, not the sender. */
    {
        uint8_t named[11] = { 0x53, 0x33, 0x4C, 0x42, 1, 0, 0, 0, 0, 0, 0 };
        struct in_addr want;
        int s;
        struct sockaddr_in to;
        named[5] = (uint8_t)(33133 & 0xFF);
        named[6] = (uint8_t)((33133 >> 8) & 0xFF);
        expect(inet_pton(AF_INET, "10.9.8.7", &want) == 1, "address bytes");
        memcpy(named + 7, &want, 4);
        s = socket(AF_INET, SOCK_DGRAM, 0);
        expect(s >= 0, "named socket");
        if (s >= 0) {
            memset(&to, 0, sizeof to);
            to.sin_family = AF_INET;
            to.sin_port = htons(33134);
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            expect(sendto(s, named, sizeof named, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof named,
                   "send named beacon");
            close(s);
        }
        sf_lobby_poll(0, 33133);
        found = 0;
        for (i = 0; i < sf_lobby_count(); i++) {
            if (sf_lobby_address(i, addr, sizeof addr) == 0 && strcmp(addr, "10.9.8.7") == 0) {
                found = 1;
            }
        }
        expect(found, "the address in the packet is listed");
    }

    sleep(4);
    sf_lobby_poll(0, 33133);
    expect(sf_lobby_count() == 0, "a silent game drops out");
    sf_lobby_close();
    return s_fail ? 1 : 0;
}
