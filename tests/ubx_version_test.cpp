#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "UBXVersion.h"

struct CapturedFrame
{
    uint8_t message_class;
    uint8_t message_id;
    uint16_t length;
    uint32_t count;
};

static void capture(void* arg, uint8_t message_class, uint8_t message_id,
                    const uint8_t*, uint16_t length)
{
    CapturedFrame* result = static_cast<CapturedFrame*>(arg);
    result->message_class = message_class;
    result->message_id = message_id;
    result->length = length;
    ++result->count;
}

static std::vector<uint8_t> frame(const std::vector<uint8_t>& payload,
                                  uint8_t message_id = 0x04,
                                  uint8_t message_class = 0x0a)
{
    std::vector<uint8_t> bytes = {0xb5, 0x62, message_class, message_id,
                                  static_cast<uint8_t>(payload.size()),
                                  static_cast<uint8_t>(payload.size() >> 8)};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    uint8_t a = 0;
    uint8_t b = 0;
    for (size_t i = 2; i < bytes.size(); ++i)
    {
        a = static_cast<uint8_t>(a + bytes[i]);
        b = static_cast<uint8_t>(b + a);
    }
    bytes.push_back(a);
    bytes.push_back(b);
    return bytes;
}

int main()
{
    UBXVersion parser;
    CapturedFrame captured = {0, 0, 0, 0};
    parser.setFrameHandler(&capture, &captured);
    assert(!parser.consume('$', 0));
    assert(!parser.consume('G', 0));

    std::vector<uint8_t> payload(100, 0);
    memcpy(&payload[0], "ROM CORE 1.00", 13);
    memcpy(&payload[30], "00070000", 8);
    memcpy(&payload[40], "PROTVER=14.00", 13);
    memcpy(&payload[70], "MOD=NEO-7M", 10);
    std::vector<uint8_t> bytes = frame(payload);
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        assert(parser.consume(bytes[i], static_cast<uint32_t>(i)));
    }
    assert(parser.valid());
    assert(parser.responses() == 1);
    assert(captured.count == 1 && captured.message_class == 0x0a &&
           captured.message_id == 0x04 && captured.length == 100);
    assert(strcmp(parser.software(), "ROM CORE 1.00") == 0);
    assert(strcmp(parser.hardware(), "00070000") == 0);
    assert(strcmp(parser.extension(0), "PROTVER=14.00") == 0);
    assert(strcmp(parser.extension(1), "MOD=NEO-7M") == 0);
    assert(strcmp(parser.extension(2), "") == 0);

    bytes[bytes.size() - 1] ^= 1;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        assert(parser.consume(bytes[i], static_cast<uint32_t>(200 + i)));
    }
    assert(parser.responses() == 1);
    assert(parser.checksumErrors() == 1);
    assert(captured.count == 1);

    std::vector<uint8_t> hardware(60, 0);
    hardware[16] = 129;
    hardware[18] = 0x6a;
    hardware[19] = 0x07;
    hardware[20] = 2;
    hardware[21] = 1;
    hardware[22] = 0x09; // RTC calibrated, interference warning
    hardware[45] = 17;
    bytes = frame(hardware, 0x09);
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        assert(parser.consume(bytes[i], static_cast<uint32_t>(400 + i)));
    }
    const UBXVersion::HardwareStatus& hw = parser.hardwareStatus();
    assert(hw.valid && hw.noise_per_ms == 129 && hw.agc_count == 1898);
    assert(hw.antenna_status == 2 && hw.antenna_power == 1);
    assert(hw.jamming_state == 2 && hw.jam_ind == 17);
    assert(hw.received_ms == 400 + bytes.size() - 1);
#ifdef UBX_DEBUG_API
    assert(parser.interferenceWarningEvents() == 1);
    assert(parser.interferenceCriticalEvents() == 0);
    assert(parser.maxJamIndLastHour(1000) == 17);
    assert(parser.hasInterferenceEvent());
    assert(parser.lastInterferenceEventMs() == hw.received_ms);
