#!/bin/sh
# Boot splash wrapper: record the splash PID so the GUI-ready path can stop it directly.

RUN_DIR=/run/epass
PID_FILE=${RUN_DIR}/boot-animation.pid
BOOT_MARK=/usr/local/bin/epass-boot-mark.sh

if [ "$#" -eq 0 ]; then
    set -- /root/splash/splash.splash
fi

mkdir -p "${RUN_DIR}" 2>/dev/null || true
printf '%s\n' "$$" > "${PID_FILE}" 2>/dev/null || true
if [ -x "${BOOT_MARK}" ]; then
    "${BOOT_MARK}" "boot-animation:wrapper-start:$$" >/dev/null 2>&1 || true
fi

exec /usr/local/bin/fb-splash "$@"
