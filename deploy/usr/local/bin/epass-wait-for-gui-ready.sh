#!/bin/sh
set -u

READY_FILE=/run/epass/gui-alive
WAIT_TICKS=${EPASS_GUI_READY_WAIT_TICKS:-100}
SETTLE_SEC=${EPASS_GUI_READY_SETTLE_SEC:-2}

while [ "${WAIT_TICKS}" -gt 0 ]; do
    if [ -e "${READY_FILE}" ]; then
        if [ "${SETTLE_SEC}" -gt 0 ] 2>/dev/null; then
            sleep "${SETTLE_SEC}"
        fi
        exit 0
    fi

    WAIT_TICKS=$((WAIT_TICKS - 1))
    sleep 0.2
done

exit 0
