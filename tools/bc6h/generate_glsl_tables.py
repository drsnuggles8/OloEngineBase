#!/usr/bin/env python3
"""Emit BC6HEncodeCommon.glsl's constant tables from BC6HEncoder.cpp.

The GPU encoder is a port of the CPU one, so its tables are derived from the C++ — which
is itself derived from bcdec by generate_cpp_mode_tables.py in this directory. Three hand
transcriptions of the same fourteen block layouts would have been three chances to differ;
this way there is one source of truth and two mechanical derivations of it.

Usage, from the repository root:
    python tools/bc6h/generate_glsl_tables.py

Paste the output over the corresponding `const uint` tables in
OloEditor/assets/shaders/include/BC6HEncodeCommon.glsl. BC6HGpuEncoderTest is what proves
the result still agrees with the CPU encoder afterwards.

See docs/agent-rules/multi-mode-block-format-encoders.md §4.
"""
import re

SRC = "OloEngine/src/OloEngine/Renderer/BC6HEncoder.cpp"
s = open(SRC, encoding="utf-8").read()

# ---- partitions -------------------------------------------------------------
pstart = s.index("constexpr std::array<std::array<u8, 16>, 32> kPartitions")
pend = s.index("} };", pstart)
rows = re.findall(r"\{ ((?:0x[0-9A-F]{2},?\s*){16})\}", s[pstart:pend])
assert len(rows) == 32, len(rows)
subsets, fixups = [], []
for r in rows:
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-F]{2})", r)]
    assert len(vals) == 16
    bits = 0
    fix = 0
    for t, v in enumerate(vals):
        if v & 0x01:
            bits |= (1 << t)
        if (v & 0x80) and t != 0:
            fix = t
    subsets.append(bits)
    fixups.append(fix)

# ---- modes ------------------------------------------------------------------
mstart = s.index("constexpr std::array<ModeSpec, kModeCount> kModes")
mend = s.index("} };", mstart)
body = s[mstart:mend]

# Each mode entry starts "{ <modeBits>, 0b<value>, <baseBits>, { d,d,d }, <hasDelta>, <subsets>, <fieldCount>,"
heads = re.findall(
    r"\{\s*(\d+),\s*(0b[01]+),\s*(\d+),\s*\{\s*(\d+),\s*(\d+),\s*(\d+)\s*\},\s*(\d+),\s*(\d+),\s*(\d+),",
    body)
assert len(heads) == 14, len(heads)

# Field lists: everything inside the "{ { ... } } }," that follows each head.
field_blocks = re.findall(r"\{\s*\{\s*((?:\{\s*\w+,\s*\d+,\s*\d+,\s*\d+\s*\},?\s*)+)\}\s*\}\s*\}", body)
assert len(field_blocks) == 14, len(field_blocks)

mode_words = []
field_words = []
offsets = []
for i, (mb, mv, bb, dr, dg, db, hd, sub, fc) in enumerate(heads):
    fields = re.findall(r"\{\s*(\w+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}", field_blocks[i])
    assert len(fields) == int(fc), (i, len(fields), fc)
    offsets.append(len(field_words))
    for slot, shift, bits, rev in fields:
        slotv = 12 if slot == "kPartitionSlot" else int(slot)
        field_words.append(slotv | (int(shift) << 4) | (int(bits) << 10) | (int(rev) << 16))
    # mode word: modeBits(3) | modeValue(5)<<3 | baseBits(5)<<8 | dR(4)<<13 | dG(4)<<17
    #            | dB(4)<<21 | hasDelta(1)<<25 | subsets(2)<<26.
    # fieldCount is NOT packed here: it would need bits 28-32 and silently truncate.
    # kFieldOffsets carries a trailing total instead, so count = offsets[m+1]-offsets[m].
    w = (int(mb) | (int(mv, 2) << 3) | (int(bb) << 8) | (int(dr) << 13) | (int(dg) << 17)
         | (int(db) << 21) | (int(hd) << 25) | (int(sub) << 26))
    assert w < (1 << 32), w
    mode_words.append(w)


def emit(name, values, per_line=8, fmt="0x{:08X}u"):
    out = ["const uint " + name + "[" + str(len(values)) + "] = uint[](",]
    line = "   "
    for i, v in enumerate(values):
        tok = " " + fmt.format(v) + ("," if i < len(values) - 1 else "")
        if len(line) + len(tok) > 108:
            out.append(line)
            line = "   "
        line += tok
    out.append(line)
    out.append(");")
    return "\n".join(out)


print(emit("kPartitionSubsets", subsets, fmt="0x{:04X}u"))
print()
print(emit("kPartitionFixup", fixups, fmt="{}u"))
print()
print(emit("kModeWords", mode_words))
print()
print(emit("kFieldOffsets", offsets + [len(field_words)], fmt="{}u"))
print()
print(emit("kFieldWords", field_words, fmt="0x{:05X}u"))
print()
print("// field words total:", len(field_words))
