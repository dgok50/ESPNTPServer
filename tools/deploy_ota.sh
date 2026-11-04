#!/usr/bin/env bash

set -Eeuo pipefail

firmware=${1:-firmware.bin}
: "${FW_SERVER:?FW_SERVER is required}"
: "${OTA_SSH_USER:?OTA_SSH_USER is required}"
: "${OTA_SSH_PORT:?OTA_SSH_PORT is required}"
: "${OTA_REMOTE_DIR:?OTA_REMOTE_DIR is required}"
: "${OTA_REMOTE_DIR_SHA256:?OTA_REMOTE_DIR_SHA256 is required}"
: "${OTA_VERIFY_HOST:?OTA_VERIFY_HOST is required}"
: "${OTA_VERIFY_PATH:?OTA_VERIFY_PATH is required}"

remote_user=$OTA_SSH_USER
remote_port=$OTA_SSH_PORT
remote_dir=$OTA_REMOTE_DIR
remote_dir_sha256=$OTA_REMOTE_DIR_SHA256
verify_host=$OTA_VERIFY_HOST
verify_path=$OTA_VERIFY_PATH
pipeline_id=${CI_PIPELINE_ID:-manual}
target="${remote_user}@${FW_SERVER}"
ssh_options=(-o BatchMode=yes -o StrictHostKeyChecking=yes -p "$remote_port")
scp_options=(-o BatchMode=yes -o StrictHostKeyChecking=yes -P "$remote_port")

test -f "$firmware"
firmware_size=$(stat --printf='%s' "$firmware")
firmware_md5=$(md5sum "$firmware" | cut -d' ' -f1)
firmware_sha256=$(sha256sum "$firmware" | cut -d' ' -f1)

case "$firmware_size" in
    ''|*[!0-9]*) echo "Invalid firmware size" >&2; exit 1 ;;
esac
test "$firmware_size" -gt 0
test "$firmware_size" -le 1044464
[[ "$firmware_md5" =~ ^[0-9a-f]{32}$ ]]
[[ "$firmware_sha256" =~ ^[0-9a-f]{64}$ ]]
[[ "$pipeline_id" =~ ^[A-Za-z0-9._-]+$ ]]
[[ "$remote_user" =~ ^[A-Za-z_][A-Za-z0-9_-]*$ ]]
[[ "$remote_port" =~ ^[0-9]{1,5}$ ]]
[[ "$remote_dir" =~ ^/var/www/[A-Za-z0-9._/-]+$ ]]
[[ "$remote_dir" != *'/../'* && "$remote_dir" != *'/./'* ]]
[[ "$remote_dir_sha256" =~ ^[0-9a-f]{64}$ ]]
test "$(printf '%s' "$remote_dir" | sha256sum | cut -d' ' -f1)" = "$remote_dir_sha256"
[[ "$verify_host" =~ ^[A-Za-z0-9.-]+$ ]]
[[ "$verify_path" =~ ^/[A-Za-z0-9._/-]+/$ ]]
[[ "$verify_path" != *'/../'* && "$verify_path" != *'/./'* ]]

remote_stage=$(ssh "${ssh_options[@]}" "$target" mktemp -d /tmp/espntp-ota-ci.XXXXXX)
[[ "$remote_stage" == /tmp/espntp-ota-ci.* ]]

scp "${scp_options[@]}" "$firmware" "$target:$remote_stage/firmware.bin"

ssh "${ssh_options[@]}" "$target" bash -s -- \
    "$remote_stage" "$remote_dir" "$remote_dir_sha256" \
    "$verify_host" "$verify_path" "$pipeline_id" \
    "$firmware_size" "$firmware_md5" "$firmware_sha256" <<'REMOTE'
set -Eeuo pipefail

stage=$1
destination=$2
destination_sha256=$3
verify_host=$4
verify_path=$5
pipeline_id=$6
expected_size=$7
expected_md5=$8
expected_sha256=$9

[[ "$stage" == /tmp/espntp-ota-ci.* ]]
[[ "$destination" =~ ^/var/www/[A-Za-z0-9._/-]+$ ]]
[[ "$destination" != *'/../'* && "$destination" != *'/./'* ]]
test "$(printf '%s' "$destination" | sha256sum | cut -d' ' -f1)" = "$destination_sha256"
[[ "$verify_host" =~ ^[A-Za-z0-9.-]+$ ]]
[[ "$verify_path" =~ ^/[A-Za-z0-9._/-]+/$ ]]
[[ "$verify_path" != *'/../'* && "$verify_path" != *'/./'* ]]
incoming="$stage/firmware.bin"
published="$destination/firmware.bin"
temporary="$destination/.firmware.bin.ci-$pipeline_id"
backup="$destination/firmware.bin.bak-ci-$pipeline_id"

test "$(stat --printf='%s' "$incoming")" = "$expected_size"
test "$(md5sum "$incoming" | cut -d' ' -f1)" = "$expected_md5"
test "$(sha256sum "$incoming" | cut -d' ' -f1)" = "$expected_sha256"

if test -f "$published"; then
    cp -a "$published" "$backup"
fi

install -m 0644 "$incoming" "$temporary"
mv -f "$temporary" "$published"

download="$stage/download.bin"
if ! curl --fail --silent --show-error \
    --header "Host: $verify_host" \
    --header 'x-ESP8266-sketch-md5: 00000000000000000000000000000000' \
    "http://127.0.0.1$verify_path" \
    --output "$download"; then
    if test -f "$backup"; then
        cp -a "$backup" "$temporary"
        mv -f "$temporary" "$published"
    fi
    exit 1
fi

if ! test "$(stat --printf='%s' "$download")" = "$expected_size" ||
   ! test "$(md5sum "$download" | cut -d' ' -f1)" = "$expected_md5" ||
   ! test "$(sha256sum "$download" | cut -d' ' -f1)" = "$expected_sha256"; then
    if test -f "$backup"; then
        cp -a "$backup" "$temporary"
        mv -f "$temporary" "$published"
    fi
    exit 1
fi

rm -f -- "$incoming" "$download"
rmdir -- "$stage"
printf 'Published firmware: size=%s md5=%s sha256=%s\n' \
    "$expected_size" "$expected_md5" "$expected_sha256"
REMOTE
