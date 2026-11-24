# AGENTS.md — Remote OS (Redmi 7 room remote)

Instructions for AI coding agents (Claude Code, Codex, etc.) working in this repository. Read this whole file
before changing anything: the target phone has **no working buttons**, so a mistake can leave it unreachable
for hours.

---

## 1. What this project is

**Remote OS** replaces Android on a Xiaomi **Redmi 7 (codename `onclite`)** with a tiny native system that boots
straight into one full-screen remote-control app for a TV and a ceiling fan (AC and a light later).

- Boot image = the phone's **TWRP recovery kernel** (patched, see §6) + **TWRP's ramdisk** in which TWRP's
  `/sbin/recovery` program is replaced by our app, plus two services (Bluetooth daemon, watchdog).
- No Android framework, no Java, no Google services, no cellular modem. Everything is static aarch64 C.
- Boots in ~20 s, deep-sleeps between uses, wakes on touchscreen double-tap, sends IR through the phone's own
  IR chip, and acts as a **Bluetooth HID keyboard/remote** for the TV (no pointing).
- Measured battery: ~10.8 mA average with heavy use (~47 wakes/day) ≈ 6.6 %/day ≈ 14 days per charge.

Partition layout on the phone (since 2026-11-08):

| Partition | Content |
|---|---|
| `boot` | **Remote OS** (current: v0.7) |
| `recovery` | **TWRP 3.5.1** (maintenance; reached from the app's System page → "Recovery") |
| `system`, `vendor`, `userdata` | old Android, untouched but no longer booted |
| full backup | `redmi7-backup/` in this repo (all 58 partitions, SHA-256 verified) |

---

## 2. Hardware facts (verified on this exact phone)

| Item | Value |
|---|---|
| SoC / kernel | Snapdragon 632 (msm8953 family), Linux 4.9.268, boot header v1, page 2048, kernel = gzip Image + appended DTBs |
| ADB | USB serial: see `LOCAL.md` (git-ignored) or `adb devices`; adb state in Remote OS is **`recovery`** (TWRP's adbd banner). Wi-Fi ADB is not available in Remote OS |
| Buttons | **power and volume are physically broken** — no key combos, no forced reboot |
| Wake | goodix touchscreen double-tap gesture → `KEY_WAKEUP` (code 143) on the touch input device; `/sys/android_touch/wakeup_gesture_enable` = 1 |
| Auto-boot | bootloader powers on when a charger is connected (`androidboot.mode=charger`; our init handles it, §6) |
| Display | `/dev/graphics/fb0`, 720×1520, 32 bpp RGBA (r offset 0), line length 2944 bytes (stride 736 px), double-height virtual |
| Touch | `goodix-ts` (found by name), ABS_MT max 721×1521 |
| Backlight | `/sys/class/leds/lcd-backlight/brightness` (max 255) |
| Vibrator | `/sys/class/leds/vibrator/duration` (ms) then write `1` to `activate` |
| IR blaster | `ir2661cl510` on SPI → **`/dev/spidev6.1`**, ~750 kHz SPI clock, spidev bufsiz 32768 |
| Bluetooth | Qualcomm WCNSS/Pronto inside the SoC; HCI over SMD: **`/dev/smd3`** (commands/events), **`/dev/smd2`** (ACL); a byte stream |
| Battery | ATL 4000 mAh (charge_full ≈ 3945 mAh), 4.4 V Li-ion, QG fuel gauge (see §9 gotcha) |
| CPU | 8 cores; Remote OS keeps **cpu0–3 online, schedutil, max 614.4 MHz** |
| Storage used by the app | `/cache` (ext4, mounted by init): `/cache/remoteos/{state,bt.tlv,bt_tv,batlog.csv}` |

Devices it controls:

| Device | Protocol | Address bytes | Notes |
|---|---|---|---|
| "Nova" 32" Android TV (Smarttech/Expressluck Apollo, MStar MT5867) | NEC ext, 38 kHz | `00 7F` | also paired over Bluetooth as HID; its BT MAC / LAN IP are in `LOCAL.md`; ADB on port 5555 makes it an IR detector (§8) |
| Walton Super Saver-IR BLDC fan | NEC ext, 38 kHz | `D8 7E` | one-way IR; app remembers last sent speed |

TV IR commands: Power 15, Input 12, Mute 16, Up 0C, Down 0D, Left 0F, Right 0E, OK 10, Back 13, Home 42, Exit 11,
Vol+ 1B, Vol− 1A, Ch+ 19, Ch− 18, Info 14, Netflix 4F, Settings 17, Media 4C, Play/Pause 43, Rew 47, FF 46,
Stop 4B, Rec 40, Help 0B, Red 53, Green 52, Yellow 51, Blue 50, digits 0–9 = 00–09.
Fan IR commands: Power 08, LED 07, Speed 1–5 = 01–05, Speed 6 = 15, Speed 7 = 13, Reverse 12, Timer 2H 09, 4H 10, Eco 14.

---

## 3. Repository layout

```
Remote-OS/
├── AGENTS.md              ← this file (CLAUDE.md imports it)
├── deploy.sh              ← build + flash + reboot + status (§5). Use this, not manual steps.
├── app/
│   ├── remoteos.c         ← THE app (UI, IR, touch, sleep, battery, BT control). ~800 lines, single file.
│   ├── fonts.h            ← generated anti-aliased Noto Sans glyphs (do not hand-edit)
│   ├── bake_fonts.py      ← regenerates fonts.h from system TTFs (needs Pillow)
│   ├── remoteos_test.c    ← first hardware bring-up test (historical)
│   └── hcitest.c          ← raw HCI smoke test for the Bluetooth chip (useful for BT debugging)
├── bt/
│   ├── btremote.c         ← Bluetooth HID daemon (BTstack + custom SMD transport)
│   ├── btstack_config.h   ← BTstack feature config (Classic only)
│   └── build.sh           ← builds bt/btremote with Zig + BTstack sources
├── boot/
│   ├── build_image.py     ← builds the boot image from recovery.img (§6). `--boot` = patched kernel for the boot partition
│   ├── rwatch.sh          ← watchdog service (reboots if the app's heartbeat counter stalls ~60 s awake time)
│   ├── recovery.img       ← TWRP image from the phone = template (kernel, DTBO, header, ramdisk). DO NOT DELETE
│   ├── rec_rd/            ← unpacked TWRP ramdisk (reference only)
│   ├── stock_boot.img     ← original Magisk/Android boot image (reference)
│   └── remoteos-v*-boot.img  ← built images (old ones can be deleted to save disk; keep the current one)
├── re/                    ← reverse-engineering of the IR chip: spi_ir.py (reference encoder), HAL traces
├── tools/
│   ├── zig-linux-x86_64-0.13.0/  ← cross compiler (`zig cc -target aarch64-linux-musl -static`)
│   └── btstack/           ← BTstack v1.6.1 source (BlueKitchen; free for non-commercial use)
└── redmi7-backup/         ← FULL PARTITION BACKUP of the phone. Never modify. manifest.txt has SHA-256 per partition
```

Host prerequisites: Linux x86_64, `adb`, `git`, `curl`, `python3` (+ Pillow only for `bake_fonts.py`). Run `tools/setup.sh` once to fetch Zig 0.13.0 and BTstack v1.6.1 into `tools/` (git-ignored).
Private, git-ignored files: `LOCAL.md` (serials, MACs, IPs), `LOCAL.env` (deploy overrides), `redmi7-backup/`, `boot/recovery.img` (your device's TWRP image — the template; see README).

---

## 4. Architecture

### 4.1 Boot / init (ramdisk = TWRP's, edited by `boot/build_image.py`)
TWRP's `init` runs `init.rc` → our additions in `init.recovery.service.rc`:
- `on fs` → `mount ext4 /dev/block/by-name/cache /cache noatime` (must happen before services: BT keys live there)
- `on boot` → `setprop sys.usb.config adb` (TWRP's own UI normally does this; ours doesn't)
- `on charger` → `trigger late-init` (charger power-on otherwise skips all services)
- `service recovery /sbin/recovery` = **our app** (TWRP's binary replaced)
- `service btremote /sbin/btremote` = Bluetooth daemon
- `service rwatch /sbin/sh /sbin/rwatch.sh` = watchdog

### 4.2 The app (`app/remoteos.c`)
Single-threaded UI loop + one sleeper thread.
- **Pages**: `PG_HOME` (default at every boot), `PG_TV`, `PG_FAN`, `PG_SYS` ("..." tab). Widgets are declared in
  `build_ui()` with `add()` / `IR()` (page, shape, x, y, w, h, action, addr bytes, cmd, repeat, arg, label, icon, colour).
  Shapes: rounded rect, circle, pill rocker (top + bottom halves), D-pad, touchpad (`S_TPAD`), tab.
- **Drawing**: software rasteriser into a back buffer (anti-aliased shapes, baked fonts), then `memcpy` to fb0 +
  `FBIOPAN_DISPLAY`. Each page's static image is **cached** (`page_cache[]`); a redraw = memcpy + pressed widget +
  overlays (BT indicator, status line) ≈ 15 ms at 614 MHz. Call `invalidate(page|-1)` whenever state shown on a
  cached page changes (e.g. fan speed highlight).
- **Input**: touch fd in an epoll with `EPOLLWAKEUP`. Normal keys fire on release; repeat keys (VOL/CH/rew/ff) fire
  on press and send NEC repeat frames every 108 ms after 400 ms. Touchpad: one arrow per `TPAD_STEP` (80 px) of
  travel, tap (<24 px, <500 ms) = OK; a drag that starts on the pad stays with the pad.
- **IR**: `ir_nec(a0, a1, cmd)` → `ir_write_pattern()` encodes marks as whole 38 kHz carrier cycles from the 40-bit
  chunk `0000000001111111111000000000001111111111`, spaces as µs × 0.8 zero bits, MSB first, one `write()` to
  `/dev/spidev6.1`. (Reverse-engineered from Xiaomi's `consumerir.default.so`; reference in `re/spi_ir.py`.)
- **Bluetooth**: TV keys go to the daemon (`bt_send_key`) if `/tmp/btstate` is `connected` and the key has a HID
  mapping (`bt_map()`), otherwise IR. Netflix/Settings/Media/Exit/F-keys are IR-only.
- **Power**: screen off after 5 s idle (`SCREEN_TIMEOUT_MS`, `SCREEN_TIMEOUT_KEY_MS`); 3 s later the wake lock is
  released and the **sleeper thread** suspends: read `/sys/power/wakeup_count` (blocks while any wakeup source is
  active — never do this on the UI thread), write it back, arm `rtc0/wakealarm`, write `mem` to `/sys/power/state`.
  RTC alarm = BT linger deadline if pending, else every 5 h (`SAFETY_WAKE_S`). The kernel has **no autosleep**.
- **Bluetooth power policy**: screen on → `on` (instant if the link was kept); screen off → `standby` (keep an
  existing HID link in sniff, no reconnects) → after `BT_LINGER_MS` (5 min) → `off` (radio off).
- **State**: `/cache/remoteos/state` = `fan_on fan_speed brightness page` (fsync + rename). Fan state is only
  "last sent" — IR is one-way.
- **Battery**: QG gauge % is only computed at power-on (§9), so the System page shows a live estimate from the
  resting voltage sampled right after each resume (`soc_from_mv()` table). Low battery: power off when voltage
  < 3.45 V for 60 s or < 3.35 V at once (while discharging). Log: `/cache/remoteos/batlog.csv`
  (`boot_h,event,est%,rest_mV,now_mV,status,gauge%`, rotated at 200 KB).
- **Heartbeat**: increments a counter in `/tmp/rhb` every loop; `rwatch.sh` reboots if it doesn't change for 6 checks
  (≈60 s of *awake* time; both are frozen during suspend, so sleep never trips it).
- **CPU**: at start the app offlines cpu4–7, sets schedutil and caps policy0 at 614400 kHz, writes
  `lpm_levels/parameters/sleep_disabled=0` (needed for deep idle).

### 4.3 Bluetooth daemon (`bt/btremote.c`)
BTstack host stack, HID **device** role (keyboard report ID 1 + consumer control report ID 2), SSP Just Works,
CoD 0x2540, local name "Nova Remote", BD_ADDR from `/cache/remoteos/bt_addr` (random locally-administered address generated on first start; `deploy.sh` seeds it from `BT_ADDR` in `LOCAL.env` so an existing pairing survives), TV address in `/cache/remoteos/bt_tv` (learned on first connection), link keys in `/cache/remoteos/bt.tlv` (fsync'd
on `HCI_EVENT_LINK_KEY_NOTIFICATION`).
Bring-up sequence (all verified; each step fixed a real failure). Our address is programmed in step 5:
1. mount `modem` (vfat, ro) at `/vendor/firmware_mnt` (WCNSS firmware);
2. mount `persist` **read-only**, copy `WCNSS_qcom_wlan_nv.bin` + `WCNSS_wlan_dictionary.dat` to
   `/lib/firmware/wlan/prima/` (RF calibration; the vendor copies are symlinks into persist);
3. open **`/dev/wcnss_wlan`** (not `/dev/subsys_wcnss` — that boots the core without RF power: "invalid hardware 0");
4. open `/dev/smd3` + `/dev/smd2` raw (cfmakeraw), retrying on EAGAIN for up to 10 s;
5. send vendor cmd `0xFC0B 01 02 06 <addr LSB first>` raw (sets BD_ADDR; survives BTstack's reset);
6. hand the SMD channels to BTstack as a synchronous transport (`can_send_packet_now = NULL`), re-framing the byte
   stream by HCI header lengths.
Pairing: if a TV is known, `pair` pages it (TV shows a dialog → OK); otherwise the phone stays discoverable for 2 min and you pair from the TV (Settings → Remotes & Accessories → Add accessory → "Nova Remote").
Control FIFO **`/tmp/btctl`** (one command per line): `on`, `off`, `standby`, `pair`, `connect`, `scan`,
`kb <hex usage> <0|1>`, `cc <hex usage> <0|1>`. State file **`/tmp/btstate`**: `off|starting|idle|pairing|connecting|connected|error`.
Log: `/tmp/btremote.log`. If the TV answers L2CAP `0x67` (it is connecting to us), back off 30 s and accept its connection.
The WCNSS core cannot be powered off from userspace once triggered (same as Android); HCI is powered off instead.

---

## 5. Build & deploy (always via `deploy.sh`)

```bash
./deploy.sh            # build app + btremote + boot image, flash `boot`, verify SHA-256, reboot, print status
./deploy.sh ram        # build, push binaries to /tmp on the phone and run them (nothing flashed; reboot reverts)
./deploy.sh status     # one-line status: version, services, BT state, battery, CPU cap
SERIAL=<serial> ./deploy.sh ...   # explicit phone (default: first device in adb state "recovery"; LOCAL.env may set it)
```
- The phone must be running Remote OS (`adb get-state` = `recovery`). If it shows `device`, it is in Android (only
  possible if someone restored the Android boot image).
- Bump `#define VERSION` in `app/remoteos.c` for every flashed change; the image is named `boot/remoteos-v<VERSION>-boot.img`.
- `deploy.sh` refuses to reboot if the written partition's SHA-256 differs from the image.
- Iterate with `ram` first, flash only verified builds.
- Building the boot image by hand: `cd boot && python3 build_image.py ../app/remoteos out.img --boot`.
- Fonts: `cd app && python3 bake_fonts.py` (regenerates `fonts.h`; sizes/charsets in the script).
- Compiler flags used: `zig cc -target aarch64-linux-musl -static -O2 -Wall -Wno-unused-variable ... -lm`.
  Static functions used before definition need a forward declaration near the `state_save` prototype block.

---

## 6. Boot image facts you must not break
- Template is `boot/recovery.img` (TWRP 3.5.1_9 by KostyaJRZ): header v1, page 2048, includes a **recovery DTBO**
  (carried over; its offset is recomputed), cmdline kept (`androidboot.selinux=permissive buildvariant=eng`).
- `--boot` **patches the kernel**: the bootloader appends `skip_initramfs` on normal boots (system-as-root,
  Android-9-launch device), which would make the kernel ignore our ramdisk. The script decompresses the gzip Image,
  renames `skip_initramfs` → `want_initramfs` (as Magisk does), recompresses and re-appends the DTBs (`d00dfeed`).
- Header SHA-1 id is recomputed (kernel, ramdisk, second, recovery_dtbo + sizes).
- The ramdisk is edited as a newc cpio in Python (keeps TWRP's symlinks/modes); no device nodes inside.
- `misc` partition: a stale "boot-recovery" BCB makes every reboot go to recovery. TWRP clears it when it starts.
  Check with `adb shell 'dd if=/dev/block/by-name/misc bs=64 count=2 | od -c'`.

---

## 7. SAFETY RULES (read twice)
1. **Never run `fastboot boot <img>`** on this phone. It froze the bootloader (USB error -71) and the only way out
   was a full battery drain (hours). Nothing else in fastboot is needed.
2. **Never write to `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `misc` (except clearing BCB), `aboot`, `sbl1`,
   `rpm`, `tz`.** Mount `persist` read-only only. Only `boot` (and, if asked, `recovery`) are ever flashed.
3. Before flashing anything new in kind (not just a new app build), confirm `redmi7-backup/manifest.txt` exists and
   tell the user. Restore any partition with:
   `zcat redmi7-backup/<part>.img.gz | adb shell "dd of=/dev/block/by-name/<part> bs=4194304"` and verify SHA-256.
4. Every image must keep: adbd, the watchdog, the app's "Restart"/"Recovery" buttons. Without buttons these are the
   only ways back. Test risky changes with `./deploy.sh ram` (reboot reverts).
5. Keep the battery ≥ ~20 % for flashing; a stall may only end by battery drain (low charge makes that faster — the
   user prefers testing at low charge for exactly that reason; ask).
6. Restoring Android: flash `redmi7-backup/boot.img.gz` to `boot`. Restoring TWRP: `redmi7-backup/recovery.img.gz` to
   `recovery` (manifest SHA `edd9bd69…`).
7. Inside `adb shell` command strings, **never use `pkill -f <name>`** when the command line contains `<name>` — it
   kills your own shell (happened 3×). Use `for p in $(pidof name); do kill $p; done`.
8. TWRP's `dd` needs numeric block sizes (`bs=4194304`, not `bs=4M`) — with `4M` it silently writes nothing.
9. Ask the user before anything that turns the TV/fan off or reboots the phone while they are using it.

---

## 8. Debugging toolbox
- **Status**: `./deploy.sh status`.
- **Logs on the phone** (RAM, lost on reboot): `/tmp/remoteos.log` (app), `/tmp/btremote.log` (BT).
  Persistent: `/cache/remoteos/batlog.csv`. Previous boot's kernel log: `mount -t pstore pstore /pst; cat /pst/console-ramoops-0`.
- **Remote control of the app** (`adb shell 'echo "<cmd>" > /tmp/cmd'`): `page N` (0 Home, 1 TV, 2 FAN, 3 System),
  `tap X Y` (same BT/IR path as a real touch), `shot` (writes the app's back buffer to `/tmp/shot.raw`).
  Wake the screen: `adb shell touch /tmp/wake`.
- **Screenshot** → PNG:
  ```bash
  adb shell 'echo shot > /tmp/cmd'; sleep 1; adb pull /tmp/shot.raw .
  python3 -c "from PIL import Image; Image.frombuffer('RGBA',(736,1520),open('shot.raw','rb').read(),'raw','RGBA',0,1).crop((0,0,720,1520)).convert('RGB').save('shot.png')"
  ```
  (Reading `/dev/graphics/fb0` directly fails with EPERM after blank/unblank cycles.)
- **Is IR reaching the TV?** The TV (`<TV_IP>:5555`, adb; IP in `LOCAL.md`) is a perfect detector:
  `adb -s <TV_IP>:5555 shell getevent -lt /dev/input/event1` (MStar IR receiver: `MSC_SCAN 00007fXX` + key) and
  `grep ' IR$' /proc/interrupts` (+34 per clean NEC frame). If the TV is in standby nothing answers — check first.
  Bluetooth keys appear on the TV as input devices "Nova Remote Keyboard"/"Nova Remote Consumer Control".
- **Suspend health**: `mkdir -p /d; mount -t debugfs none /d; cat /d/suspend_stats; cat /d/wakeup_sources`.
  USB connected = `7000000.ssusb` wakeup source active = no suspend (test sleep unplugged).
- **BT radio sanity**: `echo scan > /tmp/btctl` then `grep found /tmp/btremote.log` (should list nearby devices);
  kernel: `dmesg | grep -i wcnss` should show `IRIS Reg` and `NV download`.
- **Battery**: `cat /sys/class/power_supply/battery/{voltage_now,current_now,status}`; the % only refreshes at boot.

---

## 9. Known gotchas (each cost real time)
- **QG fuel gauge**: SOC is computed only at power-on (`qg_determine_pon_soc`); at runtime it waits for an
  Android-side algorithm via `/dev/qg` that Remote OS lacks → `capacity` stays frozen until reboot. Use the
  voltage estimate. After frequent reboots it may restore a stale "shutdown SOC".
- **Framebuffer**: path is `/dev/graphics/fb0` in recovery environments, not `/dev/fb0`.
- **USB/ADB** depends on `sys.usb.config=adb` being set by our rc; if TWRP's UI is stopped in a stock TWRP session,
  USB flaps.
- **SMD is a byte stream**: one read can contain two HCI packets; always re-frame by length.
- **Display/touch while asleep**: touch must stay in gesture mode for double-tap wake; don't power the touch controller off.
- **Pick-up wake** is not possible cheaply: accelerometer is behind the ADSP; FPC fingerprint IRQ needs the TEE. Don't retry.
- **TV pairing**: phone pages the TV → TV shows a pairing dialog → user presses OK. The TV must be awake (standby
  TV doesn't answer pages). First pairing needs the screen on (Pair keeps it on 2 min).

---

## 10. Conventions
- C99, single-file modules, no dynamic allocation in hot paths, no external libraries except BTstack.
- UI coordinates are physical pixels on a 720×1520 canvas; keep ≥16 px gaps, frequent controls ≥110 px, dangerous
  controls (power) away from frequent ones (see the Home layout rationale in git history/notes: reach order top→bottom
  = power, placeholders, fan grid, touchpad + VOL, Home/Back/Mute).
- Colours: tokens `COL_BG 0E1015, COL_CARD 1A1D25, COL_CARD2 232733, COL_PRESS 3A4152, COL_FG E8EAF0,
  COL_MUTED 8A90A2, COL_ACC 5B7CFA, COL_RED E5484D, COL_GREEN 46C37B, COL_AMBER F5A524`.
- Log every IR send and state change with `logf_`; keep messages short (RAM log).
- Measure power claims; don't assert battery numbers without data (gauge granularity is 1 % ≈ 39 mAh).

---

## 11. Status and roadmap
Done: native IR, TV+fan control, Home/TV/FAN/System pages, touchpad, deep sleep + double-tap wake, Bluetooth HID with
saved pairing and instant reconnect (5-min link hold), low-battery shutdown, battery estimate + log, CPU/power tuning,
one-command deploy.

Open / next:
1. **AC page** — waiting for the AC brand/model (AC remotes send the full state every press; port an encoder from
   IRremoteESP8266). Home has an AC power placeholder (`A_SOON`) and an AC card at (24,296 328×104).
2. **Light** card placeholder (368,296 328×104) — device not decided.
3. Measure sleep current with Bluetooth never started (A/B overnight) — if WCNSS costs ≥3 mA, start it on first TV key.
4. Optional: long-press (~400 ms) on power keys; left-handed mirror of the TV cluster (x' = 720 − x − w).

Related repositories: the Android remote app is in `../Nova-Remote-App`; the IR research, TV code tables and the
reusable `ir-remote-lab` skill are in `../Tv-Remove--revarse-eng` (`.claude/skills/ir-remote-lab/`).
