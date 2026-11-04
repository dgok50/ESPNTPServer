/*
 * NTP.cpp
 *
 * Copyright 2017 Christopher B. Liebman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *  Created on: Feb 27, 2018
 *      Author: chris.l
 */

#include <lwip/def.h> // htonl()
#include <lwip/pbuf.h>
#include <stddef.h>

#include "NTP.h"

#include "Log.h"
static const char* TAG = "NTP";

//#define NTP_PACKET_DEBUG

#define NTP_PORT               123
#define NTP_PRECISION          -20 // micros() has one-microsecond resolution

typedef struct ntp_packet
{
    uint8_t  flags;
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t delay;
    uint32_t dispersion;
    uint8_t  ref_id[4];
    NTPTime  ref_time;
    NTPTime  orig_time;
    NTPTime  recv_time;
    NTPTime  xmit_time;
} NTPPacket;

static_assert(sizeof(NTPPacket) == 48, "NTP packet must be exactly 48 bytes");

#define LI_NONE         0
#define LI_SIXTY_ONE    1
#define LI_FIFTY_NINE   2
#define LI_NOSYNC       3

#define MODE_RESERVED   0
#define MODE_ACTIVE     1
#define MODE_PASSIVE    2
#define MODE_CLIENT     3
#define MODE_SERVER     4
#define MODE_BROADCAST  5
#define MODE_CONTROL    6
#define MODE_PRIVATE    7

#define NTP_VERSION     4

#define REF_ID          "GPS "  // "GPS " when we have one!

#define setLI(value)    ((value&0x03)<<6)
#define setVERS(value)  ((value&0x07)<<3)
#define setMODE(value)  ((value&0x07))

#define getLI(value)    ((value>>6)&0x03)
#define getVERS(value)  ((value>>3)&0x07)
#define getMODE(value)  (value&0x07)

#define SEVENTY_YEARS   2208988800L
#define toEPOCH(t)      ((uint32_t)t-SEVENTY_YEARS)
#define toNTP(t)        ((uint32_t)t+SEVENTY_YEARS)

#ifdef NTP_PACKET_DEBUG
#include <time.h>
char* timestr(long int t)
{
    t = toEPOCH(t);
    return ctime(&t);
}

void dumpNTPPacket(NTPPacket* ntp)
{
    dbprintf("size:       %u\n", sizeof(*ntp));
    dbprintf("firstbyte:  0x%02x\n", *(uint8_t*)ntp);
    dbprintf("li:         %u\n", getLI(ntp->flags));
    dbprintf("version:    %u\n", getVERS(ntp->flags));
    dbprintf("mode:       %u\n", getMODE(ntp->flags));
    dbprintf("stratum:    %u\n", ntp->stratum);
    dbprintf("poll:       %u\n", ntp->poll);
    dbprintf("precision:  %d\n", ntp->precision);
    dbprintf("delay:      %u\n", ntp->delay);
    dbprintf("dispersion: %u\n", ntp->dispersion);
    dbprintf("ref_id:     %02x:%02x:%02x:%02x\n", ntp->ref_id[0], ntp->ref_id[1], ntp->ref_id[2], ntp->ref_id[3]);
    dbprintf("ref_time:   %08x:%08x\n", ntp->ref_time.seconds, ntp->ref_time.fraction);
    dbprintf("orig_time:  %08x:%08x\n", ntp->orig_time.seconds, ntp->orig_time.fraction);
    dbprintf("recv_time:  %08x:%08x\n", ntp->recv_time.seconds, ntp->recv_time.fraction);
    dbprintf("xmit_time:  %08x:%08x\n", ntp->xmit_time.seconds, ntp->xmit_time.fraction);
}
#else
#define dumpNTPPacket(x)
#endif

NTP::NTP(GPS& gps) :
    _gps(gps),
    _pcb(nullptr),
    _req_count(0),
    _rsp_count(0),
    _last_processing_us(0),
    _max_processing_us(0),
    _precision(NTP_PRECISION)
{
}

NTP::~NTP()
{
    if (_pcb != nullptr)
    {
        udp_remove(_pcb);
        _pcb = nullptr;
    }
}

void NTP::begin()
{
    while (_pcb == nullptr)
    {
        _pcb = udp_new();
        if (_pcb != nullptr)
        {
            const err_t error = udp_bind(_pcb, IP_ADDR_ANY, NTP_PORT);
            if (error == ERR_OK)
            {
                break;
            }
            dlog.error(TAG, F("failed to bind UDP port %d: %d"), NTP_PORT, error);
            udp_remove(_pcb);
            _pcb = nullptr;
        }
        else
        {
            dlog.error(TAG, F("failed to allocate UDP control block"));
        }

        delay(1000);
        dlog.warning(TAG, F("setup: retrying!"));
    }

    udp_recv(_pcb, &NTP::receive, this);
    dlog.info(TAG, F("raw UDP NTP server started on port %d"), NTP_PORT);
}

void NTP::getNTPTime(NTPTime *time)
{
    struct timeval tv;
    _gps.getTime(&tv);
    time->seconds = toNTP(tv.tv_sec);
    // 281474977 / 2^16 approximates 2^32 / 1,000,000 with less
    // than 1.1 ns error over the complete microsecond range and avoids
    // an expensive 64-bit division in the response path.
    time->fraction = static_cast<uint32_t>(
            (static_cast<uint64_t>(tv.tv_usec) * 281474977ULL) >> 16);
}

