/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - netplay.c                                               *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2020 loganmc10                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define SETTINGS_SIZE 24
#define NETPLAY_MAX_PENDING_EVENTS 255

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "main.h"
#include "util.h"
#include "plugin/plugin.h"
#include "backends/plugins_compat/plugins_compat.h"
#include "netplay.h"

#include <mupen64plus-next_common.h>
#include <libretro_private.h>

/* Joey64 (2026-11-10): POSIX sockets instead of SDL_net, and no SDL threads, so the libretro core can
 * build netplay in. The wire format is the mupen64plus netplay protocol as before. */
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { int len; uint8_t data[512]; } np_packet;

static int l_udpChannel;
static int np_udp = -1, np_tcp = -1;

static void np_w32(void *p, uint32_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
}
static uint32_t np_r32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static uint32_t np_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static uint64_t np_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

//Joey64 (2026-11-10): connection numbers, logged every 5 s in a room so a session log shows a bad connection:
//round trip of input requests (sent to the server's answer), the waits for input (a frame that couldn't go on),
//input events heard, copies heard again (resends and gratuitous ones), requests sent, and the server's lag.
static struct {
    uint64_t req_at[4];          // when the oldest unanswered request for that player went out (us), 0 = none
    uint64_t rtt_sum, rtt_max;   // us
    uint32_t rtt_n;
    uint32_t waits, wait_ms, wait_max;
    uint32_t heard, dup, sent_req, packets;
    uint32_t own;                // our own inputs placed at once (UDP_JOEY_OWN_PLAN)
    uint32_t since;
} l_ns;
static void np_udp_send(np_packet *p)
{
    if (np_udp >= 0) send(np_udp, p->data, p->len, 0);
}
static int np_udp_recv(np_packet *p)
{
    if (np_udp < 0) return 0;
    ssize_t n = recv(np_udp, p->data, sizeof(p->data), MSG_DONTWAIT);
    if (n < 0) return 0;
    p->len = (int)n;
    return 1;
}
static int np_tcp_send(const void *d, int n)
{
    const char *c = (const char *)d; int done = 0;
    while (np_tcp >= 0 && done < n) {
        ssize_t k = send(np_tcp, c + done, n - done, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { log_cb(RETRO_LOG_ERROR, "Netplay: TCP send failed\n"); l_udpChannel = -1; break; }
        done += (int)k;
    }
    return done;
}
/* The callers loop until they have n bytes; on a closed or failed connection the rest is zeros and
 * the connection counts as lost (the next input wait stops the game). */
static int np_tcp_recv(void *d, int n)
{
    char *c = (char *)d;
    while (np_tcp >= 0) {
        ssize_t k = recv(np_tcp, c, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k > 0) return (int)k;
        break;
    }
    log_cb(RETRO_LOG_ERROR, "Netplay: TCP connection lost\n");
    l_udpChannel = -1;
    memset(c, 0, n);
    return n;
}

/* Joey64 (2026-11-10): the app is leaving while the core waits on the room (the ROM check, registration until
 * every player is in, settings, input). Shutting the TCP socket ends a blocked recv at once and the connection
 * counts as lost, so the game stops instead of waiting up to the room's 30 s (or for ever on a room that never
 * answers). Called from the app's thread; the emulation thread closes the sockets as before. */
__attribute__((visibility("default"))) void joey64_netplay_abort(void)
{
    int t = np_tcp;
    l_udpChannel = -1;
    if (t >= 0)
        shutdown(t, SHUT_RDWR);
}
#define SDLNet_Write32(v, p) np_w32((p), (uint32_t)(v))
#define SDLNet_Read32(p) np_r32(p)
#define SDLNet_UDP_Send(s, ch, p) np_udp_send(p)
#define SDLNet_UDP_Recv(s, p) np_udp_recv(p)
#define SDLNet_TCP_Send(s, d, n) np_tcp_send((d), (n))
#define SDLNet_TCP_Recv(s, d, n) np_tcp_recv((d), (n))

static int l_canFF;
static int l_netplay_controller;
static int l_netplay_control[4];
static int l_udpSocket = -1;
static int l_tcpSocket = -1;
static int l_spectator;
static int l_netplay_is_init = 0;
static uint32_t l_vi_counter;
static uint8_t l_status;
static uint32_t l_reg_id;
static struct controller_input_compat *l_cin_compats;
static uint8_t l_plugin[4];
static uint8_t l_buffer_target;
static uint8_t l_player_lag[4];
//Joey64: when the server last sent us anything (see netplay_ensure_valid)
static uint32_t l_last_heard;
//Joey64: our last inputs, sent again (see netplay_send_input). More than a Joey64 room's largest input delay (12):
//an input lost on the way is needed delay frames later, and only the last NP_RESEND are ever sent again.
#define NP_RESEND 20
//Joey64 (2026-11-10): [8, player, n, n x (count32, keys32, plugin)], several inputs in one packet (Joey64 rooms)
#define UDP_JOEY_KEY_BATCH 8
static struct { uint32_t count, keys; uint8_t plugin; } l_sent[4][NP_RESEND];
static uint8_t l_sent_n[4], l_sent_pos[4];
//Joey64 (2026-11-13): [9, player, R32, offset at R, H32, n, n x (count32, change int8)], from a Joey64 room to the
//game playing that player after each of its inputs: where they go. An input sent at count c goes to slot c + its
//offset, the offset at R plus every change at a count after R up to c; a change of +a at X also fills the a slots
//after count X - 1's with copies of it, one of -1 at X drops count X's input. The room plans no change below H, so
//for R <= c < H we place our own input in our own list as we send it: it no longer waits a round trip through the
//room (the joiner's own inputs were the ones that set the room's delay). N64_NETPLAY_LOCAL=0 turns it off.
#define UDP_JOEY_OWN_PLAN 9
static struct { int have; uint32_t r, h; int off_r; uint8_t n; uint32_t at[12]; int8_t ch[12]; } l_plan[4];
static int l_own_local = 1;
static int np_after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

//Joey64 (2026-11-24, rollback prototype; Joey: "Yes, prototype it"; dev switch N64_NETPLAY_ROLLBACK=<counts>, 2-60):
//an input not here yet is guessed (that player's newest one heard) instead of waited for, up to <counts> past the
//first count still missing. Every input used, heard or guessed, is kept by count in l_hist. When one heard later
//differs from the guess used, l_rb_bad keeps the earliest such count; the app then loads the state it took before
//that count (joey64_netplay_rb_mark / _restore) and runs the frames again with what was heard. While it does
//(l_rb_replaying) nothing goes out: our inputs went the first time, and requests and the desync check wait.
#define NP_HIST 256
struct np_hist { uint32_t count, keys, used_keys; uint8_t real, used, plugin, used_plugin; };
static struct np_hist l_hist[4][NP_HIST];
static int l_rb_window;
static int l_rb_replaying;
static uint32_t l_rb_done[4];                      // every input below this count is heard
static uint32_t l_rb_last_keys[4], l_rb_last_count[4];
static uint8_t l_rb_last_plugin[4], l_rb_have_last[4];
static uint8_t l_rb_bad_mask;
static uint32_t l_rb_bad[4];
static uint64_t l_rb_rtt_us;                       // smoothed round trip of input requests
static uint32_t l_rb_stall_vi;                     // the VI of the last frame held back to let the others catch up
static uint32_t l_rb_floor[4];                     // the input counts of the oldest state the app keeps
static int l_rb_floor_set;
//One input per player per frame with rollback (the room's counts are frames): a game reads the pad 0, 1 or more times
//a frame, so counts per read made "how far back" in inputs differ from frames (Mario Kart 64 reads every other frame;
//wrong guesses older than every kept state, and both games holding frames for each other). Reads in a frame all get
//that frame's input; the frame's end uses (and sends our own) one for any player the game didn't read.
static uint8_t l_rb_read[4], l_rb_sent[4];
static uint32_t l_rb_frame_keys[4], l_rb_own_last[4];
static struct { uint32_t guessed, wrong, stalls, waits; } l_rbs;
//the desync check's CP0 copy waits until every input before it is heard and none was guessed wrong
static struct { int have; uint32_t vi, counts[4]; int len; uint8_t data[CP0_REGS_COUNT * 4 + 5]; } l_rb_sync;
static uint32_t l_rb_sync_sent_vi;
static int l_rb_sync_sent;
//Time sync (2026-11-24, rb12): each game's lead over player p is its count less the newest input heard from p, averaged
//over 10 frames. Alone it is biased by the network's one-way time (each game took itself to be ahead: both held a frame
//every 20 VIs, the joiner's stutters). So games swap theirs through the room ([10, player, 0, 0, 4 x int16 lead*16
//over players 1-4]); half the difference between ours over p and p's over us is how far ahead we really are, the
//one-way times cancelling as in GGPO's frame advantage.
#define UDP_JOEY_LEAD 10
static int32_t l_rb_lead_sum[4];
static uint32_t l_rb_lead_n, l_rb_lead_frames;
static int16_t l_rb_my_lead[4];          // our averaged lead over each player, x16
static int16_t l_rb_their_lead[4];       // player q's averaged lead over us, x16
static uint8_t l_rb_their_have[4], l_rb_my_have;
static uint32_t l_rb_their_frame[4];     // our frame count when q's lead came
static int32_t l_rb_ahead16[4];          // the last advantage worked out, x16 (for the log)
static uint32_t l_rb_prev_last[4], l_rb_quiet[4], l_rb_quiet_max[4]; // frames without a newer input heard (log)

static struct np_hist *hist_at(uint8_t p, uint32_t c)
{
    struct np_hist *h = &l_hist[p][c % NP_HIST];
    return h->count == c && (h->real || h->used) ? h : NULL;
}

static struct np_hist *hist_new(uint8_t p, uint32_t c)
{
    struct np_hist *h = &l_hist[p][c % NP_HIST];
    if (h->count != c || !(h->real || h->used))
    {
        memset(h, 0, sizeof *h);
        h->count = c;
    }
    return h;
}

//a heard input (from the room, or our own placed where the room puts it)
static void rb_put(uint8_t p, uint32_t c, uint32_t keys, uint8_t plugin)
{
    const uint32_t n = l_cin_compats[p].netplay_count;
    if ((int32_t)(c - n) >= NP_HIST / 2 || (int32_t)(n - c) >= NP_HIST / 2)
        return; // outside what the history holds
    struct np_hist *h = hist_new(p, c);
    if (h->real)
    {
        ++l_ns.dup;
        return;
    }
    h->real = 1;
    h->keys = keys;
    h->plugin = plugin;
    ++l_ns.heard;
    if (h->used && (h->used_keys != keys || h->used_plugin != plugin))
    {
        if (!(l_rb_bad_mask & (1u << p)) || np_after(l_rb_bad[p], c))
            l_rb_bad[p] = c;
        l_rb_bad_mask |= (uint8_t)(1u << p);
        ++l_rbs.wrong;
        if ((int32_t)(n - c) > 20)
            log_cb(RETRO_LOG_INFO, "Netplay: rollback: late wrong guess for player %u: count %u heard at %u, all heard below %u, floor %u, replaying %d\n",
                   p + 1, c, n, l_rb_done[p], l_rb_floor[p], l_rb_replaying);
    }
    if (!l_rb_have_last[p] || np_after(c, l_rb_last_count[p]))
    {
        if (l_rb_have_last[p] && (int32_t)(c - l_rb_last_count[p]) > 15)
            log_cb(RETRO_LOG_INFO, "Netplay: rollback: player %u's newest input jumped %u -> %u at count %u (a burst after a quiet spell)\n",
                   p + 1, l_rb_last_count[p], c, n);
        l_rb_have_last[p] = 1;
        l_rb_last_count[p] = c;
        l_rb_last_keys[p] = keys;
        l_rb_last_plugin[p] = plugin;
    }
    struct np_hist *d;
    while ((d = hist_at(p, l_rb_done[p])) != NULL && d->real)
        ++l_rb_done[p];
}

static np_packet *l_check_sync_packet;

static void rb_try_sync(void)
{
    if (!l_rb_sync.have || l_rb_replaying || l_rb_bad_mask)
        return;
    for (int p = 0; p < 4; ++p)
        if (Controls[p].Present && np_after(l_rb_sync.counts[p], l_rb_done[p]))
            return;
    memcpy(l_check_sync_packet->data, l_rb_sync.data, l_rb_sync.len);
    l_check_sync_packet->len = l_rb_sync.len;
    SDLNet_UDP_Send(l_udpSocket, l_udpChannel, l_check_sync_packet);
    l_rb_sync_sent_vi = l_rb_sync.vi;
    l_rb_sync_sent = 1;
    l_rb_sync.have = 0;
}

//UDP packets
static np_packet l_packets[4];
static np_packet *l_request_input_packet;
static np_packet *l_send_input_packet;
static np_packet *l_process_packet;
static np_packet *l_check_sync_packet;
static const int32_t l_check_sync_packet_size = (CP0_REGS_COUNT * 4) + 5;

//UDP packet formats
#define UDP_SEND_KEY_INFO 0
#define UDP_RECEIVE_KEY_INFO 1
#define UDP_REQUEST_KEY_INFO 2
#define UDP_RECEIVE_KEY_INFO_GRATUITOUS 3
#define UDP_SYNC_DATA 4

//TCP packet formats
#define TCP_SEND_SAVE 1
#define TCP_RECEIVE_SAVE 2
#define TCP_SEND_SETTINGS 3
#define TCP_RECEIVE_SETTINGS 4
#define TCP_REGISTER_PLAYER 5
#define TCP_GET_REGISTRATION 6
#define TCP_DISCONNECT_NOTICE 7
/* Joey64 (2026-11-10): [0x40, the ROM's MD5 as 32 hex characters]; the room answers 2 when it is the host's
 * ROM, 1 when not. The host's own (it joins from this device, 127.0.0.1) is the one the others must have. */
#define TCP_JOEY_ROM_CHECK 0x40

#define CS4 32

m64p_error netplay_start(const char* host, int port)
{
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    snprintf(portstr, sizeof(portstr), "%d", port);
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL)
    {
        log_cb(RETRO_LOG_ERROR, "Netplay: could not resolve %s\n", host);
        return M64ERR_SYSTEM_FAIL;
    }
    np_udp = socket(res->ai_family, SOCK_DGRAM, 0);
    if (np_udp < 0 || connect(np_udp, res->ai_addr, res->ai_addrlen) != 0)
    {
        log_cb(RETRO_LOG_ERROR, "Netplay: UDP socket to %s:%d failed\n", host, port);
        if (np_udp >= 0) close(np_udp);
        np_udp = -1;
        freeaddrinfo(res);
        return M64ERR_SYSTEM_FAIL;
    }
    {
        const int tos_local = CS4 << 2;
        setsockopt(np_udp, IPPROTO_IP, IP_TOS, &tos_local, sizeof(tos_local));
    }
    np_tcp = socket(res->ai_family, SOCK_STREAM, 0);
    if (np_tcp < 0 || connect(np_tcp, res->ai_addr, res->ai_addrlen) != 0)
    {
        log_cb(RETRO_LOG_ERROR, "Netplay: TCP connection to %s:%d failed\n", host, port);
        if (np_tcp >= 0) close(np_tcp);
        close(np_udp);
        np_tcp = np_udp = -1;
        freeaddrinfo(res);
        return M64ERR_SYSTEM_FAIL;
    }
    {
        const int one = 1;
        setsockopt(np_tcp, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    freeaddrinfo(res);
    l_udpSocket = np_udp;
    l_tcpSocket = np_tcp;
    l_udpChannel = 0;
    memset(l_packets, 0, sizeof(l_packets));
    l_request_input_packet = &l_packets[0];
    l_send_input_packet = &l_packets[1];
    l_process_packet = &l_packets[2];
    l_check_sync_packet = &l_packets[3];
    log_cb(RETRO_LOG_INFO, "Netplay: connected to %s:%d\n", host, port);

    for (int i = 0; i < 4; ++i)
    {
        l_netplay_control[i] = -1;
        l_plugin[i] = 0;
        l_player_lag[i] = 0;
        l_sent_n[i] = 0;
        l_sent_pos[i] = 0;
        memset(&l_ns, 0, sizeof l_ns);
    }
    memset(l_plan, 0, sizeof l_plan);
    {
        const char *e = getenv("N64_NETPLAY_LOCAL");
        l_own_local = !(e && e[0] == '0');
        log_cb(RETRO_LOG_INFO, "Netplay: own inputs %s\n", l_own_local ? "used at once where the room says they go" : "through the room (N64_NETPLAY_LOCAL=0)");
    }
    {
        const char *e = getenv("N64_NETPLAY_ROLLBACK");
        l_rb_window = e && *e ? atoi(e) : 0;
        if (l_rb_window && l_rb_window < 2) l_rb_window = 2;
        if (l_rb_window > 60) l_rb_window = 60;
        memset(l_hist, 0, sizeof l_hist);
        memset(l_rb_done, 0, sizeof l_rb_done);
        memset(l_rb_have_last, 0, sizeof l_rb_have_last);
        memset(&l_rbs, 0, sizeof l_rbs);
        memset(&l_rb_sync, 0, sizeof l_rb_sync);
        l_rb_bad_mask = 0;
        l_rb_replaying = 0;
        l_rb_rtt_us = 0;
        l_rb_stall_vi = 0;
        memset(l_rb_lead_sum, 0, sizeof l_rb_lead_sum);
        l_rb_lead_n = l_rb_lead_frames = 0;
        memset(l_rb_their_have, 0, sizeof l_rb_their_have);
        l_rb_my_have = 0;
        l_rb_sync_sent = 0;
        l_rb_floor_set = 0;
        memset(l_rb_read, 0, sizeof l_rb_read);
        memset(l_rb_sent, 0, sizeof l_rb_sent);
        memset(l_rb_own_last, 0, sizeof l_rb_own_last);
        if (l_rb_window)
            log_cb(RETRO_LOG_INFO, "Netplay: rollback on, guessing up to %d inputs ahead (N64_NETPLAY_ROLLBACK)\n", l_rb_window);
    }

    l_canFF = 0;
    l_netplay_controller = 0;
    l_netplay_is_init = 1;
    l_spectator = 1;
    l_vi_counter = 0;
    l_status = 0;
    l_reg_id = 0;

    return M64ERR_SUCCESS;
}

m64p_error netplay_stop()
{
    if (l_udpSocket < 0)
        return M64ERR_INVALID_STATE;
    else
    {
        if (l_cin_compats != NULL)
        {
            for (int i = 0; i < 4; ++i)
            {
                struct netplay_event* current = l_cin_compats[i].event_first;
                struct netplay_event* next;
                while (current != NULL)
                {
                    next = current->next;
                    free(current);
                    current = next;
                }
            }
        }

        char output_data[5];
        output_data[0] = TCP_DISCONNECT_NOTICE;
        SDLNet_Write32(l_reg_id, &output_data[1]);
        SDLNet_TCP_Send(l_tcpSocket, &output_data[0], 5);

        close(np_udp);
        close(np_tcp);
        np_udp = np_tcp = -1;
        l_tcpSocket = -1;
        l_udpSocket = -1;
        l_udpChannel = -1;
        l_request_input_packet = NULL;
        l_send_input_packet = NULL;
        l_process_packet = NULL;
        l_check_sync_packet = NULL;

        l_netplay_is_init = 0;
        return M64ERR_SUCCESS;
    }
}

int netplay_is_init()
{
    return l_netplay_is_init;
}

static uint8_t buffer_size(uint8_t control_id)
{
    //This function returns the size of the local input buffer
    uint8_t counter = 0;
    if (l_rb_window)
    {
        const uint32_t n = l_cin_compats[control_id].netplay_count;
        struct np_hist *h;
        while (counter < 250 && (h = hist_at(control_id, n + counter)) != NULL && h->real)
            ++counter;
        return counter;
    }
    struct netplay_event* current = l_cin_compats[control_id].event_first;
    while (current != NULL)
    {
        current = current->next;
        ++counter;
    }
    return counter;
}

static int netplay_packet_has_data(const np_packet* packet, size_t offset, size_t size)
{
    return packet->len >= 0 && offset <= (size_t) packet->len
        && size <= (size_t) packet->len - offset;
}

static uint8_t netplay_validate_plugin(uint8_t player, uint8_t plugin)
{
    if ((player > 0 && plugin == PLUGIN_MEMPAK)
     || (plugin != PLUGIN_NONE && plugin != PLUGIN_MEMPAK
      && plugin != PLUGIN_RUMBLE_PAK && plugin != PLUGIN_RAW))
    {
        return PLUGIN_NONE;
    }

    return plugin;
}

static void netplay_request_input(uint8_t control_id)
{
    l_request_input_packet->data[0] = UDP_REQUEST_KEY_INFO;
    l_request_input_packet->data[1] = control_id; //The player we need input for
    SDLNet_Write32(l_reg_id, &l_request_input_packet->data[2]); //our registration ID
    //Joey64 rollback: from the first input still missing, not the current count: a guessed one lost on the way
    //(passed without waiting) would otherwise never be sent again
    {
        uint32_t from = l_cin_compats[control_id].netplay_count;
        if (l_rb_window && np_after(from, l_rb_done[control_id]))
            from = l_rb_done[control_id];
        if (l_rb_window && (int32_t)(l_cin_compats[control_id].netplay_count - l_rb_done[control_id]) > 20 &&
            l_cin_compats[control_id].netplay_count > 600)
        {
            static int told;
            if (told++ < 40)
                log_cb(RETRO_LOG_INFO, "Netplay: rollback: overdue: asking player %u's inputs from %u\n", control_id + 1, from);
        }
        SDLNet_Write32(from, &l_request_input_packet->data[6]); //the current event count
    }
    l_request_input_packet->data[10] = l_spectator; //whether we are a spectator
    l_request_input_packet->data[11] = buffer_size(control_id); //our local buffer size
    l_request_input_packet->len = 12;
    SDLNet_UDP_Send(l_udpSocket, l_udpChannel, l_request_input_packet);
    if (!l_ns.req_at[control_id]) l_ns.req_at[control_id] = np_us();
    ++l_ns.sent_req;
}

static int check_valid(uint8_t control_id, uint32_t count)
{
    //Check if we already have this event recorded locally, returns 1 if we do
    struct netplay_event* current = l_cin_compats[control_id].event_first;
    while (current != NULL)
    {
        if (current->count == count) //event already recorded
            return 1;
        current = current->next;
    }
    return 0;
}

static void netplay_process()
{
    //In this function we process data we have received from the server
    uint32_t curr, count, keys;
    uint8_t plugin, player, current_status;
    while (SDLNet_UDP_Recv(l_udpSocket, l_process_packet) == 1)
    {
        l_last_heard = np_ticks();
        if (!netplay_packet_has_data(l_process_packet, 0, 5))
        {
            DebugMessage(M64MSG_ERROR, "Netplay: received truncated message from server");
            continue;
        }

        switch (l_process_packet->data[0])
        {
            case UDP_RECEIVE_KEY_INFO:
            case UDP_RECEIVE_KEY_INFO_GRATUITOUS:
                player = l_process_packet->data[1];
                if (player >= 4)
                {
                    DebugMessage(M64MSG_ERROR, "Netplay: received invalid player from server");
                    break;
                }
                if (l_process_packet->data[4] >
                    ((size_t) l_process_packet->len - 5) / 9)
                {
                    DebugMessage(M64MSG_ERROR, "Netplay: received truncated input data from server");
                    break;
                }
                //current_status is a status update from the server
                //it will let us know if another player has disconnected, or the games have desynced
                current_status = l_process_packet->data[2];
                ++l_ns.packets;
                if (l_process_packet->data[0] == UDP_RECEIVE_KEY_INFO)
                {
                    l_player_lag[player] = l_process_packet->data[3];
                    if (l_ns.req_at[player])
                    {
                        uint64_t rtt = np_us() - l_ns.req_at[player];
                        l_ns.rtt_sum += rtt; ++l_ns.rtt_n;
                        l_rb_rtt_us = l_rb_rtt_us ? (l_rb_rtt_us * 7 + rtt) / 8 : rtt;
                        if (rtt > l_ns.rtt_max) l_ns.rtt_max = rtt;
                        l_ns.req_at[player] = 0;
                    }
                }
                if (current_status != l_status)
                {
                    if (((current_status & 0x1) ^ (l_status & 0x1)) != 0)
                        log_cb(RETRO_LOG_INFO, "Netplay: players have de-synced at VI %u\n", l_vi_counter);
                    for (int dis = 1; dis < 5; ++dis)
                    {
                        if (((current_status & (0x1 << dis)) ^ (l_status & (0x1 << dis))) != 0)
                            log_cb(RETRO_LOG_INFO, "Netplay: player %u has disconnected\n", dis);
                    }
                    //Joey64 (2026-11-10): Joey64's room puts the paused player (1-4, 0 = none) in bits 5-7;
                    //other servers leave them 0. The app shows who paused from these lines.
                    if (((current_status ^ l_status) & 0xE0) != 0)
                    {
                        if (current_status >> 5)
                            log_cb(RETRO_LOG_INFO, "Netplay: player %u paused\n", current_status >> 5);
                        else
                            log_cb(RETRO_LOG_INFO, "Netplay: nobody is paused\n");
                    }
                    l_status = current_status;
                }
                curr = 5;
                if (l_rb_window && l_process_packet->data[4] && l_cin_compats &&
                    (int32_t)(l_cin_compats[player].netplay_count - l_rb_done[player]) > 20 && l_cin_compats[player].netplay_count > 600)
                {
                    //rb15 diagnosis: what comes while an input is overdue
                    static int told;
                    if (told++ < 60)
                        log_cb(RETRO_LOG_INFO, "Netplay: rollback: overdue %u (at %u): packet kind %u player %u counts %u..%u\n",
                               l_rb_done[player], l_cin_compats[player].netplay_count, l_process_packet->data[0], player + 1,
                               SDLNet_Read32(&l_process_packet->data[5]),
                               SDLNet_Read32(&l_process_packet->data[5]) + l_process_packet->data[4] - 1);
                }
                //this loop processes input data from the server, inserting new events into the linked list for each player
                //it skips events that we have already recorded, or if we receive data for an event that has already happened
                for (uint8_t i = 0; i < l_process_packet->data[4]; ++i)
                {
                    count = SDLNet_Read32(&l_process_packet->data[curr]);
                    curr += 4;

                    if (l_rb_window)
                    {
                        keys = SDLNet_Read32(&l_process_packet->data[curr]);
                        plugin = netplay_validate_plugin(player, l_process_packet->data[curr + 4]);
                        curr += 5;
                        rb_put(player, count, keys, plugin);
                        continue;
                    }

                    if (((count - l_cin_compats[player].netplay_count) > (UINT32_MAX / 2)) || (check_valid(player, count))) //event doesn't need to be recorded
                    {
                        ++l_ns.dup;
                        curr += 5;
                        continue;
                    }

                    if (buffer_size(player) >= NETPLAY_MAX_PENDING_EVENTS)
                    {
                        DebugMessage(M64MSG_ERROR, "Netplay: received too many pending input events");
                        break;
                    }

                    keys = SDLNet_Read32(&l_process_packet->data[curr]);
                    curr += 4;
                    plugin = netplay_validate_plugin(player, l_process_packet->data[curr]);
                    curr += 1;

                    //insert new event at beginning of linked list
                    struct netplay_event* new_event = (struct netplay_event*)malloc(sizeof(struct netplay_event));
                    if (new_event == NULL)
                    {
                        DebugMessage(M64MSG_ERROR, "Netplay: could not allocate input event");
                        break;
                    }
                    new_event->count = count;
                    ++l_ns.heard;
                    new_event->buttons = keys;
                    new_event->plugin = plugin;
                    new_event->next = l_cin_compats[player].event_first;
                    l_cin_compats[player].event_first = new_event;
                }
                break;
            case UDP_JOEY_LEAD:
            {
                const uint8_t *d = l_process_packet->data;
                if (!netplay_packet_has_data(l_process_packet, 0, 12) || d[1] >= 4)
                    break;
                int me = -1;
                for (int i = 0; i < 4; ++i)
                    if (l_netplay_control[i] != -1) { me = i; break; }
                if (me < 0 || d[1] == me)
                    break;
                l_rb_their_lead[d[1]] = (int16_t)(d[4 + me * 2] << 8 | d[5 + me * 2]);
                l_rb_their_have[d[1]] = l_rb_their_lead[d[1]] != INT16_MIN;
                l_rb_their_frame[d[1]] = l_rb_lead_frames;
                break;
            }
            case UDP_JOEY_OWN_PLAN:
            {
                const uint8_t *d = l_process_packet->data;
                if (!netplay_packet_has_data(l_process_packet, 0, 12) || d[1] >= 4 || d[11] > 12 ||
                    !netplay_packet_has_data(l_process_packet, 12, (size_t)d[11] * 5))
                    break;
                player = d[1];
                uint32_t h = SDLNet_Read32(&d[7]);
                if (l_plan[player].have && !np_after(h, l_plan[player].h))
                    break; // an older one, come late
                l_plan[player].r = SDLNet_Read32(&d[2]);
                l_plan[player].off_r = d[6];
                l_plan[player].h = h;
                l_plan[player].n = d[11];
                for (uint8_t i = 0; i < d[11]; ++i)
                {
                    l_plan[player].at[i] = SDLNet_Read32(&d[12 + i * 5]);
                    l_plan[player].ch[i] = (int8_t)d[12 + i * 5 + 4];
                }
                l_plan[player].have = 1;
                break;
            }
            default:
                log_cb(RETRO_LOG_INFO, "Netplay: received unknown message from server\n");
                break;
        }
    }
}

static void netplay_resend_inputs(void);

//Joey64: with rollback, the input is here, or near enough past the first missing one to be guessed
static int have_input(uint8_t control_id)
{
    const uint32_t c = l_cin_compats[control_id].netplay_count;
    if (!l_rb_window)
        return check_valid(control_id, c);
    struct np_hist *h = hist_at(control_id, c);
    if (h && h->real)
        return 1;
    //guess only while the app still keeps a state from before the first missing input (else a wrong guess there
    //couldn't be undone: the games would go out of step)
    //(2 to spare: the app drops its oldest state every frame while the input may still be missing, and a game that
    //reads the pad every other frame moves on 2 frames between reads; seen 2026-11-24: count 114 guessed wrong with the
    //oldest state at 115)
    if (!l_rb_floor_set || np_after(l_rb_floor[control_id] + 2, l_rb_done[control_id]))
        return 0;
    return (int32_t)(c - l_rb_done[control_id]) < l_rb_window;
}

/* Joey64 rollback: for each player, the first input still missing when a guess past it was used (else its next
 * count: nothing to undo). A state from before it has to stay; the app holds the frame while it would drop the last. */
__attribute__((visibility("default"))) void joey64_netplay_rb_need(uint32_t *out)
{
    for (int p = 0; p < 4; ++p)
    {
        const uint32_t n = l_cin_compats ? l_cin_compats[p].netplay_count : 0;
        out[p] = Controls[p].Present && np_after(n, l_rb_done[p]) ? l_rb_done[p] : n;
    }
}

/* Joey64 rollback: read the room for up to ms (asking for what's missing), while the app holds a frame */
__attribute__((visibility("default"))) int joey64_netplay_rb_pump(int ms)
{
    if (l_udpChannel == -1)
        return 0;
    netplay_resend_inputs();
    for (uint8_t p = 0; p < 4; ++p)
        if (Controls[p].Present && l_cin_compats && np_after(l_cin_compats[p].netplay_count, l_rb_done[p]))
            netplay_request_input(p);
    struct pollfd pfd = { np_udp, POLLIN, 0 };
    poll(&pfd, 1, ms);
    netplay_process();
    return np_ticks() - l_last_heard < 10000;
}

/* Joey64 rollback: the input counts of the oldest state the app keeps, each frame */
__attribute__((visibility("default"))) void joey64_netplay_rb_floor(const uint32_t *counts)
{
    for (int p = 0; p < 4; ++p)
        l_rb_floor[p] = counts[p];
    l_rb_floor_set = 1;
}

static int netplay_ensure_valid(uint8_t control_id)
{
    //This function makes sure we have data for a certain event
    //If we don't have the data, it will create a new thread that will request the data
    if (have_input(control_id))
        return 1;
    if (l_rb_window)
        ++l_rbs.waits;

    if (l_udpChannel == -1)
        return 0;

    //We need this event's input and don't have it: ask the server every 5 ms while reading what it sends.
    //Joey64 (2026-11-10): the connection counts as lost only when the server has sent nothing for 10 s (or its TCP
    //connection closed). While it still answers, the input is late because that player is paused or slow (a pause
    //menu, a call, the screen off), so we keep waiting: before, 10 s of any wait dropped every other player.
    const uint32_t start = np_ticks();
    uint32_t asked = start;
    int said = 0;
    l_last_heard = start;
    while (!have_input(control_id))
    {
        const uint32_t now = np_ticks();
        if (l_udpChannel == -1 || now - l_last_heard > 10000)
        {
            l_udpChannel = -1;
            return 0;
        }
        if (!said && now - start > 1000)
        {
            log_cb(RETRO_LOG_INFO, "Netplay: waiting for player %u\n", control_id + 1);
            said = 1;
        }
        if (now - asked >= 5)
        {
            netplay_resend_inputs();
            netplay_request_input(control_id);
            asked = now;
        }
        struct pollfd pfd = { np_udp, POLLIN, 0 };
        poll(&pfd, 1, 1);
        netplay_process();
    }
    if (said)
        log_cb(RETRO_LOG_INFO, "Netplay: player %u is back after %u ms\n", control_id + 1, np_ticks() - start);
    {
        uint32_t waited = np_ticks() - start;
        ++l_ns.waits; l_ns.wait_ms += waited;
        if (waited > l_ns.wait_max) l_ns.wait_max = waited;
    }
    return 1;
}

static void netplay_delete_event(struct netplay_event* current, uint8_t control_id)
{
    //This function deletes an event from the linked list
    struct netplay_event* find = l_cin_compats[control_id].event_first;
    while (find != NULL)
    {
        if (find->next == current)
        {
            find->next = current->next;
            break;
        }
        find = find->next;
    }
    if (current == l_cin_compats[control_id].event_first)
        l_cin_compats[control_id].event_first = l_cin_compats[control_id].event_first->next;
    free(current);
}

static void netplay_log_stats(void)
{
    uint32_t now = np_ticks();
    if (!l_ns.since) { l_ns.since = now; return; }
    if (now - l_ns.since < 5000) return;
    //(no round trip here: we read replies once a frame, so it would time the game loop; the app pings the room itself)
    log_cb(RETRO_LOG_INFO, "Netplay: net: %u waits for input (%u ms, worst %u ms), "
           "%u inputs heard, %u copies, %u requests, %u packets, %u own placed in %.1f s, lag %u/%u/%u/%u\n",
           l_ns.waits, l_ns.wait_ms, l_ns.wait_max, l_ns.heard, l_ns.dup, l_ns.sent_req, l_ns.packets, l_ns.own,
           (now - l_ns.since) / 1000.0, l_player_lag[0], l_player_lag[1], l_player_lag[2], l_player_lag[3]);
    if (l_rb_window)
    {
        log_cb(RETRO_LOG_INFO, "Netplay: rollback: %u inputs guessed, %u guessed wrong, %u waits (too far ahead), "
               "%u frames held back, round trip %.1f ms\n",
               l_rbs.guessed, l_rbs.wrong, l_rbs.waits, l_rbs.stalls, l_rb_rtt_us / 1000.0);
        for (int p = 0; p < 4; ++p)
        {
            if (!Controls[p].Present || !l_cin_compats)
                continue;
            const uint32_t n = l_cin_compats[p].netplay_count, d = l_rb_done[p];
            struct np_hist *h = &l_hist[p][d % NP_HIST];
            log_cb(RETRO_LOG_INFO, "Netplay: rollback: player %d at %u, all heard below %u (slot holds %u real %u used %u), newest heard %u; "
                   "lead ours %.2f theirs %.2f, ahead %.2f; most frames without a newer input %u\n",
                   p + 1, n, d, h->count, h->real, h->used, l_rb_last_count[p],
                   l_rb_my_lead[p] / 16.0, l_rb_their_have[p] ? l_rb_their_lead[p] / 16.0 : 0.0, l_rb_ahead16[p] / 16.0,
                   l_rb_quiet_max[p]);
            l_rb_quiet_max[p] = 0;
        }
        memset(&l_rbs, 0, sizeof l_rbs);
    }
    uint64_t keep[4];
    memcpy(keep, l_ns.req_at, sizeof keep);
    memset(&l_ns, 0, sizeof l_ns);
    memcpy(l_ns.req_at, keep, sizeof keep);
    l_ns.since = now;
}

//Joey64 rollback: hold this frame back (about a frame's wait) when we're well ahead of player p's game: where it is
//now is its newest input heard, less the room's delay, plus half the round trip. Both games run at the same rate,
//so without this one that started first stays ahead and guesses (and rolls back) for both. At most every 20 VIs.
//(rb12: the leads swapped, see UDP_JOEY_LEAD; held at most every 40 frames, so the next decision sees both averages
//taken after the last hold)
static void rb_time_sync(uint8_t p, uint32_t c)
{
    (void)c;
    if (!l_rb_my_have || l_rb_my_lead[p] == INT16_MIN || !l_rb_their_have[p] || l_rb_lead_frames - l_rb_their_frame[p] > 60 ||
        l_rb_lead_frames - l_rb_stall_vi < 40)
        return;
    const int32_t ahead16 = ((int32_t)l_rb_my_lead[p] - l_rb_their_lead[p]) / 2;
    l_rb_ahead16[p] = ahead16;
    if (ahead16 < 16)
        return;
    l_rb_stall_vi = l_rb_lead_frames;
    ++l_rbs.stalls;
    const uint32_t start = np_ticks();
    while (np_ticks() - start < 16)
    {
        struct pollfd pfd = { np_udp, POLLIN, 0 };
        poll(&pfd, 1, 2);
        netplay_process();
    }
}

static uint32_t rb_get_input(uint8_t p)
{
    const uint32_t c = l_cin_compats[p].netplay_count;
    struct np_hist *h = hist_at(p, c);
    if (!l_rb_replaying && !(h && h->real))
    {
        if (!netplay_ensure_valid(p))
        {
            log_cb(RETRO_LOG_INFO, "Netplay: lost connection to server\n");
            main_core_state_set(M64CORE_EMU_STATE, M64EMU_STOPPED);
            return 0;
        }
        h = hist_at(p, c);
    }
    uint32_t keys;
    uint8_t plugin;
    if (h && h->real)
    {
        keys = h->keys;
        plugin = h->plugin;
    }
    else
    {
        keys = l_rb_have_last[p] ? l_rb_last_keys[p] : 0;
        plugin = l_rb_have_last[p] ? l_rb_last_plugin[p] : Controls[p].Plugin;
        ++l_rbs.guessed;
    }
    h = hist_new(p, c);
    h->used = 1;
    h->used_keys = keys;
    h->used_plugin = plugin;
    Controls[p].Plugin = plugin;
    ++l_cin_compats[p].netplay_count;
    rb_try_sync();
    return keys;
}

static uint32_t netplay_get_input(uint8_t control_id)
{
    uint32_t keys;
    struct retro_fastforwarding_override ff_override;

    if (l_rb_window && l_rb_read[control_id])
        return l_rb_frame_keys[control_id]; // this frame's input, read again
    if (l_rb_window && l_rb_replaying)
    {
        netplay_process();
        l_rb_frame_keys[control_id] = rb_get_input(control_id);
        l_rb_read[control_id] = 1;
        return l_rb_frame_keys[control_id];
    }

    netplay_log_stats();

    netplay_process();
    netplay_request_input(control_id);

    //l_buffer_target is set by the server upon registration
    //l_player_lag is how far behind we are from the lead player
    //buffer_size is the local buffer size
    if (l_player_lag[control_id] > 0 && buffer_size(control_id) > l_buffer_target)
    {
        l_canFF = 1;
        main_core_state_set(M64CORE_SPEED_LIMITER, 0);

        // Enable Fast Forward
        ff_override.fastforward = true;
        // Prevent user toggling Fast Forward
        ff_override.inhibit_toggle = true;
        environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &ff_override);
    }
    else
    {
        main_core_state_set(M64CORE_SPEED_LIMITER, 1);
        
        // Disable Fast Forward
        ff_override.fastforward = false;
        // Prevent user toggling Fast Forward
        ff_override.inhibit_toggle = true;
        environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &ff_override);

        l_canFF = 0;
    }

    if (l_rb_window)
    {
        l_rb_frame_keys[control_id] = rb_get_input(control_id);
        l_rb_read[control_id] = 1;
        return l_rb_frame_keys[control_id];
    }

    if (netplay_ensure_valid(control_id))
    {
        //We grab the event from the linked list, the delete it once it has been used
        //Finally we increment the event counter
        struct netplay_event* current = l_cin_compats[control_id].event_first;
        while (current->count != l_cin_compats[control_id].netplay_count)
            current = current->next;
        keys = current->buttons;
        Controls[control_id].Plugin = current->plugin;
        netplay_delete_event(current, control_id);
        ++l_cin_compats[control_id].netplay_count;
    }
    else
    {
        log_cb(RETRO_LOG_INFO, "Netplay: lost connection to server\n");
        main_core_state_set(M64CORE_EMU_STATE, M64EMU_STOPPED);
        keys = 0;
    }

    return keys;
}

