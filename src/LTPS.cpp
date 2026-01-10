/*
 * LTPS.cpp
 *
 * Legacy Time Protocols Support
 * Implementation of RFC 867 (Daytime Protocol) and RFC 868 (Time Protocol)
 */

#include <functional>
#include <lwip/def.h> // htonl() & ntohl()
#include <time.h>

#include "LTPS.h"
#include "Log.h"

static const char* TAG = "LTPS";

#define DAYTIME_PORT       13   // RFC 867 - Daytime Protocol
#define TIME_PORT          37   // RFC 868 - Time Protocol
#define PRECISION_COUNT    10000

#define SEVENTY_YEARS      2208988800L
#define toEPOCH(t)         ((uint32_t)t-SEVENTY_YEARS)
#define toNTP(t)           ((uint32_t)t+SEVENTY_YEARS)

LTPS::LTPS(GPS& gps) :
    _gps(gps),
    _server13(nullptr),
    _server37(nullptr),
    _req_count(0),
    _rsp_count(0),
    _precision(0)
{
}

LTPS::~LTPS()
{
    if (_server13)
    {
        delete _server13;
    }
    if (_server37)
    {
        delete _server37;
    }
}

void LTPS::begin()
{
    _precision = computePrecision();
    
    // Set up Daytime Protocol server (port 13)
    _server13 = new AsyncServer(DAYTIME_PORT);
    _server13->onClient([this](void* arg, AsyncClient* client) {
        this->handleClient13(arg, client);
    }, nullptr);
    _server13->begin();
    dlog.info(TAG, F("Daytime Protocol (RFC 867) server started on port %d"), DAYTIME_PORT);
    
    // Set up Time Protocol server (port 37)
    _server37 = new AsyncServer(TIME_PORT);
    _server37->onClient([this](void* arg, AsyncClient* client) {
        this->handleClient37(arg, client);
    }, nullptr);
    _server37->begin();
    dlog.info(TAG, F("Time Protocol (RFC 868) server started on port %d"), TIME_PORT);
}

int8_t LTPS::computePrecision()
{
    LTPSTime t;
    unsigned long start = micros();
    for (int i = 0; i < PRECISION_COUNT; ++i)
    {
        getLTPSTime(&t);
    }
    unsigned long end   = micros();
    double        total = (double)(end - start) / 1000000.0;
    double        time  = total / PRECISION_COUNT;
    double        prec  = log2(time);
    dlog.info(TAG, F("computePrecision: total:%f time:%f prec:%f (%d)"), total, time, prec, (int8_t)prec);
    return (int8_t)prec;
}

void LTPS::getLTPSTime(LTPSTime *time)
{
    struct timeval tv;
    _gps.getTime(&tv);
    time->seconds = toNTP(tv.tv_sec);

    double percent = us2s(tv.tv_usec);
    time->fraction = (uint32_t)(percent * (double)4294967296L);
}

// RFC 867 - Daytime Protocol (Port 13)
// Returns human-readable date and time in ASCII format
void LTPS::handleClient13(void* arg, AsyncClient* client)
{
    (void)arg; // unused
    
    if (!client)
    {
        return;
    }
    
    ++_req_count;
    
    if (!_gps.isValid())
    {
        dlog.warning(TAG, F("handleClient13: GPS data not valid!"));
        client->close(true);
        return;
    }

    struct timeval tv;
    _gps.getTime(&tv);
    struct tm tm;
    gmtime_r(&tv.tv_sec, &tm);

    // RFC 867 doesn't specify exact format, but suggests readable format
    // Common format: "Weekday, Month DD, YYYY HH:MM:SS-TZ"
    char buffer[64];
    const char* weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const char* months[] = {"January", "February", "March", "April", "May", "June", 
                           "July", "August", "September", "October", "November", "December"};
    
    snprintf(buffer, sizeof(buffer), "%s, %s %d, %d %02d:%02d:%02d-UTC\r\n",
             weekdays[tm.tm_wday],
             months[tm.tm_mon],
             tm.tm_mday,
             tm.tm_year + 1900,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec);
    
    client->write(buffer, strlen(buffer));
    client->close(true);
    ++_rsp_count;
    
    dlog.info(TAG, F("handleClient13: sent daytime response"));
}

// RFC 868 - Time Protocol (Port 37)
// Returns time as 32-bit unsigned integer (seconds since 1900-01-01 00:00:00 UTC)
void LTPS::handleClient37(void* arg, AsyncClient* client)
{
    (void)arg; // unused
    
    if (!client)
    {
        return;
    }
    
    ++_req_count;
    
    if (!_gps.isValid())
    {
        dlog.warning(TAG, F("handleClient37: GPS data not valid!"));
        client->close(true);
        return;
    }

    LTPSTime time;
    getLTPSTime(&time);
    
    // RFC 868 specifies network byte order (big-endian)
    uint32_t time_network = htonl(time.seconds);
    
    client->write((const char*)&time_network, sizeof(time_network));
    client->close(true);
    ++_rsp_count;
    
    dlog.info(TAG, F("handleClient37: sent time response: %lu"), time.seconds);
}
