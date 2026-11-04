/*
 * LTPS.h
 *
 * Legacy Time Protocols Support (RFC 867 and RFC 868)
 */

#ifndef LTPS_H_
#define LTPS_H_

#include "Arduino.h"
#include "ESPAsyncTCP.h"
#include "ESPAsyncUDP.h"
#include "GPS.h"

class LTPS
{
public:
    explicit LTPS(GPS& gps);

    void begin();

    uint32_t getReqCount() const { return _req_count; }
    uint32_t getRspCount() const { return _rsp_count; }

    LTPS(const LTPS&) = delete;
    LTPS& operator=(const LTPS&) = delete;

private:
    GPS&        _gps;
    AsyncServer _tcp13;
    AsyncServer _tcp37;
    AsyncUDP    _udp13;
    AsyncUDP    _udp37;
    uint32_t    _req_count;
    uint32_t    _rsp_count;

    size_t formatDaytime(char* buffer, size_t size);
    uint32_t getTimeProtocolSeconds();
    void prepareClient(AsyncClient* client);
    bool sendTCP(AsyncClient* client, const void* data, size_t size);
    void handleTCP13(AsyncClient* client);
    void handleTCP37(AsyncClient* client);
    void handleUDP13(AsyncUDPPacket& packet);
    void handleUDP37(AsyncUDPPacket& packet);
};

#endif /* LTPS_H_ */