//Joey64 (2026-11-10): each input went out once, so one UDP packet lost on Wi-Fi left every player waiting
//for it until the connection counted as lost. Our last NP_RESEND inputs are kept and sent again: the one
//before with each new one, and all of them while we wait for input (servers keep the first copy of a count).

static void np_send_key(uint8_t control_id, uint32_t count, uint32_t keys, uint8_t plugin)
{
    l_send_input_packet->data[0] = UDP_SEND_KEY_INFO;
    l_send_input_packet->data[1] = control_id; //player number
    SDLNet_Write32(count, &l_send_input_packet->data[2]); // event count
    SDLNet_Write32(keys, &l_send_input_packet->data[6]); //key data
    l_send_input_packet->data[10] = plugin; //current plugin
    l_send_input_packet->len = 11;
    SDLNet_UDP_Send(l_udpSocket, l_udpChannel, l_send_input_packet);
}

static void netplay_resend_inputs(void)
{
    //all of a player's kept inputs in one packet (a Joey64 room; the join's ROM check made sure it is one)
    for (uint8_t id = 0; id < 4; ++id)
    {
        if (!l_sent_n[id])
            continue;
        uint8_t *d = l_send_input_packet->data;
        d[0] = UDP_JOEY_KEY_BATCH;
        d[1] = id;
        d[2] = l_sent_n[id];
        int k = 3;
        for (uint8_t i = 0; i < l_sent_n[id]; ++i)
        {
            SDLNet_Write32(l_sent[id][i].count, &d[k]);
            SDLNet_Write32(l_sent[id][i].keys, &d[k + 4]);
            d[k + 8] = l_sent[id][i].plugin;
            k += 9;
        }
        l_send_input_packet->len = k;
        SDLNet_UDP_Send(l_udpSocket, l_udpChannel, l_send_input_packet);
    }
}

