/* Netplay: two machines play one versus game over UDP, by direct IP, with
 * rollback.
 *
 * Both run the same game: the port computes the same frames on every
 * build and processor (the sync hash, src/port/sync.c), so two machines
 * that start from the same state and read the same pads each frame stay
 * the same. A session starts both from the state start-up left
 * (src/port/state.c), with the host's game settings, and walks the menus
 * to the versus character select on its own; from there each frame's
 * pads are the host's on port 0 (player 1) and the guest's on port 1.
 *
 * A frame does not wait for the other machine's pad: it runs on a guess
 * (the pad that machine held last), and when the real pad comes and
 * differs, the game goes back to that frame (src/port/history.c) and
 * runs the frames since again, unseen and unheard, before the next shown
 * one. Each machine's own pad is used a few frames late (the input
 * delay), which leaves the other's that long to arrive.
 *
 * Every packet carries this machine's pads not yet acknowledged, the
 * last frame of the other's it has, the frame it is on (the one ahead
 * waits a frame now and then to keep the two together), and now and then
 * the sync hash of a frame both have settled: two that differ end the
 * game (a desync).
 *
 * SF_NET_HOST=PORT or SF_NET_JOIN=ADDRESS[:PORT] start a session at
 * start-up, for tests. SF_NET_DELAY sets the input delay (frames);
 * SF_NET_LAG (ms) and SF_NET_LOSS (percent) hold back and drop this
 * machine's packets; SF_NET_FUZZ=SEED presses random pads;
 * SF_NET_FRAMES=N ends the program after frame N. */

#include "port/pad.h"
#include "port/port.h"

#include "types.h"
#include "structs.h"

int sf_net_settings(struct _SAVE_W *sw, SystemDir *dir);

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI /* its Polygon is the game's name for another thing */
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_NONE INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_NONE (-1)
#define sock_close close
#endif

#define NET_MAGIC 0x504E3353u /* "S3NP" */
#define NET_VERSION 1
#define RING 256          /* frames of pads and hashes kept */
#define MAX_AHEAD 8       /* frames run past the other's last pad */
#define SEND_MAX 40       /* pads in one packet */
#define HASH_EVERY 30     /* frames between sync hashes */
#define TIMEOUT_MS 6000   /* no packet that long: the other has gone */
#define PAD_BYTES 27

enum { PK_HELLO = 1, PK_WELCOME, PK_REJECT, PK_INPUT, PK_BYE };
enum { ST_OFF, ST_HOSTING, ST_JOINING, ST_PLAYING };
enum { REJ_VERSION = 1, REJ_DATA, REJ_IMAGE, REJ_BUSY };

extern struct _SAVE_W save_w[6];
extern SystemDir system_dir[6];

/* The menu walk from start-up to the versus character select: the same
 * presses on both machines (frames from the session's start). */
typedef struct Press {
    int frame;
    int port;
    uint16_t buttons;
    int analog; /* XB_A.. or -1 */
} Press;

static const Press k_walk[] = {
    { 300, 0, XB_START, -1 }, /* the Capcom logo */
    { 420, 0, XB_START, -1 }, /* the title: player 1 */
    { 440, 1, 0, XB_X }, /* player 2 joins (a pad joins on its first press) */
    { 560, 0, XB_DPAD_DOWN, -1 },
    { 600, 0, 0, XB_A }, /* VERSUS */
};
#define WALK_END 700

typedef struct Packet {
    uint8_t b[1400];
    int n;
} Packet;

typedef struct Delayed {
    Packet p;
    Uint64 due;
} Delayed;

static struct {
    int state;
    int host; /* this machine is player 1 */
    sock_t s;
    struct sockaddr_storage peer;
    socklen_t peer_n;
    int have_peer;
    uint32_t nonce;
    Uint64 last_heard;
    Uint64 last_hello;

    /* the settings the session plays on (the host's) */
    struct _SAVE_W save;
    SystemDir dir;
    int japanese;
    int settings_on;
    uint8_t guest_pad[sizeof(_PAD_INFOR)];

    int delay;
    int cur; /* the last frame run */
    int resim;

    XbGamepad local[RING];
    int local_last; /* the last frame this machine's pad is set for */
    int local_acked;

