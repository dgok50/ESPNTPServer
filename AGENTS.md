# ESPNTPServer maintainer runbook

This is the publishable operational guide for the repository. Deployment
addresses, device identifiers, private project details, and incident snapshots
belong in the ignored `AGENTS.local.md` on the maintainer's machine. Do not
commit that file or copy its contents into issues, logs, or CI output.

## Safety and credentials

- Never commit or print access tokens, SSH private keys, Wi-Fi credentials,
  device configuration secrets, or deployment-specific addresses and paths.
- Use `GITLAB_TOKEN` from the environment for GitLab Git/API access. The
  maintainer's shell may export it only in an interactive session. For Git over
  HTTPS, use a temporary `GIT_ASKPASS` helper that reads the environment; for
  the API, pass the token only in a request header. Never place its value in a
  URL, command-line argument, repository file, or tool output.
- Keep CI deployment credentials in GitLab CI variables. A public SSH host key
  is not a secret, but its deployment-specific value should also be held in CI.
- Deploy OTA images as an unprivileged account. Preserve the last known-good
  image, publish atomically, and verify the exact bytes through the real HTTP
  OTA endpoint before declaring a release complete.
- Publishing an OTA image does not ask the device to install it. Device
  activation requires a separate, explicit action.
- Before changing or publishing, inspect `git status --short --branch`,
  `git branch -vv`, and `git remote -v`. Preserve unrelated user changes.
- Use the repository-local maintainer Git identity for commits; do not change
  global Git configuration.

## Build and source layout

The normal PlatformIO environment is `default` in `platformio.ini`: ESP12E,
Arduino framework, ESP8266 core 2.6.3, 160 MHz CPU, QIO 4 MiB flash, and
`-Wall -Wextra -Werror`. Keep dependency pins unless intentionally upgrading.
The default environment currently enables `UBX_DEBUG_API` for field diagnosis;
this is temporary and must not be treated as a production security model.

For an isolated local build:

```sh
PLATFORMIO_CORE_DIR=/tmp/espntp-platformio \
PLATFORMIO_SETTING_ENABLE_TELEMETRY=no \
platformio run -e default
```

The normal binary is `.pio/build/default/firmware.bin`. Before any release:

1. Require a warning-clean build and `git -c core.whitespace=cr-at-eol diff --check`.
2. Ensure the binary is non-empty, at most 1044464 bytes, and accepted by
   `python -m esptool image_info`.
3. Record size, MD5, and SHA-256 for the CI artifact, published OTA download,
   and installed device; do not claim byte identity without matching all three.
4. Verify that the CI deployment job made a recoverable backup and that no
   per-device emergency hold is active unintentionally.

CI validates and builds pipeline commits. Only the configured default branch
may publish an OTA image. The deployment script is deliberately fail-closed:
it requires CI-provided target, host-key, destination, and verification-route
variables, stages the upload, verifies hashes, replaces the image atomically,
downloads through nginx, and restores the backup on failure. A documentation
commit with `[skip ci]` does not publish an image.

## Firmware invariants

The complete listener/transport map and wire formats are in
[docs/PROTOCOLS.md](docs/PROTOCOLS.md). Keep it synchronized with changes.

### Time services

- NTP v3/v4: UDP 123, 48-byte server response, stratum 1, reference ID `GPS `.
- RFC 867 Daytime: TCP and UDP 13, human-readable UTC line.
- RFC 868 Time: TCP and UDP 37, exactly four big-endian 1900-epoch bytes.
- All time services stay silent until a valid GNSS fix, confirmed RMC UTC,
  and the first subsequent PPS edge. There is no fixed startup or recovery
  delay. A GPS or PPS outage makes service silent again; the roughly
  one-second PPS-loss detector remains separate.
- The PPS ISR captures `micros()` immediately and stays IRAM-only. NTP uses
  raw lwIP UDP, captures T2 at callback entry and T3 immediately before send,
  and advertises a conservative 5 ms software-timestamp uncertainty floor.
  The ESP8266 does not provide Wi-Fi MAC timestamps.
- Preserve atomic GPS time reads, safe asynchronous TCP copies, checked UDP
  writes, request/response counters, and invalid-NTP-packet rejection.

