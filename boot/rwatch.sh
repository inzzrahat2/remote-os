#!/sbin/sh
# Remote OS safety watchdog (phone has no working buttons).
# The app writes an increasing counter to /tmp/rhb every loop (>= once per second while awake).
# If the counter does not change for 6 consecutive checks (~60 s of *awake* time), reboot.
# Both this script and the app are frozen during suspend, so sleeping never trips it.
sleep 60
last=""; same=0
while true; do
  c=$(cat /tmp/rhb 2>/dev/null)
  if [ -n "$c" ] && [ "$c" = "$last" ]; then same=$((same + 1)); else same=0; last="$c"; fi
  if [ "$same" -ge 6 ]; then
    echo "rwatch: app stuck at beat $c - rebooting" >> /tmp/remoteos.log
    sync; /sbin/reboot || echo b > /proc/sysrq-trigger
  fi
  sleep 10
done