static void np_add_event(uint8_t id, uint32_t slot, uint32_t keys, uint8_t plugin)
{
    if (l_rb_window)
    {
        if (!check_valid(id, slot) && !np_after(l_cin_compats[id].netplay_count, slot))
            ++l_ns.own;
        rb_put(id, slot, keys, plugin);
        return;
    }
    if (np_after(l_cin_compats[id].netplay_count, slot) || check_valid(id, slot) || buffer_size(id) >= NETPLAY_MAX_PENDING_EVENTS)
        return;
    struct netplay_event* e = (struct netplay_event*)malloc(sizeof(struct netplay_event));
    if (e == NULL)
        return;
    e->count = slot;
    e->buttons = keys;
    e->plugin = plugin;
    e->next = l_cin_compats[id].event_first;
    l_cin_compats[id].event_first = e;
    ++l_ns.own;
}

//Joey64 (2026-11-13): our input sent at count c, into our own list where the room puts it (UDP_JOEY_OWN_PLAN)
static void np_place_own(uint8_t id, uint32_t c, uint32_t keys, uint8_t plugin)
{
    if (!l_own_local || !l_plan[id].have)
        return;
    const uint32_t r = l_plan[id].r, h = l_plan[id].h;
    if (np_after(r, c) || !np_after(h, c))
        return; // outside what the room promised
    int off = l_plan[id].off_r, raise = 0;
    for (uint8_t i = 0; i < l_plan[id].n; ++i)
    {
        const uint32_t at = l_plan[id].at[i];
        const int ch = l_plan[id].ch[i];
        if (!np_after(at, c))
            off += ch;
        if (at == c && ch < 0)
            return; // a frame taken away here: the room drops this input
        if (at == c + 1 && ch > 0)
            raise = ch;
    }
    const uint32_t slot = c + (uint32_t)off;
    np_add_event(id, slot, keys, plugin);
    for (int k = 1; k <= raise; ++k)
        np_add_event(id, slot + (uint32_t)k, keys, plugin);
}

