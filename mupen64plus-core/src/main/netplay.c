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
    SDLNet_Write32(l_cin_compats[control_id].netplay_count, &l_request_input_packet->data[6]); //the current event count
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
                //this loop processes input data from the server, inserting new events into the linked list for each player
                //it skips events that we have already recorded, or if we receive data for an event that has already happened
                for (uint8_t i = 0; i < l_process_packet->data[4]; ++i)
                {
                    count = SDLNet_Read32(&l_process_packet->data[curr]);
                    curr += 4;

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
            default:
                log_cb(RETRO_LOG_INFO, "Netplay: received unknown message from server\n");
                break;
        }
    }
}

static void netplay_resend_inputs(void);

static int netplay_ensure_valid(uint8_t control_id)
{
    //This function makes sure we have data for a certain event
    //If we don't have the data, it will create a new thread that will request the data
    if (check_valid(control_id, l_cin_compats[control_id].netplay_count))
        return 1;

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
    while (!check_valid(control_id, l_cin_compats[control_id].netplay_count))
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
           "%u inputs heard, %u copies, %u requests, %u packets in %.1f s, lag %u/%u/%u/%u\n",
           l_ns.waits, l_ns.wait_ms, l_ns.wait_max, l_ns.heard, l_ns.dup, l_ns.sent_req, l_ns.packets,
           (now - l_ns.since) / 1000.0, l_player_lag[0], l_player_lag[1], l_player_lag[2], l_player_lag[3]);
    uint64_t keep[4];
    memcpy(keep, l_ns.req_at, sizeof keep);
    memset(&l_ns, 0, sizeof l_ns);
    memcpy(l_ns.req_at, keep, sizeof keep);
    l_ns.since = now;
}

static uint32_t netplay_get_input(uint8_t control_id)
{
    uint32_t keys;
    struct retro_fastforwarding_override ff_override;

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

static void netplay_send_input(uint8_t control_id, uint32_t keys)
{
    uint8_t pos = l_sent_pos[control_id];
    l_sent[control_id][pos].count = l_cin_compats[control_id].netplay_count;
    l_sent[control_id][pos].keys = keys;
    l_sent[control_id][pos].plugin = l_plugin[control_id];
    l_sent_pos[control_id] = (uint8_t)((pos + 1) % NP_RESEND);
    if (l_sent_n[control_id] < NP_RESEND) ++l_sent_n[control_id];
    np_send_key(control_id, l_cin_compats[control_id].netplay_count, keys, l_plugin[control_id]);
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
        SDLNet_UDP_Send(l_udpSocket, l_udpChannel, l_check_sync_packet);
    }

    ++l_vi_counter;
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
    for (int i = 0; i < 4; ++i)
    {
        if (l_netplay_control[i] != -1)
        {
            if (pif->channels[i].tx
             && (*pif->channels[i].tx & 0x3f) == 1
             && (*pif->channels[i].rx & 0x3f) == 4
             && pif->channels[i].tx_buf[0] == JCMD_CONTROLLER_READ)
                netplay_send_input(i, *(uint32_t*)pif->channels[i].rx_buf);
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
