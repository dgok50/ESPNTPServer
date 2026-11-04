#!/usr/bin/env python3
"""Generate CI-only firmware defaults from protected environment variables.

Never print values or publish the generated header as a CI artifact.
"""

import json
import os
from pathlib import Path


def required(name):
    value = os.environ.get(name, "")
    if not value:
        raise SystemExit(name + " CI variable is required")
    if "\x00" in value or "\n" in value or "\r" in value:
        raise SystemExit(name + " contains an invalid control character")
    return value


ota_url = required("OTA_URL")
syslog_host = required("SYSLOG_DEFAULT_HOST")
syslog_port = required("SYSLOG_DEFAULT_PORT")

if len(ota_url) > 127 or len(syslog_host) > 63 or len(syslog_port) > 7:
    raise SystemExit("deployment default exceeds firmware field length")
if not syslog_port.isdecimal() or not 0 < int(syslog_port) < 65536:
    raise SystemExit("SYSLOG_DEFAULT_PORT is invalid")

header = Path(__file__).resolve().parents[1] / "src" / "DeploymentDefaults.h"
header.write_text(
    "#ifndef ESPNTP_DEPLOYMENT_DEFAULTS_H_\n"
    "#define ESPNTP_DEPLOYMENT_DEFAULTS_H_\n"
    "// Generated inside the protected CI build; do not commit.\n"
    "#define ESPNTP_DEFAULT_OTA_URL " + json.dumps(ota_url) + "\n"
    "#define ESPNTP_DEFAULT_SYSLOG_HOST " + json.dumps(syslog_host) + "\n"
    "#define ESPNTP_DEFAULT_SYSLOG_PORT " + json.dumps(syslog_port) + "\n"
    "#endif\n"
)
print("deployment defaults generated for protected CI build")