static void netplay_send_input(uint8_t control_id, uint32_t keys)
{
    uint8_t pos = l_sent_pos[control_id];
    if (l_rb_window && l_sent_n[control_id])
    {
        //rb14 diagnosis: every count goes out once, in order
        static int told;
        uint32_t prevc = l_sent[control_id][(uint8_t)((pos + NP_RESEND - 1) % NP_RESEND)].count;
        uint32_t cc = l_cin_compats[control_id].netplay_count;
        if (cc != prevc + 1 && told++ < 20)
            log_cb(RETRO_LOG_INFO, "Netplay: rollback: own input for count %u sent after %u (replaying %d)\n", cc, prevc, l_rb_replaying);
    }
    l_sent[control_id][pos].count = l_cin_compats[control_id].netplay_count;
    l_sent[control_id][pos].keys = keys;
    l_sent[control_id][pos].plugin = l_plugin[control_id];
    l_sent_pos[control_id] = (uint8_t)((pos + 1) % NP_RESEND);
    if (l_sent_n[control_id] < NP_RESEND) ++l_sent_n[control_id];
    np_send_key(control_id, l_cin_compats[control_id].netplay_count, keys, l_plugin[control_id]);
    np_place_own(control_id, l_cin_compats[control_id].netplay_count, keys, l_plugin[control_id]);
    if (l_sent_n[control_id] > 1)
    {
        uint8_t prev = (uint8_t)((pos + NP_RESEND - 1) % NP_RESEND);
        np_send_key(control_id, l_sent[control_id][prev].count, l_sent[control_id][prev].keys, l_sent[control_id][prev].plugin);
    }
}

