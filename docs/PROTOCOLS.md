# ESPNTPServer protocol reference

This describes the implemented interfaces in firmware `1.3.5-debug`. It is a
snapshot of the code, not a claim that every interface is always reachable:
Wi-Fi, GPS input, PPS, and network conditions determine live availability.
The debug-only UBX route is deliberately not a production security design.

## Interface map

| Interface | Direction | Transport and port | Availability |
| --- | --- | --- | --- |
| NTP v3/v4 | Client to device | UDP 123 | After 60 continuous seconds of valid GPS time and PPS |
| RFC 867 Daytime | Client to device | TCP/UDP 13 | Same time-validity gate |
| RFC 868 Time | Client to device | TCP/UDP 37 | Same time-validity gate |
| NMEA 0183 stream | Device to local clients | TCP 10110 | With checksum-valid GGA/RMC and usable fix |
| NMEA 0183 broadcast | Device to local subnet | UDP 10110 | With checksum-valid GGA/RMC and usable fix |
| mDNS and DNS-SD | Bidirectional LAN discovery | UDP 5353 | After the Wi-Fi services start |
| Management | Client to device | Plain HTTP, TCP 80 | Current local IP subnet only |
| UBX command bridge | Client ↔ device ↔ receiver | Plain HTTP, TCP 80; UART1 | Only when `UBX_DEBUG_API` is built in; local subnet only |
| Firmware download | Device to OTA server | Configured HTTP or HTTPS URL | Once on boot, before GPS/PPS initialization |
| Syslog | Device to configured collector | UDP, configured port (often 514) | Only when host and port are configured |
| GNSS input | Receiver to device | NMEA/UBX at 9600 baud on swapped ESP UART; PPS GPIO12/D6 | After boot OTA check |

The firmware uses the current Wi-Fi subnet mask for its local-client check and
for the directed UDP broadcast address. It does not expose an authenticated
public API. In particular, the debug UBX command bridge can change receiver
configuration for *any* client on the local subnet.

## Time services

`gps_valid` becomes true only after GPS time and the PPS input have both been
continuously valid for 60 seconds. The countdown restarts after a PPS or GPS
validity loss. The PPS-loss detector itself is separate (about one second);
the 60 seconds are a safety delay before serving time, not a network timeout.
Until `gps_valid=true`, NTP and both RFC 867/868 transports deliberately give
no time response. NMEA output uses its own fix filter and does not wait for this
time-service delay.

### NTP — UDP 123

- Accepts client-mode NTP v3 and v4 requests of at least 48 bytes. Unsupported
  modes, versions, and short packets are ignored.
- Replies with a 48-byte server-mode packet, preserving the request version,
  stratum 1, reference ID `GPS `, and the request transmit timestamp as origin.
  The reference timestamp comes from the latest PPS edge.
- Receive and transmit timestamps are captured in the software lwIP path.
  There are no ESP8266 Wi-Fi MAC timestamps. Root dispersion therefore has a
  conservative 5 ms minimum; the reported PPS jitter alone is *not* network
  timestamp accuracy.
- Diagnostics: `ntp_requests`, `ntp_responses`, `ntp_processing_last_us`,
  `ntp_processing_max_us`, and `ntp_timestamp_uncertainty_us`.

### RFC 867 Daytime — TCP/UDP 13

Returns one human-readable UTC line, ending in CRLF, for a TCP connection or
a UDP datagram. It is for human inspection, not precision synchronization.
TCP closes after queuing the line. A UDP request needs no special payload; the
service replies to its source address and port.

### RFC 868 Time — TCP/UDP 37

Returns exactly four bytes: unsigned seconds since 1900-01-01 00:00:00 UTC,
in network (big-endian) byte order. TCP closes after queuing the value; a UDP
request receives one four-byte datagram. As specified by the 32-bit format,
the counter wraps; consumers must handle the appropriate era. Both legacy
services share `legacy_requests` and `legacy_responses` diagnostic counters.

## NMEA 0183 and discovery

Only checksum-valid GGA with a non-zero fix quality and RMC with active status
are forwarded. Frames end in CRLF; unsupported sentence types and no-fix
sentences are discarded. Output depends on an actual usable receiver fix.

- TCP 10110: persistent plain NMEA stream, restricted to two clients in the
  device's local subnet. A bounded four-frame queue and slow-client eviction
  protect ESP8266 memory. The service is advertised as
  `_nmea-0183._tcp.local` with TXT `accuracy=exact` for GeoClue clients.
- UDP 10110: each sentence is one datagram to the directed broadcast address
  calculated from the device IPv4 address and mask. No receiver acknowledgement
  or retry is attempted; a newer fix supersedes a dropped datagram. Advertised
  as `_nmea-0183._udp.local`.
