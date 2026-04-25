#!/bin/sh
set -u

PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

BOOT_MARK=/usr/local/bin/epass-boot-mark.sh
DATA_LABEL=EPASSDATA
DATA_MNT=/mnt/epass-data

mark_stage() {
    if [ -x "$BOOT_MARK" ]; then
        "$BOOT_MARK" "$1" >/dev/null 2>&1 || true
    else
        printf '<6>epass-data: %s\n' "$1" > /dev/kmsg 2>/dev/null || true
    fi
}

log_info() {
    printf 'epass-data-mount: %s\n' "$1" >&2
}

is_mounted() {
    target=$1
    grep -q " ${target} " /proc/mounts 2>/dev/null
}

find_data_device() {
    blkid -t "LABEL=${DATA_LABEL}" -o device 2>/dev/null | head -n 1
}

ensure_dir() {
    mkdir -p "$1"
}

dir_has_entries() {
    dir=$1
    [ -d "$dir" ] || return 1
    find "$dir" -mindepth 1 -print -quit 2>/dev/null | grep -q .
}

seed_dir_if_empty() {
    src=$1
    dst=$2
    label=$3

    ensure_dir "$src"
    ensure_dir "$dst"

    if dir_has_entries "$dst"; then
        return 0
    fi

    if ! dir_has_entries "$src"; then
        return 0
    fi

    if ! cp -a "$src"/. "$dst"/; then
        log_info "failed to seed ${label} into shared data partition"
        return 1
    fi

    log_info "seeded ${label} into shared data partition"
    return 0
}

bind_mount() {
    src=$1
    dst=$2

    ensure_dir "$src"
    ensure_dir "$dst"

    if is_mounted "$dst"; then
        return 0
    fi

    if ! mount --bind "$src" "$dst"; then
        log_info "bind mount failed: ${src} -> ${dst}"
        return 1
    fi

    return 0
}

ensure_local_fallback_dirs() {
    ensure_dir /assets
    ensure_dir /dispimg
    ensure_dir /root/themes
    ensure_dir "$DATA_MNT"
    ensure_dir "${DATA_MNT}/apps-inbox"
    ensure_dir "${DATA_MNT}/import-log"
}

main() {
    dev=""

    mark_stage "epass-data:mount-start"
    ensure_local_fallback_dirs

    dev=$(find_data_device || true)
    if [ -z "$dev" ]; then
        log_info "shared data partition not found, using rootfs directories"
        mark_stage "epass-data:mount-missing"
        exit 0
    fi

    if ! is_mounted "$DATA_MNT"; then
        if ! mount -t vfat -o rw,uid=0,gid=0,umask=022,utf8=1 "$dev" "$DATA_MNT"; then
            log_info "failed to mount ${dev} at ${DATA_MNT}, using rootfs directories"
            mark_stage "epass-data:mount-failed"
            exit 0
        fi
    fi

    ensure_dir "${DATA_MNT}/assets"
    ensure_dir "${DATA_MNT}/display-images"
    ensure_dir "${DATA_MNT}/themes"
    ensure_dir "${DATA_MNT}/apps-inbox"
    ensure_dir "${DATA_MNT}/import-log"

    seed_dir_if_empty /assets "${DATA_MNT}/assets" assets || true
    seed_dir_if_empty /dispimg "${DATA_MNT}/display-images" display-images || true
    seed_dir_if_empty /root/themes "${DATA_MNT}/themes" themes || true

    bind_mount "${DATA_MNT}/assets" /assets || true
    bind_mount "${DATA_MNT}/display-images" /dispimg || true
    bind_mount "${DATA_MNT}/themes" /root/themes || true

    mark_stage "epass-data:mount-ok"
    exit 0
}

main "$@"