/* 1 = the host's ROM, 0 = another one, -1 = no answer (the room closed the connection: not a Joey64 room) */
int netplay_check_rom(const char *md5)
{
    char out[33];
    uint8_t answer = 0;
    out[0] = TCP_JOEY_ROM_CHECK;
    for (int i = 0; i < 32; i++)
    {
        char ch = md5 && md5[i] ? md5[i] : '0';
        out[1 + i] = (ch >= 'a' && ch <= 'f') ? (char)(ch - 'a' + 'A') : ch;
        if (md5 && !md5[i]) md5 = NULL;
    }
    SDLNet_TCP_Send(l_tcpSocket, out, 33);
    SDLNet_TCP_Recv(l_tcpSocket, &answer, 1);
    log_cb(RETRO_LOG_INFO, "Netplay: ROM check %.32s: %s\n", out + 1,
           answer == 2 ? "same as the host's" : answer == 1 ? "not the host's" : "no answer");
    return answer == 2 ? 1 : answer == 1 ? 0 : -1;
}

uint8_t netplay_register_player(uint8_t player, uint8_t plugin, uint8_t rawdata, uint32_t reg_id)
{
    l_reg_id = reg_id;
    char output_data[8];
    output_data[0] = TCP_REGISTER_PLAYER;
    output_data[1] = player; //player number we'd like to register
    output_data[2] = plugin; //current plugin
    output_data[3] = rawdata; //whether we are using a RawData input plugin
    SDLNet_Write32(l_reg_id, &output_data[4]);

    SDLNet_TCP_Send(l_tcpSocket, &output_data[0], 8);

    uint8_t response[2];
    size_t recv = 0;
    while (recv < 2)
        recv += SDLNet_TCP_Recv(l_tcpSocket, &response[recv], 2 - recv);
    l_buffer_target = response[1]; //local buffer size target
    return response[0];
}

