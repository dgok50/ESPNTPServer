/*
 * NMEAService.h
 *
 * NMEA 0183 over TCP and UDP broadcast with DNS-SD discovery.
 */

#ifndef NMEASERVICE_H_
#define NMEASERVICE_H_

#include "Arduino.h"
#include "ESPAsyncTCP.h"
#include "ESPAsyncUDP.h"

class NMEAService
{
public:
    static constexpr uint16_t PORT = 10110;
    static constexpr uint8_t MAX_CLIENTS = 2;

    NMEAService();

    void begin(const char* hostname);
    void process();
    void queueSentence(const char* sentence);

    bool isMDNSAvailable() const { return _mdns_available; }
    bool isUDPBroadcastEnabled() const { return _udp_broadcast_enabled; }
    uint8_t getClientCount() const { return _client_count; }
    uint32_t getConnectionCount() const { return _connection_count; }
    uint32_t getSentenceCount() const { return _sentence_count; }
    uint32_t getFrameSentCount() const { return _frame_sent_count; }
    uint32_t getFrameDropCount() const { return _frame_drop_count; }
    uint32_t getQueueDropCount() const { return _queue_drop_count; }
    uint32_t getUDPFrameSentCount() const { return _udp_frame_sent_count; }
    uint32_t getUDPFrameDropCount() const { return _udp_frame_drop_count; }

    NMEAService(const NMEAService&) = delete;
    NMEAService& operator=(const NMEAService&) = delete;

private:
    static constexpr uint8_t QUEUE_SIZE = 4;
    static constexpr size_t MAX_SENTENCE_LENGTH = 127;
    static constexpr size_t MAX_FRAME_LENGTH = MAX_SENTENCE_LENGTH + 2;
    static constexpr uint8_t MAX_CONSECUTIVE_DROPS = 5;

    struct ClientSlot
    {
        AsyncClient* client;
        uint8_t consecutive_drops;
    };

    struct Frame
    {
        char data[MAX_FRAME_LENGTH];
        uint8_t length;
        uint8_t pending_clients;
        bool udp_pending;
    };

    AsyncServer _server;
    AsyncUDP _udp;
    ClientSlot _clients[MAX_CLIENTS];
    Frame _queue[QUEUE_SIZE];
    uint8_t _queue_head;
    uint8_t _queue_count;
    uint8_t _client_count;
    bool _mdns_available;
    bool _udp_broadcast_enabled;
    uint32_t _connection_count;
    uint32_t _sentence_count;
    uint32_t _frame_sent_count;
    uint32_t _frame_drop_count;
    uint32_t _queue_drop_count;
    uint32_t _udp_frame_sent_count;
    uint32_t _udp_frame_drop_count;

    static void onClient(void* arg, AsyncClient* client);
    static void onDisconnect(void* arg, AsyncClient* client);
    static void onTimeout(void* arg, AsyncClient* client, uint32_t time);
    static void deleteClient(void* arg, AsyncClient* client);

    void acceptClient(AsyncClient* client);
    void removeClient(AsyncClient* client);
    bool isLocalClient(AsyncClient* client) const;
    void dropFrame(Frame& frame);
    void clearClientFromQueue(uint8_t client_index);
    IPAddress broadcastAddress() const;
    static bool isSupportedSentence(const char* sentence, size_t length);
    static const char* field(const char* sentence, uint8_t number);
};

#endif /* NMEASERVICE_H_ */