### Network NMEA

- Forward only checksum-valid GGA/RMC with usable fix/status, CRLF framed.
- TCP 10110 is a persistent local-subnet stream with at most two clients.
  Bounded queueing and slow-reader eviction protect device memory.
- UDP 10110 broadcasts each accepted sentence in one datagram to the directed
  local-subnet broadcast address. Do not block or retry stale fixes; count
  drops instead.
- DNS-SD advertises `_nmea-0183._tcp.local` with `accuracy=exact`,
  `_nmea-0183._udp.local`, and management as `_http._tcp.local`. Do not
  advertise unimplemented GPS protocols.
- NMEA sends and `MDNS.update()` run in the latter half of the PPS second.

### OTA boot order

The order is mandatory: connect Wi-Fi; complete the boot-time OTA check and
any flash write; only then call `gps.begin()` and enable PPS; then start NTP,
legacy services, management, NMEA, and mDNS. Never move PPS initialization
above OTA or add an interrupt/timer before OTA without proving its entire call
path safe during flash writes. A forced OTA request schedules a controlled
reboot so installation follows the same order.

### Management and debug UBX

The HTTP management server listens on TCP 80 and accepts only clients on the
device's current local IP subnet. `GET /diagnostics.txt` reports version,
sketch hash, Wi-Fi, GPS/PPS, cached GNSS telemetry, protocol counters, and OTA
state. `POST /ota` requires `X-OTA-Confirm: reboot`; it schedules a reboot, not
an image upload. Do not remove the subnet check to enable CI activation.

The temporary `UBX_DEBUG_API` adds `POST /debug/ubx` and `GET /debug/ubx` for
checked UBX request/response frames. It intentionally has no password, key,
HTTPS, or command policy beyond the local-subnet restriction. A local client
can change receiver settings. Keep it only on a trusted network and remove the
flag before a normal production image. `tools/ubx_debug.py --inventory` is
read-only; arbitrary `--send` commands require care. In debug builds, queued
UBX commands must still be processed without a GNSS fix or PPS; with PPS,
keep their UART writes in the latter half of the second.

Receiver MON-HW, NAV-PVT, NAV-DOP, NAV-SVINFO, and MON-IO data are cached from
periodic UART output. HTTP diagnostics do not poll the receiver. Check each
age field; `unavailable` or stale is not equivalent to healthy. The physical
UART connection and module settings are installation-specific and recorded
only in `AGENTS.local.md`.

## Manual verification

After installation or a protocol change:

1. Fetch diagnostics. Confirm version/hash, reset reason, heap, Wi-Fi,
   `gps_input_valid`, `gps_pps_valid`, satellite count, and timeout counters.
2. Confirm `gps_valid=true` on the first PPS edge after valid GNSS input and
   confirmed RMC UTC. `gps_valid_delay_s` remains zero. A GPS/PPS outage must
   make all time services silent until reacquisition.
3. Test NTP UDP/123: 48-byte v3/v4 server reply, stratum 1, reference ID,
   matching origin, recent PPS reference timestamp, and non-zero dispersion.
4. Test RFC 867 on TCP and UDP 13 for UTC text; test RFC 868 on TCP and UDP 37
   for exactly four big-endian bytes.
5. Test NMEA TCP and UDP 10110: GGA/RMC only, CRLF and valid checksums; browse
   both DNS-SD services and confirm the TCP `accuracy=exact` TXT record.
6. Test HTTP diagnostics under moderate sequential load, plus the read-only
   debug UBX poll when that image is installed. Compare counters, heap,
   PPS timeouts, UART errors, and NMEA drops before and after.
7. Check the installed image identity and the actual OTA endpoint bytes
   separately; an OTA publication is not proof of device activation.

## History and publication

Keep private operational notes out of tracked documentation. When preparing a
public release, scan all refs and historical blobs for secrets and deployment
metadata, not just the current working tree. Rewriting Git history changes
commit IDs and requires coordination with every remote and collaborator. Keep
a recoverable offline bundle before any forced update and do not claim that
force-push erases old objects from third-party caches or clones.