int netplay_lag()
{
    return l_canFF;
}

int netplay_next_controller()
{
    return l_netplay_controller;
}

void netplay_set_controller(uint8_t player)
{
    l_netplay_control[player] = l_netplay_controller++;
    l_spectator = 0;
}

int netplay_get_controller(uint8_t player)
{
    return l_netplay_control[player];
}

file_status_t netplay_read_storage(const char *filename, void *data, size_t size)
{
    //This function syncs save games.
    //If the client is controlling player 1, it sends its save game to the server
    //All other players receive save files from the server
    const char *file_extension = strrchr(filename, '.');
    file_extension += 1;

    uint32_t buffer_pos = 0;
    char *output_data = malloc(size + strlen(file_extension) + 6);

    file_status_t ret;
    uint8_t request;
    if (l_netplay_control[0] != -1)
    {
        request = TCP_SEND_SAVE;
        memcpy(&output_data[buffer_pos], &request, 1);
        ++buffer_pos;

         //send file extension
        memcpy(&output_data[buffer_pos], file_extension, strlen(file_extension) + 1);
        buffer_pos += strlen(file_extension) + 1;

        // Not wanted for libretro
        //ret = read_from_file(filename, data, size);
        //if (ret == file_open_error)
        //    memset(data, 0, size); //all zeros means there is no save file
        SDLNet_Write32((int32_t)size, &output_data[buffer_pos]); //file data size
        buffer_pos += 4;
        memcpy(&output_data[buffer_pos], data, size); //file data
        buffer_pos += size;

        SDLNet_TCP_Send(l_tcpSocket, &output_data[0], buffer_pos);
    }
    else
    {
        request = TCP_RECEIVE_SAVE;
        memcpy(&output_data[buffer_pos], &request, 1);
        ++buffer_pos;

        //extension of the file we are requesting
        memcpy(&output_data[buffer_pos], file_extension, strlen(file_extension) + 1);
        buffer_pos += strlen(file_extension) + 1;

        SDLNet_TCP_Send(l_tcpSocket, &output_data[0], buffer_pos);
        size_t recv = 0;
        char *data_array = data;
        while (recv < size)
            recv += SDLNet_TCP_Recv(l_tcpSocket, data_array + recv, size - recv);

        int sum = 0;
        for (int i = 0; i < size; ++i)
            sum |= data_array[i];

        if (sum == 0) //all zeros means there is no save file
            ret = file_open_error;
        else
            ret = file_ok;
    }
    free(output_data);
    return ret;
}

