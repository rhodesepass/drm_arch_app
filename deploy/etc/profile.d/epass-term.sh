#!/bin/sh

tty_path=$(tty 2>/dev/null || true)

ensure_tty_geometry() {
    tty_size=$(stty size 2>/dev/null || true)

    case "${tty_size}" in
        ""|"0 0"|"0 "*|"* 0")
            stty rows "${EPASS_TERM_ROWS:-24}" cols "${EPASS_TERM_COLS:-80}" 2>/dev/null || true
            LINES=${EPASS_TERM_ROWS:-24}
            COLUMNS=${EPASS_TERM_COLS:-80}
            export LINES COLUMNS
            ;;
        *)
            LINES=${tty_size% *}
            COLUMNS=${tty_size#* }
            export LINES COLUMNS
            ;;
    esac
}

case "$tty_path" in
    /dev/ttyS0)
        TERM=vt100
        export TERM
        ;;
esac

case "${TERM:-}" in
    "")
        TERM=vt100
        export TERM
        ;;
esac

case "$tty_path" in
    /dev/ttyS0|/dev/tty0|/dev/tty1|/dev/console)
        ensure_tty_geometry
        ;;
esac