    XbGamepad remote[RING];
    int remote_last; /* the other's pads are here up to this frame */
    XbGamepad guess[RING]; /* the guess each frame ran on */
    int guessed[RING];
    int mismatch; /* the first frame run on a wrong guess, or 0 */
    int remote_frame;

    uint32_t hash[RING];
    int hash_sent;
    int rhash_frame;
    uint32_t rhash;

    int stall_wait;
    Uint32 rtt_ms;
    Uint32 echo;

    /* tests */
    int lag_ms;
    int loss;
    uint32_t fuzz;
    int fuzz_on;
    int end_at;
    Delayed out[64];
    int out_n;
    char status[160];
    char calling[64];
    int heard_input; /* the guest's pads have come: it has begun */
    int banner;
    int menu_at; /* the frame the game came back to the mode menu, or 0 */
    int back_held;
} s_n;

/* ---- the wire ---- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void pad_put(uint8_t *p, const XbGamepad *x)
{
    float f[4] = { x->lx, x->ly, x->rx, x->ry };
    int i;
    p[0] = (uint8_t)(x->connected != 0);
    p[1] = (uint8_t)x->buttons;
    p[2] = (uint8_t)(x->buttons >> 8);
    memcpy(p + 3, x->analog, 8);
    for (i = 0; i < 4; i++) {
        uint32_t u;
        memcpy(&u, &f[i], 4);
        put32(p + 11 + i * 4, u);
    }
}

static void pad_get(XbGamepad *x, const uint8_t *p)
{
    float *f[4] = { &x->lx, &x->ly, &x->rx, &x->ry };
    int i;
    memset(x, 0, sizeof *x);
    x->connected = p[0];
    x->buttons = (uint16_t)(p[1] | (p[2] << 8));
    memcpy(x->analog, p + 3, 8);
    for (i = 0; i < 4; i++) {
        uint32_t u = get32(p + 11 + i * 4);
        memcpy(f[i], &u, 4);
    }
}

static int pad_same(const XbGamepad *a, const XbGamepad *b)
{
    uint8_t x[PAD_BYTES];
    uint8_t y[PAD_BYTES];
    pad_put(x, a);
    pad_put(y, b);
    return memcmp(x, y, sizeof x) == 0;
}

static void pad_neutral(XbGamepad *x)
{
    memset(x, 0, sizeof *x);
    x->connected = 1;
}

/* What both machines must have alike to play one game: this code, the
 * structures' sizes, the disc, and the retail image for the tables the
 * game reads past (src/port/retail.c). */
static uint32_t compat(void)
{
    uint64_t h = sf_afs_identity();
    h = (h ^ NET_VERSION) * 0x100000001B3ull;
    h = (h ^ sizeof(struct _SAVE_W)) * 0x100000001B3ull;
    h = (h ^ sizeof(SystemDir)) * 0x100000001B3ull;
    return (uint32_t)(h ^ (h >> 32));
}

static void head(Packet *p, int type)
{
    put32(p->b, NET_MAGIC);
    p->b[4] = (uint8_t)type;
    put32(p->b + 5, s_n.nonce);
    p->n = 9;
}

static void raw_send(const Packet *p)
{
    if (s_n.s == SOCK_NONE || !s_n.have_peer) {
        return;
    }
    sendto(s_n.s, (const char *)p->b, p->n, 0, (const struct sockaddr *)&s_n.peer, s_n.peer_n);
}

static void send_packet(const Packet *p)
{
    if (s_n.loss > 0 && (int)(SDL_rand(100)) < s_n.loss) {
        return;
    }
    if (s_n.lag_ms > 0 && s_n.out_n < 64) {
        s_n.out[s_n.out_n].p = *p;
        s_n.out[s_n.out_n].due = SDL_GetTicks() + (Uint64)s_n.lag_ms;
        s_n.out_n++;
        return;
    }
    raw_send(p);
}

static void send_due(void)
{
    Uint64 now = SDL_GetTicks();
    int i = 0;
    while (i < s_n.out_n) {
        if (s_n.out[i].due <= now) {
            raw_send(&s_n.out[i].p);
            memmove(&s_n.out[i], &s_n.out[i + 1], (size_t)(s_n.out_n - i - 1) * sizeof s_n.out[0]);
            s_n.out_n--;
        } else {
            i++;
        }
    }
}

/* ---- the socket ---- */

