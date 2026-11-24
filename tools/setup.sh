#!/bin/bash
# Fetch the build toolchain into tools/ (git-ignored):
#   - Zig 0.13.0 (C cross-compiler for static aarch64-linux-musl binaries), SHA-256 verified against ziglang.org
#   - BTstack v1.6.1 (BlueKitchen Bluetooth stack; free for non-commercial use, see tools/btstack/LICENSE)
set -eo pipefail
cd "$(dirname "$0")"
if [ ! -x zig-linux-x86_64-0.13.0/zig ]; then
    mkdir -p dl
    curl -sSfL -o dl/index.json https://ziglang.org/download/index.json
    read -r URL SUM < <(python3 -c "import json;d=json.load(open('dl/index.json'))['0.13.0']['x86_64-linux'];print(d['tarball'],d['shasum'])")
    curl -sSfL -o dl/zig.tar.xz "$URL"
    echo "$SUM  dl/zig.tar.xz" | sha256sum -c -
    tar -xf dl/zig.tar.xz
fi
[ -d btstack ] || git clone --depth 1 --branch v1.6.1 https://github.com/bluekitchen/btstack.git btstack
zig-linux-x86_64-0.13.0/zig version && echo "toolchain ready"
