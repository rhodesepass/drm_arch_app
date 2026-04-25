#!/bin/sh
set -u

RUN_DIR=/run/epass
REASON_FILE=${RUN_DIR}/gui-failure-reason
BOOT_MARK=/usr/local/bin/epass-boot-mark.sh
APP=/usr/local/bin/drm_arch_app
RUNNER=/usr/local/bin/drm-arch-app-runner.sh
DRM_DEVICE=/dev/dri/card0
REQUIRED_DIRS="/assets /app /dispimg /root/res"

mark_stage() {
    if [ -x "$BOOT_MARK" ]; then
        "$BOOT_MARK" "$1" || true
    else
        printf '<6>epass-boot: %s\n' "$1" > /dev/kmsg 2>/dev/null || true
    fi
}

fail_preflight() {
    code="$1"
    reason="$2"
    mkdir -p "$RUN_DIR" 2>/dev/null || true
    printf '%s\n' "$reason" > "$REASON_FILE" 2>/dev/null || true
    echo "ERROR: $reason" >&2
    mark_stage "drm-arch-app:preflight-${code}"
    exit 1
}

log_mount_layout() {
    mounts=""

    if command -v findmnt >/dev/null 2>&1; then
        mounts=$(findmnt -no TARGET,SOURCE,FSTYPE,OPTIONS / /mnt/epass-data /assets /app /dispimg /root/res /root/themes 2>/dev/null || true)
        if [ -n "$mounts" ]; then
            printf '%s\n' "$mounts"
            return
        fi
    fi

    if [ -r /proc/self/mountinfo ]; then
        awk '
            $5 == "/" ||
            $5 == "/mnt/epass-data" ||
            $5 == "/assets" ||
            $5 == "/app" ||
            $5 == "/dispimg" ||
            $5 == "/root/res" ||
            $5 == "/root/themes" {
                print "mountinfo " $0;
                found = 1;
            }
            END {
                if (!found) {
                    exit 1;
                }
            }
        ' /proc/self/mountinfo 2>/dev/null && return
    fi

    if [ -r /proc/mounts ]; then
        awk '
            $2 == "/" ||
            $2 == "/mnt/epass-data" ||
            $2 == "/assets" ||
            $2 == "/app" ||
            $2 == "/dispimg" ||
            $2 == "/root/res" ||
            $2 == "/root/themes" {
                print "mounts " $0;
            }
        ' /proc/mounts 2>/dev/null || true
    fi
}

mkdir -p "$RUN_DIR" 2>/dev/null || true
rm -f "$REASON_FILE" 2>/dev/null || true
mark_stage "drm-arch-app:preflight-start"

[ -x "$APP" ] || fail_preflight "missing-app" "$APP is missing or not executable"
[ -x "$RUNNER" ] || fail_preflight "missing-runner" "$RUNNER is missing or not executable"
[ -e /etc/epass-firstboot/configured ] || fail_preflight "firstboot-incomplete" "firstboot selection is incomplete: /etc/epass-firstboot/configured is missing"
grep -qw 'epass.firstboot=1' /proc/cmdline && fail_preflight "firstboot-cmdline" "firstboot kernel mode is still active"
grep -qw 'epass.resize=1' /proc/cmdline && fail_preflight "resize-cmdline" "resize kernel mode is still active"

for path in ${REQUIRED_DIRS}; do
    [ -d "$path" ] || fail_preflight "missing-dir" "required directory $path is missing"
    [ -r "$path" ] || fail_preflight "unreadable-dir" "required directory $path is not readable"
done

log_mount_layout || true

tries=40
while [ "$tries" -gt 0 ]; do
    [ -c "$DRM_DEVICE" ] && break
    tries=$((tries - 1))
    sleep 0.2
done
[ -c "$DRM_DEVICE" ] || fail_preflight "missing-drm-device" "DRM device $DRM_DEVICE is not available"

mark_stage "drm-arch-app:preflight-ok"
exit 0