static int sock_open(int port)
{
    struct sockaddr_in6 a6;
    struct sockaddr_in a4;
    int off = 0;
#ifdef _WIN32
    static int wsa;
    u_long nb = 1;
    if (!wsa) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
            return -1;
        }
        wsa = 1;
    }
#endif
    /* both IPv6 and IPv4 on one socket where the system allows */
    s_n.s = socket(AF_INET6, SOCK_DGRAM, 0);
    if (s_n.s != SOCK_NONE) {
        setsockopt(s_n.s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off);
        memset(&a6, 0, sizeof a6);
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_any;
        a6.sin6_port = htons((uint16_t)port);
        if (bind(s_n.s, (struct sockaddr *)&a6, sizeof a6) != 0) {
            sock_close(s_n.s);
            s_n.s = SOCK_NONE;
        }
    }
    if (s_n.s == SOCK_NONE) {
        s_n.s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s_n.s == SOCK_NONE) {
            return -1;
        }
        memset(&a4, 0, sizeof a4);
        a4.sin_family = AF_INET;
        a4.sin_addr.s_addr = htonl(INADDR_ANY);
        a4.sin_port = htons((uint16_t)port);
        if (bind(s_n.s, (struct sockaddr *)&a4, sizeof a4) != 0) {
            sock_close(s_n.s);
            s_n.s = SOCK_NONE;
            return -1;
        }
    }
#ifdef _WIN32
    ioctlsocket(s_n.s, FIONBIO, &nb);
#else
    fcntl(s_n.s, F_SETFL, fcntl(s_n.s, F_GETFL) | O_NONBLOCK);
#endif
    return 0;
}

/* The other machine's address, as the socket's family takes it. */
static int resolve(const char *text, int port)
{
    char host[256];
    char serv[16];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct sockaddr_storage mine;
    socklen_t mine_n = sizeof mine;
    const char *colon = strrchr(text, ':');
    int family;
    snprintf(host, sizeof host, "%s", text);
    /* ADDRESS:PORT, [V6]:PORT, or an address alone */
    if (host[0] == '[') {
        char *end = strchr(host, ']');
        if (end) {
            *end = 0;
            if (end[1] == ':') {
                port = atoi(end + 2);
            }
            memmove(host, host + 1, strlen(host + 1) + 1);
        }
    } else if (colon && strchr(text, ':') == colon) {
        host[colon - text] = 0;
        port = atoi(colon + 1);
    }
    snprintf(serv, sizeof serv, "%d", port);
    getsockname(s_n.s, (struct sockaddr *)&mine, &mine_n);
    family = mine.ss_family;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = family;
    hints.ai_socktype = SOCK_DGRAM;
    if (family == AF_INET6) {
        hints.ai_flags = AI_V4MAPPED | AI_ALL;
    }
    if (getaddrinfo(host, serv, &hints, &res) != 0 || !res) {
        return -1;
    }
    memcpy(&s_n.peer, res->ai_addr, res->ai_addrlen);
    s_n.peer_n = (socklen_t)res->ai_addrlen;
    s_n.have_peer = 1;
    freeaddrinfo(res);
    return 0;
}

/* ---- the session ---- */

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_n.status, sizeof s_n.status, fmt, ap);
    va_end(ap);
    fprintf(stderr, "net: %s\n", s_n.status);
}

/* The game starts again from start-up, both machines alike. */
static void begin(void)
{
    int i;
    sf_hist_stop();
    sf_state_boot_restore();
    sf_language_force(s_n.japanese);
    sf_port_frame = 0;
    s_n.cur = 0;
    s_n.local_last = 0;
    s_n.local_acked = 0;
    s_n.remote_last = 0;
    s_n.remote_frame = 0;
    s_n.mismatch = 0;
    s_n.hash_sent = 0;
    s_n.rhash_frame = 0;
    s_n.heard_input = 0;
    s_n.menu_at = 0;
    s_n.back_held = 0;
    for (i = 0; i < RING; i++) {
        pad_neutral(&s_n.local[i]);
        pad_neutral(&s_n.remote[i]);
        s_n.guessed[i] = 0;
    }
    /* the first frames' own pads, before the delay has any */
    for (i = 1; i <= s_n.delay; i++) {
        pad_neutral(&s_n.local[i % RING]);
    }
    s_n.local_last = s_n.delay;
    s_n.settings_on = 1;
    s_n.state = ST_PLAYING;
    s_n.last_heard = SDL_GetTicks();
    sf_hist_start(0);
    say("playing (input delay %d)", s_n.delay);
}

