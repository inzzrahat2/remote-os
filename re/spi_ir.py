"""Redmi 7 (onclite) native IR: encode an IR timing pattern as the bitstream the
ir2661 SPI blaster expects on /dev/spidev6.1 (same scheme as Xiaomi's consumerir HAL)."""
SPACE_BITS_PER_US = 0.8
CHUNK = "0" * 9 + "1" * 10 + "0" * 11 + "1" * 10        # two 38 kHz carrier cycles (40 bits)

def encode(pattern, freq=38000):
    out = []
    for i, us in enumerate(pattern):
        if i % 2 == 0:                                   # mark: whole carrier cycles
            cycles = int(us * freq / 1e6)
            full, half = divmod(cycles, 2)
            out.append(CHUNK * full + (CHUNK[:19] if half else ""))
        else:                                            # space: zeros
            out.append("0" * round(us * SPACE_BITS_PER_US))
    bits = "".join(out)
    bits += "0" * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))

def nec(cmd, a0=0x00, a1=0x7F):
    p = [9000, 4500]
    for b in (a0, a1, cmd & 0xFF, (~cmd) & 0xFF):
        for bit in range(8):
            p += [562, 1687 if (b >> bit) & 1 else 562]
    return p + [562]

if __name__ == "__main__":
    import sys
    cmd = int(sys.argv[1], 16)
    a0, a1 = (int(sys.argv[2], 16), int(sys.argv[3], 16)) if len(sys.argv) > 3 else (0x00, 0x7F)
    sys.stdout.buffer.write(encode(nec(cmd, a0, a1)))
