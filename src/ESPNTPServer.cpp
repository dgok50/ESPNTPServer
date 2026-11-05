/*
 * ESPNTPServer.cpp
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
 *
 *  Created on: Oct 29, 2017
 *      Author: liebman
 */

#include "ESPNTPServer.h"
#include "ESPNTPServerVersion.h"
#include "ESP8266WebServer.h"

#include "Log.h"
#include "DLogPrintWriter.h"
#include "DLogSyslogWriter.h"

#include "GPS.h"
#include "NTP.h"
#include "LTPS.h"
#include "NMEAService.h"
#include "Display.h"
#include "Config.h"

DLog& dlog = DLog::getLog();
GPS gps(Serial, SYNC_PIN);
NTP ntp(gps);
LTPS ltps(gps);
NMEAService nmeaService;
Display display(gps, ntp, SDA_PIN, SCL_PIN);
Config config;
ESP8266WebServer managementServer(80);

char devicename[32];
char mdnsHostname[32];
bool otaRestartPending = false;
uint32_t otaRestartAt = 0;

const char* SETUP_TAG = "setup";
const char* LOOP_TAG  = "loop";

namespace
{
const char* MANAGEMENT_HEADERS[] = { "X-OTA-Confirm" };

const char* wifiStatusName(int status)
{
    switch (status)
    {
        case WL_CONNECTED:       return "connected";
        case WL_NO_SSID_AVAIL:   return "no_ssid";
        case WL_CONNECT_FAILED:  return "failed";
        case WL_IDLE_STATUS:     return "idle";
        case WL_DISCONNECTED:    return "disconnected";
        default:                 return "unknown";
    }
}

bool isLocalRequest()
{
    const IPAddress remote = managementServer.client().remoteIP();
    const IPAddress local = WiFi.localIP();
    const IPAddress mask = WiFi.subnetMask();

    for (uint8_t i = 0; i < 4; ++i)
    {
        if ((remote[i] & mask[i]) != (local[i] & mask[i]))
        {
            return false;
        }
    }
    return true;
}

void sendText(int status, const String& body)
{
    managementServer.sendHeader(F("Cache-Control"), F("no-store"));
    managementServer.send(status, F("text/plain; charset=utf-8"), body);
}

void handleDiagnostics()
{
    if (!isLocalRequest())
    {
        sendText(403, F("error=local_network_only\n"));
        return;
    }

    struct timeval tv;
    gps.getTime(&tv);

    String body;
#ifdef UBX_DEBUG_API
    body.reserve(3200);
#else
    body.reserve(2200);
#endif
    body += F("service=ESPNTPServer\n");
    body += F("version="); body += ESPNTP_SERVER_VERSION; body += '\n';
    body += F("build="); body += __DATE__; body += ' '; body += __TIME__; body += '\n';
    body += F("uptime_ms="); body += millis(); body += '\n';
    body += F("reset_reason="); body += ESP.getResetReason(); body += '\n';
    body += F("chip_id="); body += String(ESP.getChipId(), HEX); body += '\n';
    body += F("cpu_mhz="); body += ESP.getCpuFreqMHz(); body += '\n';
    body += F("free_heap="); body += ESP.getFreeHeap(); body += '\n';
    body += F("flash_size="); body += ESP.getFlashChipSize(); body += '\n';
    body += F("flash_real_size="); body += ESP.getFlashChipRealSize(); body += '\n';
    body += F("sketch_size="); body += ESP.getSketchSize(); body += '\n';
    body += F("free_sketch_space="); body += ESP.getFreeSketchSpace(); body += '\n';
    body += F("sketch_md5="); body += ESP.getSketchMD5(); body += '\n';
    body += F("wifi_status="); body += wifiStatusName(WiFi.status()); body += '\n';
    body += F("wifi_rssi_dbm="); body += WiFi.RSSI(); body += '\n';
    body += F("wifi_ip="); body += WiFi.localIP().toString(); body += '\n';
    body += F("wifi_gateway="); body += WiFi.gatewayIP().toString(); body += '\n';
    body += F("wifi_netmask="); body += WiFi.subnetMask().toString(); body += '\n';
    body += F("wifi_mac="); body += WiFi.macAddress(); body += '\n';
    body += F("gps_valid="); body += gps.isValid() ? F("true\n") : F("false\n");
    body += F("gps_input_valid="); body += gps.isGPSValid() ? F("true\n") : F("false\n");
    body += F("gps_pps_valid="); body += gps.isPPSValid() ? F("true\n") : F("false\n");
    body += F("gps_satellites="); body += gps.getSatelliteCount(); body += '\n';
    body += F("gps_valid_delay_s="); body += gps.getValidDelay(); body += '\n';
    body += F("gps_unix_time="); body += static_cast<uint32_t>(tv.tv_sec); body += '\n';
    body += F("gps_jitter_us="); body += gps.getJitter(); body += '\n';
    const uint32_t ppsAgeMicros = gps.getPPSAgeMicros();
    body += F("gps_pps_age_us=");
    if (ppsAgeMicros == UINT32_MAX)
    {
        body += F("unavailable\n");
    }
    else
    {
        body += ppsAgeMicros; body += '\n';
    }
    body += F("gps_pps_timeouts="); body += gps.getPPSTimeoutCount(); body += '\n';
    body += F("gps_valid_count="); body += gps.getValidCount(); body += '\n';
    body += F("gps_uart_bytes="); body += gps.getUARTByteCount(); body += '\n';
    body += F("gps_nmea_sentences_raw="); body += gps.getNMEASentenceCount(); body += '\n';
    body += F("gps_ubx_version_polls="); body += gps.getVersionPollCount(); body += '\n';
    body += F("gps_ubx_poll_write_failures="); body += gps.getVersionPollWriteFailures(); body += '\n';
    body += F("gps_ubx_checksum_errors="); body += gps.getUBXChecksumErrors(); body += '\n';
#ifdef UBX_DEBUG_API
    const UBXVersion& telemetry = gps.getUBXTelemetry();
    const uint32_t telemetryNowMs = millis();
    const UBXVersion::HardwareStatus& hw = gps.getReceiverHardwareStatus();
    body += F("gps_interference_status=");
    if (!hw.valid)
    {
        body += F("unavailable\n");
    }
    else
    {
        body += hw.jamming_state == 1 ? F("ok\n") :
                hw.jamming_state == 2 ? F("warning\n") :
                hw.jamming_state == 3 ? F("critical\n") : F("unknown\n");
        body += F("gps_interference_age_ms="); body += telemetryNowMs - hw.received_ms; body += '\n';
        body += F("gps_jam_ind="); body += hw.jam_ind; body += '\n';
        body += F("gps_noise_per_ms="); body += hw.noise_per_ms; body += '\n';
        body += F("gps_agc_count="); body += hw.agc_count; body += '\n';
        body += F("gps_antenna_status=");
        body += hw.antenna_status == 2 ? F("ok\n") :
                hw.antenna_status == 3 ? F("short\n") :
                hw.antenna_status == 4 ? F("open\n") : F("unknown\n");
        body += F("gps_antenna_power=");
        body += hw.antenna_power == 1 ? F("on\n") :
                hw.antenna_power == 0 ? F("off\n") : F("unknown\n");
    }
    body += F("gps_interference_warning_events="); body += telemetry.interferenceWarningEvents(); body += '\n';
    body += F("gps_interference_critical_events="); body += telemetry.interferenceCriticalEvents(); body += '\n';
    body += F("gps_jam_ind_max_1h="); body += telemetry.maxJamIndLastHour(telemetryNowMs); body += '\n';
    body += F("gps_interference_last_event_age_ms=");
    if (telemetry.hasInterferenceEvent())
    {
        body += telemetryNowMs - telemetry.lastInterferenceEventMs();
        body += '\n';
    }
    else body += F("unavailable\n");
    body += F("gps_interference_last_event_unix=");
    if (telemetry.hasInterferenceEvent() && gps.isGPSValid() &&
            static_cast<uint32_t>(tv.tv_sec) >
            (telemetryNowMs - telemetry.lastInterferenceEventMs()) / 1000)
    {
        body += static_cast<uint32_t>(tv.tv_sec) -
                (telemetryNowMs - telemetry.lastInterferenceEventMs()) / 1000;
        body += '\n';
    }
    else body += F("unavailable\n");

    const UBXVersion::NavigationStatus& nav = telemetry.navigationStatus();
    body += F("gps_nav_pvt_status="); body += nav.valid ? F("received\n") : F("unavailable\n");
    if (nav.valid)
    {
        body += F("gps_nav_pvt_age_ms="); body += telemetryNowMs - nav.received_ms; body += '\n';
        body += F("gps_nav_fix_type="); body += nav.fix_type; body += '\n';
        body += F("gps_nav_fix_ok="); body += nav.fix_ok ? F("true\n") : F("false\n");
        body += F("gps_nav_satellites_used="); body += nav.satellites_used; body += '\n';
        body += F("gps_nav_time_accuracy_ns="); body += nav.time_accuracy_ns; body += '\n';
        body += F("gps_nav_horizontal_accuracy_mm="); body += nav.horizontal_accuracy_mm; body += '\n';
        body += F("gps_nav_pdop_x100="); body += nav.position_dop_x100; body += '\n';
        body += F("gps_nav_differential="); body += nav.differential_solution ? F("true\n") : F("false\n");
    }
    const UBXVersion::DopStatus& dop = telemetry.dopStatus();
    body += F("gps_nav_dop_status="); body += dop.valid ? F("received\n") : F("unavailable\n");
    if (dop.valid)
    {
        body += F("gps_nav_dop_age_ms="); body += telemetryNowMs - dop.received_ms; body += '\n';
        body += F("gps_nav_tdop_x100="); body += dop.time_dop_x100; body += '\n';
    }
    const UBXVersion::SatelliteStatus& sv = telemetry.satelliteStatus();
    body += F("gps_svinfo_status="); body += sv.valid ? F("received\n") : F("unavailable\n");
    if (sv.valid)
    {
        body += F("gps_svinfo_age_ms="); body += telemetryNowMs - sv.received_ms; body += '\n';
        body += F("gps_svinfo_channels="); body += sv.channels; body += '\n';
        body += F("gps_svinfo_gps_visible="); body += sv.gps_visible; body += '\n';
        body += F("gps_svinfo_gps_locked="); body += sv.gps_locked; body += '\n';
        body += F("gps_svinfo_gps_used="); body += sv.gps_used; body += '\n';
        body += F("gps_svinfo_gps_ephemeris="); body += sv.gps_ephemeris; body += '\n';
        body += F("gps_svinfo_used_cno_min_dbhz="); body += sv.used_cno_min_dbhz; body += '\n';
        body += F("gps_svinfo_used_cno_avg_dbhz="); body += sv.used_cno_avg_dbhz; body += '\n';
    }
    const UBXVersion::PortStatus& port = telemetry.portStatus();
    body += F("gps_uart1_mon_io_status="); body += port.valid ? F("received\n") : F("unavailable\n");
    if (port.valid)
    {
        body += F("gps_uart1_mon_io_age_ms="); body += telemetryNowMs - port.received_ms; body += '\n';
        body += F("gps_uart1_parity_errors="); body += port.parity_errors; body += '\n';
        body += F("gps_uart1_framing_errors="); body += port.framing_errors; body += '\n';
        body += F("gps_uart1_overrun_errors="); body += port.overrun_errors; body += '\n';
        body += F("gps_uart1_break_conditions="); body += port.break_conditions; body += '\n';
    }
#endif
    body += F("gps_ubx_version_responses="); body += gps.getReceiverVersionResponses(); body += '\n';
    body += F("gps_ubx_version_status=");
    body += gps.hasReceiverVersion() ? F("received\n") :
            (gps.versionPollTimedOut() ? F("timeout\n") : F("pending\n"));
    if (gps.hasReceiverVersion())
    {
        body += F("gps_ubx_software="); body += gps.getReceiverSoftware(); body += '\n';
        body += F("gps_ubx_hardware="); body += gps.getReceiverHardware(); body += '\n';
        for (uint8_t i = 0; i < 3; ++i)
        {
            const char* extension = gps.getReceiverExtension(i);
            if (extension[0] != '\0')
            {
                body += F("gps_ubx_extension_"); body += i; body += '=';
                body += extension; body += '\n';
            }
        }
    }
    if (gps.isValid())
    {
        body += F("gps_valid_since="); body += static_cast<uint32_t>(gps.getValidSince()); body += '\n';
    }
    body += F("ntp_requests="); body += ntp.getReqCount(); body += '\n';
    body += F("ntp_responses="); body += ntp.getRspCount(); body += '\n';
    body += F("ntp_timestamp_mode=software_lwip\n");
    body += F("ntp_timestamp_uncertainty_us="); body += NTP::TIMESTAMP_UNCERTAINTY_US; body += '\n';
    body += F("ntp_processing_last_us="); body += ntp.getLastProcessingMicros(); body += '\n';
    body += F("ntp_processing_max_us="); body += ntp.getMaxProcessingMicros(); body += '\n';
    body += F("legacy_requests="); body += ltps.getReqCount(); body += '\n';
    body += F("legacy_responses="); body += ltps.getRspCount(); body += '\n';
    body += F("nmea_tcp_port="); body += NMEAService::PORT; body += '\n';
    body += F("nmea_udp_port="); body += NMEAService::PORT; body += '\n';
    body += F("nmea_udp_broadcast="); body += nmeaService.isUDPBroadcastEnabled() ? F("true\n") : F("false\n");
    body += F("nmea_mdns="); body += nmeaService.isMDNSAvailable() ? F("true\n") : F("false\n");
    body += F("nmea_clients="); body += nmeaService.getClientCount(); body += '\n';
    body += F("nmea_connections="); body += nmeaService.getConnectionCount(); body += '\n';
    body += F("nmea_sentences="); body += nmeaService.getSentenceCount(); body += '\n';
    body += F("nmea_frames_sent="); body += nmeaService.getFrameSentCount(); body += '\n';
    body += F("nmea_frames_dropped="); body += nmeaService.getFrameDropCount(); body += '\n';
    body += F("nmea_queue_dropped="); body += nmeaService.getQueueDropCount(); body += '\n';
    body += F("nmea_udp_frames_sent="); body += nmeaService.getUDPFrameSentCount(); body += '\n';
    body += F("nmea_udp_frames_dropped="); body += nmeaService.getUDPFrameDropCount(); body += '\n';
    body += F("ota_trigger=POST /ota\n");
    body += F("ota_pending="); body += otaRestartPending ? F("true\n") : F("false\n");

    sendText(200, body);
}

#ifdef UBX_DEBUG_API
// TODO: This intentionally unauthenticated experiment must never be enabled
// in the normal firmware. Add an authenticated command policy before any
// production receiver-control API is considered.
int ubxHexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char ubxHexDigit(uint8_t nibble)
{
    return "0123456789abcdef"[nibble & 0x0f];
}

void handleUBXCommand()
{
    if (!isLocalRequest())
    {
        sendText(403, F("error=local_network_only\n"));
        return;
    }

    const String body = managementServer.arg("plain");
    if (body.length() < 16 || body.length() > GPS::UBX_COMMAND_MAX * 2 ||
            (body.length() & 1) != 0)
    {
        sendText(400, F("error=invalid_frame_length\n"));
        return;
    }

    uint8_t frame[GPS::UBX_COMMAND_MAX];
    const uint16_t length = body.length() / 2;
    for (uint16_t i = 0; i < length; ++i)
    {
        const int high = ubxHexValue(body[i * 2]);
        const int low = ubxHexValue(body[i * 2 + 1]);
        if (high < 0 || low < 0)
        {
            sendText(400, F("error=invalid_hex\n"));
            return;
        }
        frame[i] = static_cast<uint8_t>((high << 4) | low);
    }
    if (frame[0] != 0xb5 || frame[1] != 0x62 ||
            static_cast<uint16_t>(frame[4] | (frame[5] << 8)) + 8 != length)
    {
        sendText(400, F("error=invalid_ubx_frame\n"));
        return;
    }
    uint8_t ckA = 0;
    uint8_t ckB = 0;
    for (uint16_t i = 2; i < length - 2; ++i)
    {
        ckA += frame[i];
        ckB += ckA;
    }
    if (frame[length - 2] != ckA || frame[length - 1] != ckB)
    {
        sendText(400, F("error=invalid_ubx_checksum\n"));
        return;
    }

    uint32_t requestId;
    if (!gps.queueUBXCommand(frame, length, &requestId))
    {
        sendText(409, F("error=command_busy\n"));
        return;
    }
    sendText(202, String(F("request_id=")) + requestId + '\n');
}

const char* ubxCommandStatusName(GPS::UBXCommandStatus status)
{
    switch (status)
    {
        case GPS::UBX_IDLE: return "idle";
        case GPS::UBX_QUEUED: return "queued";
        case GPS::UBX_WAITING: return "waiting";
        case GPS::UBX_REPLIED: return "replied";
        case GPS::UBX_TIMEOUT: return "timeout";
        case GPS::UBX_WRITE_FAILED: return "write_failed";
        default: return "unknown";
    }
}

void handleUBXResult()
{
    if (!isLocalRequest())
    {
        sendText(403, F("error=local_network_only\n"));
        return;
    }
    String body;
    body.reserve(2200);
    body += F("request_id="); body += gps.getUBXCommandId(); body += '\n';
    body += F("status="); body += ubxCommandStatusName(gps.getUBXCommandStatus());
    body += '\n';
    if (gps.getUBXCommandStatus() == GPS::UBX_REPLIED)
    {
        body += F("response_class="); body += gps.getUBXResponseClass(); body += '\n';
        body += F("response_id="); body += gps.getUBXResponseId(); body += '\n';
        body += F("response_length="); body += gps.getUBXResponseLength(); body += '\n';
        body += F("response_payload_hex=");
        const uint8_t* payload = gps.getUBXResponsePayload();
        for (uint16_t i = 0; i < gps.getUBXResponseLength(); ++i)
        {
            body += ubxHexDigit(payload[i] >> 4);
            body += ubxHexDigit(payload[i]);
        }
        body += '\n';
    }
    sendText(200, body);
}
#endif

void handleOTARequest()
{
    if (!isLocalRequest())
    {
        sendText(403, F("error=local_network_only\n"));
        return;
    }
    if (managementServer.header(F("X-OTA-Confirm")) != F("reboot"))
    {
        sendText(400, F("error=missing_header\nrequired=X-OTA-Confirm: reboot\n"));
        return;
    }
    if (otaRestartPending)
    {
        sendText(409, F("error=ota_already_pending\n"));
        return;
    }

    otaRestartPending = true;
    otaRestartAt = millis() + 1000;
    dlog.warning("management", F("forced OTA check requested by %s"),
            managementServer.client().remoteIP().toString().c_str());
    sendText(202, F("status=ota_check_scheduled\nreboot_in_ms=1000\n"));
}

void beginManagementServer()
{
    managementServer.collectHeaders(MANAGEMENT_HEADERS,
            sizeof(MANAGEMENT_HEADERS) / sizeof(MANAGEMENT_HEADERS[0]));
    managementServer.on("/", HTTP_GET, []() {
        if (!isLocalRequest())
        {
            sendText(403, F("error=local_network_only\n"));
            return;
        }
#ifdef UBX_DEBUG_API
        sendText(200, F("ESPNTPServer DEBUG\nGET /diagnostics.txt\n"
                        "POST /debug/ubx\nGET /debug/ubx\nPOST /ota\n"));
#else
        sendText(200, F("ESPNTPServer\nGET /diagnostics.txt\nPOST /ota\n"));
#endif
    });
    managementServer.on("/diagnostics.txt", HTTP_GET, handleDiagnostics);
#ifdef UBX_DEBUG_API
    managementServer.on("/debug/ubx", HTTP_POST, handleUBXCommand);
    managementServer.on("/debug/ubx", HTTP_GET, handleUBXResult);
#endif
    managementServer.on("/ota", HTTP_POST, handleOTARequest);
    managementServer.onNotFound([]() {
        sendText(404, F("error=not_found\n"));
    });
    managementServer.begin();
    dlog.info("management", F("HTTP diagnostics and OTA trigger started on port 80"));
}

void queueNMEASentence(void* arg, const char* sentence)
{
    static_cast<NMEAService*>(arg)->queueSentence(sentence);
}
}

