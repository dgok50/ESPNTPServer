/*
 * GPS.h
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
 *  Created on: Feb 26, 2018
 *      Author: chris.l
 */

#ifndef GPS_H_
#define GPS_H_
#include "Arduino.h"
#include "MicroNMEA.h"
#include "UBXVersion.h"

#define REASON_SIZE       128
#define NMEA_BUFFER_SIZE  250
#define VALID_DELAY       60   // continuous GPS/PPS seconds before time service
#define PPS_TIMEOUT_US    1001000UL
#define NMEA_TIMEOUT_US   1100000UL

#define MICROS_PER_SEC    1000000UL

class GPS
{
public:
    typedef void (*SentenceHandler)(void* arg, const char* sentence);
    enum UBXCommandStatus
    {
        UBX_IDLE, UBX_QUEUED, UBX_WAITING, UBX_REPLIED,
        UBX_TIMEOUT, UBX_WRITE_FAILED
    };
    static const uint16_t UBX_COMMAND_MAX = 128;
    static const uint16_t UBX_RESPONSE_MAX = 1024;

    GPS(Stream& gps_serial, int pps_pin);
    virtual ~GPS();

    void     begin();
    void     process();
    void     end();

    bool     isValid() const       { return _valid; }
    bool     isGPSValid() const    { return _gps_valid; }
    bool     isPPSValid() const    { return !_pps_timed_out; }
    uint32_t getJitter();
    uint32_t getDispersionMicros();
    uint32_t getValidCount() const { return _valid_count; }
    uint32_t getValidDelay() const { return _valid_delay; }
    uint32_t getPPSTimeoutCount() const { return _timeouts; }
    uint32_t getPPSAgeMicros();
    void     pollVersionIfNeeded();
    bool     queueUBXCommand(const uint8_t* frame, uint16_t length,
                             uint32_t* request_id);
    void     processUBXCommand();
    UBXCommandStatus getUBXCommandStatus() const { return _ubx_command_status; }
    uint32_t getUBXCommandId() const { return _ubx_command_id; }
    uint8_t  getUBXResponseClass() const { return _ubx_response_class; }
    uint8_t  getUBXResponseId() const { return _ubx_response_id; }
    uint16_t getUBXResponseLength() const { return _ubx_response_length; }
    const uint8_t* getUBXResponsePayload() const { return _ubx_response_payload; }
    bool     hasReceiverVersion() const { return _ubx_version.valid(); }
    const char* getReceiverSoftware() const { return _ubx_version.software(); }
    const char* getReceiverHardware() const { return _ubx_version.hardware(); }
    const char* getReceiverExtension(uint8_t index) const
             { return _ubx_version.extension(index); }
    uint32_t getReceiverVersionResponses() const { return _ubx_version.responses(); }
    uint32_t getUBXChecksumErrors() const { return _ubx_version.checksumErrors(); }
    const UBXVersion::HardwareStatus& getReceiverHardwareStatus() const
             { return _ubx_version.hardwareStatus(); }
#ifdef UBX_DEBUG_API
    const UBXVersion& getUBXTelemetry() const { return _ubx_version; }
#endif
    uint32_t getVersionPollCount() const { return _version_polls; }
    bool versionPollTimedOut() const
             { return _version_polls >= 3 &&
                      millis() - _last_version_poll_ms >= 5000; }
    uint32_t getVersionPollWriteFailures() const { return _version_poll_write_failures; }
    uint32_t getUARTByteCount() const { return _uart_bytes; }
    uint32_t getNMEASentenceCount() const { return _nmea_sentences; }
    time_t   getValidSince() const { return _valid_since; }
    uint8_t  getSatelliteCount() { return _nmea.getNumSatellites(); }
    time_t   getSeconds();
    void     getTime(struct timeval* tv);
    void     getReferenceTime(struct timeval* tv);
    void     setSentenceHandler(SentenceHandler handler, void* arg)
             { _sentence_handler = handler; _sentence_handler_arg = arg; }

    // we don't allow copying this guy!
    GPS(const GPS&)            = delete;
    GPS& operator=(const GPS&) = delete;

private:
    Stream&           _stream;
    char              _buffer[NMEA_BUFFER_SIZE];
    MicroNMEA         _nmea;
    UBXVersion        _ubx_version;
    uint32_t          _uart_bytes;
    uint32_t          _nmea_sentences;
    uint32_t          _version_polls;
    uint32_t          _version_poll_write_failures;
    uint32_t          _last_version_poll_ms;
    uint8_t           _ubx_command[UBX_COMMAND_MAX];
    uint16_t          _ubx_command_length;
    uint8_t           _ubx_command_class;
    uint8_t           _ubx_command_message_id;
    uint32_t          _ubx_command_id;
    uint32_t          _ubx_command_sent_ms;
    UBXCommandStatus  _ubx_command_status;
    bool              _ubx_ack_received;
    uint8_t           _ubx_response_class;
    uint8_t           _ubx_response_id;
    uint16_t          _ubx_response_length;
    uint8_t           _ubx_response_payload[UBX_RESPONSE_MAX];
    volatile time_t   _seconds;
    volatile uint32_t _pps_count;
    volatile uint32_t _min_micros;
    volatile uint32_t _max_micros;
    volatile uint32_t _last_micros;

    uint8_t           _pps_pin;
    uint32_t          _processed_pps_count;
    uint32_t          _valid_delay;
    uint32_t          _valid_count;
    uint32_t          _timeouts;
    uint32_t          _last_rmc_micros;
    time_t            _valid_since;
    bool              _gps_valid;
    volatile bool     _valid;
    bool              _pps_timed_out;
    bool              _rmc_initialized;
    SentenceHandler   _sentence_handler;
    void*             _sentence_handler_arg;
    char              _reason[REASON_SIZE];

    static void ICACHE_RAM_ATTR ppsISR(void* arg);
    static void onUBXFrame(void* arg, uint8_t message_class, uint8_t message_id,
                           const uint8_t* payload, uint16_t length);
    void ICACHE_RAM_ATTR pps(uint32_t cur_micros);
    void processPPS();
    void setReason(const char* fmt, ...);
    void invalidate(const char* fmt, ...);
};

#endif /* GPS_H_ */
