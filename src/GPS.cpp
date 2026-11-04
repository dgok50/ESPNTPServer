/*
 * GPS.cpp
 *
 * Copyright 2018 Christopher B. Liebman
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

#include "GPS.h"
#include <interrupts.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>

#include "Log.h"
static const char* TAG = "GPS";

GPS::GPS(Stream& gps_stream, int pps_pin) :
    _stream(gps_stream),
    _nmea(_buffer, NMEA_BUFFER_SIZE),
    _uart_bytes(0),
    _nmea_sentences(0),
    _version_polls(0),
    _version_poll_write_failures(0),
    _last_version_poll_ms(0),
    _ubx_command(),
    _ubx_command_length(0),
    _ubx_command_class(0),
    _ubx_command_message_id(0),
    _ubx_command_id(0),
    _ubx_command_sent_ms(0),
    _ubx_command_status(UBX_IDLE),
    _ubx_ack_received(false),
    _ubx_response_class(0),
    _ubx_response_id(0),
    _ubx_response_length(0),
    _ubx_response_payload(),
    _seconds(0),
    _pps_count(0),
    _min_micros(0),
    _max_micros(0),
    _last_micros(0),
    _pps_pin(pps_pin),
    _processed_pps_count(0),
    _valid_delay(0),
    _valid_count(0),
    _timeouts(0),
    _last_rmc_micros(0),
    _valid_since(0),
    _gps_valid(false),
    _valid(false),
    _pps_timed_out(true),
    _rmc_initialized(false),
    _sentence_handler(nullptr),
    _sentence_handler_arg(nullptr)
{
    _reason[0] = '\0';
    _ubx_version.setFrameHandler(&GPS::onUBXFrame, this);
}

GPS::~GPS()
{
    end();
}

void GPS::begin()
{
    pinMode(_pps_pin, INPUT);
    attachInterruptArg(_pps_pin, &GPS::ppsISR, this, RISING);
}

void GPS::end()
{
    detachInterrupt(_pps_pin);
}

void GPS::getTime(struct timeval* tv)
{
    uint32_t cur_micros;
    uint32_t last_micros;
    time_t seconds;

    {
        // Keep the PPS-updated second and microsecond base from different
        // seconds from being combined when the interrupt fires mid-read.
        esp8266::InterruptLock lock;
        cur_micros = micros();
        seconds = _seconds;
        last_micros = _last_micros;
    }

    tv->tv_sec = seconds;
    tv->tv_usec = static_cast<uint32_t>(cur_micros - last_micros);

    //
    // if micros_delta is at or bigger than one second then
    // use the max just under 1 second.
    //
    if (last_micros == 0 || tv->tv_usec >= static_cast<suseconds_t>(MICROS_PER_SEC))
    {
        tv->tv_usec = MICROS_PER_SEC - 1;
    }
}

void GPS::getReferenceTime(struct timeval* tv)
{
    esp8266::InterruptLock lock;
    tv->tv_sec = _seconds;
    tv->tv_usec = 0;
}

time_t GPS::getSeconds()
{
    esp8266::InterruptLock lock;
    return _seconds;
}

uint32_t GPS::getJitter()
{
    uint32_t min_micros;
    uint32_t max_micros;
    {
        esp8266::InterruptLock lock;
        min_micros = _min_micros;
        max_micros = _max_micros;
    }
    if (min_micros == 0 || max_micros < min_micros)
    {
        return 0;
    }
    return max_micros - min_micros;
}

uint32_t GPS::getDispersionMicros()
{
    uint32_t min_micros;
    uint32_t max_micros;
    {
        esp8266::InterruptLock lock;
        min_micros = _min_micros;
        max_micros = _max_micros;
    }
    if (min_micros == 0 || max_micros == 0)
    {
        return MICROS_PER_SEC;
    }

    const uint32_t min_error = min_micros > MICROS_PER_SEC
            ? min_micros - MICROS_PER_SEC : MICROS_PER_SEC - min_micros;
    const uint32_t max_error = max_micros > MICROS_PER_SEC
            ? max_micros - MICROS_PER_SEC : MICROS_PER_SEC - max_micros;
    return min_error > max_error ? min_error : max_error;
}

uint32_t GPS::getPPSAgeMicros()
{
    uint32_t last_micros;
    uint32_t cur_micros;
    {
        esp8266::InterruptLock lock;
        cur_micros = micros();
        last_micros = _last_micros;
    }
    return last_micros == 0 ? UINT32_MAX : cur_micros - last_micros;
}

void GPS::process()
{
    processPPS();

    if (_reason[0] != '\0')
    {
        dlog.warning(TAG, F("REASON: %s"), _reason);
        _reason[0] = '\0';
    }

    while (_stream.available() > 0)
    {
        int c = _stream.read();
        ++_uart_bytes;
        if (_ubx_version.consume(static_cast<uint8_t>(c), millis()))
        {
            continue;
        }
        dlog.trace(TAG, F("c: %c"), c);
        if (_nmea.process(c))
        {
            ++_nmea_sentences;
            dlog.debug(TAG, F("'%s'"), _nmea.getSentence());

            const char* sentence = _nmea.getSentence();
            if (_sentence_handler != nullptr && sentence[0] == '$' &&
                    MicroNMEA::testChecksum(sentence))
            {
                _sentence_handler(_sentence_handler_arg, sentence);
            }

            const char * id = _nmea.getMessageID();

            //
            // if it was a RMC and its valid then check and maybe update the time
            //
            if (_nmea.getYear() > 2017 && strcmp("RMC", id) == 0)
            {
                struct tm tm;
                memset(&tm, 0, sizeof(tm));
                tm.tm_year  = _nmea.getYear() - 1900;
                tm.tm_mon   = _nmea.getMonth() - 1;
                tm.tm_mday  = _nmea.getDay();
                tm.tm_hour  = _nmea.getHour();
                tm.tm_min   = _nmea.getMinute();
                tm.tm_sec   = _nmea.getSecond();
                tm.tm_isdst = 0;
                time_t new_seconds = mktime(&tm);

                // Ignore the first mismatch after an RMC gap. A continuous
                // RMC stream is safe to use to label the PPS-driven seconds;
                // the 60-second validity delay hides any correction.
                const uint32_t cur_micros = micros();
                const bool continuous_rmc = _rmc_initialized &&
                        cur_micros - _last_rmc_micros <= NMEA_TIMEOUT_US;
                _last_rmc_micros = cur_micros;

                time_t old_seconds;
                bool adjusted = false;
                {
                    esp8266::InterruptLock lock;
                    old_seconds = _seconds;
                    if (old_seconds != new_seconds &&
                            (!_rmc_initialized || continuous_rmc))
                    {
                        _seconds = new_seconds;
                        adjusted = true;
                    }
                }

                if (adjusted)
                {
                    invalidate("seconds adjusted from %lu to %lu", old_seconds, new_seconds);
                    dlog.info(TAG, F("adjusting seconds from %lu to %lu from:'%s'"),
                            old_seconds, new_seconds, _nmea.getSentence());
                }
                else if (old_seconds != new_seconds)
                {
                    dlog.debug(TAG, F("ignoring first RMC after a gap: '%s'"),
                            _nmea.getSentence());
                }
                _rmc_initialized = true;
            }

            if (_nmea.isValid() && _nmea.getNumSatellites() >= 3)
            {
                //
                // if gps was not valid, it is now
                //
                if (!_gps_valid)
                {
                    {
                        // Only PPS edges strictly after this valid NMEA fix
                        // count toward the continuous validation interval.
                        esp8266::InterruptLock lock;
                        _processed_pps_count = _pps_count;
                    }
                    _valid_delay = VALID_DELAY;
                    _gps_valid = true;
                    _valid = false;
                    dlog.info(TAG, F("GPS valid!"));
                }
            }
            else /* nmea not valid or sat count < 4 */
            {
                if (_gps_valid || _valid_delay)
                {
                    invalidate("NMEA:%s SATS:%d from: '%s'",
                            _nmea.isValid() ? "valid" : "invalid",
                            _nmea.getNumSatellites(), _nmea.getSentence());
                }
            }
        }
    }

    processPPS();
}