static void end(const char *why)
{
    Packet p;
    if (s_n.state == ST_OFF) {
        return;
    }
    head(&p, PK_BYE);
    raw_send(&p);
    say("%s", why);
    sf_hist_stop();
    s_n.state = ST_OFF;
    s_n.settings_on = 0;
    if (s_n.s != SOCK_NONE) {
        sock_close(s_n.s);
        s_n.s = SOCK_NONE;
    }
    s_n.have_peer = 0;
    /* this machine's own game again, from the title */
    sf_state_boot_restore();
    sf_language_force(sf_language_japanese());
    sf_port_frame = 0;
    s_n.banner = 300; /* said over the title for five seconds (sf_net_banner) */
    if (s_n.end_at) {
        exit(2);
    }
}

static void env_tests(void)
{
    const char *e;
    s_n.delay = (e = getenv("SF_NET_DELAY")) ? atoi(e) : 2;
    if (s_n.delay < 0 || s_n.delay > 8) {
        s_n.delay = 2;
    }
    s_n.lag_ms = (e = getenv("SF_NET_LAG")) ? atoi(e) : 0;
    s_n.loss = (e = getenv("SF_NET_LOSS")) ? atoi(e) : 0;
    s_n.fuzz_on = (e = getenv("SF_NET_FUZZ")) != NULL;
    s_n.fuzz = e ? (uint32_t)strtoul(e, NULL, 0) * 2654435761u + 1 : 0;
    s_n.end_at = (e = getenv("SF_NET_FRAMES")) ? atoi(e) : 0;
}

int sf_net_host(int port)
{
    memset(&s_n, 0, sizeof s_n);
    s_n.s = SOCK_NONE;
    env_tests();
    if (!sf_retail_image_ok()) {
        say("Import the disc again first.");
        return -1;
    }
    if (sock_open(port) != 0) {
        say("port %d could not be opened", port);
        return -1;
    }
    s_n.host = 1;
    s_n.state = ST_HOSTING;
    s_n.save = save_w[1];
    s_n.dir = system_dir[1];
    s_n.japanese = sf_language_japanese();
    say("waiting for a player on port %d", port);
    return 0;
}

/* Hosting with the page's input delay. */
int sf_net_host_delay(int port, int delay)
{
    if (sf_net_host(port) != 0) {
        return -1;
    }
    if (!getenv("SF_NET_DELAY")) {
        s_n.delay = delay;
    }
    return 0;
}

/* This machine's address on its network, as another machine there would
 * call it (the way out a packet would take; nothing is sent). */
void sf_net_local_address(char *out, size_t n)
{
    struct sockaddr_in to;
    struct sockaddr_in me;
    socklen_t me_n = sizeof me;
    sock_t s;
    snprintf(out, n, "?");
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
        inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)n);
    }
    sock_close(s);
}

int sf_net_join(const char *address)
{
    memset(&s_n, 0, sizeof s_n);
    s_n.s = SOCK_NONE;
    env_tests();
    if (!sf_retail_image_ok()) {
        say("Import the disc again first.");
        return -1;
    }
    if (sock_open(0) != 0 || resolve(address, SF_NET_PORT) != 0) {
        say("%s could not be found", address);
        if (s_n.s != SOCK_NONE) {
            sock_close(s_n.s);
        }
        s_n.s = SOCK_NONE;
        return -1;
    }
    s_n.host = 0;
    s_n.state = ST_JOINING;
    s_n.nonce = SDL_rand_bits();
    memcpy(s_n.guest_pad, &save_w[1].Pad_Infor[0], sizeof s_n.guest_pad);
    s_n.last_heard = SDL_GetTicks();
    snprintf(s_n.calling, sizeof s_n.calling, "%s", address);
    say("calling %s", address);
    return 0;
}

/* Before a game begins (waiting, or calling): no more. */
void sf_net_cancel(void)
{
    if (s_n.state == ST_HOSTING || s_n.state == ST_JOINING) {
        if (s_n.s != SOCK_NONE) {
            sock_close(s_n.s);
            s_n.s = SOCK_NONE;
        }
        s_n.have_peer = 0;
        s_n.state = ST_OFF;
    }
}

