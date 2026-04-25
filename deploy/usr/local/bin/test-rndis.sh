#!/bin/sh
# test-rndis.sh — RNDIS USB 网络栈完整验证
# 用法: test-rndis.sh [-v]
# 设计: ArkEPass F1C200s, POSIX sh 兼容

RED='\033[31m'; GRN='\033[32m'; YLW='\033[33m'
CYN='\033[36m'; RST='\033[0m'

PASS_N=0; FAIL_N=0; WARN_N=0
VERBOSE=0
[ "$1" = "-v" ] && VERBOSE=1

GADGET=/sys/kernel/config/usb_gadget/g1
EXPECT_IP="192.168.137.2"
GATEWAY="192.168.137.1"

pass() { PASS_N=$((PASS_N+1)); printf "${GRN}PASS${RST} %s\n" "$1"; }
fail() { FAIL_N=$((FAIL_N+1)); printf "${RED}FAIL${RST} %s\n" "$1"; }
warn() { WARN_N=$((WARN_N+1)); printf "${YLW}WARN${RST} %s\n" "$1"; }
info() { printf "${CYN}INFO${RST} %s\n" "$1"; }

sysrd() { [ -f "$1" ] && cat "$1" 2>/dev/null | tr -d '\n' || echo ""; }

# === Phase 1: ConfigFS ===
printf "\n${CYN}=== Phase 1: ConfigFS ===${RST}\n"

if mountpoint -q /sys/kernel/config 2>/dev/null; then
    pass "configfs mounted"
else
    fail "configfs NOT mounted"
fi

if [ ! -d "$GADGET" ]; then
    fail "gadget g1 not found"
else
    pass "gadget g1 exists"

    VID=$(sysrd "$GADGET/idVendor")
    PID=$(sysrd "$GADGET/idProduct")
    CLS=$(sysrd "$GADGET/bDeviceClass")
    SUB=$(sysrd "$GADGET/bDeviceSubClass")
    PRT=$(sysrd "$GADGET/bDeviceProtocol")
    info "USB VID=$VID PID=$PID Class=$CLS:$SUB:$PRT"

    USE=$(sysrd "$GADGET/os_desc/use")
    SIGN=$(sysrd "$GADGET/os_desc/qw_sign")
    BVND=$(sysrd "$GADGET/os_desc/b_vendor_code")
    [ "$USE" = "1" ] && pass "os_desc/use=1" || fail "os_desc/use=$USE (need 1)"
    case "$SIGN" in
        MSFT100*) pass "qw_sign=MSFT100" ;;
        *) fail "qw_sign='$SIGN' (need MSFT100)" ;;
    esac
    info "b_vendor_code=$BVND"

    CID=$(sysrd "$GADGET/functions/rndis.usb0/os_desc/interface.rndis/compatible_id")
    case "$CID" in
        RNDIS*) pass "compatible_id=RNDIS" ;;
        *) fail "compatible_id='$CID' (need RNDIS)" ;;
    esac

    HADDR=$(sysrd "$GADGET/functions/rndis.usb0/host_addr")
    DADDR=$(sysrd "$GADGET/functions/rndis.usb0/dev_addr")
    info "MAC host=$HADDR dev=$DADDR"

    UDC=$(sysrd "$GADGET/UDC")
    if [ -n "$UDC" ]; then
        case "$UDC" in
            *musb*) pass "UDC=$UDC" ;;
            *) warn "UDC=$UDC (expected musb)" ;;
        esac
    else
        fail "UDC not bound"
    fi
fi

# === Phase 2: Network ===
printf "\n${CYN}=== Phase 2: Network ===${RST}\n"

if ! ip link show usb0 >/dev/null 2>&1; then
    fail "usb0 not found"
else
    pass "usb0 exists"

    ADDR=$(ip -4 -o addr show usb0 2>/dev/null | awk '{print $4}')
    case "$ADDR" in
        ${EXPECT_IP}*) pass "IP=$ADDR" ;;
        "") fail "usb0 no IPv4" ;;
        *) warn "IP=$ADDR (expected $EXPECT_IP)" ;;
    esac

    FLAGS=$(ip link show usb0 2>/dev/null)
    case "$FLAGS" in
        *LOWER_UP*) pass "usb0 carrier UP" ;;
        *UP*) warn "usb0 UP but NO carrier (cable?)" ;;
        *) fail "usb0 DOWN" ;;
    esac

    DFRT=$(ip route show default 2>/dev/null)
    case "$DFRT" in
        *${GATEWAY}*usb0*) pass "route via $GATEWAY" ;;
        *${GATEWAY}*) warn "route via $GATEWAY (not usb0)" ;;
        *) fail "no default route" ;;
    esac

    NS=$(grep -c '^nameserver' /etc/resolv.conf 2>/dev/null || echo 0)
    [ "$NS" -gt 0 ] 2>/dev/null && pass "DNS: $NS nameserver(s)" || fail "no DNS"
fi

# === Phase 3: Connectivity ===
printf "\n${CYN}=== Phase 3: Connectivity ===${RST}\n"

do_ping() {
    _label="$1"; _host="$2"; _timeout="$3"; _strict="$4"
    _out=$(ping -c 1 -W "$_timeout" "$_host" 2>&1)
    _rc=$?
    _ms=$(echo "$_out" | grep -o 'time=[0-9.]*' | cut -d= -f2)
    if [ $_rc -eq 0 ]; then
        pass "$_label (${_ms}ms)"
    elif [ "$_strict" = "1" ]; then
        fail "$_label"
    else
        warn "$_label (need Windows ICS)"
    fi
}

do_ping "gw $GATEWAY" "$GATEWAY" 2 1
do_ping "ext 8.8.8.8" "8.8.8.8" 3 0
do_ping "dns google.com" "google.com" 3 0

# === Phase 4: Debug ===
if [ "$VERBOSE" = "1" ]; then
    printf "\n${CYN}=== Phase 4: Debug ===${RST}\n"
    _st=$(systemctl is-active usb-rndis.service 2>/dev/null || echo "unknown")
    info "usb-rndis.service: $_st"
    info "kernel messages:"
    dmesg 2>/dev/null | grep -iE "rndis|gadget|musb|usb0" | tail -5
else
    info "add -v for debug details"
fi

# === Summary ===
TOTAL=$((PASS_N + FAIL_N))
printf "\n--- RNDIS Test: ${GRN}${PASS_N}${RST}/${TOTAL} PASS"
[ "$WARN_N" -gt 0 ] && printf ", ${YLW}${WARN_N}${RST} WARN"
[ "$FAIL_N" -gt 0 ] && printf ", ${RED}${FAIL_N}${RST} FAIL"
printf " ---\n"

if [ "$FAIL_N" -eq 0 ]; then
    printf "OVERALL: ${GRN}PASS${RST}\n"
    exit 0
else
    printf "OVERALL: ${RED}FAIL${RST}\n"
    exit 1
fi
