/*
 * NMEAService.cpp
 *
 * Streams checksum-valid, usable GGA/RMC fixes as CRLF-terminated NMEA 0183
 * sentences. TCP and UDP broadcast are advertised through DNS-SD.
 */

#include <string.h>

#include "ESP8266WiFi.h"
#include "ESP8266mDNS.h"
#include "NMEAService.h"
#include "Log.h"

static const char* TAG = "NMEA";

NMEAService::NMEAService() :
    _server(PORT),
    _queue_head(0),
    _queue_count(0),
    _client_count(0),
    _mdns_available(false),
    _udp_broadcast_enabled(false),
    _connection_count(0),
    _sentence_count(0),
    _frame_sent_count(0),
    _frame_drop_count(0),
    _queue_drop_count(0),
    _udp_frame_sent_count(0),
    _udp_frame_drop_count(0)
{
    for (uint8_t i = 0; i < MAX_CLIENTS; ++i)
    {
        _clients[i].client = nullptr;
        _clients[i].consecutive_drops = 0;
    }
}

void NMEAService::begin(const char* hostname)
{
    _server.setNoDelay(true);
    _server.onClient(&NMEAService::onClient, this);
    _server.begin();
    _udp_broadcast_enabled = true;
    dlog.info(TAG, F("NMEA 0183 TCP and UDP broadcast started on port %u"), PORT);

    // WiFiManager may own a previous responder. Recreate it after OTA and
    // publish all run-time services under an RFC-compliant hostname.
    MDNS.end();
    if (!MDNS.begin(hostname))
    {
        dlog.error(TAG, F("failed to start mDNS responder as '%s'"), hostname);
        return;
    }

    const MDNSResponder::hMDNSService httpService =
            MDNS.addService("ESPNTP Management", "http", "tcp", 80);
    const MDNSResponder::hMDNSService nmeaService =
            MDNS.addService("ESPNTP NMEA", "nmea-0183", "tcp", PORT);
    const MDNSResponder::hMDNSService nmeaUDPService =
            MDNS.addService("ESPNTP NMEA Broadcast", "nmea-0183", "udp", PORT);
    if (httpService == nullptr || nmeaService == nullptr || nmeaUDPService == nullptr ||
            MDNS.addServiceTxt(nmeaService, "accuracy", "exact") == nullptr)
    {
        dlog.error(TAG, F("failed to publish DNS-SD services"));
        return;
    }

    _mdns_available = true;
    dlog.info(TAG, F("published _nmea-0183._tcp.local and _nmea-0183._udp.local as '%s.local'"), hostname);
}

void NMEAService::process()
{
    if (_mdns_available)
    {
        MDNS.update();
    }

    while (_queue_count != 0)
    {
        Frame& frame = _queue[_queue_head];
        if (frame.udp_pending)
        {
            const size_t written = _udp.writeTo(
                    reinterpret_cast<const uint8_t*>(frame.data), frame.length,
                    broadcastAddress(), PORT);
            if (written == frame.length)
            {
                ++_udp_frame_sent_count;
            }
            else
            {
                ++_udp_frame_drop_count;
            }
            // UDP has no acknowledgement or useful retry semantics for a
            // continuously refreshed navigation fix. Never hold the queue.
            frame.udp_pending = false;
        }

        for (uint8_t i = 0; i < MAX_CLIENTS; ++i)
        {
            const uint8_t clientBit = static_cast<uint8_t>(1U << i);
            if ((frame.pending_clients & clientBit) == 0)
            {
                continue;
            }

            ClientSlot& slot = _clients[i];
            AsyncClient* client = slot.client;
            if (client == nullptr || !client->connected())
            {
                frame.pending_clients &= static_cast<uint8_t>(~clientBit);
                continue;
            }

            if (!client->canSend() || client->space() < frame.length)
            {
                continue;
            }

            const size_t written = client->write(
                    frame.data, frame.length, ASYNC_WRITE_FLAG_COPY);
            if (written == frame.length)
            {
                slot.consecutive_drops = 0;
                ++_frame_sent_count;
                frame.pending_clients &= static_cast<uint8_t>(~clientBit);
            }
            else
            {
                ++_frame_drop_count;
                frame.pending_clients &= static_cast<uint8_t>(~clientBit);
                // A partial NMEA frame would desynchronize the stream.
                client->close();
            }
        }

        if (frame.pending_clients != 0)
        {
            // Retry after lwIP receives acknowledgements. Queue overflow, not
            // ordinary TCP backpressure, decides when a slow client drops data.
            return;
        }

        _queue_head = (_queue_head + 1) % QUEUE_SIZE;
        --_queue_count;
    }
}

void NMEAService::queueSentence(const char* sentence)
{
    if (sentence == nullptr)
    {
        return;
    }

    const size_t length = strlen(sentence);
    if (length == 0 || length > MAX_SENTENCE_LENGTH ||
            !isSupportedSentence(sentence, length))
    {
        return;
    }

    ++_sentence_count;
    if (_client_count == 0 && !_udp_broadcast_enabled)
    {
        return;
    }

    if (_queue_count == QUEUE_SIZE)
    {
        // Prefer the newest navigation fix when the producer outruns TCP.
        dropFrame(_queue[_queue_head]);
        _queue_head = (_queue_head + 1) % QUEUE_SIZE;
        --_queue_count;
        ++_queue_drop_count;
    }

    const uint8_t index = (_queue_head + _queue_count) % QUEUE_SIZE;
    Frame& frame = _queue[index];
    memcpy(frame.data, sentence, length);
    frame.data[length] = '\r';
    frame.data[length + 1] = '\n';
    frame.length = static_cast<uint8_t>(length + 2);
    frame.pending_clients = 0;
    frame.udp_pending = _udp_broadcast_enabled;
    for (uint8_t i = 0; i < MAX_CLIENTS; ++i)
    {
        if (_clients[i].client != nullptr)
        {
            frame.pending_clients |= static_cast<uint8_t>(1U << i);
        }
    }
    ++_queue_count;
}

