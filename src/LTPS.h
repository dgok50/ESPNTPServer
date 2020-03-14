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
    AsyncClient _tcpc;
    uint32_t _req_count;
    uint32_t _rsp_count;
    uint8_t  _precision;

    void getLTPSTime(LTPSTime *time);
    int8_t computePrecision();
    void ltps13(AsyncClient& atc); // daytime service (port 13) RFC867
    void ltps37(AsyncClient& atc); // time service (port 37) RFC868
};

#endif /* LTPS_H_ */
