#!/usr/bin/env python3
"""Draw resources/fdcplustest.ico: an S-100 card with a green check mark.

Uses only the standard library. Run from the repository root:
    python3 tools/make_icon.py
"""
import math
import struct
import zlib

SIZES = [16, 24, 32, 48, 64, 128, 256]
SUB = 4  # subsamples per pixel axis

BOARD = (0x1F, 0x6B, 0x3A)      # S-100 card green
BOARD_EDGE = (0x12, 0x40, 0x22)
GOLD = (0xD9, 0xA4, 0x2B)
CHIP = (0x22, 0x22, 0x26)
CHIP_PIN = (0xC8, 0xCC, 0xD2)
TRACE = (0x7C, 0xC0, 0x8C)
BLUE = (0x4A, 0x7A, 0xB0)       # Altair blue
WHITE = (0xFF, 0xFF, 0xFF)


def rounded_rect(x0, y0, x1, y1, r):
    def inside(x, y):
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r and x0 <= x <= x1 and y0 <= y <= y1
    return inside


def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def polyline(points, width):
    h = width / 2

    def inside(x, y):
        for (ax, ay), (bx, by) in zip(points, points[1:]):
            dx, dy = bx - ax, by - ay
            t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
            if (x - ax - t * dx) ** 2 + (y - ay - t * dy) ** 2 <= h * h:
                return True
        return False
    return inside


# Layers, painted in order. Coordinates run 0..1, y downwards.
LAYERS = [
    (rounded_rect(0.06, 0.08, 0.94, 0.80, 0.05), BOARD_EDGE),
    (rounded_rect(0.08, 0.10, 0.92, 0.78, 0.04), BOARD),
    (rounded_rect(0.14, 0.78, 0.86, 0.92, 0.01), BOARD),            # edge connector tab
]
LAYERS += [(rounded_rect(0.16 + i * 0.07, 0.80, 0.20 + i * 0.07, 0.91, 0.005), GOLD) for i in range(10)]
LAYERS += [
    (rounded_rect(0.16, 0.18, 0.30, 0.22, 0.01), TRACE),
    (rounded_rect(0.16, 0.30, 0.30, 0.34, 0.01), TRACE),
    (rounded_rect(0.16, 0.42, 0.30, 0.46, 0.01), TRACE),
    (rounded_rect(0.32, 0.14, 0.62, 0.50, 0.02), CHIP_PIN),          # the microcontroller
    (rounded_rect(0.34, 0.16, 0.60, 0.48, 0.02), CHIP),
    (circle(0.38, 0.20, 0.015), CHIP_PIN),
    (rounded_rect(0.68, 0.16, 0.84, 0.28, 0.02), BLUE),              # serial connector
    (circle(0.72, 0.72, 0.25), WHITE),
    (circle(0.72, 0.72, 0.21), (0x3C, 0xAA, 0x3C)),
    (polyline([(0.61, 0.73), (0.69, 0.81), (0.84, 0.63)], 0.075), WHITE),
]


def render(size):
    rows = []
    n = SUB * SUB
    for py in range(size):
        row = bytearray()
        for px in range(size):
            r = g = b = a = 0
            for sy in range(SUB):
                y = (py + (sy + 0.5) / SUB) / size
                for sx in range(SUB):
                    x = (px + (sx + 0.5) / SUB) / size
                    color = None
                    for inside, c in LAYERS:
                        if inside(x, y):
                            color = c
                    if color:
                        r += color[0]
                        g += color[1]
                        b += color[2]
                        a += 1
            if a:
                row += bytes((r // a, g // a, b // a, a * 255 // n))
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\0" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def bmp(size, rows):
    # 32 bit BGRA DIB, bottom-up, followed by a 1 bit AND mask.
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = b"".join(
        bytes(v for i in range(0, len(row), 4) for v in (row[i + 2], row[i + 1], row[i], row[i + 3]))
        for row in reversed(rows))
    mask_stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for row in reversed(rows):
        bits = bytearray(mask_stride)
        for x in range(size):
            if row[x * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + pixels + bytes(mask)


def main():
    images = []
    for size in SIZES:
        rows = render(size)
        images.append((size, png(size, rows) if size >= 256 else bmp(size, rows)))

    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in images:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b"".join(data for _, data in images)
    with open("resources/fdcplustest.ico", "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
