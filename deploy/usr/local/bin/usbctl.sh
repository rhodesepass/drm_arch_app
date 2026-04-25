#!/bin/sh
# Unified USB Gadget Controller for ArkEPass (Arch Linux ARM)
# Based on: board/rhodesisland/epass/rootfs/bin/usbctl
#
# Consolidates usb-rndis.sh, usb-mtp.sh, usb-mass-storage.sh into one script
# with a single init_clean_gadget() to ensure complete cleanup between mode switches.
#
# configfs gadget teardown order (from drivers/usb/gadget/configfs.c):
#   1. Write "" to UDC          (unbind composite driver)
#   2. Remove os_desc symlinks   (configfs.c:850-877 os_desc_link)
#   3. Remove function symlinks from configs/c.1/
#   4. rmdir function directories
#   5. Recreate clean structure

set -eu

GADGET=/sys/kernel/config/usb_gadget/g1
BOOT_MARK=/usr/local/bin/epass-boot-mark.sh
DATA_MOUNT_HELPER=/usr/local/bin/epass-data-mount.sh

mark_stage() {
    if [ -x "$BOOT_MARK" ]; then
        "$BOOT_MARK" "$1" >/dev/null 2>&1 || true
    else
        printf '<6>epass-boot: %s\n' "$1" > /dev/kmsg 2>/dev/null || true
    fi
}

log_usbctl() {
    printf 'usbctl:%s\n' "$1" >&2
}

fail_step() {
    mode=${1:-unknown}
    step=${2:-unknown}
    shift 2
    log_usbctl "${mode}: ERROR ${step}: $*"
    mark_stage "usbctl:${mode}:fail:${step}"
    exit 1
}

ensure_dir() {
    mode=$1
    step=$2
    dir=$3

    if ! mkdir -p "$dir"; then
        fail_step "$mode" "$step" "mkdir -p $dir failed"
    fi
}

write_value() {
    mode=$1
    step=$2
    path=$3
    value=$4

    if ! printf '%s' "$value" > "$path"; then
        fail_step "$mode" "$step" "write '$value' to $path failed"
    fi
}

clear_value() {
    mode=$1
    step=$2
    path=$3

    if [ -e "$path" ] && ! : > "$path"; then
        fail_step "$mode" "$step" "clear $path failed"
    fi
}

remove_path_if_exists() {
    mode=$1
    step=$2
    path=$3

    if [ -e "$path" ] || [ -L "$path" ]; then
        if ! rm -f "$path"; then
            fail_step "$mode" "$step" "remove $path failed"
        fi
    fi
}

remove_dir_if_exists() {
    mode=$1
    step=$2
    path=$3

    if [ -d "$path" ]; then
        if ! rmdir "$path"; then
            fail_step "$mode" "$step" "rmdir $path failed"
        fi
    fi
}

ensure_link() {
    mode=$1
    step=$2
    target=$3
    link_path=$4

    if ! ln -s "$target" "$link_path"; then
        fail_step "$mode" "$step" "link $link_path -> $target failed"
    fi
}

mount_configfs_if_needed() {
    mode=$1

    if [ ! -d /sys/kernel/config/usb_gadget ]; then
        if ! mount -t configfs none /sys/kernel/config; then
            fail_step "$mode" "configfs-mount" "mount configfs failed"
        fi
    fi

    if [ ! -d /sys/kernel/config/usb_gadget ]; then
        fail_step "$mode" "configfs-check" "/sys/kernel/config/usb_gadget missing"
    fi
}

