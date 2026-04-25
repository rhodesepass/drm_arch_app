#!/bin/sh
set -u

APP=/usr/local/bin/drm_arch_app
APPSTART=/tmp/appstart
SRGN_CONFIG=/usr/local/bin/srgn_config
SRGN_CONFIG_RUNNER=/usr/local/bin/epass-run-srgn-config.sh
APP_IMPORTER=/usr/local/bin/epass-app-import.sh
BOOT_MARK=/usr/local/bin/epass-boot-mark.sh
RUN_DIR=/run/epass
REASON_FILE=${RUN_DIR}/gui-failure-reason
APP_LOG=${RUN_DIR}/drm_arch_app.log
LAST_LOG=${RUN_DIR}/drm_arch_app.last.log
GUI_ALIVE_FILE=${RUN_DIR}/gui-alive

mark_stage() {
    if [ -x "$BOOT_MARK" ]; then
        "$BOOT_MARK" "$1" || true
    else
        printf '<6>epass-boot: %s\n' "$1" > /dev/kmsg 2>/dev/null || true
    fi
}

persist_failure_reason() {
    status="$1"
    summary=""

    if [ -r "$APP_LOG" ]; then
        cp "$APP_LOG" "$LAST_LOG" 2>/dev/null || true
        summary=$(awk '/app-exit code=/{line=$0} END{print line}' "$APP_LOG" 2>/dev/null)
    fi

    if [ -n "$summary" ]; then
        printf 'drm_arch_app exited with status %s; %s\n' "$status" "$summary" > "$REASON_FILE" 2>/dev/null || true
    else
        printf 'drm_arch_app exited with status %s\n' "$status" > "$REASON_FILE" 2>/dev/null || true
    fi
}

while :; do
    if [ ! -x "$APP" ]; then
        echo "ERROR: $APP is missing or not executable" >&2
        mkdir -p "$RUN_DIR" 2>/dev/null || true
        printf '%s is missing or not executable\n' "$APP" > "$REASON_FILE" 2>/dev/null || true
        mark_stage "drm-arch-app:runner-missing-binary"
        exit 127
    fi

    mkdir -p "$RUN_DIR" 2>/dev/null || true
    rm -f "$GUI_ALIVE_FILE" 2>/dev/null || true
    if [ -x "$APP_IMPORTER" ]; then
        "$APP_IMPORTER" >/dev/null 2>&1 || true
    fi
    mark_stage "drm-arch-app:runner-exec"
    "$APP"
    status=$?
    mark_stage "drm-arch-app:runner-exit-${status}"

    case "$status" in
        0)
            exit 0
            ;;
        1)
            echo "drm_arch_app requested restart"
            if [ -e "$GUI_ALIVE_FILE" ]; then
                rm -f "$REASON_FILE" 2>/dev/null || true
            else
                persist_failure_reason "$status"
            fi
            ;;
        2)
            if [ -s "$APPSTART" ]; then
                echo "drm_arch_app requested foreground app: $APPSTART"
                chmod +x "$APPSTART" 2>/dev/null || true
                /bin/sh "$APPSTART"
                appstart_status=$?
                echo "foreground app exited with status $appstart_status"
                mark_stage "drm-arch-app:foreground-app-exit-${appstart_status}"
                rm -f "$APPSTART"
            else
                echo "WARNING: appstart requested but $APPSTART is missing" >&2
            fi
            ;;
        3)
            echo "drm_arch_app requested poweroff"
            /usr/bin/systemctl poweroff
            exit 0
            ;;
        5)
            if [ -x "$SRGN_CONFIG_RUNNER" ]; then
                echo "drm_arch_app requested srgn_config"
                mark_stage "drm-arch-app:srgn-config-start"
                "$SRGN_CONFIG_RUNNER" "$SRGN_CONFIG"
                srgn_status=$?
                echo "srgn_config exited with status $srgn_status"
                mark_stage "drm-arch-app:srgn-config-exit-${srgn_status}"
            elif [ -x "$SRGN_CONFIG" ]; then
                echo "drm_arch_app requested srgn_config"
                "$SRGN_CONFIG" || true
            elif command -v srgn_config >/dev/null 2>&1; then
                echo "drm_arch_app requested srgn_config"
                srgn_config || true
            else
                echo "WARNING: srgn_config is not available" >&2
            fi
            ;;
        *)
            echo "drm_arch_app exited with unexpected status $status" >&2
            persist_failure_reason "$status"
            exit "$status"
            ;;
    esac
done
