#!/bin/sh
set -u

RUN_DIR=/run/epass
REASON_FILE=${RUN_DIR}/gui-failure-reason
TEXT_FILE=${RUN_DIR}/gui-fallback.txt
SPLASH_FILE=${RUN_DIR}/gui-fallback.splash
BOOT_MARK=/usr/local/bin/epass-boot-mark.sh
FB_SPLASH=/usr/local/bin/fb-splash

mark_stage() {
    if [ -x "$BOOT_MARK" ]; then
        "$BOOT_MARK" "$1" || true
    else
        printf '<6>epass-boot: %s\n' "$1" > /dev/kmsg 2>/dev/null || true
    fi
}

summarize_reason() {
    if [ -r "$REASON_FILE" ]; then
        IFS= read -r line < "$REASON_FILE" || true
        if [ -n "${line:-}" ]; then
            printf '%s' "$line"
            return
        fi
    fi

    systemctl show -p Result -p ExecMainStatus -p ActiveState -p SubState drm-arch-app.service \
        2>/dev/null | tr '\n' ' ' | sed 's/[[:space:]]\+/ /g; s/ $//'
}

mkdir -p "$RUN_DIR" 2>/dev/null || true
reason=$(summarize_reason)
[ -n "$reason" ] || reason="drm-arch-app.service failed before a GUI frame was shown"

printf 'GUI startup failed\n' > "$TEXT_FILE"
printf 'GUI startup failed: %s\n' "$reason" >&2
printf '\001\000\001\000\000\000' > "$SPLASH_FILE"

mark_stage "drm-arch-app:fallback-start"

if [ -x "$FB_SPLASH" ]; then
    "$FB_SPLASH" --play-once --text-file "$TEXT_FILE" "$SPLASH_FILE" || true
fi

printf '\033[2J\033[1;1HArkEPass GUI startup failed.\n%s\nCheck serial or journal for details.\n' "$reason" > /dev/tty1 2>/dev/null || true
printf 'ArkEPass GUI startup failed: %s\n' "$reason" > /dev/ttyS0 2>/dev/null || true

mark_stage "drm-arch-app:fallback-done"
exit 0
