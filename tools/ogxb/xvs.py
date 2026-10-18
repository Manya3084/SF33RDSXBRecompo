"""Xbox vertex-program files (.xvu).

A file is a uint16 magic 0x2078, a uint16 instruction count, then that many
16-byte NV2A transform slots. Each slot is four little-endian dwords. Word 0
is stored and is zero in this title. Words 1..3 hold one dual-issue MAC op
and one ILU op.

Field positions, low bit first:

- ILU word1[27:25], MAC word1[24:21], constant word1[20:13], input word1[12:9]
- source A: negate word1[8], swizzle word1[7:0], register word2[31:28], mux word2[27:26]
- source B: negate word2[25], swizzle word2[24:17], register word2[16:13], mux word2[12:11]
- source C: negate word2[10], swizzle word2[9:2], register word2[1:0] then word3[31:30], mux word3[29:28]
- MAC mask word3[27:24], temp dest word3[23:20], ILU mask word3[19:16]
- output mask word3[15:12], output-register bit word3[11], output address word3[10:3]
- output-from-ILU word3[2], A0.x word3[1], FINAL word3[0]

A swizzle is four 2-bit selectors, x at the top. Mux 1 is a temporary (R12
reads as oPos), 2 is the input named by word1[12:9], 3 is the constant named
by word1[20:13], and anything else reads as zero. D3D constant c0 is slot 96.
When both opcodes are set, the ILU result is written to R1 and a MAC write
to R1 is dropped. ARL writes the address register, not the temporary.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

XVS_MAGIC = 0x2078
SLOT_BYTES = 16

MAC_OPS = (
    "NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4",
    "DST", "MIN", "MAX", "SLT", "SGE", "ARL", "?14", "?15",
)
ILU_OPS = ("NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT")
OUT_REGS = (
    "oPos", "o1", "o2", "oD0", "oD1", "oFog", "oPts", "oB0",
    "oB1", "oT0", "oT1", "oT2", "oT3", "o13", "o14", "o15",
)

_MUX_R = 1
_MUX_V = 2
_MUX_C = 3
_MAC_ARL = 13


def _field(word: int, lo: int, width: int) -> int:
    return (word >> lo) & ((1 << width) - 1)


def _swizzle(word: int, lo: int) -> tuple[int, int, int, int]:
    return (
        _field(word, lo + 6, 2),
        _field(word, lo + 4, 2),
        _field(word, lo + 2, 2),
        _field(word, lo, 2),
    )


@dataclass(frozen=True)
class Operand:
    neg: int
    swizzle: tuple[int, int, int, int]
    reg: int
    mux: int


@dataclass(frozen=True)
class Slot:
    word0: int
    ilu: int
    mac: int
    const_index: int
    input_index: int
    a: Operand
    b: Operand
    c: Operand
    mac_mask: int
    dest: int
    ilu_mask: int
    out_mask: int
    out_is_reg: int
    out_addr: int
    out_from_ilu: int
    a0x: int
    final: int

    @property
    def ilu_dest(self) -> int | None:
        if self.ilu == 0:
            return None
        if self.mac != 0:
            return 1
        return self.dest

    @property
    def mac_writes_dest(self) -> bool:
        if self.mac == 0 or self.mac == _MAC_ARL or self.mac_mask == 0:
            return False
        if self.ilu != 0 and self.dest == 1:
            return False
        return True


@dataclass(frozen=True)
class Xvu:
    path: Path
    count: int

    @property
    def size(self) -> int:
        return 4 + self.count * SLOT_BYTES


def parse_xvu(blob: bytes) -> int | None:
    if len(blob) < 4:
        return None
    magic, count = struct.unpack_from("<HH", blob, 0)
    if magic != XVS_MAGIC:
        return None
    if 4 + count * SLOT_BYTES != len(blob):
        return None
    return count


def decode_slot(raw: bytes) -> Slot:
    if len(raw) != SLOT_BYTES:
        raise ValueError(f"slot is {len(raw)} bytes")
    word0, word1, word2, word3 = struct.unpack("<4I", raw)
    return Slot(
        word0=word0,
        ilu=_field(word1, 25, 3),
        mac=_field(word1, 21, 4),
        const_index=_field(word1, 13, 8),
        input_index=_field(word1, 9, 4),
        a=Operand(
            _field(word1, 8, 1),
            _swizzle(word1, 0),
            _field(word2, 28, 4),
            _field(word2, 26, 2),
        ),
        b=Operand(
            _field(word2, 25, 1),
            _swizzle(word2, 17),
            _field(word2, 13, 4),
            _field(word2, 11, 2),
        ),
        c=Operand(
            _field(word2, 10, 1),
            _swizzle(word2, 2),
            (_field(word2, 0, 2) << 2) | _field(word3, 30, 2),
            _field(word3, 28, 2),
        ),
        mac_mask=_field(word3, 24, 4),
        dest=_field(word3, 20, 4),
        ilu_mask=_field(word3, 16, 4),
        out_mask=_field(word3, 12, 4),
        out_is_reg=_field(word3, 11, 1),
        out_addr=_field(word3, 3, 8),
        out_from_ilu=_field(word3, 2, 1),
        a0x=_field(word3, 1, 1),
        final=_field(word3, 0, 1),
    )


def decode_program(blob: bytes) -> list[Slot] | None:
    count = parse_xvu(blob)
    if count is None:
        return None
    return [decode_slot(blob[4 + index * SLOT_BYTES : 4 + (index + 1) * SLOT_BYTES]) for index in range(count)]


def _mask(mask: int) -> str:
    text = ""
    if mask & 8:
        text += "x"
    if mask & 4:
        text += "y"
    if mask & 2:
        text += "z"
    if mask & 1:
        text += "w"
    return text


def _with_mask(name: str, mask: int) -> str:
    text = _mask(mask)
    if not text:
        return name
    return f"{name}.{text}"


def _swizzle_text(selectors: tuple[int, int, int, int]) -> str:
    return "".join("xyzw"[item] for item in selectors)


def _source(slot: Slot, operand: Operand) -> str:
    if operand.mux == _MUX_R:
        base = "oPos" if operand.reg == 12 else f"R{operand.reg}"
    elif operand.mux == _MUX_V:
        base = f"v{slot.input_index}"
    elif operand.mux == _MUX_C:
        if slot.a0x:
            base = f"c[{slot.const_index}+A0.x]"
        else:
            base = f"c[{slot.const_index}]"
    else:
        base = "0"
    sign = "-" if operand.neg else ""
    return f"{sign}{base}.{_swizzle_text(operand.swizzle)}"


def _mac_operands(slot: Slot) -> list[Operand]:
    if slot.mac in (1, _MAC_ARL):
        return [slot.a]
    if slot.mac in (2, 5, 6, 7, 8, 9, 10, 11, 12):
        return [slot.a, slot.b]
    if slot.mac == 3:
        return [slot.a, slot.c]
    if slot.mac == 4:
        return [slot.a, slot.b, slot.c]
    if slot.mac == 0:
        return []
    return [slot.a, slot.b, slot.c]


def format_slot(index: int, slot: Slot) -> str:
    text = f"{index:3d}: {MAC_OPS[slot.mac]:<4}"
    if slot.mac_writes_dest:
        text += " " + _with_mask(f"R{slot.dest}", slot.mac_mask)
    if slot.ilu:
        text += f" | {ILU_OPS[slot.ilu]:<4}"
        if slot.ilu_mask:
            text += " " + _with_mask(f"R{slot.ilu_dest}", slot.ilu_mask)
    if slot.out_mask:
        if slot.out_is_reg:
            name = OUT_REGS[slot.out_addr & 15]
        elif slot.a0x:
            name = f"c[{slot.out_addr}+A0.x]"
        else:
            name = f"c[{slot.out_addr}]"
        which = "ILU" if slot.out_from_ilu else "MAC"
        text += " | " + _with_mask(name, slot.out_mask) + f" from {which}"
    operands = _mac_operands(slot)
    if slot.ilu and slot.c not in operands:
        operands.append(slot.c)
    if operands:
        text += " " + ", ".join(_source(slot, item) for item in operands)
    if slot.a0x:
        text += " A0X"
    if slot.final:
        text += " FINAL"
    if slot.word0:
        text += f" word0={slot.word0:#x}"
    return text


def list_shaders(directory: Path) -> tuple[list[Xvu], list[Path]]:
    programs: list[Xvu] = []
    other: list[Path] = []
    for path in sorted(directory.iterdir()):
        if not path.is_file():
            continue
        count = parse_xvu(path.read_bytes())
        if count is None:
            other.append(path)
        else:
            programs.append(Xvu(path, count))
    return programs, other
