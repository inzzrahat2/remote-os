#!/bin/bash
# Remote OS one-command build + deploy.
#   ./deploy.sh            build app + btremote + boot image, flash to `boot` (sha-verified), reboot, show status
#   ./deploy.sh ram        build, push binaries to /tmp and restart them (nothing flashed; lost on reboot)
#   ./deploy.sh status     just show status
# Env (or LOCAL.env, git-ignored): SERIAL=<adb serial> (default: the first device in adb state "recovery"),
#      BT_ADDR=<our Bluetooth address> (seeded into /cache/remoteos/bt_addr if the phone has none yet, so an
#      existing TV pairing survives; otherwise Remote OS generates a random address on first start).
set -eo pipefail
cd "$(dirname "$0")"
[ -f LOCAL.env ] && . ./LOCAL.env
MODE=${1:-flash}
[ -n "$SERIAL" ] || SERIAL=$(adb devices | awk '$2=="recovery" {print $1; exit}')
[ -n "$SERIAL" ] || { echo "no phone running Remote OS found (adb state 'recovery'); connect USB or set SERIAL"; exit 1; }
S=$SERIAL
Z=tools/zig-linux-x86_64-0.13.0/zig
A="adb -s $S"

status() {
    timeout 20 $A shell 'echo "$(head -1 /tmp/remoteos.log)  |  services: app=$(getprop init.svc.recovery) bt=$(getprop init.svc.btremote) watchdog=$(getprop init.svc.rwatch)  |  bt=$(cat /tmp/btstate)  |  battery $(cat /sys/class/power_supply/battery/capacity)% $(($(cat /sys/class/power_supply/battery/voltage_now)/1000)) mV  |  cpu max $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq)"'
}
wait_up() {
    sleep 5
    for i in $(seq 1 90); do [ "$($A get-state 2>/dev/null)" = recovery ] && return 0; sleep 1; done
    echo "phone did not come back on adb"; return 1
}
[ "$MODE" = status ] && { status; exit 0; }

[ "$($A get-state 2>/dev/null)" = recovery ] || { echo "phone $S not in Remote OS (adb state: $($A get-state 2>&1))"; exit 1; }

[ -n "$BT_ADDR" ] && timeout 10 $A shell "[ -s /cache/remoteos/bt_addr ] || echo $BT_ADDR > /cache/remoteos/bt_addr" || true
[ -x tools/zig-linux-x86_64-0.13.0/zig ] && [ -d tools/btstack ] || { echo "toolchain missing: run tools/setup.sh"; exit 1; }

echo "== build app"
$Z cc -target aarch64-linux-musl -static -O2 -Wall -Wno-unused-variable -o app/remoteos app/remoteos.c -lm 2>&1 | grep -E " error|warning:" | grep -v -E "dlstart|rcrt1|GETFUNCSYM|__dls2" || true
echo "== build btremote"
bt/build.sh | tail -1
VER=$(grep -m1 '#define VERSION' app/remoteos.c | cut -d'"' -f2)

if [ "$MODE" = ram ]; then
    $A push app/remoteos /tmp/remoteos.new >/dev/null; $A push bt/btremote /tmp/btremote.new >/dev/null
    timeout 30 $A shell 'setprop ctl.stop recovery; setprop ctl.stop btremote; for p in $(pidof remoteos.new btremote.new); do kill $p; done; sleep 1
        chmod 755 /tmp/remoteos.new /tmp/btremote.new; (/tmp/btremote.new >/dev/null 2>&1 &); sleep 1; (/tmp/remoteos.new >/dev/null 2>&1 &); sleep 4'
    echo "== running v$VER from RAM (reboot returns to the flashed version)"; status; exit 0
fi

IMG=boot/remoteos-v$VER-boot.img
echo "== image $IMG"
(cd boot && python3 build_image.py ../app/remoteos "$(basename $IMG)" --boot | tail -1)
L=$(stat -c %s $IMG); H=$(sha256sum $IMG | awk '{print $1}')
$A push $IMG /tmp/ro.img >/dev/null
GOT=$(timeout 60 $A shell "dd if=/tmp/ro.img of=/dev/block/by-name/boot bs=4194304 >/dev/null 2>&1; sync; head -c $L /dev/block/by-name/boot | sha256sum; rm /tmp/ro.img" | awk '{print $1}')
[ "$GOT" = "$H" ] || { echo "VERIFY FAILED: boot sha $GOT != $H — not rebooting"; exit 1; }
echo "== flashed + verified ($H)"
$A reboot; wait_up; sleep 6
status
