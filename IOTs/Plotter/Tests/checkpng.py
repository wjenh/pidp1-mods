#!/usr/bin/env python3
"""checkpng.py -- checks the plotter's 1-bit PNG files, with nothing outside Python's own library.

Each file is checked from its bytes: the signature, every chunk's CRC, IHDR (1-bit grayscale,
no interlace), pHYs, the zlib header and Adler-32, each row's filter byte, and every pixel
against an expected bitmap. The expected bitmap is a plain PBM (P1) file, 1 for ink, as the
tests write it.

Usage:
    checkpng.py FILE.png [FILE.pbm] [--ppm N]   check one file; exit 0 if it passes
    checkpng.py --manifest LIST                 check every "png pbm ppm [fail]" line in LIST;
                                                a line ending "fail" is a control that must not
                                                match
    checkpng.py --selftest FILE.png FILE.pbm N  check that a good file passes, and that a
                                                flipped pixel, a changed IDAT byte, a changed
                                                byte with its CRC repaired, and a wrong pHYs
                                                each fail as they should

Each check prints PASS or FAIL with the reason; the exit status is 0 only if all passed.
"""

import os
import struct
import sys
import zlib

SIGNATURE = b"\x89PNG\r\n\x1a\n"


class PngError(Exception):
    """A check failed. kind names the check: signature, crc, ihdr, phys, zlib, adler, filter,
    size or pixels."""

    def __init__(self, kind, message):
        super().__init__(f"{kind}: {message}")
        self.kind = kind


def read_chunks(data):
    """Returns the chunks of data as (type, body) pairs, checking the signature and each CRC."""
    if data[:8] != SIGNATURE:
        raise PngError("signature", "not a PNG signature")
    chunks = []
    pos = 8
    while pos < len(data):
        if pos + 12 > len(data):
            raise PngError("size", f"truncated chunk header at byte {pos}")
        length, = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if len(body) != length or pos + 12 + length > len(data):
            raise PngError("size", f"chunk {ctype!r} runs past the end of the file")
        crc, = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        if zlib.crc32(ctype + body) & 0xffffffff != crc:
            raise PngError("crc", f"chunk {ctype.decode('latin-1')} CRC mismatch")
        chunks.append((ctype, body))
        pos += 12 + length
        if ctype == b"IEND":
            break
    if pos != len(data):
        raise PngError("size", f"{len(data) - pos} bytes after IEND")
    return chunks


def decode(data):
    """Decodes a 1-bit grayscale PNG. Returns (width, height, ppm, rows), ppm None without
    pHYs, rows a list of lists of 0 (white) and 1 (ink)."""
    chunks = read_chunks(data)
    types = [c[0] for c in chunks]
    if not types or types[0] != b"IHDR" or types[-1] != b"IEND":
        raise PngError("ihdr", f"chunk order {types}")
    ihdr = chunks[0][1]
    if len(ihdr) != 13:
        raise PngError("ihdr", "IHDR is not 13 bytes")
    width, height, depth, color, comp, filt, interlace = struct.unpack(">IIBBBBB", ihdr)
    if (depth, color, comp, filt, interlace) != (1, 0, 0, 0, 0):
        raise PngError("ihdr", f"depth {depth} color {color} compression {comp} "
                               f"filter {filt} interlace {interlace}")
    if width == 0 or height == 0:
        raise PngError("ihdr", "zero width or height")

    ppm = None
    for ctype, body in chunks:
        if ctype == b"pHYs":
            if len(body) != 9:
                raise PngError("phys", "pHYs is not 9 bytes")
            x, y, unit = struct.unpack(">IIB", body)
            if x != y or unit != 1:
                raise PngError("phys", f"pHYs {x} x {y} unit {unit}")
            ppm = x

    stream = b"".join(body for ctype, body in chunks if ctype == b"IDAT")
    if len(stream) < 6:
        raise PngError("zlib", "IDAT too short")
    cmf, flg = stream[0], stream[1]
    if (cmf & 0x0f) != 8 or ((cmf << 8) | flg) % 31 != 0 or (flg & 0x20):
        raise PngError("zlib", f"bad zlib header {cmf:02x} {flg:02x}")
    try:
        raw = zlib.decompress(stream[2:-4], -15)
    except zlib.error as e:
        raise PngError("zlib", f"deflate stream: {e}")
    adler, = struct.unpack(">I", stream[-4:])
    if zlib.adler32(raw) & 0xffffffff != adler:
        raise PngError("adler", "Adler-32 mismatch")

    row_bytes = (width + 7) // 8
    if len(raw) != height * (row_bytes + 1):
        raise PngError("size", f"{len(raw)} bytes of image data, expected "
                               f"{height * (row_bytes + 1)}")
    rows = []
    for r in range(height):
        start = r * (row_bytes + 1)
        if raw[start] != 0:
            raise PngError("filter", f"row {r} has filter type {raw[start]}")
        line = raw[start + 1:start + 1 + row_bytes]
        rows.append([0 if (line[x >> 3] >> (7 - (x & 7))) & 1 else 1 for x in range(width)])
    return width, height, ppm, rows


def read_pbm(path):
    """Reads a plain (P1) PBM. Returns (width, height, rows), 1 for ink."""
    tokens = []
    with open(path, "r", encoding="ascii") as f:
        for line in f:
            line = line.split("#", 1)[0]
            tokens.extend(line.split())
    if not tokens or tokens[0] != "P1":
        raise ValueError(f"{path}: not a P1 PBM")
    width, height = int(tokens[1]), int(tokens[2])
    bits = "".join(tokens[3:])
    if len(bits) != width * height:
        raise ValueError(f"{path}: {len(bits)} pixels, expected {width * height}")
    rows = [[int(bits[r * width + x]) for x in range(width)] for r in range(height)]
    return width, height, rows