void sf_net_stop(void)
{
    if (s_n.state == ST_PLAYING) {
        end("The network game ended.");
    } else {
        sf_net_cancel();
    }
}

/* The program is closing: tell the other player, without going back to
 * the title. Sent a few times, as a lost goodbye leaves them waiting. */
void sf_net_quit(void)
{
    Packet p;
    int i;
    if (s_n.state == ST_PLAYING) {
        head(&p, PK_BYE);
        for (i = 0; i < 3; i++) {
            raw_send(&p);
        }
        if (s_n.s != SOCK_NONE) {
            sock_close(s_n.s);
            s_n.s = SOCK_NONE;
        }
        s_n.state = ST_OFF;
    }
    sf_net_cancel();
}

/* ---- packets in ---- */

static void on_input(const uint8_t *b, int n)
{
    int first;
    int count;
    int i;
    int at;
    if (n < 9 + 5) {
        return;
    }
    first = (int)get32(b + 9);
    count = b[13];
    at = 14;
    if (n < at + count * PAD_BYTES + 24) {
        return;
    }
    for (i = 0; i < count; i++, at += PAD_BYTES) {
        int f = first + i;
        if (f == s_n.remote_last + 1) {
            XbGamepad x;
            pad_get(&x, b + at);
            s_n.remote[f % RING] = x;
            s_n.remote_last = f;
            if (f <= s_n.cur && f >= WALK_END && s_n.guessed[f % RING] && !pad_same(&x, &s_n.guess[f % RING]) &&
                (s_n.mismatch == 0 || f < s_n.mismatch)) {
                s_n.mismatch = f;
            }
        }
    }
    {
        int ack = (int)get32(b + at);
        int frame = (int)get32(b + at + 4);
        int hf = (int)get32(b + at + 8);
        uint32_t hv = get32(b + at + 12);
        uint32_t ts = get32(b + at + 16);
        uint32_t echo = get32(b + at + 20);
        if (ack > s_n.local_acked) {
            s_n.local_acked = ack;
        }
        if (frame > s_n.remote_frame) {
            s_n.remote_frame = frame;
        }
        if (hf > s_n.rhash_frame) {
            s_n.rhash_frame = hf;
            s_n.rhash = hv;
        }
        s_n.echo = ts;
        if (echo) {
            s_n.rtt_ms = (Uint32)SDL_GetTicks() - echo;
        }
    }
}

static void on_packet(const uint8_t *b, int n, const struct sockaddr_storage *from, socklen_t from_n)
{
    Packet p;
    int type;
    if (n < 9 || get32(b) != NET_MAGIC) {
        return;
    }
    type = b[4];
    if (s_n.state == ST_HOSTING && type == PK_HELLO) {
        uint32_t want = compat();
        if (n < 9 + 4 + 2 + (int)sizeof s_n.guest_pad) {
            return;
        }
        memcpy(&s_n.peer, from, from_n);
        s_n.peer_n = from_n;
        s_n.have_peer = 1;
        s_n.nonce = get32(b + 5);
        head(&p, PK_REJECT);
        if (b[13] != NET_VERSION) {
            p.b[p.n++] = REJ_VERSION;
            raw_send(&p);
            s_n.have_peer = 0;
            return;
        }
        if (get32(b + 9) != want) {
            p.b[p.n++] = REJ_DATA;
            raw_send(&p);
            s_n.have_peer = 0;
            return;
        }
        memcpy(s_n.guest_pad, b + 15, sizeof s_n.guest_pad);
        memcpy(&s_n.save.Pad_Infor[1], s_n.guest_pad, sizeof s_n.guest_pad);
        say("a player joined");
        begin();
        s_n.last_hello = 0;
    }
    if (!s_n.have_peer || get32(b + 5) != s_n.nonce) {
        return; /* another session's, or a stranger's */
    }
    s_n.last_heard = SDL_GetTicks();
    switch (type) {
    case PK_WELCOME:
        if (s_n.state == ST_JOINING && n >= 9 + 2 + (int)sizeof(struct _SAVE_W) + (int)sizeof(SystemDir)) {
            s_n.delay = b[9];
            s_n.japanese = b[10];
            memcpy(&s_n.save, b + 11, sizeof s_n.save);
            memcpy(&s_n.dir, b + 11 + sizeof s_n.save, sizeof s_n.dir);
            say("joined");
            begin();
        }
        break;
    case PK_REJECT:
        if (s_n.state == ST_JOINING) {
            int why = n > 9 ? b[9] : 0;
            say("%s", why == REJ_VERSION ? "The other machine has another version."
                      : why == REJ_DATA  ? "The other machine has other game data."
                                         : "The other machine said no.");
            sf_net_cancel();
        }
        break;
    case PK_INPUT:
        if (s_n.state == ST_PLAYING) {
            s_n.heard_input = 1;
            on_input(b, n);
        }
        break;
    case PK_BYE:
        if (s_n.state == ST_PLAYING) {
            end("The other player left the game.");
        }
        break;
    default:
        break;
    }
}

