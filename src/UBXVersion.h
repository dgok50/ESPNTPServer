#ifndef UBXVERSION_H_
#define UBXVERSION_H_

#include <stddef.h>
#include <stdint.h>

// Bounded UBX frame parser. It consumes binary frames before the NMEA parser
// sees them, retains the MON-VER fields, and can pass checked frames to a
// debug-only caller.
class UBXVersion
{
public:
    struct HardwareStatus
    {
        bool valid;
        uint32_t received_ms;
        uint16_t noise_per_ms;
        uint16_t agc_count;
        uint8_t antenna_status;
        uint8_t antenna_power;
        uint8_t jamming_state;
        uint8_t jam_ind;
    };
#ifdef UBX_DEBUG_API
    struct NavigationStatus
    {
        bool valid;
        uint32_t received_ms;
        uint32_t time_accuracy_ns;
        uint32_t horizontal_accuracy_mm;
        uint16_t position_dop_x100;
        uint8_t fix_type;
        uint8_t satellites_used;
        bool fix_ok;
        bool differential_solution;
    };
    struct DopStatus
    {
        bool valid;
        uint32_t received_ms;
        uint16_t time_dop_x100;
    };
    struct SatelliteStatus
    {
        bool valid;
        uint32_t received_ms;
        uint8_t channels;
        uint8_t gps_visible;
        uint8_t gps_locked;
        uint8_t gps_used;
        uint8_t gps_ephemeris;
        uint8_t used_cno_min_dbhz;
        uint8_t used_cno_avg_dbhz;
    };
    struct PortStatus
    {
        bool valid;
        uint32_t received_ms;
        uint16_t parity_errors;
        uint16_t framing_errors;
        uint16_t overrun_errors;
        uint16_t break_conditions;
    };
#endif
    typedef void (*FrameHandler)(void* arg, uint8_t message_class,
                                 uint8_t message_id, const uint8_t* payload,
                                 uint16_t length);
    UBXVersion();

    bool consume(uint8_t byte, uint32_t now_ms);
    void setFrameHandler(FrameHandler handler, void* arg)
        { _frame_handler = handler; _frame_handler_arg = arg; }
    bool valid() const { return _valid; }
    const char* software() const { return _software; }
    const char* hardware() const { return _hardware; }
    const char* extension(uint8_t index) const
        { return index < 3 ? _extensions[index] : ""; }
    uint32_t responses() const { return _responses; }
    uint32_t checksumErrors() const { return _checksum_errors; }
    const HardwareStatus& hardwareStatus() const { return _hardware_status; }
#ifdef UBX_DEBUG_API
    const NavigationStatus& navigationStatus() const { return _navigation_status; }
    const DopStatus& dopStatus() const { return _dop_status; }
    const SatelliteStatus& satelliteStatus() const { return _satellite_status; }
    const PortStatus& portStatus() const { return _port_status; }
    uint32_t interferenceWarningEvents() const { return _warning_events; }
    uint32_t interferenceCriticalEvents() const { return _critical_events; }
    bool hasInterferenceEvent() const { return _has_interference_event; }
    uint32_t lastInterferenceEventMs() const { return _last_interference_event_ms; }
    uint8_t maxJamIndLastHour(uint32_t now_ms) const;
#endif

private:
    static const uint16_t MAX_FRAME_LENGTH = 1024;
    static const size_t STORED_PAYLOAD_LENGTH = 1024;

    uint8_t _state;
    uint8_t _class;
    uint8_t _id;
    uint8_t _ck_a;
    uint8_t _ck_b;
    bool _first_checksum_valid;
    uint16_t _length;
    uint16_t _position;
    uint32_t _last_byte_ms;
    uint8_t _payload[STORED_PAYLOAD_LENGTH];
    bool _valid;
    uint32_t _responses;
    uint32_t _checksum_errors;
    HardwareStatus _hardware_status;
#ifdef UBX_DEBUG_API
    NavigationStatus _navigation_status;
    DopStatus _dop_status;
    SatelliteStatus _satellite_status;
    PortStatus _port_status;
    uint32_t _warning_events;
    uint32_t _critical_events;
    uint32_t _last_interference_event_ms;
    bool _has_interference_event;
    uint8_t _jam_max_by_minute[60];
    uint16_t _jam_minute_tag[60];
#endif
    char _software[31];
    char _hardware[11];
    char _extensions[3][31];
    FrameHandler _frame_handler;
    void* _frame_handler_arg;

    void addChecksum(uint8_t byte);
    void finish();
    void resetParser();
    static void copyText(char* dest, size_t dest_size,
                         const uint8_t* source, size_t source_size);
    static uint16_t readU2(const uint8_t* source);
    static uint32_t readU4(const uint8_t* source);
};

#endif