void netplay_sync_settings(uint32_t *count_per_op, uint32_t *count_per_op_denom_pot, uint32_t *disable_extra_mem, int32_t *si_dma_duration, uint32_t *emumode, int32_t *no_compiled_jump)
{
    if (!netplay_is_init())
        return;

    char output_data[SETTINGS_SIZE + 1];
    uint8_t request;
    if (l_netplay_control[0] != -1) //player 1 is the source of truth for settings
    {
        request = TCP_SEND_SETTINGS;
        memcpy(&output_data[0], &request, 1);
        SDLNet_Write32(*count_per_op, &output_data[1]);
        SDLNet_Write32(*count_per_op_denom_pot, &output_data[5]);
        SDLNet_Write32(*disable_extra_mem, &output_data[9]);
        SDLNet_Write32(*si_dma_duration, &output_data[13]);
        SDLNet_Write32(*emumode, &output_data[17]);
        SDLNet_Write32(*no_compiled_jump, &output_data[21]);
        SDLNet_TCP_Send(l_tcpSocket, &output_data[0], SETTINGS_SIZE + 1);
    }
    else
    {
        request = TCP_RECEIVE_SETTINGS;
        memcpy(&output_data[0], &request, 1);
        SDLNet_TCP_Send(l_tcpSocket, &output_data[0], 1);
        int32_t recv = 0;
        while (recv < SETTINGS_SIZE)
            recv += SDLNet_TCP_Recv(l_tcpSocket, &output_data[recv], SETTINGS_SIZE - recv);
        *count_per_op = SDLNet_Read32(&output_data[0]);
        *count_per_op_denom_pot = SDLNet_Read32(&output_data[4]);
        *disable_extra_mem = SDLNet_Read32(&output_data[8]);
        *si_dma_duration = SDLNet_Read32(&output_data[12]);
        *emumode = SDLNet_Read32(&output_data[16]);
        *no_compiled_jump = SDLNet_Read32(&output_data[20]);
    }
}

void netplay_check_sync(struct cp0* cp0)
{
    //This function is used to check if games have desynced
    //Every 600 VIs, it sends the value of the CP0 registers to the server
    //The server will compare the values, and update the status byte if it detects a desync
    if (!netplay_is_init())
        return;

    if (l_rb_window && l_cin_compats)
    {
        //the frame's end (see l_rb_read): our own input goes out for this frame's count if the game didn't read the
        //pad, and every player's count moves on by one
        for (uint8_t p = 0; p < 4; ++p)
        {
            if (!Controls[p].Present)
                continue;
            if (l_netplay_control[p] != -1 && !l_rb_sent[p] && !l_rb_replaying)
                netplay_send_input(p, l_rb_own_last[p]);
            if (!l_rb_read[p])
                ++l_cin_compats[p].netplay_count; // an input nobody reads: no guess to check, nothing to wait for
            l_rb_read[p] = l_rb_sent[p] = 0;
        }
        //time sync: sample our lead over each other player, and every 10 frames send the averages
        if (!l_rb_replaying)
        {
            //rb21: read the room every frame. Packets were read only when the game read a pad, so in stretches where it
            //doesn't (Mario Kart 64's menus and loading, ~28 frames) nothing came in: the newest input heard froze,
            //the leads swung to 15-30 frames, time sync couldn't act and a wrong guess was found 26-30 frames late.
            netplay_process();
            int me = -1;
            for (int i = 0; i < 4; ++i)
                if (l_netplay_control[i] != -1) { me = i; break; }
            //and time sync every frame (it ran only on a pad read whose input wasn't here)
            for (uint8_t p = 0; p < 4; ++p)
                if (Controls[p].Present && p != me && l_rb_have_last[p])
                    rb_time_sync(p, l_cin_compats[p].netplay_count);
            ++l_rb_lead_frames;
            ++l_rb_lead_n;
            for (uint8_t p = 0; p < 4; ++p)
                if (Controls[p].Present && p != me && l_rb_have_last[p])
                {
                    l_rb_lead_sum[p] += (int32_t)(l_cin_compats[p].netplay_count - l_rb_last_count[p]);
                    if (l_rb_last_count[p] != l_rb_prev_last[p]) { l_rb_prev_last[p] = l_rb_last_count[p]; l_rb_quiet[p] = 0; }
                    else if (++l_rb_quiet[p] > l_rb_quiet_max[p]) l_rb_quiet_max[p] = l_rb_quiet[p];
                }
            if (l_rb_lead_n >= 10 && me >= 0)
            {
                uint8_t out[12] = { UDP_JOEY_LEAD, (uint8_t)me, 0, 0 };
                for (int p = 0; p < 4; ++p)
                {
                    int16_t v = INT16_MIN;
                    if (Controls[p].Present && p != me && l_rb_have_last[p])
                    {
                        int32_t a = l_rb_lead_sum[p] * 16 / (int32_t)l_rb_lead_n;
                        v = (int16_t)(a < -32000 ? -32000 : a > 32000 ? 32000 : a);
                    }
                    l_rb_my_lead[p] = v;
                    out[4 + p * 2] = (uint8_t)((uint16_t)v >> 8);
                    out[5 + p * 2] = (uint8_t)v;
                }
                l_rb_my_have = 1;
                if (np_udp >= 0)
                    send(np_udp, out, sizeof out, 0);
                memset(l_rb_lead_sum, 0, sizeof l_rb_lead_sum);
                l_rb_lead_n = 0;
            }
        }
    }

    if (l_vi_counter % 600 == 0)
    {
        const uint32_t* cp0_regs = r4300_cp0_regs(cp0);

        l_check_sync_packet->data[0] = UDP_SYNC_DATA;
        SDLNet_Write32(l_vi_counter, &l_check_sync_packet->data[1]); //current VI count
        for (int i = 0; i < CP0_REGS_COUNT; ++i)
        {
            SDLNet_Write32(cp0_regs[i], &l_check_sync_packet->data[(i * 4) + 5]);
        }
        l_check_sync_packet->len = l_check_sync_packet_size;
        if (!l_rb_window)
            SDLNet_UDP_Send(l_udpSocket, l_udpChannel, l_check_sync_packet);
        else if (!l_rb_sync_sent || np_after(l_vi_counter, l_rb_sync_sent_vi))
        {
            //rollback: kept until every input before here is heard and right (a frame run again replaces it)
            l_rb_sync.have = 1;
            l_rb_sync.vi = l_vi_counter;
            for (int p = 0; p < 4; ++p)
                l_rb_sync.counts[p] = l_cin_compats ? l_cin_compats[p].netplay_count : 0;
            l_rb_sync.len = l_check_sync_packet_size;
            memcpy(l_rb_sync.data, l_check_sync_packet->data, l_check_sync_packet_size);
        }
    }

    ++l_vi_counter;
    if (l_rb_window)
        rb_try_sync();
}