#endif

    bytes.back() ^= 1;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        assert(parser.consume(bytes[i], static_cast<uint32_t>(600 + i)));
    }
    assert(parser.hardwareStatus().received_ms == hw.received_ms);

#ifdef UBX_DEBUG_API
    hardware[22] = 0x0d; // critical
    hardware[45] = 31;
    bytes = frame(hardware, 0x09);
    for (size_t i = 0; i < bytes.size(); ++i)
        assert(parser.consume(bytes[i], static_cast<uint32_t>(700 + i)));
    assert(parser.interferenceCriticalEvents() == 1);
    assert(parser.maxJamIndLastHour(1000) == 31);
    for (size_t i = 0; i < bytes.size(); ++i)
        assert(parser.consume(bytes[i], static_cast<uint32_t>(800 + i)));
    assert(parser.interferenceCriticalEvents() == 1); // not every sample
    assert(parser.maxJamIndLastHour(3600000) == 0);

    std::vector<uint8_t> pvt(84, 0);
    pvt[12] = 21; // tAcc = 21 ns
    pvt[20] = 3;  // 3D fix
    pvt[21] = 3;  // valid fix and differential solution
    pvt[23] = 4;
    pvt[40] = 0x34;
    pvt[41] = 0x12;
    pvt[76] = 0x38;
    pvt[77] = 0x05;
    bytes = frame(pvt, 0x07, 0x01);
    for (size_t i = 0; i < bytes.size(); ++i)
        assert(parser.consume(bytes[i], static_cast<uint32_t>(1200 + i)));
    const UBXVersion::NavigationStatus& nav = parser.navigationStatus();
    assert(nav.valid && nav.fix_type == 3 && nav.fix_ok);
    assert(nav.differential_solution && nav.satellites_used == 4);
    assert(nav.time_accuracy_ns == 21 && nav.horizontal_accuracy_mm == 0x1234);
    assert(nav.position_dop_x100 == 1336);

    std::vector<uint8_t> dop(18, 0);
    dop[8] = 0x7b;
    dop[9] = 0x01;
    bytes = frame(dop, 0x04, 0x01);
    for (size_t i = 0; i < bytes.size(); ++i)
        assert(parser.consume(bytes[i], static_cast<uint32_t>(1400 + i)));
    assert(parser.dopStatus().valid && parser.dopStatus().time_dop_x100 == 379);

    std::vector<uint8_t> svinfo(8 + 12 * 3, 0);
    svinfo[4] = 3;
    svinfo[9] = 5; svinfo[10] = 0x09; svinfo[11] = 5; svinfo[12] = 32;
    svinfo[21] = 9; svinfo[22] = 0x01; svinfo[23] = 4; svinfo[24] = 40;
    svinfo[33] = 33; svinfo[34] = 0x01; svinfo[35] = 5; svinfo[36] = 50; // SBAS
    bytes = frame(svinfo, 0x30, 0x01);
    for (size_t i = 0; i < bytes.size(); ++i)
        assert(parser.consume(bytes[i], static_cast<uint32_t>(1600 + i)));
    const UBXVersion::SatelliteStatus& sv = parser.satelliteStatus();
    assert(sv.valid && sv.channels == 3 && sv.gps_visible == 2);
    assert(sv.gps_locked == 2 && sv.gps_used == 2 && sv.gps_ephemeris == 1);
    assert(sv.used_cno_min_dbhz == 32 && sv.used_cno_avg_dbhz == 36);

    std::vector<uint8_t> io(120, 0);
    io[28] = 1; io[30] = 2; io[32] = 3; io[34] = 4;
    bytes = frame(io, 0x02);
    for (size_t i = 0; i < bytes.size(); ++i)
        assert(parser.consume(bytes[i], static_cast<uint32_t>(1800 + i)));
    const UBXVersion::PortStatus& port = parser.portStatus();
    assert(port.valid && port.parity_errors == 1 && port.framing_errors == 2);
    assert(port.overrun_errors == 3 && port.break_conditions == 4);
#endif

    assert(parser.consume(0xb5, 1000));
    assert(!parser.consume('$', 2001)); // stale partial frame is discarded
    return 0;
}
