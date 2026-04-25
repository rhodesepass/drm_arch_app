#!/bin/sh
# F1C200s CPU overclock controller
# Usage: overclock.sh [freq_mhz]   — set specific frequency
#        overclock.sh autotune      — find highest stable frequency

PLL_CPU=0x01C20000
CPU_CLK=0x01C20050

set_freq() {
    FREQ=$1
    N=$(( FREQ / 24 - 1 ))
    if [ $N -lt 1 ] || [ $N -gt 31 ]; then
        echo "[overclock] Error: ${FREQ}MHz out of range (48-768MHz)"
        return 1
    fi
    if [ "$FREQ" -le 408 ]; then
        echo "[overclock] ${FREQ}MHz <= default 408MHz, skipping"
        return 0
    fi
    echo "[overclock] Setting CPU to ${FREQ}MHz (N=$N)..."
    devmem $CPU_CLK 32 0x00010000
    PLL_VAL=$(printf "0x%08x" $(( 0x80000000 | ($N << 8) )))
    devmem $PLL_CPU 32 $PLL_VAL
    sleep 0.05
    devmem $CPU_CLK 32 0x00020000
    echo "[overclock] Done: CPU at ${FREQ}MHz"
}

autotune() {
    FREQS="480 504 528 552 600"
    RESULT_FILE="/root/.oc_freq"
    ALIVE_FILE="/tmp/.oc_alive"

    stress_test() {
        local i=0
        while [ $i -lt 500 ]; do
            echo "stress$i" | md5sum > /dev/null 2>&1
            i=$((i+1))
        done
    }

    echo "=== F1C200s Auto-Tune ==="
    BEST=408

    for freq in $FREQS; do
        printf "  Testing %sMHz... " "$freq"
        set_freq $freq > /dev/null 2>&1
        rm -f $ALIVE_FILE
        ( stress_test && echo "ok" > $ALIVE_FILE ) &
        PID=$!
        sleep 15
        kill $PID 2>/dev/null
        wait $PID 2>/dev/null
        if [ -f "$ALIVE_FILE" ]; then
            BEST=$freq
            echo "PASS"
        else
            echo "FAIL"
            set_freq $BEST > /dev/null 2>&1
            break
        fi
    done

    echo $BEST > $RESULT_FILE
    echo "=== Sweet spot: ${BEST}MHz (saved) ==="
    set_freq $BEST > /dev/null 2>&1
}

case "$1" in
    autotune) autotune ;;
    "")
        if [ -f /root/.oc_freq ]; then
            set_freq $(cat /root/.oc_freq)
        else
            echo "Usage: overclock.sh {freq_mhz|autotune}"
        fi
        ;;
    *) set_freq "$1" ;;
esac