void NMEAService::onClient(void* arg, AsyncClient* client)
{
    static_cast<NMEAService*>(arg)->acceptClient(client);
}

void NMEAService::onDisconnect(void* arg, AsyncClient* client)
{
    NMEAService* service = static_cast<NMEAService*>(arg);
    service->removeClient(client);
    delete client;
}

void NMEAService::onTimeout(void* arg, AsyncClient* client, uint32_t time)
{
    (void)arg;
    (void)time;
    client->close();
}

void NMEAService::deleteClient(void* arg, AsyncClient* client)
{
    (void)arg;
    delete client;
}

void NMEAService::acceptClient(AsyncClient* client)
{
    if (client == nullptr)
    {
        return;
    }

    if (!isLocalClient(client) || _client_count >= MAX_CLIENTS)
    {
        client->onDisconnect(&NMEAService::deleteClient, nullptr);
        client->close();
        return;
    }

    for (uint8_t i = 0; i < MAX_CLIENTS; ++i)
    {
        if (_clients[i].client == nullptr)
        {
            _clients[i].client = client;
            _clients[i].consecutive_drops = 0;
            ++_client_count;
            ++_connection_count;
            client->setNoDelay(true);
            client->setAckTimeout(3000);
            client->onTimeout(&NMEAService::onTimeout, this);
            client->onDisconnect(&NMEAService::onDisconnect, this);
            return;
        }
    }

    client->onDisconnect(&NMEAService::deleteClient, nullptr);
    client->close();
}

void NMEAService::removeClient(AsyncClient* client)
{
    for (uint8_t i = 0; i < MAX_CLIENTS; ++i)
    {
        if (_clients[i].client == client)
        {
            _clients[i].client = nullptr;
            _clients[i].consecutive_drops = 0;
            clearClientFromQueue(i);
            if (_client_count != 0)
            {
                --_client_count;
            }
            return;
        }
    }
}

bool NMEAService::isLocalClient(AsyncClient* client) const
{
    const IPAddress remote = client->remoteIP();
    const IPAddress local = WiFi.localIP();
    const IPAddress mask = WiFi.subnetMask();

    for (uint8_t i = 0; i < 4; ++i)
    {
        if ((remote[i] & mask[i]) != (local[i] & mask[i]))
        {
            return false;
        }
    }
    return true;
}

void NMEAService::dropFrame(Frame& frame)
{
    if (frame.udp_pending)
    {
        ++_udp_frame_drop_count;
        frame.udp_pending = false;
    }

    for (uint8_t i = 0; i < MAX_CLIENTS; ++i)
    {
        const uint8_t clientBit = static_cast<uint8_t>(1U << i);
        if ((frame.pending_clients & clientBit) == 0)
        {
            continue;
        }

        ++_frame_drop_count;
        ClientSlot& slot = _clients[i];
        if (slot.client != nullptr &&
                ++slot.consecutive_drops >= MAX_CONSECUTIVE_DROPS)
        {
            slot.client->close();
        }
    }
    frame.pending_clients = 0;
}

IPAddress NMEAService::broadcastAddress() const
{
    const IPAddress local = WiFi.localIP();
    const IPAddress mask = WiFi.subnetMask();
    IPAddress broadcast;
    for (uint8_t i = 0; i < 4; ++i)
    {
        broadcast[i] = static_cast<uint8_t>(local[i] | ~mask[i]);
    }
    return broadcast;
}

void NMEAService::clearClientFromQueue(uint8_t client_index)
{
    const uint8_t clientBit = static_cast<uint8_t>(1U << client_index);
    for (uint8_t offset = 0; offset < _queue_count; ++offset)
    {
        const uint8_t index = (_queue_head + offset) % QUEUE_SIZE;
        _queue[index].pending_clients &= static_cast<uint8_t>(~clientBit);
    }
}

const char* NMEAService::field(const char* sentence, uint8_t number)
{
    const char* cursor = sentence;
    while (*cursor != '\0')
    {
        if (*cursor++ == ',' && --number == 0)
        {
            return cursor;
        }
    }
    return nullptr;
}

bool NMEAService::isSupportedSentence(const char* sentence, size_t length)
{
    if (length < 10 || sentence[0] != '$' || sentence[6] != ',')
    {
        return false;
    }

    const bool isGGA = memcmp(sentence + 3, "GGA", 3) == 0;
    const bool isRMC = memcmp(sentence + 3, "RMC", 3) == 0;
    if (!isGGA && !isRMC)
    {
        return false;
    }

    const char* validity = field(sentence, isGGA ? 6 : 2);
    if (validity == nullptr)
    {
        return false;
    }
    return isGGA ? (*validity >= '1' && *validity <= '9')
                 : (*validity == 'A' && validity[1] == ',');
}
