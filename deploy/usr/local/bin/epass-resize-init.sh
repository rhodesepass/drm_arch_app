#!/bin/sh
set -eu

PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin

BOOT_ENV_TOOL=/usr/local/bin/epass-bootenv
BOOT_MNT=/mnt/boot
BOOT_ENV=${BOOT_MNT}/epass/boot.env
DEFAULT_ROOT_PARTUUID=45504153-02
DATA_LABEL=EPASSDATA

log() {
    printf '%s\n' "$*" > /dev/console 2>/dev/null || true
    printf '<6>epass-resize: %s\n' "$*" > /dev/kmsg 2>/dev/null || true
}

fatal() {
    log "ERROR: $*"
    if mount_boot rw; then
        "${BOOT_ENV_TOOL}" -f "${BOOT_ENV}" set \
            epass_resize_stage=error \
            epass_resize_pending=1 \
            epass_boot_mode=resize \
            epass_firstboot=0 || true
        sync
        umount "${BOOT_MNT}" 2>/dev/null || true
    fi
    log "Resize mode halted."
    while :; do
        sleep 3600
    done
}

mount_api_fs() {
    mkdir -p /proc /sys /dev /run /tmp "${BOOT_MNT}"
    mount -t proc proc /proc 2>/dev/null || true
    mount -t sysfs sysfs /sys 2>/dev/null || true
    mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
    mount -t tmpfs tmpfs /run 2>/dev/null || true
    mount -t tmpfs tmpfs /tmp 2>/dev/null || true
}

root_partuuid() {
    for arg in $(cat /proc/cmdline); do
        case "${arg}" in
            root=PARTUUID=*)
                printf '%s\n' "${arg#root=PARTUUID=}"
                return 0
                ;;
        esac
    done
    printf '%s\n' "${DEFAULT_ROOT_PARTUUID}"
}

detect_boot_partition() {
    local dev="$1"
    local boot_part

    boot_part=$(partition_device "${dev}" 3)
    if [ -b "${boot_part}" ]; then
        printf '%s\n' "${boot_part}"
        return 0
    fi

    boot_part=$(partition_device "${dev}" 1)
    if [ -b "${boot_part}" ]; then
        printf '%s\n' "${boot_part}"
        return 0
    fi

    return 1
}

resolve_root_partition() {
    local partuuid part
    partuuid=$(root_partuuid)
    part=$(blkid -t "PARTUUID=${partuuid}" -o device 2>/dev/null || true)

    case "${part}" in
        /dev/mmcblk*p2)
            ROOT_PART="${part}"
            ROOT_DEV="${part%p2}"
            BOOT_PART=$(detect_boot_partition "${ROOT_DEV}") || return 1
            return 0
            ;;
        /dev/sd?2|/dev/vd?2)
            ROOT_PART="${part}"
            ROOT_DEV="${part%2}"
            BOOT_PART=$(detect_boot_partition "${ROOT_DEV}") || return 1
            return 0
            ;;
    esac

    if [ -b /dev/mmcblk0p2 ]; then
        ROOT_PART=/dev/mmcblk0p2
        ROOT_DEV=/dev/mmcblk0
        BOOT_PART=$(detect_boot_partition "${ROOT_DEV}") || return 1
        return 0
    fi

    return 1
}

partition_device() {
    local dev="$1"
    local index="$2"

    case "${dev}" in
        *[0-9])
            printf '%sp%s\n' "${dev}" "${index}"
            ;;
        *)
            printf '%s%s\n' "${dev}" "${index}"
            ;;
    esac
}

detect_resize_target() {
    local data_part

    data_part=$(partition_device "${ROOT_DEV}" 1)
    [ -b "${data_part}" ] || fatal "shared data partition ${data_part} is missing"
    DATA_PART="${data_part}"
    TARGET_PART="${DATA_PART}"
    TARGET_NUM=1
}

