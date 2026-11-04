#include "UBXVersion.h"

#include <string.h>

UBXVersion::UBXVersion() :
    _state(0), _class(0), _id(0), _ck_a(0), _ck_b(0),
    _first_checksum_valid(false), _length(0), _position(0),
    _last_byte_ms(0), _valid(false), _responses(0), _checksum_errors(0),
    _hardware_status(),
#ifdef UBX_DEBUG_API
    _navigation_status(), _dop_status(), _satellite_status(), _port_status(),
    _warning_events(0), _critical_events(0),
    _last_interference_event_ms(0), _has_interference_event(false),
#endif
    _frame_handler(nullptr), _frame_handler_arg(nullptr)
{
    memset(_payload, 0, sizeof(_payload));
    _software[0] = '\0';
    _hardware[0] = '\0';
    memset(_extensions, 0, sizeof(_extensions));
#ifdef UBX_DEBUG_API
    memset(_jam_max_by_minute, 0, sizeof(_jam_max_by_minute));
    memset(_jam_minute_tag, 0xff, sizeof(_jam_minute_tag));
#endif
}

uint16_t UBXVersion::readU2(const uint8_t* source)
{
    return static_cast<uint16_t>(source[0]) |
            (static_cast<uint16_t>(source[1]) << 8);
}

uint32_t UBXVersion::readU4(const uint8_t* source)
{
    return static_cast<uint32_t>(readU2(source)) |
            (static_cast<uint32_t>(readU2(source + 2)) << 16);
}

#ifdef UBX_DEBUG_API
uint8_t UBXVersion::maxJamIndLastHour(uint32_t now_ms) const
{
    const uint16_t current_minute = now_ms / 60000;
    uint8_t maximum = 0;
    for (size_t i = 0; i < 60; ++i)
    {
        if (_jam_minute_tag[i] != UINT16_MAX &&
                static_cast<uint16_t>(current_minute - _jam_minute_tag[i]) < 60 &&
                _jam_max_by_minute[i] > maximum)
        {
            maximum = _jam_max_by_minute[i];
        }
    }
    return maximum;
}
#endif

void UBXVersion::resetParser()
{
    _state = 0;
    _length = 0;
    _position = 0;
}

void UBXVersion::addChecksum(uint8_t byte)
{
    _ck_a = static_cast<uint8_t>(_ck_a + byte);
    _ck_b = static_cast<uint8_t>(_ck_b + _ck_a);
}

void UBXVersion::copyText(char* dest, size_t dest_size,
                          const uint8_t* source, size_t source_size)
{
    size_t i = 0;
    while (i + 1 < dest_size && i < source_size && source[i] != 0)
    {
        const uint8_t value = source[i];
        // Keep the plain-text HTTP diagnostics single-line and printable.
        dest[i] = value >= 32 && value <= 126 ? static_cast<char>(value) : '?';
        ++i;
    }
    dest[i] = '\0';
}