void logTimeFirst(DLogBuffer& buffer, DLogLevel level)
{
    (void)level; // not used
    struct timeval tv;
    struct tm tm;
    gps.getTime(&tv);
    gmtime_r(&tv.tv_sec, &tm);

    buffer.printf(F("%04d/%02d/%02d %02d:%02d:%02d.%06ld "),
            tm.tm_year+1900,
            tm.tm_mon+1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec,
            tv.tv_usec);
}

void processOTA(const char* ota_url, const char* ota_fp)
{
    static PROGMEM const char TAG[] = "processOTA";
    dlog.info(FPSTR(TAG), F("OTA update"));

	dlog.info(FPSTR(TAG), F("checking for OTA from: '%s'"), ota_url);
	display.message("Starting OTA");

	ESPhttpUpdate.rebootOnUpdate(false);

#ifdef USE_CERT_STORE
    (void)ota_fp; // we are using the cert store so don't need this
    CertStore certStore;
#endif
    t_httpUpdate_return ret;

    if (strncmp(ota_url, "https:", 6) == 0)
    {
#ifdef USE_CERT_STORE
        int numCerts = certStore.initCertStore(SPIFFS, PSTR("/certs.idx"), PSTR("/certs.ar"));
        dlog.info(FPSTR(TAG), F("Number of CA certs read: %d"), numCerts);
#endif
        BearSSL::WiFiClientSecure client;
#ifdef USE_CERT_STORE
        // Integrate the cert store with this connection
        dlog.info(FPSTR(TAG), F("adding cert store to connection"));
        client.setCertStore(&certStore);
#else
        if (ota_fp[0] != '\0' && strcmp(ota_fp, "-") != 0)
        {
            if (!client.setFingerprint(ota_fp))
            {
                dlog.error(FPSTR(TAG), F("invalid OTA certificate fingerprint"));
                display.message("BAD OTA FP");
                delay(10000);
                return;
            }
        }
        else
        {
            dlog.warning(FPSTR(TAG), F("HTTPS OTA certificate validation disabled"));
            client.setInsecure();
        }
#endif
        ret = ESPhttpUpdate.update(client, ota_url, ESPNTP_SERVER_VERSION);
    }
    else
    {
        WiFiClient client;
        ret = ESPhttpUpdate.update(client, ota_url, ESPNTP_SERVER_VERSION);
    }

	String reason = ESPhttpUpdate.getLastErrorString();

	switch(ret)
	{
		case HTTP_UPDATE_OK:
			dlog.info(FPSTR(TAG), F("OTA update OK! restarting..."));
			dlog.end();
			display.message("Success!");
			delay(10000);
			ESP.restart();
			break;

		case HTTP_UPDATE_FAILED:
			dlog.info(FPSTR(TAG), F("OTA update failed! reason:'%s'"), reason.c_str());
			display.message("FAILED!");
			delay(10000);
			break;

		case HTTP_UPDATE_NO_UPDATES:
			dlog.info(FPSTR(TAG), F("OTA no updates! reason:'%s'"), reason.c_str());
			display.message("NO Update!");
			delay(10000);
			break;

		default:
			dlog.info(FPSTR(TAG), F("OTA update WTF? unexpected return code: %d reason: '%s'"), ret, reason.c_str());
			display.message("Unknown!");
			delay(10000);
			break;
	}
}