mkfs_vfat() {
    command -v mkfs.vfat >/dev/null 2>&1 || fatal "missing mkfs.vfat"
    mkfs.vfat -F 32 -n "${DATA_LABEL}" "${TARGET_PART}" || fatal "mkfs.vfat failed"
}

wait_for_block() {
    local dev="$1"
    local tries=50

    while [ "${tries}" -gt 0 ]; do
        [ -b "${dev}" ] && return 0
        tries=$((tries - 1))
        sleep 0.2
    done
    return 1
}

mount_boot() {
    local mode="${1:-ro}"
    local opts="${mode}"

    mkdir -p "${BOOT_MNT}"
    if grep -q " ${BOOT_MNT} " /proc/mounts 2>/dev/null; then
        return 0
    fi

    wait_for_block "${BOOT_PART}" || return 1
    mount -t vfat -o "${opts}" "${BOOT_PART}" "${BOOT_MNT}"
}

read_bootenv_key() {
    local key="$1"

    mount_boot ro || fatal "failed to mount boot partition"
    [ -x "${BOOT_ENV_TOOL}" ] || fatal "missing ${BOOT_ENV_TOOL}"
    [ -f "${BOOT_ENV}" ] || fatal "missing ${BOOT_ENV}"
    "${BOOT_ENV_TOOL}" -f "${BOOT_ENV}" show "${key}"
    umount "${BOOT_MNT}" 2>/dev/null || true
}

write_bootenv() {
    mount_boot rw || fatal "failed to mount boot partition rw"
    "${BOOT_ENV_TOOL}" -f "${BOOT_ENV}" set "$@" || {
        umount "${BOOT_MNT}" 2>/dev/null || true
        fatal "failed to update boot.env"
    }
    sync
    umount "${BOOT_MNT}" 2>/dev/null || true
}

reboot_now() {
    sync
    umount "${BOOT_MNT}" 2>/dev/null || true
    reboot -f
    fatal "reboot failed"
}

partition_stage() {
    local part_name start_file part_start

    part_name="${TARGET_PART##*/}"
    start_file="/sys/class/block/${part_name}/start"
    [ -r "${start_file}" ] || fatal "missing ${start_file}"
    part_start=$(cat "${start_file}")

    log "Resize stage partition: expanding ${TARGET_PART} on ${ROOT_DEV}"
    printf '%s,+\n' "${part_start}" | sfdisk --force -N "${TARGET_NUM}" "${ROOT_DEV}" || fatal "sfdisk failed"

    write_bootenv \
        epass_firstboot=0 \
        epass_resize_pending=1 \
        epass_resize_stage=fs \
        epass_boot_mode=resize
    log "Partition table updated. Rebooting into filesystem resize stage."
    reboot_now
}

fs_stage() {
    log "Resize stage fs: formatting ${TARGET_PART} as ${DATA_LABEL} (FAT32)"
    mkfs_vfat

    write_bootenv \
        epass_firstboot=0 \
        epass_resize_pending=0 \
        epass_resize_stage=done \
        epass_boot_mode=normal
    log "Resize complete. Rebooting into normal mode."
    reboot_now
}

main() {
    local stage pending mode

    mount_api_fs
    resolve_root_partition || fatal "failed to resolve root partition"
    detect_resize_target
    wait_for_block "${TARGET_PART}" || fatal "resize target ${TARGET_PART} not ready"

    pending=$(read_bootenv_key epass_resize_pending)
    mode=$(read_bootenv_key epass_boot_mode)
    stage=$(read_bootenv_key epass_resize_stage)

    [ "${pending}" = "1" ] || fatal "resize mode entered without pending state"
    [ "${mode}" = "resize" ] || fatal "resize mode entered with boot_mode=${mode}"

    case "${stage}" in
        partition)
            partition_stage
            ;;
        fs)
            fs_stage
            ;;
        *)
            fatal "unknown resize stage ${stage}"
            ;;
    esac
}

main "$@"
