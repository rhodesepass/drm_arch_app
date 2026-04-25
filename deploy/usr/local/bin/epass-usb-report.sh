#!/bin/sh
set -u

RUN_DIR=/run/epass
GADGET=/sys/kernel/config/usb_gadget/g1

print_header() {
    printf '== %s ==\n' "$1"
}

print_file() {
    label=$1
    path=$2

    print_header "$label"
    if [ -r "$path" ]; then
        cat "$path"
    else
        echo "(missing)"
    fi
    echo
}

print_symlinks() {
    label=$1
    dir=$2
    found=no

    print_header "$label"
    for path in "$dir"/*; do
        if [ -L "$path" ]; then
            printf '%s -> %s\n' "$(basename "$path")" "$(readlink "$path")"
            found=yes
        fi
    done
    if [ "$found" = "no" ]; then
        echo "(none)"
    fi
    echo
}

print_dirs() {
    label=$1
    dir=$2
    found=no

    print_header "$label"
    for path in "$dir"/*; do
        if [ -d "$path" ]; then
            printf '%s\n' "$(basename "$path")"
            found=yes
        fi
    done
    if [ "$found" = "no" ]; then
        echo "(none)"
    fi
    echo
}

print_cmd() {
    label=$1
    shift

    print_header "$label"
    if ! "$@" 2>/dev/null; then
        echo "(unavailable)"
    fi
    echo
}

print_file "requested" "${RUN_DIR}/usb-mode-requested"
print_file "status" "${RUN_DIR}/usb-mode-status"
print_file "last-error" "${RUN_DIR}/usb-mode-last-error"
print_file "last-success" "${RUN_DIR}/usb-mode-last-success"
print_file "lock" "${RUN_DIR}/usb-mode.lock"
print_file "mass-storage-lun" "${GADGET}/functions/mass_storage.usb0/lun.0/file"
print_file "udc" "${GADGET}/UDC"
print_symlinks "config-functions" "${GADGET}/configs/c.1"
print_dirs "gadget-functions" "${GADGET}/functions"

print_header "functionfs-mounts"
if [ -r /proc/mounts ]; then
    if ! grep ' /dev/ffs-mtp ' /proc/mounts; then
        echo "(none)"
    fi
else
    echo "(missing /proc/mounts)"
fi
echo

print_header "umtprd"
if ! ps -eo pid,args | grep '[u]mtprd'; then
    echo "(not running)"
fi
echo

print_header "epass-usb-mode"
if ! ps -eo pid,args | grep '[e]pass-usb-mode'; then
    echo "(not running)"
fi
echo

print_cmd "usb0" ip addr show usb0
