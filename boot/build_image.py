"""Build Remote OS test boot image from the TWRP recovery image:
same kernel/header/cmdline, ramdisk = TWRP ramdisk with /sbin/recovery replaced by our app
and a watchdog service added. usage: python3 build_image.py <app binary> <out.img>"""
import gzip, hashlib, struct, sys
SRC = "recovery.img"
app = open(sys.argv[1], "rb").read()
out = sys.argv[2]  # add --boot to build the boot-partition variant
img = open(SRC, "rb").read()
assert img[:8] == b"ANDROID!"
ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, hver, osv = struct.unpack_from("<10I", img, 8)
assert hver == 1, hver
pg = lambda n: (n + page - 1) // page * page
kernel = img[page:page + ksz]
if "--boot" in sys.argv:
    # Bootloader appends skip_initramfs on normal (non-recovery) boots of this SAR device; rename it
    # in the kernel exactly like Magisk does so our ramdisk is still used.
    import zlib
    dz = zlib.decompressobj(16 + zlib.MAX_WBITS)
    image = dz.decompress(kernel); tail = dz.unused_data          # tail = appended DTBs
    assert image.count(b"skip_initramfs") == 1, image.count(b"skip_initramfs")
    image = image.replace(b"skip_initramfs", b"want_initramfs")
    kernel = gzip.compress(image, 9, mtime=0) + tail
    print("kernel patched: skip_initramfs -> want_initramfs")
ramdisk = gzip.decompress(img[page + pg(ksz):page + pg(ksz) + rsz])
rdo_sz, = struct.unpack_from("<I", img, 1632)
rdo_off, = struct.unpack_from("<Q", img, 1636)
assert ssz == 0, ssz
rdo = img[rdo_off:rdo_off + rdo_sz]           # recovery DTBO, carried over unchanged

# ---- edit newc cpio
def entries(d):
    p = 0
    while True:
        assert d[p:p + 6] == b"070701", p
        f = [int(d[p + 6 + i * 8:p + 14 + i * 8], 16) for i in range(13)]
        nsz, fsz = f[11], f[6]
        name = d[p + 110:p + 110 + nsz - 1].decode()
        q = (p + 110 + nsz + 3) & ~3
        data = d[q:q + fsz]
        p = (q + fsz + 3) & ~3
        if name == "TRAILER!!!": return
        yield name, f, data

def hdr(name, f, data):
    f = list(f); f[6] = len(data); f[11] = len(name) + 1
    h = b"070701" + b"".join(b"%08X" % v for v in f) + name.encode() + b"\0"
    h += b"\0" * (-len(h) % 4)
    return h + data + b"\0" * (-len(data) % 4)

WATCH_RC = b"""
# Remote OS: mount /cache (state, Bluetooth keys) before any service starts
on fs
    mount ext4 /dev/block/by-name/cache /cache noatime

# Remote OS: TWRP's recovery binary normally configures USB; ours doesn't, so request adb here
on boot
    setprop sys.usb.config adb

# Remote OS: when the bootloader starts us because a charger was plugged in
# (androidboot.mode=charger), init only fires "charger" - run the normal boot instead.
on charger
    trigger late-init

# Remote OS: Bluetooth HID daemon (controlled by the UI via /tmp/btctl)
service btremote /sbin/btremote
    seclabel u:r:recovery:s0

# Remote OS: safety watchdog
service rwatch /sbin/sh /sbin/rwatch.sh
    seclabel u:r:recovery:s0
"""
parts, seen, maxino = [], set(), 0
for name, f, data in entries(ramdisk):
    maxino = max(maxino, f[0])
    if name == "sbin/recovery":
        data = app; f[1] = 0o100755
    elif name == "init.recovery.service.rc":
        data = data + WATCH_RC
    seen.add(name)
    parts.append(hdr(name, f, data))
assert "sbin/recovery" in seen and "init.recovery.service.rc" in seen
watch = open("rwatch.sh", "rb").read()
bt = open("../bt/btremote", "rb").read()
parts.append(hdr("sbin/btremote", [maxino + 2, 0o100755, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0], bt))
parts.append(hdr("sbin/rwatch.sh", [maxino + 1, 0o100755, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0], watch))
parts.append(hdr("TRAILER!!!", [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0], b""))
cpio = b"".join(parts); cpio += b"\0" * (-len(cpio) % 512)
new_rd = gzip.compress(cpio, 9)

# ---- header v1
h = bytearray(img[:page])
struct.pack_into("<I", h, 16, len(new_rd))
sha = hashlib.sha1()
for blob in (kernel, new_rd, b""):
    sha.update(blob); sha.update(struct.pack("<I", len(blob)))
sha.update(rdo); sha.update(struct.pack("<I", len(rdo)))   # recovery_dtbo (v1)
h[576:608] = sha.digest().ljust(32, b"\0")
pad = lambda b: b + b"\0" * (pg(len(b)) - len(b))
new_rdo_off = page + pg(len(kernel)) + pg(len(new_rd))
struct.pack_into("<Q", h, 1636, new_rdo_off)
open(out, "wb").write(bytes(h) + pad(kernel) + pad(new_rd) + pad(rdo))
print(f"{out}: kernel {len(kernel)} ramdisk {len(new_rd)} (cpio {len(cpio)}), page {page}, cmdline kept")