static void receive(void)
{
    uint8_t b[1500];
    for (;;) {
        struct sockaddr_storage from;
        socklen_t from_n = sizeof from;
        int n = (int)recvfrom(s_n.s, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &from_n);
        if (n <= 0) {
            break;
        }
        on_packet(b, n, &from, from_n);
        if (s_n.state == ST_OFF) {
            break;
        }
    }
}

/* ---- packets out ---- */

static void send_hello(void)
{
    Packet p;
    head(&p, PK_HELLO);
    put32(p.b + p.n, compat());
    p.n += 4;
    p.b[p.n++] = NET_VERSION;
    p.b[p.n++] = 0;
    memcpy(p.b + p.n, s_n.guest_pad, sizeof s_n.guest_pad);
    p.n += (int)sizeof s_n.guest_pad;
    send_packet(&p);
}

static void send_welcome(void)
{
    Packet p;
    head(&p, PK_WELCOME);
    p.b[p.n++] = (uint8_t)s_n.delay;
    p.b[p.n++] = (uint8_t)s_n.japanese;
    memcpy(p.b + p.n, &s_n.save, sizeof s_n.save);
    p.n += (int)sizeof s_n.save;
    memcpy(p.b + p.n, &s_n.dir, sizeof s_n.dir);
    p.n += (int)sizeof s_n.dir;
    send_packet(&p);
}

static void send_input(void)
{
    Packet p;
    int first = s_n.local_acked + 1;
    int count = s_n.local_last - s_n.local_acked;
    int settled = s_n.cur < s_n.remote_last ? s_n.cur : s_n.remote_last;
    int hf;
    int i;
    if (count > SEND_MAX) {
        first = s_n.local_last - SEND_MAX + 1;
        count = SEND_MAX;
    }
    if (count < 0) {
        count = 0;
    }
    head(&p, PK_INPUT);
    put32(p.b + p.n, (uint32_t)first);
    p.b[p.n + 4] = (uint8_t)count;
    p.n += 5;
    for (i = 0; i < count; i++) {
        pad_put(p.b + p.n, &s_n.local[(first + i) % RING]);
        p.n += PAD_BYTES;
    }
    hf = settled - settled % HASH_EVERY;
    if (hf < 0 || settled - hf >= RING / 2) {
        hf = 0;
    }
    put32(p.b + p.n, (uint32_t)s_n.remote_last);
    put32(p.b + p.n + 4, (uint32_t)s_n.cur);
    put32(p.b + p.n + 8, (uint32_t)hf);
    put32(p.b + p.n + 12, hf ? s_n.hash[hf % RING] : 0);
    put32(p.b + p.n + 16, (uint32_t)SDL_GetTicks() | 1);
    put32(p.b + p.n + 20, s_n.echo);
    p.n += 24;
    send_packet(&p);
}

/* ---- the frames ---- */

static void fuzz_pad(XbGamepad *x)
{
    /* a held pattern that changes every few frames, like hands do */
    static XbGamepad held;
    static int left;
    if (left-- <= 0) {
        uint32_t r;
        s_n.fuzz = s_n.fuzz * 1664525u + 1013904223u;
        r = s_n.fuzz >> 8;
        pad_neutral(&held);
        held.buttons = (uint16_t)(r & 0xF);
        if (r & 0x10) {
            held.analog[(r >> 5) % 6] = 0xFF;
        }
        left = 2 + (int)((r >> 9) % 12);
    }
    *x = held;
}