uint32_t NTP::getRootDispersion()
{
    uint64_t micros = _gps.getDispersionMicros();
    if (micros < TIMESTAMP_UNCERTAINTY_US)
    {
        // The ESP8266 cannot timestamp packets in the Wi-Fi MAC. Advertise
        // a conservative floor for uncertainty between the radio/driver and
        // the raw lwIP callback even when the PPS itself is more accurate.
        // Live comparisons against independent NTP sources showed about 3 ms
        // of residual end-to-end bias, so one millisecond was too optimistic.
        micros = TIMESTAMP_UNCERTAINTY_US;
    }
    uint64_t fixed = (micros * 65536ULL + MICROS_PER_SEC - 1) / MICROS_PER_SEC;
    if (fixed == 0)
    {
        fixed = 1;
    }
    if (fixed > UINT32_MAX)
    {
        fixed = UINT32_MAX;
    }
    return static_cast<uint32_t>(fixed);
}

void NTP::receive(void* arg, udp_pcb* pcb, pbuf* packet,
        const ip_addr_t* address, uint16_t port)
{
    (void)pcb;
    if (arg != nullptr && packet != nullptr && address != nullptr)
    {
        static_cast<NTP*>(arg)->ntp(packet, address, port);
    }
    if (packet != nullptr)
    {
        pbuf_free(packet);
    }
}

void NTP::ntp(pbuf* packet, const ip_addr_t* address, uint16_t port)
{
    NTPTime   recv_time;
    getNTPTime(&recv_time);
    const uint32_t processing_start_micros = micros();
    ++_req_count;

    if (packet->tot_len < sizeof(NTPPacket))
    {
        dlog.warning(TAG, F("receivePacket: ignoring packet with length %d (minimum %d)"),
                packet->tot_len, sizeof(NTPPacket));
        return;
    }

    if (!_gps.isValid())
    {
        dlog.warning(TAG, F("receivePacket: GPS data not valid!"));
        return;
    }

    NTPPacket request;
    if (pbuf_copy_partial(packet, &request, sizeof(request), 0) != sizeof(request))
    {
        dlog.error(TAG, F("failed to copy NTP request"));
        return;
    }

    const uint8_t request_version = getVERS(request.flags);
    if (getMODE(request.flags) != MODE_CLIENT ||
            (request_version != 3 && request_version != NTP_VERSION))
    {
        dlog.warning(TAG, F("ignoring unsupported NTP version/mode: %u/%u"),
                request_version, getMODE(request.flags));
        return;
    }

    //
    // Build the response directly in network byte order. The request transmit
    // timestamp is already in network order and becomes the response origin.
    //
    NTPPacket ntp;
    memset(&ntp, 0, sizeof(ntp));
    ntp.flags      = setLI(LI_NONE) | setVERS(request_version) | setMODE(MODE_SERVER);
    ntp.stratum    = 1;
    ntp.poll       = request.poll;
    ntp.precision  = _precision;
    ntp.delay      = htonl(0);
    ntp.dispersion = htonl(getRootDispersion());
    memcpy(ntp.ref_id, REF_ID, sizeof(ntp.ref_id));
    ntp.orig_time  = request.xmit_time;
    ntp.recv_time.seconds  = htonl(recv_time.seconds);
    ntp.recv_time.fraction = htonl(recv_time.fraction);

    struct timeval reference_time;
    _gps.getReferenceTime(&reference_time);
    ntp.ref_time.seconds = htonl(toNTP(reference_time.tv_sec));
    ntp.ref_time.fraction = 0;

    pbuf* response = pbuf_alloc(PBUF_TRANSPORT, sizeof(ntp), PBUF_RAM);
    if (response == nullptr)
    {
        dlog.error(TAG, F("failed to allocate NTP response"));
        return;
    }

    const err_t copy_error = pbuf_take(response, &ntp, sizeof(ntp));
    if (copy_error != ERR_OK)
    {
        pbuf_free(response);
        dlog.error(TAG, F("failed to copy NTP response: %d"), copy_error);
        return;
    }

    // Capture T3 after allocation and the full-packet copy. Only timestamp
    // conversion, an eight-byte in-place update, and udp_sendto() remain.
    NTPTime transmit_time;
    getNTPTime(&transmit_time);
    transmit_time.seconds  = htonl(transmit_time.seconds);
    transmit_time.fraction = htonl(transmit_time.fraction);
    const err_t timestamp_error = pbuf_take_at(response, &transmit_time,
            sizeof(transmit_time), offsetof(NTPPacket, xmit_time));
    const err_t send_error = timestamp_error == ERR_OK
            ? udp_sendto(_pcb, response, address, port) : timestamp_error;
    const uint32_t processing_us = micros() - processing_start_micros;
    _last_processing_us = processing_us;
    if (processing_us > _max_processing_us)
    {
        _max_processing_us = processing_us;
    }
    pbuf_free(response);

    if (send_error == ERR_OK)
    {
        ++_rsp_count;
    }
    else
    {
        dlog.error(TAG, F("failed to send NTP response: %d"), send_error);
    }
}