void GPS::pollVersionIfNeeded()
{
    if (_ubx_version.valid() || _version_polls >= 3)
    {
        return;
    }
    const uint32_t now = millis();
    if (_version_polls != 0 && now - _last_version_poll_ms < 5000)
    {
        return;
    }

    // UBX-MON-VER poll: class 0x0a, id 0x04, zero-length payload.
    // Sending this fixed read-only request never changes receiver settings.
    static const uint8_t poll[] = { 0xb5, 0x62, 0x0a, 0x04,
                                    0x00, 0x00, 0x0e, 0x34 };
    if (_stream.write(poll, sizeof(poll)) != sizeof(poll))
    {
        ++_version_poll_write_failures;
    }
    _last_version_poll_ms = now;
    ++_version_polls;
}

bool GPS::queueUBXCommand(const uint8_t* frame, uint16_t length,
                          uint32_t* request_id)
{
    if (frame == nullptr || length < 8 || length > UBX_COMMAND_MAX ||
            _ubx_command_status == UBX_QUEUED ||
            _ubx_command_status == UBX_WAITING)
    {
        return false;
    }
    memcpy(_ubx_command, frame, length);
    _ubx_command_length = length;
    _ubx_command_class = frame[2];
    _ubx_command_message_id = frame[3];
    ++_ubx_command_id;
    _ubx_response_length = 0;
    _ubx_ack_received = false;
    _ubx_command_status = UBX_QUEUED;
    if (request_id != nullptr) *request_id = _ubx_command_id;
    return true;
}