void setup()
{
    //delay(5000); // delay for IDE to re-open serial

    //
    // Setup DLog with Serial1, set the pre function to add the date/time
    //
    Serial1.begin(76800);
    dlog.begin(new DLogPrintWriter(Serial1));
    dlog.setPreFunc(&logTimeFirst);
    //dlog.setLevel("GPS", DLogLevel::DLOG_LEVEL_DEBUG);
    dlog.setLevel("Config", DLogLevel::DLOG_LEVEL_DEBUG);

    dlog.info(SETUP_TAG, F("Startup!"));

    snprintf(devicename, sizeof(devicename), "ESPNTP:%08x", ESP.getChipId());
    snprintf(mdnsHostname, sizeof(mdnsHostname), "espntp-%08x", ESP.getChipId());
    dlog.info(SETUP_TAG, "Device name: %s", devicename);

    dlog.info(SETUP_TAG, F("initializing display"));
    display.begin();


    dlog.info(SETUP_TAG, F("initializing serial for GPS"));
    Serial.begin(9600);
    Serial.swap();

    bool force_config = false;

    config.begin();

    if (!config.load())
    {
        dlog.warning(SETUP_TAG, "no config found, forcing config portal!");
        force_config = true;
    }

    // if the reset/config button is pressed then force config
    if (digitalRead(CONFIG_PIN) == 0)
    {
        time_t start = millis();

        while (digitalRead(CONFIG_PIN) == 0)
        {
            time_t delta = millis() - start;
			if (!force_config && delta > CONFIG_DELAY)
			{
                dlog.info(SETUP_TAG, F("reset button held, forcing config!"));
                force_config = true;
                display.message("Force Config");
			}
            delay(1);
        }
    }

#if !defined(USE_NO_WIFI)
    dlog.info(SETUP_TAG, F("initializing wifi"));
    display.message("Starting WiFi");
    WiFiSetup wifi(config, display, Serial1, false, devicename);
    wifi.connect(force_config);

    const char* syslog_host = config.getSyslogHost();
    uint16_t    syslog_port = config.getSyslogPort();
    if (syslog_host != nullptr && strlen(syslog_host) > 0 && syslog_port != 0)
    {
        dlog.info(SETUP_TAG, "enabling syslog: '%s:%u'", syslog_host, syslog_port);
        dlog.begin(new DLogSyslogWriter(syslog_host, syslog_port, devicename, ESPNTP_SERVER_VERSION));
    }

    dlog.info(SETUP_TAG, "ESP::FullVersion: %s", ESP.getFullVersion().c_str());

    const char* url = wifi.getOTAURL();
    const char* fp  = wifi.getOTAFP();
    if (fp == nullptr)
    {
    	fp = "";
    }

    if (url != nullptr)
    {
    	processOTA(url, fp);
    }

    dlog.info(SETUP_TAG, F("WiFi mode: %d"), WiFi.getMode());
    dlog.info(SETUP_TAG, F("wifi sleep mode: %d"), WiFi.getSleepMode());
#endif

    // Start GPS only after OTA. The PPS ISR is deliberately IRAM-only and
    // minimal, but keeping every interrupt disabled during a flash write is
    // still the safest boot/update sequence for the ESP8266.
    dlog.info(SETUP_TAG, F("initializing GPS"));
    display.message("Starting GPS");
    display.message(__TIME__ " " __DATE__);
    gps.begin();
    gps.setSentenceHandler(&queueNMEASentence, &nmeaService);
    display.process();

    dlog.info(SETUP_TAG, F("initializing NTP"));
    ntp.begin();
    
    dlog.info(SETUP_TAG, F("initializing LTPS (Legacy Time Protocols)"));
    ltps.begin();

    beginManagementServer();

    dlog.info(SETUP_TAG, F("initializing NMEA 0183 TCP, UDP broadcast, and DNS-SD"));
    nmeaService.begin(mdnsHostname);
}