def check(data, expected=None, ppm=None):
    """Checks a PNG's bytes against an expected (width, height, rows) and pHYs. Raises PngError."""
    width, height, got_ppm, rows = decode(data)
    if ppm is not None and got_ppm != ppm:
        raise PngError("phys", f"pHYs {got_ppm}, expected {ppm}")
    if expected is not None:
        ew, eh, erows = expected
        if (width, height) != (ew, eh):
            raise PngError("pixels", f"size {width} x {height}, expected {ew} x {eh}")
        for r in range(height):
            if rows[r] != erows[r]:
                x = next(i for i in range(width) if rows[r][i] != erows[r][i])
                raise PngError("pixels", f"first difference at row {r} column {x}")
    return width, height


def check_file(png, pbm=None, ppm=None):
    """Checks one file and prints the outcome. Returns True if it passed."""
    try:
        with open(png, "rb") as f:
            data = f.read()
        expected = read_pbm(pbm) if pbm else None
        w, h = check(data, expected, ppm)
    except (OSError, ValueError, PngError) as e:
        print(f"FAIL {png}: {e}")
        return False
    print(f"PASS {png} ({w} x {h})")
    return True


def find_chunk(data, ctype):
    """Returns the offset of the first chunk of type ctype (its length field)."""
    pos = 8
    while pos < len(data):
        length, = struct.unpack(">I", data[pos:pos + 4])
        if data[pos + 4:pos + 8] == ctype:
            return pos, length
        pos += 12 + length
    raise ValueError(f"no {ctype!r} chunk")


def repair_crc(data, pos, length):
    """Returns data with the CRC of the chunk at pos recomputed."""
    body_end = pos + 8 + length
    crc = zlib.crc32(data[pos + 4:body_end]) & 0xffffffff
    return data[:body_end] + struct.pack(">I", crc) + data[body_end + 4:]


def selftest(png, pbm, ppm):
    """Requires the good file to pass and each control to fail with the right kind. Returns True
    if every leg came out as it should."""
    with open(png, "rb") as f:
        good = f.read()
    expected = read_pbm(pbm)
    ok = True

    def leg(name, data, exp, want_ppm, want_kind):
        nonlocal ok
        try:
            check(data, exp, want_ppm)
            kind = None
        except PngError as e:
            kind = e.kind
        passed = kind == want_kind
        ok = ok and passed
        outcome = "passed" if kind is None else f"failed ({kind})"
        print(f"{'PASS' if passed else 'FAIL'} selftest {name}: {outcome}, "
              f"wanted {'pass' if want_kind is None else want_kind}")

    leg("good file", good, expected, ppm, None)

    # A pixel flipped in the expectation: the picture no longer matches.
    w, h, rows = expected
    flipped = [list(r) for r in rows]
    flipped[h // 2][w // 2] ^= 1
    leg("one pixel flipped", good, (w, h, flipped), ppm, "pixels")

    # A byte in the middle of IDAT's data changed: its CRC fails first.
    pos, length = find_chunk(good, b"IDAT")
    mid = pos + 8 + length // 2
    bad = good[:mid] + bytes([good[mid] ^ 0x01]) + good[mid + 1:]
    leg("IDAT byte changed", bad, expected, ppm, "crc")

    # The same change with the CRC repaired: Adler-32 must catch it. The byte is the last data
    # byte of the stream (just before the Adler-32), always image data in a stored block.
    last = pos + 8 + length - 5
    bad = good[:last] + bytes([good[last] ^ 0x01]) + good[last + 1:]
    bad = repair_crc(bad, pos, length)
    leg("IDAT byte changed, CRC repaired", bad, expected, ppm, "adler")

    # The right file read against the wrong density.
    leg("wrong pHYs expected", good, expected, ppm + 1, "phys")
    return ok


def main(argv):
    if len(argv) >= 2 and argv[1] == "--manifest":
        base = os.path.dirname(argv[2])
        ok = True
        count = 0
        with open(argv[2], "r", encoding="ascii") as f:
            for line in f:
                parts = line.split()
                if not parts:
                    continue
                png, pbm, ppm = parts[:3]
                want = parts[3] if len(parts) > 3 else "pass"
                if want == "fail":
                    # A control: the file must not match this picture.
                    print("control, must fail: ", end="")
                    passed = not check_file(os.path.join(base, png), os.path.join(base, pbm), int(ppm))
                    print(f"{'PASS' if passed else 'FAIL'} control {png} against {pbm}")
                else:
                    passed = check_file(os.path.join(base, png), os.path.join(base, pbm), int(ppm))
                ok = passed and ok
                count += 1
        if count == 0:
            print(f"FAIL {argv[2]}: no files listed")
            ok = False
        return 0 if ok else 1
    if len(argv) == 5 and argv[1] == "--selftest":
        return 0 if selftest(argv[2], argv[3], int(argv[4])) else 1
    args = argv[1:]
    ppm = None
    if "--ppm" in args:
        i = args.index("--ppm")
        ppm = int(args[i + 1])
        del args[i:i + 2]
    if len(args) not in (1, 2):
        print(__doc__)
        return 2
    return 0 if check_file(args[0], args[1] if len(args) == 2 else None, ppm) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
