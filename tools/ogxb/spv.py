"""SPIR-V 1.0 words.

Opcode numbers are the public SPIR-V header. The GLSL.std.450 numbers used
here were checked against a module from glslang: Trunc 3, FAbs 4, FSign 6,
Floor 8, Fract 10, Pow 26, Exp2 29, Log2 30, InverseSqrt 32, FMin 37,
FMax 40, FClamp 43.
"""

from __future__ import annotations

import struct

MAGIC = 0x07230203
VERSION = 0x00010000

OP_NAME = 5
OP_EXT_IMPORT = 11
OP_EXT_INST = 12
OP_MEMORY_MODEL = 14
OP_ENTRY = 15
OP_EXEC_MODE = 16
OP_CAPABILITY = 17
OP_TYPE_VOID = 19
OP_TYPE_BOOL = 20
OP_TYPE_INT = 21
OP_TYPE_FLOAT = 22
OP_TYPE_VECTOR = 23
OP_TYPE_IMAGE = 25
OP_TYPE_SAMPLED = 27
OP_TYPE_ARRAY = 28
OP_TYPE_STRUCT = 30
OP_TYPE_POINTER = 32
OP_TYPE_FUNCTION = 33
OP_CONSTANT = 43
OP_CONSTANT_COMPOSITE = 44
OP_FUNCTION = 54
OP_FUNCTION_END = 56
OP_VARIABLE = 59
OP_LOAD = 61
OP_STORE = 62
OP_ACCESS = 65
OP_DECORATE = 71
OP_MEMBER_DECORATE = 72
OP_SHUFFLE = 79
OP_COMPOSITE = 80
OP_EXTRACT = 81
OP_SAMPLE = 87
OP_F_TO_S = 110
OP_BITCAST = 124
OP_FNEG = 127
OP_IADD = 128
OP_FADD = 129
OP_FSUB = 131
OP_FMUL = 133
OP_FDIV = 136
OP_DOT = 148
OP_LOGICAL_NOT = 168
OP_SELECT = 169
OP_IEQ = 170
OP_INE = 171
OP_SGT = 173
OP_SLT = 177
OP_F_ORD_EQ = 180
OP_F_ORD_LT = 184
OP_F_ORD_GT = 186
OP_F_ORD_LE = 188
OP_F_ORD_GE = 190
OP_SHR = 194
OP_AND = 199
OP_SELECTION_MERGE = 247
OP_LABEL = 248
OP_BRANCH = 249
OP_BRANCH_COND = 250
OP_KILL = 252
OP_RETURN = 253

CAP_SHADER = 1
MODEL_VERTEX = 0
MODEL_FRAGMENT = 4
ADDR_LOGICAL = 0
MEM_GLSL450 = 1
EXEC_ORIGIN_UPPER_LEFT = 7

CLASS_UNIFORM_CONSTANT = 0
CLASS_INPUT = 1
CLASS_UNIFORM = 2
CLASS_OUTPUT = 3
CLASS_FUNCTION = 7

DIM_2D = 1

DECO_BLOCK = 2
DECO_ARRAY_STRIDE = 6
DECO_BUILTIN = 11
DECO_LOCATION = 30
DECO_BINDING = 33
DECO_SET = 34
DECO_OFFSET = 35

BUILTIN_POSITION = 0
BUILTIN_POINT_SIZE = 1

GLSL_TRUNC = 3
GLSL_FABS = 4
GLSL_FLOOR = 8
GLSL_FRACT = 10
GLSL_POW = 26
GLSL_EXP2 = 29
GLSL_LOG2 = 30
GLSL_INVSQRT = 32
GLSL_FMIN = 37
GLSL_FMAX = 40
GLSL_FCLAMP = 43

_SECTIONS = ("cap", "ext", "mem", "entry", "mode", "debug", "ann", "types", "globals", "code")


def literal(text: str) -> tuple[int, ...]:
    raw = text.encode("utf-8") + b"\x00"
    raw += b"\x00" * ((-len(raw)) % 4)
    return struct.unpack("<" + "I" * (len(raw) // 4), raw)


class Module:
    """Collects instructions and serializes them in SPIR-V order.

    Types and constants share one section. Global variables follow them, so a
    constant created while the function is being built still sits before the
    variables and the function.
    """

    def __init__(self) -> None:
        self._next = 1
        self._sections: dict[str, list[tuple[int, tuple[int, ...]]]] = {name: [] for name in _SECTIONS}

    def id(self) -> int:
        result = self._next
        self._next += 1
        return result

    def instr(self, section: str, opcode: int, *operands: int) -> None:
        self._sections[section].append((opcode, operands))

    def result(self, section: str, opcode: int, type_id: int | None, operands: tuple[int, ...]) -> int:
        result = self.id()
        if type_id is None:
            self.instr(section, opcode, result, *operands)
        else:
            self.instr(section, opcode, type_id, result, *operands)
        return result

    def to_bytes(self) -> bytes:
        body = bytearray()
        for name in _SECTIONS:
            for opcode, operands in self._sections[name]:
                count = 1 + len(operands)
                words = [((count << 16) | opcode), *operands]
                body += struct.pack("<" + "I" * len(words), *words)
        header = struct.pack("<5I", MAGIC, VERSION, 0, self._next, 0)
        return header + body
