#!/bin/bash
# Build btremote (static aarch64) with Zig + BTstack sources.
set -e
cd "$(dirname "$0")"
Z=../tools/zig-linux-x86_64-0.13.0/zig
B=../tools/btstack
SRC="$B/src/btstack_memory.c $B/src/btstack_linked_list.c $B/src/btstack_memory_pool.c $B/src/btstack_run_loop.c
 $B/src/btstack_util.c $B/src/hci.c $B/src/hci_cmd.c $B/src/hci_dump.c $B/src/hci_event.c $B/src/l2cap.c
 $B/src/l2cap_signaling.c $B/src/btstack_tlv.c $B/src/btstack_crypto.c $B/src/ad_parser.c $B/src/btstack_hid.c
 $B/src/btstack_hid_parser.c
 $B/src/classic/sdp_server.c $B/src/classic/sdp_util.c $B/src/classic/hid_device.c $B/src/classic/device_id_server.c
 $B/src/classic/btstack_link_key_db_tlv.c
 $B/platform/posix/btstack_run_loop_posix.c $B/platform/posix/btstack_tlv_posix.c
 $B/3rd-party/rijndael/rijndael.c"
$Z cc -target aarch64-linux-musl -static -Os -w -I. -I$B/src -I$B/platform/posix -I$B/platform/embedded -I$B/3rd-party/rijndael -I$B/3rd-party/bluedroid/encoder/include -I$B/3rd-party/bluedroid/decoder/include -I$B/3rd-party/micro-ecc -I$B/3rd-party/yxml \
   -o btremote btremote.c $SRC
ls -l btremote