/* xb_PADRead's pads in a netplay game: the frame's two players, or the
 * menu walk's presses (src/port/input.c). */
int sf_net_pads(XbGamepad *x, int ports)
{
    int f = sf_port_frame;
    int me = s_n.host ? 0 : 1;
    int other = 1 - me;
    int i;
    if (s_n.state != ST_PLAYING) {
        return 0;
    }
    for (i = 0; i < ports; i++) {
        memset(&x[i], 0, sizeof x[i]);
    }
    if (f < WALK_END) {
        pad_neutral(&x[0]);
        pad_neutral(&x[1]);
        for (i = 0; i < (int)(sizeof k_walk / sizeof k_walk[0]); i++) {
            if (f >= k_walk[i].frame && f < k_walk[i].frame + 6) {
                x[k_walk[i].port].buttons |= k_walk[i].buttons;
                if (k_walk[i].analog >= 0) {
                    x[k_walk[i].port].analog[k_walk[i].analog] = 0xFF;
                }
            }
        }
        s_n.guessed[f % RING] = 0;
        return 1;
    }
    x[me] = s_n.local[f % RING];
    if (f <= s_n.remote_last) {
        x[other] = s_n.remote[f % RING];
        s_n.guessed[f % RING] = 0;
    } else {
        /* not here yet: the last the other machine held */
        x[other] = s_n.remote[s_n.remote_last % RING];
        s_n.guess[f % RING] = x[other];
        s_n.guessed[f % RING] = 1;
    }
    return 1;
}

int sf_net_active(void)
{
    return s_n.state == ST_PLAYING || s_n.settings_on;
}

int sf_net_waiting(void)
{
    return s_n.state == ST_HOSTING || s_n.state == ST_JOINING;
}

int sf_net_resim(void)
{
    return s_n.resim;
}

/* The mode menu is running (src/port/netpage.c, from the menu): in a
 * session past the walk, the match is over. */
void sf_net_at_menu(void)
{
    if (s_n.state == ST_PLAYING && sf_port_frame > WALK_END && !s_n.menu_at) {
        s_n.menu_at = sf_port_frame;
    }
}

/* What the last session ended on, for a few seconds after (menu_ext.c
 * draws it); NULL when there is nothing to say. */
const char *sf_net_banner(void)
{
    if (s_n.banner <= 0 || s_n.state != ST_OFF) {
        return NULL;
    }
    s_n.banner--;
    return s_n.status;
}

const char *sf_net_status(void)
{
    return s_n.status;
}

/* The host's settings for the session's start-up (src/port/save.c). */
int sf_net_settings(struct _SAVE_W *sw, SystemDir *dir)
{
    if (!s_n.settings_on) {
        return 0;
    }
    *sw = s_n.save;
    *dir = s_n.dir;
    return 1;
}

static void run_frame(void)
{
    sf_game_frame();
    s_n.cur = sf_port_frame;
    s_n.hash[s_n.cur % RING] = sf_sync_hash();
    sf_hist_save(s_n.cur);
}

