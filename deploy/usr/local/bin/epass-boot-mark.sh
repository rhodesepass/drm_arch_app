#!/bin/sh
set -u

STAGE="${1:-}"
RUN_DIR=/run/epass
STAGE_FILE=${RUN_DIR}/boot-stage
LOG_FILE=/var/log/epass-boot.log

if [ -z "${STAGE}" ]; then
    echo "usage: $0 <stage>" >&2
    exit 64
fi

mkdir -p "${RUN_DIR}" /var/log 2>/dev/null || true
printf '%s\n' "${STAGE}" > "${STAGE_FILE}" 2>/dev/null || true

if ! grep -qw 'epass.resize=1' /proc/cmdline 2>/dev/null; then
    STAMP=$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null || true)
    if [ -n "${STAMP}" ]; then
        printf '%s %s\n' "${STAMP}" "${STAGE}" >> "${LOG_FILE}" 2>/dev/null || true
    else
        printf '%s\n' "${STAGE}" >> "${LOG_FILE}" 2>/dev/null || true
    fi
fi

printf '<6>epass-boot: %s\n' "${STAGE}" > /dev/kmsg 2>/dev/null || true
if [ "${EPASS_BOOT_MARK_STDOUT:-0}" = "1" ]; then
    printf 'epass-boot: %s\n' "${STAGE}"
fi