void loop()
{
    static int last_wifi_status;
    static IPAddress last_ip;

    int wifi_status = WiFi.status();
    if (wifi_status != last_wifi_status)
    {
        const char* status = wifiStatusName(wifi_status);
        dlog.info(LOOP_TAG, F("wifi status change %d -> %d '%s'"), last_wifi_status, wifi_status, status);
        last_wifi_status = wifi_status;
    }

    IPAddress ip = WiFi.localIP();
    if (ip != last_ip)
    {
        dlog.warning(LOOP_TAG, F("ip address change %s -> %s"), last_ip.toString().c_str(), ip.toString().c_str());
        if (ip == IPAddress(0,0,0,0))
        {
            dlog.error(LOOP_TAG, F("lost connectivity!  Resetting!"));
            ESP.reset();
            delay(10000);
        }
        last_ip = ip;
    }

    managementServer.handleClient();
    if (otaRestartPending && static_cast<int32_t>(millis() - otaRestartAt) >= 0)
    {
        display.message("Forced OTA");
        dlog.warning("management", F("restarting for forced OTA check"));
        delay(100);
        dlog.end();
        ESP.restart();
        return;
    }

    gps.process();

    static time_t last_background_seconds;
    struct timeval tv;
    gps.getTime(&tv);

    if (tv.tv_usec >= 500000)
    {
        gps.pollVersionIfNeeded();
#ifdef UBX_DEBUG_API
        gps.processUBXCommand();
#endif
        nmeaService.process();
    }
#ifdef UBX_DEBUG_API
    else if (!gps.isPPSValid())
    {
        // Keep receiver control alive when PPS is absent. After a long PPS
        // outage tv_usec can leave the normal second range entirely.
        gps.processUBXCommand();
    }
#endif

    // Keep display I2C traffic, status logging, and optional UDP syslog away
    // from the PPS edge. They can delay delivery of an NTP packet to lwIP even
    // though the PPS ISR itself remains deterministic.
    if (tv.tv_usec >= 500000 && tv.tv_sec != last_background_seconds)
    {
        if ((tv.tv_sec % 300) == 0 || gps.getValidDelay())
        {
            dlog.info("loop", F("jitter:%lu valid_count:%lu valid:%s gpsvalid:%s numsat:%d heap:%ld valid_delay:%d"),
                    gps.getJitter(),
                    gps.getValidCount(),
                    gps.isValid() ? "true" : "false",
                    gps.isGPSValid() ? "true" : "false",
                    gps.getSatelliteCount(),
                    ESP.getFreeHeap(),
                    gps.getValidDelay());
        }

        if (tv.tv_sec < last_background_seconds)
        {
            dlog.warning(LOOP_TAG, F("OOPS: time went backwards: last:%lu now:%lu delta:%ld"),
                    last_background_seconds, tv.tv_sec, tv.tv_sec-last_background_seconds);
        }

        display.process();
        last_background_seconds = tv.tv_sec;
    }
}
