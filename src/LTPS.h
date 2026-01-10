/*
 * LTPS.h
 *
 * Legacy Time Protocols Support
 */

#ifndef LTPS_H_
#define LTPS_H_

#include "Arduino.h"
#include "ESPAsyncTCP.h"
#include "GPS.h"

typedef struct ltps_time
{
    uint32_t seconds;
    uint32_t fraction;
} LTPSTime;

class LTPS
{
public:
    LTPS(GPS& gps);
    virtual ~LTPS();

    void     begin();

    uint32_t getReqCount() { return _req_count; }
    uint32_t getRspCount() { return _rsp_count; }

private:
    GPS&     _gps;
    AsyncServer* _server13;  // Daytime Protocol server (port 13)
    AsyncServer* _server37;  // Time Protocol server (port 37)
    uint32_t _req_count;
    uint32_t _rsp_count;
    uint8_t  _precision;

    void getLTPSTime(LTPSTime *time);
    int8_t computePrecision();
    void handleClient13(void* arg, AsyncClient* client); // daytime service (port 13) RFC867
    void handleClient37(void* arg, AsyncClient* client); // time service (port 37) RFC868
};

#endif /* LTPS_H_ */
