#!/bin/sh
set -u

BOOT_MARK=/usr/local/bin/epass-boot-mark.sh

mark_stage() {
    if [ -x "$BOOT_MARK" ]; then
        "$BOOT_MARK" "$1" || true
    else
        printf '<6>epass-boot: %s\n' "$1" > /dev/kmsg 2>/dev/null || true
    fi
}

skip_start() {
    code="$1"
    reason="$2"

    echo "Skip GUI start: $reason" >&2
    mark_stage "drm-arch-app:skip-${code}"
    exit 1
}

mark_stage "drm-arch-app:should-start-check"

[ -e /etc/epass-firstboot/configured ] \
    || skip_start "firstboot-incomplete" "firstboot selection is incomplete: /etc/epass-firstboot/configured is missing"
grep -qw 'epass.firstboot=1' /proc/cmdline \
    && skip_start "firstboot-cmdline" "firstboot kernel mode is still active"
grep -qw 'epass.resize=1' /proc/cmdline \
    && skip_start "resize-cmdline" "resize kernel mode is still active"

mark_stage "drm-arch-app:should-start-ok"
exit 0