/* Joey64 rollback (see l_hist): the app keeps each frame's state with these numbers and puts them back with it. */
__attribute__((visibility("default"))) int joey64_netplay_rb_window(void)
{
    return l_netplay_is_init ? l_rb_window : 0;
}

/* out[0-3]: each player's next input count, out[4]: the VI count */
__attribute__((visibility("default"))) void joey64_netplay_rb_mark(uint32_t *out)
{
    for (int p = 0; p < 4; ++p)
        out[p] = l_cin_compats ? l_cin_compats[p].netplay_count : 0;
    out[4] = l_vi_counter;
}

__attribute__((visibility("default"))) void joey64_netplay_rb_restore(const uint32_t *in)
{
    if (l_cin_compats)
        for (int p = 0; p < 4; ++p)
            l_cin_compats[p].netplay_count = in[p];
    l_vi_counter = in[4];
    memset(l_rb_read, 0, sizeof l_rb_read);
    memset(l_rb_sent, 0, sizeof l_rb_sent);
}

/* the players with an input guessed wrong since the last call, and for each the earliest such count */
__attribute__((visibility("default"))) unsigned joey64_netplay_rb_bad(uint32_t *counts)
{
    unsigned m = l_rb_bad_mask;
    for (int p = 0; p < 4; ++p)
        counts[p] = l_rb_bad[p];
    l_rb_bad_mask = 0;
    return m;
}

__attribute__((visibility("default"))) void joey64_netplay_rb_replaying(int on)
{
    l_rb_replaying = on;
}

void netplay_read_registration(struct controller_input_compat* cin_compats)
{
    //This function runs right before the game starts
    //The server shares the registration details about each player
    if (!netplay_is_init())
        return;

    l_cin_compats = cin_compats;

    uint32_t reg_id;
    char output_data = TCP_GET_REGISTRATION;
    char input_data[24];
    SDLNet_TCP_Send(l_tcpSocket, &output_data, 1);
    size_t recv = 0;
    while (recv < 24)
        recv += SDLNet_TCP_Recv(l_tcpSocket, &input_data[recv], 24 - recv);
    uint32_t curr = 0;
    for (int i = 0; i < 4; ++i)
    {
        reg_id = SDLNet_Read32(&input_data[curr]);
        curr += 4;

        Controls[i].Type = CONT_TYPE_STANDARD; //make sure VRU is disabled

        if (reg_id == 0) //No one registered to control this player
        {
            Controls[i].Present = 0;
            Controls[i].Plugin = PLUGIN_NONE;
            Controls[i].RawData = 0;
            curr += 2;
        }
        else
        {
            Controls[i].Present = 1;
            Controls[i].Plugin = netplay_validate_plugin(i, input_data[curr]);
            l_plugin[i] = Controls[i].Plugin;
            ++curr;
            Controls[i].RawData = input_data[curr];
            ++curr;
        }
    }
}

static void netplay_send_raw_input(struct pif* pif)
{
    if (l_rb_replaying)
        return; // a frame run again: its inputs went out the first time
    for (int i = 0; i < 4; ++i)
    {
        if (l_netplay_control[i] != -1)
        {
            if (pif->channels[i].tx
             && (*pif->channels[i].tx & 0x3f) == 1
             && (*pif->channels[i].rx & 0x3f) == 4
             && pif->channels[i].tx_buf[0] == JCMD_CONTROLLER_READ)
            {
                if (l_rb_window)
                {
                    if (l_rb_sent[i])
                        continue; // one input a frame
                    l_rb_sent[i] = 1;
                    l_rb_own_last[i] = *(uint32_t*)pif->channels[i].rx_buf;
                }
                netplay_send_input(i, *(uint32_t*)pif->channels[i].rx_buf);
            }
        }
    }
}

static void netplay_get_raw_input(struct pif* pif)
{
    for (int i = 0; i < 4; ++i)
    {
        if (Controls[i].Present == 1)
        {
            if (pif->channels[i].tx)
            {
                *pif->channels[i].rx &= ~0xC0; //Always show the controller as connected

                if (pif->channels[i].tx_buf[0] == JCMD_CONTROLLER_READ
                 && (*pif->channels[i].tx & 0x3f) == 1
                 && (*pif->channels[i].rx & 0x3f) == 4)
                {
                    *(uint32_t*)pif->channels[i].rx_buf = netplay_get_input(i);
                }
                else if ((pif->channels[i].tx_buf[0] == JCMD_STATUS || pif->channels[i].tx_buf[0] == JCMD_RESET)
                      && (*pif->channels[i].tx & 0x3f) == 1
                      && (*pif->channels[i].rx & 0x3f) == 3
                      && Controls[i].RawData)
                {
                    //a bit of a hack for raw input controllers, force the status
                    uint16_t type = JDT_JOY_ABS_COUNTERS | JDT_JOY_PORT;
                    pif->channels[i].rx_buf[0] = (uint8_t)(type >> 0);
                    pif->channels[i].rx_buf[1] = (uint8_t)(type >> 8);
                    pif->channels[i].rx_buf[2] = 0;
                }
                else if (pif->channels[i].tx_buf[0] == JCMD_PAK_READ
                      && (*pif->channels[i].tx & 0x3f) == 3
                      && (*pif->channels[i].rx & 0x3f) == 33
                      && Controls[i].RawData)
                {
                    //also a hack for raw input, we return "mempak not present" if the game tries to read the mempak
                    pif->channels[i].rx_buf[32] = 255;
                }
                else if (pif->channels[i].tx_buf[0] == JCMD_PAK_WRITE
                      && (*pif->channels[i].tx & 0x3f) == 35
                      && (*pif->channels[i].rx & 0x3f) == 1
                      && Controls[i].RawData)
                {
                    //also a hack for raw input, we return "mempak not present" if the game tries to write to mempak
                    pif->channels[i].rx_buf[0] = 255;
                }
            }
        }
    }
}

void netplay_update_input(struct pif* pif)
{
    if (netplay_is_init())
    {
        netplay_send_raw_input(pif);
        netplay_get_raw_input(pif);
    }
}

m64p_error netplay_send_config(char* data, int size)
{
    if (!netplay_is_init())
        return M64ERR_NOT_INIT;

    if (l_netplay_control[0] != -1 || size == 1) //Only P1 sends settings, we allow all players to send if the size is 1, this may be a request packet
    {
        int result = SDLNet_TCP_Send(l_tcpSocket, data, size);
        if (result < size)
            return M64ERR_SYSTEM_FAIL;
        return M64ERR_SUCCESS;
    }
    else
        return M64ERR_INVALID_STATE;
}

m64p_error netplay_receive_config(char* data, int size)
{
    if (!netplay_is_init())
        return M64ERR_NOT_INIT;

    if (l_netplay_control[0] == -1) //Only P2-4 receive settings
    {
        int recv = 0;
        while (recv < size)
        {
            recv += SDLNet_TCP_Recv(l_tcpSocket, &data[recv], size - recv);
            if (recv < 1)
                return M64ERR_SYSTEM_FAIL;
        }
        return M64ERR_SUCCESS;
    }
    else
        return M64ERR_INVALID_STATE;
}
