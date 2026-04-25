#!/bin/sh
set -u

SRGN_CONFIG_BIN=${1:-/usr/local/bin/srgn_config}
BOOT_MNT=/boot
BOOT_UENV=${BOOT_MNT}/uEnv.txt
VT_REBOUND=0
BOOT_MOUNTED_BY_US=0

log_line() {
    printf '<6>epass-srgn-config: %s\n' "$*" > /dev/kmsg 2>/dev/null || true
}

is_mountpoint() {
    local path="$1"

    awk -v target="$path" '
        $2 == target {
            found = 1;
            exit 0;
        }
        END {
            exit found ? 0 : 1;
        }
    ' /proc/mounts 2>/dev/null
}

mount_boot_rw() {
    local dev

    mkdir -p "${BOOT_MNT}" 2>/dev/null || true
    if is_mountpoint "${BOOT_MNT}"; then
        return 0
    fi

    for dev in \
        /dev/disk/by-label/EPASSBOOT \
        /dev/mmcblk0p3 \
        /dev/mmcblk1p3 \
        /dev/mmcblk0p1 \
        /dev/mmcblk1p1
    do
        [ -e "${dev}" ] || continue
        if mount -t vfat -o rw "${dev}" "${BOOT_MNT}" 2>/dev/null; then
            BOOT_MOUNTED_BY_US=1
            log_line "mounted boot partition from ${dev}"
            return 0
        fi
    done

    log_line "failed to mount boot partition"
    return 1
}

set_vt_bind_state() {
    local state="$1"
    local node

    for node in /sys/class/vtconsole/vtcon*/bind; do
        [ -w "${node}" ] || continue
        printf '%s\n' "${state}" > "${node}" 2>/dev/null || true
        VT_REBOUND=1
    done
}

choose_tty() {
    local tty

    for tty in /dev/tty0 /dev/tty1 /dev/console /dev/ttyS0; do
        if [ -c "${tty}" ] && [ -r "${tty}" ] && [ -w "${tty}" ]; then
            printf '%s\n' "${tty}"
            return 0
        fi
    done

    return 1
}

get_tty_stty_state() {
    local tty="$1"

    stty -F "${tty}" -g 2>/dev/null && return 0
    stty -g < "${tty}" 2>/dev/null
}

set_tty_stty() {
    local tty="$1"
    shift

    stty -F "${tty}" "$@" >/dev/null 2>&1 && return 0
    stty "$@" < "${tty}" >/dev/null 2>&1
}

drain_tty_input() {
    local tty="$1"
    local saved_state
    local pass

    saved_state=$(get_tty_stty_state "${tty}" || true)
    [ -n "${saved_state}" ] || return 0

    set_tty_stty "${tty}" -echo -icanon min 0 time 1 || return 0

    pass=0
    while [ "${pass}" -lt 8 ]; do
        dd if="${tty}" of=/dev/null bs=64 count=1 2>/dev/null || true
        pass=$((pass + 1))
    done

    set_tty_stty "${tty}" "${saved_state}" || true
}

cleanup() {
    if [ "${BOOT_MOUNTED_BY_US}" = "1" ] && is_mountpoint "${BOOT_MNT}"; then
        sync || true
        umount "${BOOT_MNT}" 2>/dev/null || true
    fi

    if [ "${VT_REBOUND}" = "1" ]; then
        set_vt_bind_state 0
    fi
}

run_srgn_config_on_tty() {
    local tty="$1"

    log_line "running srgn_config on ${tty}"
    drain_tty_input "${tty}"
    printf '\033c' > "${tty}" 2>/dev/null || true
    "${SRGN_CONFIG_BIN}" --uenv "${BOOT_UENV}" < "${tty}" > "${tty}" 2> "${tty}"
}

main() {
    local tty
    local rc

    trap cleanup EXIT INT TERM HUP

    if [ ! -x "${SRGN_CONFIG_BIN}" ]; then
        log_line "missing srgn_config binary: ${SRGN_CONFIG_BIN}"
        exit 127
    fi

    mount_boot_rw || true
    set_vt_bind_state 1
    sleep 0.1

    tty=$(choose_tty || true)
    if [ -n "${tty}" ]; then
        run_srgn_config_on_tty "${tty}"
        rc=$?
    else
        log_line "no usable tty found, falling back to direct exec"
        "${SRGN_CONFIG_BIN}" --uenv "${BOOT_UENV}"
        rc=$?
    fi

    sync || true
    log_line "srgn_config exited rc=${rc}"
    exit "${rc}"
}

main "$@"