void GPS::processUBXCommand()
{
    if (_ubx_command_status == UBX_WAITING &&
            millis() - _ubx_command_sent_ms > 2000)
    {
        _ubx_command_status = _ubx_ack_received ? UBX_REPLIED : UBX_TIMEOUT;
    }
    if (_ubx_command_status != UBX_QUEUED) return;

    _ubx_command_status = UBX_WAITING;
    _ubx_command_sent_ms = millis();
    if (_stream.write(_ubx_command, _ubx_command_length) !=
            _ubx_command_length)
    {
        _ubx_command_status = UBX_WRITE_FAILED;
    }
}

void GPS::onUBXFrame(void* arg, uint8_t message_class, uint8_t message_id,
                     const uint8_t* payload, uint16_t length)
{
    GPS* self = static_cast<GPS*>(arg);
    if (self->_ubx_command_status != UBX_WAITING) return;

    const bool directReply = message_class == self->_ubx_command_class &&
            message_id == self->_ubx_command_message_id;
    const bool acknowledgment = message_class == 0x05 &&
            (message_id == 0x00 || message_id == 0x01) && length == 2 &&
            payload[0] == self->_ubx_command_class &&
            payload[1] == self->_ubx_command_message_id;
    if (!directReply && !acknowledgment) return;

    self->_ubx_response_class = message_class;
    self->_ubx_response_id = message_id;
    self->_ubx_response_length = length;
    if (length != 0)
    {
        memcpy(self->_ubx_response_payload, payload, length);
    }
    if (directReply || message_id == 0x00)
    {
        self->_ubx_command_status = UBX_REPLIED;
    }
    else
    {
        self->_ubx_ack_received = true;
    }
}

void GPS::setReason(const char* fmt, ...)
{
    if (_reason[0] == '\0')
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(_reason, REASON_SIZE-1, fmt, ap);
        _reason[REASON_SIZE-1] = '\0';
        va_end(ap);
    }
}

void GPS::invalidate(const char* fmt, ...)
{
    if (_reason[0] == '\0')
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(_reason, REASON_SIZE-1, fmt, ap);
        _reason[REASON_SIZE-1] = '\0';
        va_end(ap);
    }
    _valid       = false;
    _gps_valid   = false;
    _valid_delay = 0;
}

void GPS::processPPS()
{
    uint32_t pps_count;
    uint32_t last_micros;
    time_t seconds;
    {
        esp8266::InterruptLock lock;
        pps_count = _pps_count;
        last_micros = _last_micros;
        seconds = _seconds;
    }

    uint32_t new_pulses = pps_count - _processed_pps_count;
    if (new_pulses != 0)
    {
        _processed_pps_count = pps_count;
        if (_pps_timed_out)
        {
            _pps_timed_out = false;
            new_pulses = 1;
            if (_gps_valid)
            {
                _valid = false;
                _valid_delay = VALID_DELAY;
            }
        }

        if (_gps_valid && _valid_delay != 0)
        {
            if (new_pulses >= _valid_delay)
            {
                const uint32_t extra_pulses = new_pulses - _valid_delay;
                _valid_delay = 0;
                _valid = true;
                _valid_since = seconds - extra_pulses;
                ++_valid_count;
                esp8266::InterruptLock lock;
                _min_micros = 0;
                _max_micros = 0;
            }
            else
            {
                _valid_delay -= new_pulses;
            }
        }
    }

    if (last_micros != 0 && micros() - last_micros > PPS_TIMEOUT_US &&
            !_pps_timed_out)
    {
        _pps_timed_out = true;
        ++_timeouts;
        _valid = false;
        _valid_delay = _gps_valid ? VALID_DELAY : 0;
        setReason("PPS timeout");
    }
}

void ICACHE_RAM_ATTR GPS::ppsISR(void* arg)
{
    const uint32_t cur_micros = micros();
    static_cast<GPS*>(arg)->pps(cur_micros);
}

void ICACHE_RAM_ATTR GPS::pps(uint32_t cur_micros)
{
    const uint32_t last_micros = _last_micros;

    _last_micros = cur_micros;
    _seconds += 1;
    _pps_count += 1;

    if (last_micros == 0)
    {
        return;
    }

    const uint32_t micros_count = cur_micros - last_micros;
    if (_min_micros == 0 || micros_count < _min_micros)
    {
        _min_micros = micros_count;
    }

    if (micros_count > _max_micros)
    {
        _max_micros = micros_count;
    }
}

