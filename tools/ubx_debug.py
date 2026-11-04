#!/usr/bin/env python3
"""Temporary readout tool for the debug-only /debug/ubx endpoint.

TODO: Remove this helper and the debug firmware flag after field inspection.
Never use a configuration-write payload during the inventory readout.
"""

import argparse
import json
import os
import struct
import time
import urllib.error
import urllib.request


POLL_INVENTORY = [
    ("CFG-ANT", 0x06, 0x13, b""),
    ("CFG-DAT", 0x06, 0x06, b""),
    ("CFG-GNSS", 0x06, 0x3E, b""),
    ("CFG-INF-UBX", 0x06, 0x02, b"\x00"),
    ("CFG-INF-NMEA", 0x06, 0x02, b"\x01"),
    ("CFG-ITFM", 0x06, 0x39, b""),
    ("CFG-LOGFILTER", 0x06, 0x47, b""),
    ("CFG-NAV5", 0x06, 0x24, b""),
    ("CFG-NAVX5", 0x06, 0x23, b""),
    ("CFG-NMEA", 0x06, 0x17, b""),
    ("CFG-PM2", 0x06, 0x3B, b""),
    ("CFG-PRT-UART1", 0x06, 0x00, b"\x01"),
    ("CFG-RATE", 0x06, 0x08, b""),
    ("CFG-RINV", 0x06, 0x34, b""),
    ("CFG-RXM", 0x06, 0x11, b""),
    ("CFG-SBAS", 0x06, 0x16, b""),
    ("CFG-TP5", 0x06, 0x31, b""),
    ("CFG-USB", 0x06, 0x1B, b""),
    ("MON-HW", 0x0A, 0x09, b""),
    ("MON-IO", 0x0A, 0x02, b""),
    ("NAV-SVINFO", 0x01, 0x30, b""),
    ("NAV-STATUS", 0x01, 0x03, b""),
    ("NAV-AOPSTATUS", 0x01, 0x60, b""),
    ("AID-HUI", 0x0B, 0x02, b""),
]


def make_frame(message_class, message_id, payload):
    frame = bytearray((0xB5, 0x62, message_class, message_id))
    frame.extend(struct.pack("<H", len(payload)))
    frame.extend(payload)
    checksum_a = checksum_b = 0
    for value in frame[2:]:
        checksum_a = (checksum_a + value) & 0xFF
        checksum_b = (checksum_b + checksum_a) & 0xFF
    frame.extend((checksum_a, checksum_b))
    if len(frame) > 128:
        raise ValueError("UBX frame exceeds debug endpoint's 128-byte limit")
    return bytes(frame)


def parse_fields(data):
    return dict(line.split("=", 1) for line in data.decode("ascii").splitlines()
                if "=" in line)


def query(opener, base_url, message_class, message_id, payload):
    frame = make_frame(message_class, message_id, payload)
    request = urllib.request.Request(
        base_url + "/debug/ubx", data=frame.hex().encode("ascii"),
        headers={"Content-Type": "text/plain"}, method="POST")
    with opener.open(request, timeout=8) as response:
        fields = parse_fields(response.read())
    request_id = fields["request_id"]
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        with opener.open(base_url + "/debug/ubx", timeout=8) as response:
            result = parse_fields(response.read())
        if result.get("request_id") != request_id:
            raise RuntimeError("result was replaced by another client")
        if result.get("status") not in ("queued", "waiting"):
            return result
        time.sleep(0.12)
    raise TimeoutError("debug endpoint did not finish command")


def summarize(name, result):
    payload = bytes.fromhex(result.get("response_payload_hex", ""))
    summary = {}
    if name == "CFG-GNSS" and len(payload) >= 4:
        labels = {0: "GPS", 1: "SBAS", 2: "Galileo", 3: "BeiDou",
                  5: "QZSS", 6: "GLONASS"}
        summary["channels_hardware"] = payload[1]
        summary["channels_configured"] = payload[2]
        summary["systems"] = []
        for offset in range(4, min(len(payload), 4 + payload[3] * 8), 8):
            if offset + 8 > len(payload):
                break
            flags = struct.unpack_from("<I", payload, offset + 4)[0]
            summary["systems"].append({
                "name": labels.get(payload[offset], str(payload[offset])),
                "enabled": bool(flags & 1),
                "reserved_channels": payload[offset + 1],
                "max_channels": payload[offset + 2],
            })
    elif name == "MON-HW" and len(payload) >= 23:
        summary = {
            "noise_per_ms": struct.unpack_from("<H", payload, 16)[0],
            "agc_count": struct.unpack_from("<H", payload, 18)[0],
            "antenna_status": ["init", "unknown", "ok", "short", "open"][
                payload[20]] if payload[20] <= 4 else str(payload[20]),
            "antenna_power": ["off", "on", "unknown"][payload[21]]
                if payload[21] <= 2 else str(payload[21]),
            "jamming_state": (payload[22] >> 2) & 3,
        }
    elif name == "NAV-SVINFO" and len(payload) >= 8:
        channels = []
        for offset in range(8, min(len(payload), 8 + payload[4] * 12), 12):
            if offset + 12 > len(payload):
                break
            channels.append({"sv_id": payload[offset + 1],
                             "used": bool(payload[offset + 2] & 1),
                             "cno_dbhz": payload[offset + 4]})
        summary = {"channel_count": payload[4], "signals": channels}
    elif name == "CFG-TP5" and len(payload) >= 32:
        flags = struct.unpack_from("<I", payload, 28)[0]
        summary = {"pulse_index": payload[0], "active": bool(flags & 1),
                   "lock_gps_freq": bool(flags & 2),
                   "period_unlocked": struct.unpack_from("<I", payload, 8)[0],
                   "period_locked": struct.unpack_from("<I", payload, 12)[0],
                   "flags_hex": f"{flags:08x}"}
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("ESPNTP_DEVICE_HOST"))
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--inventory", action="store_true")
    group.add_argument("--send", nargs=2, metavar=("CLASS", "ID"))
    parser.add_argument("--payload-hex", default="")
    args = parser.parse_args()
    if not args.host:
        parser.error("--host or ESPNTP_DEVICE_HOST is required")

    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    base_url = f"http://{args.host}"
    if args.inventory:
        for name, message_class, message_id, payload in POLL_INVENTORY:
            try:
                result = query(opener, base_url, message_class, message_id, payload)
                result["name"] = name
                result["decoded"] = summarize(name, result)
                print(json.dumps(result, sort_keys=True), flush=True)
            except (TimeoutError, urllib.error.URLError, ValueError,
                    RuntimeError) as error:
                print(json.dumps({"name": name, "error": str(error)}),
                      flush=True)
    else:
        message_class, message_id = (int(value, 0) for value in args.send)
        if not 0 <= message_class <= 255 or not 0 <= message_id <= 255:
            parser.error("class and ID must be bytes")
        payload = bytes.fromhex(args.payload_hex)
        result = query(opener, base_url, message_class, message_id, payload)
        print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
