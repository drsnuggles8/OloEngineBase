#!/usr/bin/env python3
"""Emit BC6HEncoder.cpp's per-mode field tables from bcdec's decoder.

BC6H is fourteen different block layouts, and each one's fields are scattered through the
bitstream in an order with no pattern to it — mode 0 writes gy[4], then by[4], then bz[4],
then rw[9:0], for twenty fields. Hand-transcribing fourteen of those is a guaranteed
defect, and a misplaced field corrupts only the blocks that pick that mode, which no
aggregate PSNR test notices.

So the tables are DERIVED from the reference decoder instead: this script parses
`bcdec_bc6h_half()`'s per-mode read sequence and prints it as the `kModes` field tables in
`OloEngine/src/OloEngine/Renderer/BC6HEncoder.cpp`, where packing is that sequence run
backwards. `BC6HModeCoverageTest` then proves the inverse per mode against bcdec itself.

Usage:
    python tools/bc6h/generate_cpp_mode_tables.py [path/to/bcdec.h]

bcdec is fetched by CPM, so its header lives under the build tree, e.g.
    build-cached/_deps/bcdec-src/bcdec.h
Paste the output into the `kModes` initialiser; the surrounding ModeSpec fields
(precisions, subset counts) come from bcdec's `actual_bits_count` table and are listed in
the summary at the end.

See docs/agent-rules/multi-mode-block-format-encoders.md for why it works this way.
"""
import re
import sys

DEFAULT_BCDEC = "build-cached/_deps/bcdec-src/bcdec.h"
CHANNEL_INDEX = {"r": 0, "g": 1, "b": 2}
PARTITION_SLOT = 12


def parse_modes(source: str):
    """-> [(mode index, pattern value, pattern bit count, [(slot, shift, bits, reversed)])]"""
    # rindex: the two names also appear as forward declarations near the top.
    start = source.rindex("BCDECDEF void bcdec_bc6h_half")
    end = source.rindex("BCDECDEF void bcdec_bc6h_float")
    body = source[start:end]

    cases = re.split(r"case (0b[01]+):", body)
    modes = []
    for i in range(1, len(cases), 2):
        pattern, block = cases[i], cases[i + 1]
        matched = re.search(r"mode = (\d+);", block)
        if not matched:
            continue
        fields = []
        for line in block.splitlines():
            line = line.strip()
            bits = re.match(
                r"(r|g|b)\[(\d)\] \|= bcdec__bitstream_read_bits(_r)?\(&bstream, (\d+)\)(?: << (\d+))?;", line)
            if bits:
                fields.append((int(bits.group(2)) * 3 + CHANNEL_INDEX[bits.group(1)],
                               int(bits.group(5) or 0), int(bits.group(4)), 1 if bits.group(3) else 0))
                continue
            bit = re.match(r"(r|g|b)\[(\d)\] \|= bcdec__bitstream_read_bit\(&bstream\)(?: << (\d+))?;", line)
            if bit:
                fields.append((int(bit.group(2)) * 3 + CHANNEL_INDEX[bit.group(1)],
                               int(bit.group(3) or 0), 1, 0))
                continue
            part = re.match(r"partition = bcdec__bitstream_read_bits\(&bstream, (\d+)\);", line)
            if part:
                fields.append((PARTITION_SLOT, 0, int(part.group(1)), 0))
        modes.append((int(matched.group(1)), int(pattern, 2), len(pattern) - 2, fields))
    modes.sort()
    return modes


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BCDEC
    try:
        source = open(path, encoding="utf-8").read()
    except OSError as error:
        print(f"cannot read bcdec header at {path}: {error}", file=sys.stderr)
        print(__doc__, file=sys.stderr)
        return 1

    modes = parse_modes(source)
    if len(modes) != 14:
        print(f"expected 14 modes, parsed {len(modes)} — did bcdec's decoder change shape?", file=sys.stderr)
        return 1

    for index, pattern, pattern_bits, fields in modes:
        total = sum(f[2] for f in fields)
        index_bits = 63 if index >= 10 else 46
        print(f"// mode {index}: pattern 0b{pattern:0{pattern_bits}b} ({pattern_bits} bits), "
              f"{len(fields)} fields, {total} + {pattern_bits} + {index_bits} = {total + pattern_bits + index_bits} bits")
        items = [f"{{ {slot}, {shift}, {bits}, {rev} }}" for slot, shift, bits, rev in fields]
        line = "  "
        for position, item in enumerate(items):
            piece = " " + item + ("," if position < len(items) - 1 else " },")
            if len(line) + len(piece) > 108:
                print(line)
                line = "  "
            line += piece
        print(line)
        print()

    print("// mode | pattern | pattern bits | field count  (cross-check against")
    print("// bcdec's actual_bits_count[][] for the endpoint precisions)")
    for index, pattern, pattern_bits, fields in modes:
        print(f"//  {index:2d}   0b{pattern:0{pattern_bits}b}   {pattern_bits}   {len(fields)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
