/*
 * LTPS.cpp
 *
 * Legacy Time Protocols Support
 * Implementation of RFC 867 (Daytime Protocol) and RFC 868 (Time Protocol)
 */

#include <functional>
#include <lwip/def.h>
#include <time.h>

#include "LTPS.h"
#include "Log.h"

static const char* TAG = "LTPS";

namespace
{
constexpr uint16_t DAYTIME_PORT = 13;
constexpr uint16_t TIME_PORT = 37;
constexpr uint32_t NTP_UNIX_EPOCH_DELTA = 2208988800UL;

void deleteClient(void* arg, AsyncClient* client)
{
    (void)arg;
    delete client;
}
}

LTPS::LTPS(GPS& gps) :
    _gps(gps),
    _tcp13(DAYTIME_PORT),
    _tcp37(TIME_PORT),
    _udp13(),
    _udp37(),
    _req_count(0),
    _rsp_count(0)
{
}

void LTPS::begin()
{
    _tcp13.setNoDelay(true);
    _tcp13.onClient([this](void* arg, AsyncClient* client) {
        (void)arg;
        handleTCP13(client);
    }, nullptr);
    _tcp13.begin();

    _tcp37.setNoDelay(true);
    _tcp37.onClient([this](void* arg, AsyncClient* client) {
        (void)arg;
        handleTCP37(client);
    }, nullptr);
    _tcp37.begin();

    while (!_udp13.listen(DAYTIME_PORT))
    {
        dlog.error(TAG, F("failed to listen on UDP port %u; retrying"), DAYTIME_PORT);
        delay(1000);
    }
    _udp13.onPacket(std::bind(&LTPS::handleUDP13, this, std::placeholders::_1));

    while (!_udp37.listen(TIME_PORT))
    {
        dlog.error(TAG, F("failed to listen on UDP port %u; retrying"), TIME_PORT);
        delay(1000);
    }
    _udp37.onPacket(std::bind(&LTPS::handleUDP37, this, std::placeholders::_1));

    dlog.info(TAG, F("RFC 867 Daytime server started on TCP/UDP port %u"), DAYTIME_PORT);
    dlog.info(TAG, F("RFC 868 Time server started on TCP/UDP port %u"), TIME_PORT);
}

size_t LTPS::formatDaytime(char* buffer, size_t size)
{
    static const char* const weekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };
    static const char* const months[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    struct timeval tv;
    _gps.getTime(&tv);
    struct tm tm;
    gmtime_r(&tv.tv_sec, &tm);

    int length = snprintf(buffer, size, "%s, %s %d, %d %02d:%02d:%02d-UTC\r\n",
            weekdays[tm.tm_wday],
            months[tm.tm_mon],
            tm.tm_mday,
            tm.tm_year + 1900,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec);

    if (length < 0 || static_cast<size_t>(length) >= size)
    {
        return 0;
    }
    return static_cast<size_t>(length);
}

uint32_t LTPS::getTimeProtocolSeconds()
{
    struct timeval tv;
    _gps.getTime(&tv);
    return static_cast<uint32_t>(tv.tv_sec) + NTP_UNIX_EPOCH_DELTA;
}

void LTPS::prepareClient(AsyncClient* client)
{
    client->onDisconnect(deleteClient, nullptr);
}

bool LTPS::sendTCP(AsyncClient* client, const void* data, size_t size)
{
    const size_t written = client->write(
            static_cast<const char*>(data), size, ASYNC_WRITE_FLAG_COPY);
    client->close();
    return written == size;
}

void LTPS::handleTCP13(AsyncClient* client)
{
    if (client == nullptr)
    {
        return;
    }
    prepareClient(client);
    ++_req_count;

    if (!_gps.isValid())
    {
        dlog.warning(TAG, F("TCP/13: GPS data not valid"));
        client->close();
        return;
    }

    char buffer[64];
    const size_t length = formatDaytime(buffer, sizeof(buffer));
    if (length != 0 && sendTCP(client, buffer, length))
    {
        ++_rsp_count;
    }
    else
    {
        dlog.error(TAG, F("TCP/13: failed to queue response"));
        if (length == 0)
        {
            client->close();
        }
    }
}

void LTPS::handleTCP37(AsyncClient* client)
{
    if (client == nullptr)
    {
        return;
    }
    prepareClient(client);
    ++_req_count;

    if (!_gps.isValid())
    {
        dlog.warning(TAG, F("TCP/37: GPS data not valid"));
        client->close();
        return;
    }

    const uint32_t seconds = getTimeProtocolSeconds();
    const uint32_t networkSeconds = htonl(seconds);
    if (sendTCP(client, &networkSeconds, sizeof(networkSeconds)))
    {
        ++_rsp_count;
    }
    else
    {
        dlog.error(TAG, F("TCP/37: failed to queue response"));
    }
}

void LTPS::handleUDP13(AsyncUDPPacket& packet)
{
    ++_req_count;
    if (!_gps.isValid())
    {
        dlog.warning(TAG, F("UDP/13: GPS data not valid"));
        return;
    }

    char buffer[64];
    const size_t length = formatDaytime(buffer, sizeof(buffer));
    if (length != 0 && packet.write(reinterpret_cast<const uint8_t*>(buffer), length) == length)
    {
        ++_rsp_count;
    }
    else
    {
        dlog.error(TAG, F("UDP/13: failed to send response"));
    }
}

void LTPS::handleUDP37(AsyncUDPPacket& packet)
{
    ++_req_count;
    if (!_gps.isValid())
    {
        dlog.warning(TAG, F("UDP/37: GPS data not valid"));
        return;
    }

    const uint32_t seconds = htonl(getTimeProtocolSeconds());
    if (packet.write(reinterpret_cast<const uint8_t*>(&seconds), sizeof(seconds)) == sizeof(seconds))
    {
        ++_rsp_count;
    }
    else
    {
        dlog.error(TAG, F("UDP/37: failed to send response"));
    }
}