void UBXVersion::finish()
{
    // MON-HW is configured for slow periodic output by the receiver. Cache
    // only checksum-verified frames; HTTP diagnostics never poll the UART.
    if (_class == 0x0a && _id == 0x09 && _length == 60)
    {
#ifdef UBX_DEBUG_API
        const uint8_t previous_state = _hardware_status.jamming_state;
        const bool had_previous = _hardware_status.valid;
#endif
        _hardware_status.received_ms = _last_byte_ms;
        _hardware_status.noise_per_ms = readU2(_payload + 16);
        _hardware_status.agc_count = readU2(_payload + 18);
        _hardware_status.antenna_status = _payload[20];
        _hardware_status.antenna_power = _payload[21];
        _hardware_status.jamming_state = (_payload[22] >> 2) & 0x03;
        _hardware_status.jam_ind = _payload[45];
        _hardware_status.valid = true;
#ifdef UBX_DEBUG_API
        if (_hardware_status.jamming_state >= 2 &&
                (!had_previous || previous_state != _hardware_status.jamming_state))
        {
            if (_hardware_status.jamming_state == 2) ++_warning_events;
            else ++_critical_events;
            _last_interference_event_ms = _last_byte_ms;
            _has_interference_event = true;
        }
        const uint32_t minute = _last_byte_ms / 60000;
        const size_t bucket = minute % 60;
        const uint16_t minute_tag = static_cast<uint16_t>(minute);
        if (_jam_minute_tag[bucket] != minute_tag)
        {
            _jam_minute_tag[bucket] = minute_tag;
            _jam_max_by_minute[bucket] = 0;
        }
        if (_payload[45] > _jam_max_by_minute[bucket])
        {
            _jam_max_by_minute[bucket] = _payload[45];
        }
#endif
    }
#ifdef UBX_DEBUG_API
    else if (_class == 0x01 && _id == 0x07 && _length == 84)
    {
        _navigation_status.received_ms = _last_byte_ms;
        _navigation_status.time_accuracy_ns = readU4(_payload + 12);
        _navigation_status.horizontal_accuracy_mm = readU4(_payload + 40);
        _navigation_status.fix_type = _payload[20];
        _navigation_status.fix_ok = (_payload[21] & 0x01) != 0;
        _navigation_status.differential_solution = (_payload[21] & 0x02) != 0;
        _navigation_status.satellites_used = _payload[23];
        _navigation_status.position_dop_x100 = readU2(_payload + 76);
        _navigation_status.valid = true;
    }
    else if (_class == 0x01 && _id == 0x04 && _length == 18)
    {
        _dop_status.received_ms = _last_byte_ms;
        _dop_status.time_dop_x100 = readU2(_payload + 8);
        _dop_status.valid = true;
    }
    else if (_class == 0x01 && _id == 0x30 && _length >= 8 &&
            _length == static_cast<uint16_t>(8 + 12 * _payload[4]))
    {
        SatelliteStatus next;
        memset(&next, 0, sizeof(next));
        next.received_ms = _last_byte_ms;
        next.channels = _payload[4];
        uint16_t used_cno_sum = 0;
        for (uint8_t i = 0; i < next.channels; ++i)
        {
            const uint8_t* channel = _payload + 8 + 12 * i;
            if (channel[1] < 1 || channel[1] > 32) continue;
            const uint8_t quality = channel[3] & 0x0f;
            if (quality >= 2) ++next.gps_visible;
            if (quality >= 4) ++next.gps_locked;
            if ((channel[2] & 0x08) != 0) ++next.gps_ephemeris;
            if ((channel[2] & 0x01) != 0)
            {
                const uint8_t cno = channel[4];
                if (next.gps_used == 0 || cno < next.used_cno_min_dbhz)
                    next.used_cno_min_dbhz = cno;
                used_cno_sum += cno;
                ++next.gps_used;
            }
        }
        if (next.gps_used != 0)
            next.used_cno_avg_dbhz = used_cno_sum / next.gps_used;
        next.valid = true;
        _satellite_status = next;
    }
    else if (_class == 0x0a && _id == 0x02 && _length >= 40 &&
            _length % 20 == 0)
    {
        // MON-IO blocks are ordered DDC, UART1, UART2, USB, SPI.
        const uint8_t* uart1 = _payload + 20;
        _port_status.received_ms = _last_byte_ms;
        _port_status.parity_errors = readU2(uart1 + 8);
        _port_status.framing_errors = readU2(uart1 + 10);
        _port_status.overrun_errors = readU2(uart1 + 12);
        _port_status.break_conditions = readU2(uart1 + 14);
        _port_status.valid = true;
    }
#endif
    if (_frame_handler != nullptr)
    {
        _frame_handler(_frame_handler_arg, _class, _id, _payload, _length);
    }
    if (_class != 0x0a || _id != 0x04 || _length < 40 ||
            (_length - 40) % 30 != 0)
    {
        return;
    }

    copyText(_software, sizeof(_software), _payload, 30);
    copyText(_hardware, sizeof(_hardware), _payload + 30, 10);
    for (uint8_t i = 0; i < 3; ++i)
    {
        if (_length >= static_cast<uint16_t>(40 + 30 * (i + 1)))
        {
            copyText(_extensions[i], sizeof(_extensions[i]),
                    _payload + 40 + 30 * i, 30);
        }
        else
        {
            _extensions[i][0] = '\0';
        }
    }
    _valid = true;
    ++_responses;
}

bool UBXVersion::consume(uint8_t byte, uint32_t now_ms)
{
    if (_state != 0 && now_ms - _last_byte_ms > 1000)
    {
        resetParser();
    }
    _last_byte_ms = now_ms;

    switch (_state)
    {
        case 0: // Sync 1
            if (byte != 0xb5)
            {
                return false;
            }
            _state = 1;
            return true;
        case 1: // Sync 2
            if (byte == 0x62)
            {
                _state = 2;
                _ck_a = 0;
                _ck_b = 0;
            }
            else
            {
                _state = byte == 0xb5 ? 1 : 0;
            }
            return true;
        case 2: // Class
            _class = byte;
            addChecksum(byte);
            _state = 3;
            return true;
        case 3: // ID
            _id = byte;
            addChecksum(byte);
            _state = 4;
            return true;
        case 4: // Length low
            _length = byte;
            addChecksum(byte);
            _state = 5;
            return true;
        case 5: // Length high
            _length |= static_cast<uint16_t>(byte) << 8;
            addChecksum(byte);
            if (_length > MAX_FRAME_LENGTH)
            {
                resetParser();
            }
            else
            {
                _position = 0;
                _state = _length == 0 ? 7 : 6;
            }
            return true;
        case 6: // Payload
            addChecksum(byte);
            if (_position < STORED_PAYLOAD_LENGTH)
            {
                _payload[_position] = byte;
            }
            if (++_position == _length)
            {
                _state = 7;
            }
            return true;
        case 7: // Checksum A
            _first_checksum_valid = byte == _ck_a;
            _state = 8;
            return true;
        case 8: // Checksum B
            if (_first_checksum_valid && byte == _ck_b)
            {
                finish();
            }
            else
            {
                ++_checksum_errors;
            }
            resetParser();
            return true;
        default:
            resetParser();
            return false;
    }
}
