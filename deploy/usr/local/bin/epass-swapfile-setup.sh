#!/bin/sh
set -u

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

SWAPFILE=/swapfile
ENABLE_SWAPFILE=${EPASS_ENABLE_SWAPFILE:-no}
SWAP_SIZE_MB=${EPASS_SWAPFILE_SIZE_MB:-256}
FSTAB_LINE='/swapfile none swap defaults,pri=1 0 0'

log() {
    printf '%s\n' "epass-swapfile-setup: $*"
}

has_cmdline_flag() {
    grep -qw "$1" /proc/cmdline 2>/dev/null
}

ensure_fstab_entry() {
    if grep -qE '^/swapfile[[:space:]]+' /etc/fstab 2>/dev/null; then
        return 0
    fi

    printf '%s\n' "${FSTAB_LINE}" >> /etc/fstab || {
        log "warning: failed to append /etc/fstab entry"
        return 1
    }
}

swapfile_active() {
    swapon --show=NAME 2>/dev/null | grep -qx "${SWAPFILE}"
}

activate_existing_swapfile() {
    swapon -p 1 "${SWAPFILE}" >/dev/null 2>&1
}

create_swapfile() {
    log "creating ${SWAPFILE} (${SWAP_SIZE_MB}MB)"
    dd if=/dev/zero of="${SWAPFILE}" bs=1M count="${SWAP_SIZE_MB}" status=none || return 1
    chmod 600 "${SWAPFILE}" || return 1
    mkswap "${SWAPFILE}" >/dev/null 2>&1 || return 1
    swapon -p 1 "${SWAPFILE}" >/dev/null 2>&1 || return 1
}

main() {
    if [ "${ENABLE_SWAPFILE}" != "yes" ]; then
        log "runtime swapfile disabled (EPASS_ENABLE_SWAPFILE=${ENABLE_SWAPFILE})"
        exit 0
    fi

    if has_cmdline_flag 'epass.firstboot=1' || has_cmdline_flag 'epass.resize=1'; then
        log "skip during firstboot/resize mode"
        exit 0
    fi

    if swapfile_active; then
        ensure_fstab_entry || true
        log "swapfile already active"
        exit 0
    fi

    if [ -e "${SWAPFILE}" ]; then
        if activate_existing_swapfile; then
            ensure_fstab_entry || true
            log "activated existing swapfile"
            exit 0
        fi

        log "existing swapfile is unusable, recreating"
        swapoff "${SWAPFILE}" >/dev/null 2>&1 || true
        rm -f "${SWAPFILE}" || {
            log "warning: failed to remove invalid swapfile"
            exit 0
        }
    fi

    if create_swapfile; then
        ensure_fstab_entry || true
        log "swapfile ready"
        exit 0
    fi

    log "warning: failed to create or activate swapfile"
    rm -f "${SWAPFILE}" >/dev/null 2>&1 || true
    exit 0
}

main "$@"
