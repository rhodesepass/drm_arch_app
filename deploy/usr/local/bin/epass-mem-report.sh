#!/bin/sh
set -u

pid="${1:-}"
if [ -z "${pid}" ]; then
    pid="$(pidof drm_arch_app 2>/dev/null | awk 'NR == 1 { print $1 }')"
fi

echo "== meminfo =="
grep -E '^(MemTotal|MemFree|MemAvailable|Cached|Buffers|Shmem|SwapTotal|SwapFree|SwapCached|CmaTotal|CmaFree):' /proc/meminfo 2>/dev/null || true
echo

if [ -n "${pid}" ] && [ -d "/proc/${pid}" ]; then
    echo "== drm_arch_app pid=${pid} status =="
    grep -E '^(VmPeak|VmSize|VmRSS|RssAnon|RssFile|RssShmem|VmSwap|Threads):' "/proc/${pid}/status" 2>/dev/null || true
    echo

    if [ -r "/proc/${pid}/smaps_rollup" ]; then
        echo "== drm_arch_app pid=${pid} smaps_rollup =="
        grep -E '^(Rss|Pss|Shared_Clean|Shared_Dirty|Private_Clean|Private_Dirty|Swap):' "/proc/${pid}/smaps_rollup" 2>/dev/null || true
        echo
    fi
else
    echo "drm_arch_app pid not found"
fi

echo "== zram =="
cat /proc/swaps 2>/dev/null || true