detect_udc() {
    for udc_path in /sys/class/udc/*; do
        if [ -e "$udc_path" ]; then
            basename "$udc_path"
            return 0
        fi
    done
    return 1
}

find_data_partition() {
    blkid -t LABEL=EPASSDATA -o device 2>/dev/null | head -n 1
}

restore_shared_data_mounts() {
    if [ -x "${DATA_MOUNT_HELPER}" ]; then
        "${DATA_MOUNT_HELPER}" >/dev/null 2>&1 || log_usbctl "shared-data: WARN restore failed"
    fi
}

init_clean_gadget() {
    mode=${1:-unknown}

    mark_stage "usbctl:${mode}:init-clean-start"

    killall umtprd 2>/dev/null || true
    ip link set usb0 down 2>/dev/null || true

    if [ -e /proc/mounts ] && grep -q ' /dev/ffs-mtp ' /proc/mounts; then
        if ! umount /dev/ffs-mtp; then
            fail_step "$mode" "ffs-umount" "umount /dev/ffs-mtp failed"
        fi
    fi

    mount_configfs_if_needed "$mode"

    if [ -d "$GADGET" ]; then
        clear_value "$mode" "udc-unbind" "$GADGET/UDC"
        sleep 1
    fi

    if [ -e "$GADGET/os_desc/use" ]; then
        write_value "$mode" "os-desc-disable" "$GADGET/os_desc/use" "0"
    fi
    remove_path_if_exists "$mode" "os-desc-link-remove" "$GADGET/os_desc/c.1"

    remove_path_if_exists "$mode" "config-rndis-link-remove" "$GADGET/configs/c.1/rndis.usb0"
    remove_path_if_exists "$mode" "config-mtp-link-remove" "$GADGET/configs/c.1/ffs.mtp"
    remove_path_if_exists "$mode" "config-storage-link-remove" "$GADGET/configs/c.1/mass_storage.usb0"
    remove_path_if_exists "$mode" "config-serial-link-remove" "$GADGET/configs/c.1/acm.usb0"

    remove_dir_if_exists "$mode" "function-rndis-remove" "$GADGET/functions/rndis.usb0"
    remove_dir_if_exists "$mode" "function-mtp-remove" "$GADGET/functions/ffs.mtp"
    remove_dir_if_exists "$mode" "function-storage-remove" "$GADGET/functions/mass_storage.usb0"
    remove_dir_if_exists "$mode" "function-serial-remove" "$GADGET/functions/acm.usb0"

    ensure_dir "$mode" "gadget-dir" "$GADGET"
    ensure_dir "$mode" "functions-dir" "$GADGET/functions"
    ensure_dir "$mode" "config-dir" "$GADGET/configs/c.1"
    ensure_dir "$mode" "strings-dir" "$GADGET/strings/0x409"
    ensure_dir "$mode" "config-strings-dir" "$GADGET/configs/c.1/strings/0x409"

    write_value "$mode" "vendor-id" "$GADGET/idVendor" "0x1D6B"
    write_value "$mode" "manufacturer" "$GADGET/strings/0x409/manufacturer" "Rhodes Island"
    write_value "$mode" "product-name" "$GADGET/strings/0x409/product" "Electric Pass"
    write_value "$mode" "serial-number" "$GADGET/strings/0x409/serialnumber" "EPASS00001"
    write_value "$mode" "max-power" "$GADGET/configs/c.1/MaxPower" "250"

    mark_stage "usbctl:${mode}:init-clean-done"
}

activate_gadget() {
    mode=${1:-unknown}
    sleep 1

    UDC=$(detect_udc || true)
    if [ -z "${UDC:-}" ]; then
        fail_step "$mode" "udc-detect" "no UDC device found"
    fi

    write_value "$mode" "udc-bind" "$GADGET/UDC" "$UDC"
    mark_stage "usbctl:${mode}:udc-bind"
}

rndis_start() {
    mark_stage "usbctl:rndis:start"
    init_clean_gadget rndis
    restore_shared_data_mounts

    write_value rndis "product-id" "$GADGET/idProduct" "0x0104"
    write_value rndis "device-class" "$GADGET/bDeviceClass" "0xEF"
    write_value rndis "device-subclass" "$GADGET/bDeviceSubClass" "0x02"
    write_value rndis "device-protocol" "$GADGET/bDeviceProtocol" "0x01"
    write_value rndis "product-string" "$GADGET/strings/0x409/product" "Electric Pass RNDIS"
    write_value rndis "config-string" "$GADGET/configs/c.1/strings/0x409/configuration" "RNDIS"

    ensure_dir rndis "function-rndis-create" "$GADGET/functions/rndis.usb0"
    write_value rndis "rndis-host-mac" "$GADGET/functions/rndis.usb0/host_addr" "02:00:00:11:22:33"
    write_value rndis "rndis-dev-mac" "$GADGET/functions/rndis.usb0/dev_addr" "02:00:00:44:55:66"
    ensure_link rndis "config-rndis-link" "$GADGET/functions/rndis.usb0" "$GADGET/configs/c.1/rndis.usb0"

    write_value rndis "os-desc-use" "$GADGET/os_desc/use" "1"
    write_value rndis "os-desc-vendor" "$GADGET/os_desc/b_vendor_code" "0xcd"
    write_value rndis "os-desc-sign" "$GADGET/os_desc/qw_sign" "MSFT100"
    write_value rndis "rndis-compatible-id" \
        "$GADGET/functions/rndis.usb0/os_desc/interface.rndis/compatible_id" "RNDIS"
    write_value rndis "rndis-sub-compatible-id" \
        "$GADGET/functions/rndis.usb0/os_desc/interface.rndis/sub_compatible_id" "5162001"
    ensure_link rndis "os-desc-config-link" "$GADGET/configs/c.1" "$GADGET/os_desc/c.1"

    activate_gadget rndis

    sleep 2
    if ! ip addr replace 192.168.137.2/24 dev usb0; then
        fail_step rndis "ip-addr" "ip addr replace on usb0 failed"
    fi
    if ! ip link set usb0 up; then
        fail_step rndis "ip-link-up" "ip link set usb0 up failed"
    fi
    if ! ip route replace default via 192.168.137.1 dev usb0; then
        fail_step rndis "ip-route" "ip route replace default failed"
    fi

    if [ -L /etc/resolv.conf ]; then
        rm -f /etc/resolv.conf 2>/dev/null || true
    fi
    printf "nameserver 8.8.8.8\nnameserver 1.1.1.1\n" > /etc/resolv.conf 2>/dev/null || \
        log_usbctl "rndis: WARN resolv.conf update failed"

    if [ "${EPASS_ENABLE_RNDIS_TIMESYNC:-no}" = "yes" ]; then
        systemctl restart systemd-timesyncd 2>/dev/null &
    fi

    echo "RNDIS active on usb0 (192.168.137.2)"
}

mtp_start() {
    mark_stage "usbctl:mtp:start"
    init_clean_gadget mtp
    restore_shared_data_mounts

    write_value mtp "product-id" "$GADGET/idProduct" "0x0200"
    write_value mtp "config-string" "$GADGET/configs/c.1/strings/0x409/configuration" "MTP"

    ensure_dir mtp "function-mtp-create" "$GADGET/functions/ffs.mtp"
    ensure_link mtp "config-mtp-link" "$GADGET/functions/ffs.mtp" "$GADGET/configs/c.1/ffs.mtp"
    ensure_dir mtp "ffs-dir" "/dev/ffs-mtp"
    if ! mount -t functionfs mtp /dev/ffs-mtp; then
        fail_step mtp "functionfs-mount" "mount -t functionfs mtp /dev/ffs-mtp failed"
    fi
    mark_stage "usbctl:mtp:functionfs-mount"

    killall umtprd 2>/dev/null || true
    /usr/local/bin/umtprd &
    mark_stage "usbctl:mtp:umtprd-start"
    sleep 1

    activate_gadget mtp
    echo "MTP mode active"
}

serial_start() {
    mark_stage "usbctl:serial:start"
    init_clean_gadget serial
    restore_shared_data_mounts

    write_value serial "product-id" "$GADGET/idProduct" "0x0201"
    write_value serial "product-string" "$GADGET/strings/0x409/product" "Electric Pass Serial"
    write_value serial "config-string" "$GADGET/configs/c.1/strings/0x409/configuration" "Serial"

    ensure_dir serial "function-serial-create" "$GADGET/functions/acm.usb0"
    ensure_link serial "config-serial-link" "$GADGET/functions/acm.usb0" "$GADGET/configs/c.1/acm.usb0"

    activate_gadget serial
    echo "USB serial active"
}

mass_storage_start() {
    data_dev=""

    mark_stage "usbctl:mass-storage:start"
    init_clean_gadget mass-storage

    write_value mass-storage "product-id" "$GADGET/idProduct" "0x0203"
    write_value mass-storage "config-string" "$GADGET/configs/c.1/strings/0x409/configuration" "Mass Storage"

    data_dev=$(find_data_partition || true)
    if [ -z "${data_dev}" ]; then
        fail_step mass-storage "data-detect" "EPASSDATA partition not found"
    fi

    for target in /assets /dispimg /root/themes /mnt/epass-data; do
        if [ -e /proc/mounts ] && grep -q " ${target} " /proc/mounts; then
            if ! umount "${target}"; then
                fail_step mass-storage "data-umount" "umount ${target} failed"
            fi
        fi
    done

    ensure_dir mass-storage "function-storage-create" "$GADGET/functions/mass_storage.usb0"
    write_value mass-storage "storage-stall" "$GADGET/functions/mass_storage.usb0/stall" "0"
    write_value mass-storage "storage-lun-file" "$GADGET/functions/mass_storage.usb0/lun.0/file" "${data_dev}"
    write_value mass-storage "storage-removable" "$GADGET/functions/mass_storage.usb0/lun.0/removable" "1"
    ensure_link mass-storage "config-storage-link" \
        "$GADGET/functions/mass_storage.usb0" "$GADGET/configs/c.1/mass_storage.usb0"

    activate_gadget mass-storage
    echo "Mass Storage active (${data_dev})"
}

stop() {
    mark_stage "usbctl:stop:start"
    init_clean_gadget stop
    restore_shared_data_mounts
    mark_stage "usbctl:stop:done"
    echo "USB gadget stopped"
}

case "${1:-}" in
    rndis)        rndis_start ;;
    mtp)          mtp_start ;;
    serial)       serial_start ;;
    mass_storage) mass_storage_start ;;
    none|stop)    stop ;;
    *)            echo "Usage: $0 {rndis|mtp|serial|mass_storage|none}"; exit 1 ;;
esac
