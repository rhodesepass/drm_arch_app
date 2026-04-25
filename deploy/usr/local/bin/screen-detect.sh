#!/bin/sh
# Apply runtime screen quirks from firstboot selection.
# TCON0 CTL register bit 23 = SUN4I_TCON0_CTL_SWAP.
TCON_CTL=0x1C0C000
SCREEN_CONF=/etc/screen_type

if [ ! -r "$SCREEN_CONF" ]; then
    echo "WARNING: $SCREEN_CONF missing; using one-shot fallback screen=laowu" >&2
    SCREEN_TYPE=laowu
else
    SCREEN_TYPE=$(tr -d '\r\n' < "$SCREEN_CONF")
fi

case "$SCREEN_TYPE" in
    hsd)
        if command -v devmem >/dev/null 2>&1; then
            local_val=$(devmem $TCON_CTL 32)
            devmem $TCON_CTL 32 $(( local_val & ~(1 << 23) ))
        fi
        echo "Screen: hsd (R/B swap disabled)"
        ;;
    laowu)
        echo "Screen: laowu (R/B swap from device tree)"
        ;;
    boe)
        echo "Screen: boe (no runtime R/B swap override; verify color order on hardware)"
        ;;
    *)
        echo "WARNING: unknown screen type '$SCREEN_TYPE'; using one-shot fallback screen=laowu" >&2
        ;;
esac