/* One turn of the main loop in a session (src/port/frame.c). */
void sf_net_tick(void)
{
    Uint64 now;
    int next;
    if (s_n.state == ST_OFF) {
        return;
    }
    receive();
    send_due();
    now = SDL_GetTicks(); /* after what came in (which sets last_heard) */
    if (s_n.state == ST_JOINING) {
        if (now - s_n.last_hello >= 250) {
            send_hello();
            s_n.last_hello = now;
        }
        if (now - s_n.last_heard > 10000) {
            say("No answer from %s.", s_n.calling);
            sf_net_cancel();
            return;
        }
        sf_game_frame(); /* this machine's own game (the page) meanwhile */
        return;
    }
    if (s_n.state == ST_HOSTING) {
        sf_game_frame();
        return;
    }
    if (s_n.state != ST_PLAYING) {
        return;
    }
    if (s_n.host && !s_n.heard_input && now - s_n.last_hello >= 100) {
        send_welcome(); /* until the guest's pads come */
        s_n.last_hello = now;
    }
    if (now - s_n.last_heard > TIMEOUT_MS) {
        end("The connection to the other player was lost.");
        return;
    }

    /* the walk to the character select: the same presses on both
     * machines, so run at once and unseen (as frames run again are) */
    if (s_n.cur < WALK_END) {
        int f;
        s_n.resim = 1;
        while (s_n.cur < WALK_END) {
            run_frame();
        }
        s_n.resim = 0;
        for (f = s_n.local_last + 1; f <= s_n.cur + s_n.delay; f++) {
            pad_neutral(&s_n.local[f % RING]);
        }
        if (s_n.local_last < s_n.cur + s_n.delay) {
            s_n.local_last = s_n.cur + s_n.delay;
        }
        /* neither machine's pads count before it: as good as had */
        if (s_n.remote_last < WALK_END) {
            s_n.remote_last = WALK_END;
        }
        if (s_n.local_acked < WALK_END) {
            s_n.local_acked = WALK_END;
        }
        send_input();
        return;
    }

    /* the other's real pads differ from a guess: back, and again */
    if (s_n.mismatch) {
        int target = s_n.mismatch - 1;
        int to = s_n.cur;
        s_n.mismatch = 0;
        if (s_n.menu_at > target) {
            s_n.menu_at = 0; /* that frame is run again: it may go otherwise */
        }
        if (sf_hist_load(target) != 0) {
            end("The game could not go back far enough (desync).");
            return;
        }
        sf_port_frame = target;
        s_n.resim = 1;
        while (sf_port_frame < to) {
            run_frame();
        }
        s_n.resim = 0;
    }

    /* a desync: a settled frame that hashes otherwise there */
    if (s_n.rhash_frame && s_n.rhash_frame <= s_n.cur && s_n.rhash_frame <= s_n.remote_last &&
        s_n.cur - s_n.rhash_frame < RING && s_n.hash[s_n.rhash_frame % RING] != s_n.rhash) {
        char why[160];
        snprintf(why, sizeof why, "The two games went apart at frame %d (desync).", s_n.rhash_frame);
        end(why);
        return;
    }

    /* the match is over: the game is back at the mode menu, on a frame
     * both machines have settled (so not on a guess) */
    if (s_n.menu_at && s_n.menu_at <= s_n.remote_last) {
        end("The network game is over.");
        return;
    }

    next = s_n.cur + 1;
    if (getenv("SF_NET_DEBUG")) {
        static Uint64 last;
        if (now - last >= 1000) {
            last = now;
            fprintf(stderr, "net: frame %d, theirs to %d, they are at %d, rtt %u ms, acked %d of %d\n", s_n.cur,
                    s_n.remote_last, s_n.remote_frame, s_n.rtt_ms, s_n.local_acked, s_n.local_last);
        }
    }
    /* too far past the other's pads, or ahead of the other machine: wait
     * a frame */
    if (next > s_n.remote_last + MAX_AHEAD ||
        (s_n.stall_wait <= 0 && next - (s_n.remote_frame + (int)(s_n.rtt_ms * 60 / 2000)) > 2)) {
        if (next <= s_n.remote_last + MAX_AHEAD) {
            s_n.stall_wait = 20; /* not again for a while */
        }
        send_input();
        sf_video_wait_frame();
        return;
    }
    if (s_n.stall_wait > 0) {
        s_n.stall_wait--;
    }

    /* this machine's pad, for a few frames on */
    if (s_n.local_last < next + s_n.delay) {
        XbGamepad x;
        if (s_n.fuzz_on && next >= WALK_END) {
            fuzz_pad(&x);
        } else {
            sf_input_local(&x);
        }
        /* BACK held two seconds: leave the game */
        s_n.back_held = (x.buttons & XB_BACK) ? s_n.back_held + 1 : 0;
        if (s_n.back_held >= 120 && !s_n.fuzz_on) {
            end("You left the network game.");
            return;
        }
        s_n.local_last = next + s_n.delay;
        s_n.local[s_n.local_last % RING] = x;
    }
    run_frame();
    send_input();
    if (s_n.end_at && s_n.cur >= s_n.end_at) {
        fprintf(stderr, "net: frame %d reached\n", s_n.cur);
        sf_net_quit();
        exit(0);
    }
}

/* SF_NET_HOST / SF_NET_JOIN: a session from start-up. */
void sf_net_env(void)
{
    const char *h = getenv("SF_NET_HOST");
    const char *j = getenv("SF_NET_JOIN");
    if (h) {
        sf_net_host(*h ? atoi(h) : SF_NET_PORT);
    } else if (j) {
        sf_net_join(j);
    }
}