- mDNS also advertises management as `_http._tcp.local` on port 80, using the
  hostname `espntp-<eight-digit-chip-id>.local`.
- NMEA sending and mDNS processing run in the latter half of the PPS second
  to avoid routine work near the most timing-sensitive edge.

Diagnostics expose the active ports, mDNS and UDP-broadcast flags, client and
connection counts, accepted sentence count, TCP sent/drop and queue-drop
counts, and UDP sent/drop counts. A zero UDP drop count does not prove delivery
to any particular listener.

## HTTP management — TCP 80

The normal management interface is plain HTTP, accepts clients only on the
device's current local IP subnet, and sends `text/plain` with no-store caching.
It does not require GPS lock. There is no TLS or application authentication.

| Request | Result |
| --- | --- |
| `GET /` | Lists the routes compiled into this firmware |
| `GET /diagnostics.txt` | `key=value` status: firmware, Wi-Fi, GPS/PPS, GNSS telemetry, protocol counters, heap and OTA state |
| `POST /ota` with `X-OTA-Confirm: reboot` | HTTP 202; schedules a reboot after about one second and an OTA check at boot |
| `POST /ota` without confirmation | HTTP 400; no reboot |
| `POST /ota` while already pending | HTTP 409 |

`POST /ota` does **not** upload an image. The device fetches the configured OTA
URL during the following boot and may receive a new image or a 304 when its
sketch MD5 already matches. The OTA server publication and device activation
are separate operations. The boot order is Wi-Fi, complete OTA check/write,
then GNSS/PPS, then network services; do not move PPS initialization earlier.
The configured OTA URL can be HTTP or HTTPS; the current deployed URL uses
HTTP. HTTPS certificate verification uses a configured fingerprint or the
optional certificate store. Without either, the current code explicitly falls
back to an insecure HTTPS connection; do not assume HTTPS alone authenticates
the OTA server.

The `1.3.6-debug` build additionally exposes these experimental routes:

| Request | Result |
| --- | --- |
| `POST /debug/ubx` | Accepts one complete UBX frame encoded as ASCII hex in a `text/plain` body; validates sync, length and checksum; HTTP 202 with `request_id` when queued |
| `GET /debug/ubx` | Returns the current request ID and `idle`, `queued`, `waiting`, `replied`, `timeout`, or `write_failed` status; a reply includes class, ID, length and hex payload |

Only one UBX command can be active; another submission returns HTTP 409. The
maximum command frame is 128 bytes, the maximum response payload is 1024
bytes, and a response times out after about two seconds. The bridge supports
both read and write UBX commands. There is intentionally **no password, key,
HTTPS, or per-command permission check** in this temporary build. Never expose
it to an untrusted subnet. Use `tools/ubx_debug.py --host <device-ip> --inventory` for read-only
polls; `--send` can change module settings and should be used deliberately.
Queued UBX commands and their two-second timeout continue to run even without
a GNSS fix or PPS. When PPS is valid, UART commands remain deferred to the
latter half of the PPS second.

Cached GNSS diagnostic fields include MON-HW interference/antenna state,
NAV-PVT fix and time accuracy, NAV-DOP, NAV-SVINFO signal summaries, and MON-IO
UART1 errors. They are periodically emitted by the receiver and cached by the
ESP; viewing `/diagnostics.txt` does not poll the module. Check each `*_age_ms`
before interpreting it. `unavailable` or `unknown` is not an all-clear.
The `gps_svinfo_gps_*` aggregates count GPS satellite IDs only. In a
GLONASS-only receiver test they can correctly read zero while GLONASS
channels are visible; poll `NAV-SVINFO` through the debug bridge to inspect
those channels.

## Other links and operational checks

The swapped ESP UART connects receiver TX to ESP GPIO13/RX and ESP GPIO15/TX
to receiver RX; PPS is separate on GPIO12/D6. UBX is the receiver-control and
telemetry protocol on that UART, while NMEA supplies navigation sentences.
Optional syslog is outbound from the ESP and does not create an inbound syslog
listener. A temporary Wi-FiManager access point/configuration portal appears
when configuration is missing or explicitly forced by the setup button; it is
not the normal station-mode management service.

For a field check, fetch `/diagnostics.txt` before and after testing. Confirm
the installed `version` and `sketch_md5`, `gps_valid=true`, `gps_pps_valid=true`,
`gps_valid_delay_s=0`, and stable `gps_pps_timeouts`. Exercise UDP 123, TCP and
UDP 13/37, TCP and UDP 10110, both NMEA DNS-SD records, HTTP diagnostics, and
the debug UBX poll if that build is installed. Compare request/response and
drop counters afterward. The detailed release procedure and OTA safeguards
remain in [AGENTS.md](../AGENTS.md).
