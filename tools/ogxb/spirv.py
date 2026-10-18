"""SPIR-V 1.0 for an NV2A vertex program or register-combiner program.

Vertex modules run the decoded slots, then snap screen x and y to 1/16 of a
pixel and convert to clip space. Y is not flipped. RCC and the position w
are held in [2^-64, 2^64]. gl_Position.z is 0.5*w. Combiner modules run the
general stages and the final stage. A final stage whose inputs are zero
writes black. Cube, bump, BRDF, and dot-product texture modes leave that
stage's texture at zero. 3D_PROJECTIVE is sampled as 2D.

MUL, RCP, and LOG are ordinary floating-point operations. The reciprocal
used by RCC is clamped after a floating-point divide, which is what keeps a
zero or an infinity inside the two powers of two.

One descriptor set, so Vulkan uses a single pipeline layout and desktop
OpenGL (GL_ARB_gl_spirv / OpenGL 4.6) can link the two stages. OpenGL
ignores the set number and matches uniform blocks by binding, so the
blocks must not share a binding. Present only when the module reads it:

- set 0 binding 0: vertex, 192 vec4 constants
- set 0 binding 1: vertex, vec4 viewport, xy in pixels
- set 0 binding 2: fragment, 16 vec4 pixel constants, then one vec4 fog color
- set 0 binding 3..6: fragment, sampler2D for texture stages 0..3

OpenGL draws these modules with an upper-left origin and zero-to-one depth,
the same clip space Vulkan uses for this gl_Position.

Vertex outputs and combiner inputs share locations. 0 and 1 are the front
colors, 2 and 3 are the back colors, 4..7 are texture coordinates, 8 is fog,
and 9 is window z as (z, 16777215-z) times w. Point size is clamp(oPts.x, 1, 64).
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from ogxb.spv import (
    ADDR_LOGICAL,
    BUILTIN_POINT_SIZE,
    BUILTIN_POSITION,
    CAP_SHADER,
    CLASS_FUNCTION,
    CLASS_INPUT,
    CLASS_OUTPUT,
    CLASS_UNIFORM,
    CLASS_UNIFORM_CONSTANT,
    DECO_ARRAY_STRIDE,
    DECO_BINDING,
    DECO_BLOCK,
    DECO_BUILTIN,
    DECO_LOCATION,
    DECO_OFFSET,
    DECO_SET,
    DIM_2D,
    EXEC_ORIGIN_UPPER_LEFT,
    GLSL_EXP2,
    GLSL_FABS,
    GLSL_FCLAMP,
    GLSL_FLOOR,
    GLSL_FMAX,
    GLSL_FMIN,
    GLSL_FRACT,
    GLSL_INVSQRT,
    GLSL_LOG2,
    GLSL_POW,
    GLSL_TRUNC,
    MEM_GLSL450,
    MODEL_FRAGMENT,
    MODEL_VERTEX,
    OP_ACCESS,
    OP_AND,
    OP_BITCAST,
    OP_BRANCH_COND,
    OP_CAPABILITY,
    OP_COMPOSITE,
    OP_CONSTANT,
    OP_CONSTANT_COMPOSITE,
    OP_DECORATE,
    OP_DOT,
    OP_ENTRY,
    OP_EXEC_MODE,
    OP_EXT_IMPORT,
    OP_EXT_INST,
    OP_EXTRACT,
    OP_F_ORD_GE,
    OP_F_ORD_GT,
    OP_F_ORD_LT,
    OP_F_TO_S,
    OP_FADD,
    OP_FDIV,
    OP_FMUL,
    OP_FNEG,
    OP_FSUB,
    OP_FUNCTION,
    OP_FUNCTION_END,
    OP_IADD,
    OP_INE,
    OP_KILL,
    OP_LABEL,
    OP_LOAD,
    OP_MEMBER_DECORATE,
    OP_MEMORY_MODEL,
    OP_RETURN,
    OP_SAMPLE,
    OP_SELECT,
    OP_SELECTION_MERGE,
    OP_SGT,
    OP_SHR,
    OP_SHUFFLE,
    OP_SLT,
    OP_STORE,
    OP_TYPE_ARRAY,
    OP_TYPE_BOOL,
    OP_TYPE_FLOAT,
    OP_TYPE_FUNCTION,
    OP_TYPE_IMAGE,
    OP_TYPE_INT,
    OP_TYPE_POINTER,
    OP_TYPE_SAMPLED,
    OP_TYPE_STRUCT,
    OP_TYPE_VECTOR,
    OP_TYPE_VOID,
    OP_VARIABLE,
    literal,
    Module,
)
from ogxb.xpu import PixelShader, parse_xpu
from ogxb.xvs import Operand, Slot, decode_program

_CONSTS = 192
_W_MIN = 0x1F800000
_W_MAX = 0x5F800000
_FLT_MAX = 0x7F7FFFFF
_OUT_ALPHA1 = {3, 4, 7, 8, 9, 10, 11, 12}
_WRITABLE = {4: "v0", 5: "v1", 8: "t0", 9: "t1", 10: "t2", 11: "t3", 12: "r0", 13: "r1"}
_SAMPLED = {1, 2}
_PASS = 4
_CLIP = 5


def _fbits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


class Builder:
    def __init__(self) -> None:
        self.mod = Module()
        self.mod.instr("cap", OP_CAPABILITY, CAP_SHADER)
        self.glsl = self.mod.result("ext", OP_EXT_IMPORT, None, literal("GLSL.std.450"))
        self.mod.instr("mem", OP_MEMORY_MODEL, ADDR_LOGICAL, MEM_GLSL450)
        self.t_void = self.mod.result("types", OP_TYPE_VOID, None, ())
        self.t_bool = self.mod.result("types", OP_TYPE_BOOL, None, ())
        self.t_u32 = self.mod.result("types", OP_TYPE_INT, None, (32, 0))
        self.t_s32 = self.mod.result("types", OP_TYPE_INT, None, (32, 1))
        self.t_f32 = self.mod.result("types", OP_TYPE_FLOAT, None, (32,))
        self.t_v2 = self._vector(self.t_f32, 2)
        self.t_v3 = self._vector(self.t_f32, 3)
        self.t_v4 = self._vector(self.t_f32, 4)
        self.t_b4 = self._vector(self.t_bool, 4)
        self.fn_type = self.mod.result("types", OP_TYPE_FUNCTION, None, (self.t_void,))
        self.fn = self.mod.id()
        self._types: dict[tuple, int] = {}
        self._const: dict[tuple, int] = {}
        self.f0 = self.fconst(0.0)
        self.f1 = self.fconst(1.0)
        self.fn1 = self.fconst(-1.0)
        self.f_half = self.fconst(0.5)
        self.f2 = self.fconst(2.0)
        self.s0 = self.sconst(0)
        self.s1 = self.sconst(1)
        self.s191 = self.sconst(191)
        self.u0 = self.uconst(0)
        self.u31 = self.uconst(31)
        self.v4_0 = self.repeat(self.t_v4, self.f0, 4)
        self.v4_1 = self.repeat(self.t_v4, self.f1, 4)
        self.v3_0 = self.repeat(self.t_v3, self.f0, 3)
        self.v3_1 = self.repeat(self.t_v3, self.f1, 3)
        self.v3_n1 = self.repeat(self.t_v3, self.fn1, 3)
        self.v3_half = self.repeat(self.t_v3, self.f_half, 3)
        self.v3_2 = self.repeat(self.t_v3, self.f2, 3)

    def _vector(self, component: int, count: int) -> int:
        return self.mod.result("types", OP_TYPE_VECTOR, None, (component, count))

    def ptr(self, storage: int, pointee: int) -> int:
        key = ("ptr", storage, pointee)
        if key not in self._types:
            self._types[key] = self.mod.result("types", OP_TYPE_POINTER, None, (storage, pointee))
        return self._types[key]

    def uconst(self, value: int) -> int:
        return self._word(self.t_u32, value & 0xFFFFFFFF)

    def sconst(self, value: int) -> int:
        return self._word(self.t_s32, value & 0xFFFFFFFF)

    def fconst(self, value: float) -> int:
        return self._word(self.t_f32, _fbits(value))

    def fbits(self, bits: int) -> int:
        return self._word(self.t_f32, bits & 0xFFFFFFFF)

    def _word(self, type_id: int, bits: int) -> int:
        key = ("c", type_id, bits)
        if key not in self._const:
            self._const[key] = self.mod.result("types", OP_CONSTANT, type_id, (bits,))
        return self._const[key]

    def composite(self, type_id: int, *parts: int) -> int:
        key = ("comp", type_id, parts)
        if key not in self._const:
            self._const[key] = self.mod.result("types", OP_CONSTANT_COMPOSITE, type_id, parts)
        return self._const[key]

    def repeat(self, type_id: int, scalar: int, count: int) -> int:
        return self.composite(type_id, *([scalar] * count))

    def decorate(self, target: int, decoration: int, *values: int) -> None:
        self.mod.instr("ann", OP_DECORATE, target, decoration, *values)

    def member(self, struct: int, index: int, decoration: int, *values: int) -> None:
        self.mod.instr("ann", OP_MEMBER_DECORATE, struct, index, decoration, *values)

    def global_var(self, pointee: int, storage: int) -> int:
        return self.mod.result("globals", OP_VARIABLE, self.ptr(storage, pointee), (storage,))

    def func_var(self, pointee: int) -> int:
        return self.mod.result("code", OP_VARIABLE, self.ptr(CLASS_FUNCTION, pointee), (CLASS_FUNCTION,))

    def block(self, member_types: tuple[int, ...], strides: tuple[int | None, ...]) -> int:
        struct = self.mod.result("types", OP_TYPE_STRUCT, None, member_types)
        self.decorate(struct, DECO_BLOCK)
        offset = 0
        for index, (type_id, stride) in enumerate(zip(member_types, strides)):
            self.member(struct, index, DECO_OFFSET, offset)
            offset += 16 if stride is None else stride
            del type_id
        return struct

    def array(self, element: int, count: int, stride: int) -> int:
        length = self.uconst(count)
        array = self.mod.result("types", OP_TYPE_ARRAY, None, (element, length))
        self.decorate(array, DECO_ARRAY_STRIDE, stride)
        return array

    def bind(self, var: int, set_id: int, binding: int) -> None:
        self.decorate(var, DECO_SET, set_id)
        self.decorate(var, DECO_BINDING, binding)

    def ubo(self, member_types: tuple[int, ...], strides: tuple[int | None, ...], binding: int) -> int:
        var = self.global_var(self.block(member_types, strides), CLASS_UNIFORM)
        self.bind(var, 0, binding)
        return var

    def entry(self, model: int, interface: list[int], fragment: bool) -> None:
        self.mod.instr("entry", OP_ENTRY, model, self.fn, *literal("main"), *interface)
        if fragment:
            self.mod.instr("mode", OP_EXEC_MODE, self.fn, EXEC_ORIGIN_UPPER_LEFT)

    def begin(self) -> None:
        self.mod.instr("code", OP_FUNCTION, self.t_void, self.fn, 0, self.fn_type)
        self.mod.instr("code", OP_LABEL, self.mod.id())

    def end(self) -> None:
        self.mod.instr("code", OP_RETURN)
        self.mod.instr("code", OP_FUNCTION_END)

    def load(self, type_id: int, ptr: int) -> int:
        return self.mod.result("code", OP_LOAD, type_id, (ptr,))

    def store(self, ptr: int, value: int) -> None:
        self.mod.instr("code", OP_STORE, ptr, value)

    def access(self, storage: int, pointee: int, base: int, *indices: int) -> int:
        return self.mod.result("code", OP_ACCESS, self.ptr(storage, pointee), (base, *indices))

    def bin(self, opcode: int, type_id: int, left: int, right: int) -> int:
        return self.mod.result("code", opcode, type_id, (left, right))

    def fadd(self, type_id: int, left: int, right: int) -> int:
        return self.bin(OP_FADD, type_id, left, right)

    def fsub(self, type_id: int, left: int, right: int) -> int:
        return self.bin(OP_FSUB, type_id, left, right)

    def fmul(self, type_id: int, left: int, right: int) -> int:
        return self.bin(OP_FMUL, type_id, left, right)

    def fdiv(self, type_id: int, left: int, right: int) -> int:
        return self.bin(OP_FDIV, type_id, left, right)

    def fneg(self, type_id: int, value: int) -> int:
        return self.mod.result("code", OP_FNEG, type_id, (value,))

    def extract(self, type_id: int, value: int, index: int) -> int:
        return self.mod.result("code", OP_EXTRACT, type_id, (value, index))

    def construct(self, type_id: int, *parts: int) -> int:
        return self.mod.result("code", OP_COMPOSITE, type_id, parts)

    def shuffle(self, type_id: int, left: int, right: int, selectors: tuple[int, ...]) -> int:
        return self.mod.result("code", OP_SHUFFLE, type_id, (left, right, *selectors))

    def select(self, type_id: int, cond: int, yes: int, no: int) -> int:
        return self.mod.result("code", OP_SELECT, type_id, (cond, yes, no))

    def ext(self, type_id: int, opcode: int, *args: int) -> int:
        return self.mod.result("code", OP_EXT_INST, type_id, (self.glsl, opcode, *args))

    def dot(self, left: int, right: int) -> int:
        return self.mod.result("code", OP_DOT, self.t_f32, (left, right))

    def discard_if(self, cond: int) -> None:
        kill = self.mod.id()
        merge = self.mod.id()
        self.mod.instr("code", OP_SELECTION_MERGE, merge, 0)
        self.mod.instr("code", OP_BRANCH_COND, cond, kill, merge)
        self.mod.instr("code", OP_LABEL, kill)
        self.mod.instr("code", OP_KILL)
        self.mod.instr("code", OP_LABEL, merge)

    def to_bytes(self) -> bytes:
        return self.mod.to_bytes()


def _mac_operands(slot: Slot) -> list[Operand]:
    if slot.mac in (1, 13):
        return [slot.a]
    if slot.mac in (2, 5, 6, 7, 8, 9, 10, 11, 12):
        return [slot.a, slot.b]
    if slot.mac == 3:
        return [slot.a, slot.c]
    if slot.mac == 4:
        return [slot.a, slot.b, slot.c]
    return []


def _uses_mac(slot: Slot) -> bool:
    if slot.mac == 13:
        return True
    if slot.mac == 0 or slot.mac >= 14:
        return False
    return slot.mac_writes_dest or bool(slot.out_mask and not slot.out_from_ilu)


def _uses_ilu(slot: Slot) -> bool:
    return bool(slot.ilu and (slot.ilu_mask or (slot.out_mask and slot.out_from_ilu)))


def _operands_used(slot: Slot) -> list[Operand]:
    used: list[Operand] = []
    if _uses_mac(slot):
        used.extend(_mac_operands(slot))
    if _uses_ilu(slot):
        used.append(slot.c)
    return used


def _writes_constant(slot: Slot) -> bool:
    return bool(slot.out_mask and not slot.out_is_reg)


def _active(slots: list[Slot]) -> list[Slot]:
    chosen: list[Slot] = []
    for slot in slots:
        chosen.append(slot)
        if slot.final:
            break
    return chosen


def _inputs_used(slots: list[Slot]) -> set[int]:
    used = {5}
    for slot in slots:
        for operand in _operands_used(slot):
            if operand.mux == 2:
                used.add(slot.input_index & 15)
    return used


def _needs_constants(slots: list[Slot]) -> bool:
    for slot in slots:
        if _writes_constant(slot):
            return True
        if any(operand.mux == 3 for operand in _operands_used(slot)):
            return True
    return False


class _Vertex:
    def __init__(self, slots: list[Slot]) -> None:
        self.slots = _active(slots)
        self.b = Builder()
        self.inputs: dict[int, int] = {}
        self.temps: list[int] = []
        self.outs: list[int] = []
        self.a0 = 0
        self.const_ubo = 0
        self.local_c = 0
        self.surf = 0
        self.exports: list[int] = []
        self.point_size = 0
        self.position = 0

    def emit(self) -> bytes:
        b = self.b
        interface: list[int] = []
        for index in sorted(_inputs_used(self.slots)):
            var = b.global_var(b.t_v4, CLASS_INPUT)
            b.decorate(var, DECO_LOCATION, index)
            self.inputs[index] = var
            interface.append(var)
        self.position = b.global_var(b.t_v4, CLASS_OUTPUT)
        b.decorate(self.position, DECO_BUILTIN, BUILTIN_POSITION)
        self.point_size = b.global_var(b.t_f32, CLASS_OUTPUT)
        b.decorate(self.point_size, DECO_BUILTIN, BUILTIN_POINT_SIZE)
        interface.extend((self.position, self.point_size))
        export_types = (
            (b.t_v4, 0), (b.t_v4, 1), (b.t_v4, 2), (b.t_v4, 3),
            (b.t_v4, 4), (b.t_v4, 5), (b.t_v4, 6), (b.t_v4, 7),
            (b.t_f32, 8), (b.t_v2, 9),
        )
        for pointee, location in export_types:
            var = b.global_var(pointee, CLASS_OUTPUT)
            b.decorate(var, DECO_LOCATION, location)
            self.exports.append(var)
            interface.append(var)
        if _needs_constants(self.slots):
            array = b.array(b.t_v4, _CONSTS, 16)
            self.const_ubo = b.ubo((array,), (16 * _CONSTS,), 0)
        surf_block = b.block((b.t_v4,), (None,))
        self.surf = b.global_var(surf_block, CLASS_UNIFORM)
        b.bind(self.surf, 0, 1)
        b.entry(MODEL_VERTEX, interface, False)
        b.begin()
        self.temps = [b.func_var(b.t_v4) for _ in range(12)]
        self.outs = [b.func_var(b.t_v4) for _ in range(16)]
        self.a0 = b.func_var(b.t_s32)
        if self.const_ubo and any(_writes_constant(slot) for slot in self.slots):
            self.local_c = b.func_var(b.array(b.t_v4, _CONSTS, 16))
        self._init()
        for slot in self.slots:
            self._slot(slot)
            if slot.final:
                break
        self._epilogue()
        b.end()
        return b.to_bytes()

    def _init(self) -> None:
        b = self.b
        for temp in self.temps:
            b.store(temp, b.v4_0)
        alpha1 = b.composite(b.t_v4, b.f0, b.f0, b.f0, b.f1)
        for index, var in enumerate(self.outs):
            if index == 5:
                continue
            b.store(var, alpha1 if index in _OUT_ALPHA1 else b.v4_0)
        carried = b.extract(b.t_f32, b.load(b.t_v4, self.inputs[5]), 0)
        b.store(self.outs[5], b.construct(b.t_v4, carried, b.f0, b.f0, b.f0))
        b.store(self.a0, b.s0)
        if self.local_c:
            for index in range(_CONSTS):
                src = b.access(CLASS_UNIFORM, b.t_v4, self.const_ubo, b.s0, b.sconst(index))
                dst = b.access(CLASS_FUNCTION, b.t_v4, self.local_c, b.sconst(index))
                b.store(dst, b.load(b.t_v4, src))

    def _temp(self, reg: int) -> int | None:
        if reg == 12:
            return self.outs[0]
        if 0 <= reg < 12:
            return self.temps[reg]
        return None

    def _const_index(self, slot: Slot, addr: int) -> int:
        b = self.b
        if slot.a0x:
            added = b.bin(OP_IADD, b.t_s32, b.sconst(addr & 0xFF), b.load(b.t_s32, self.a0))
            low = b.bin(OP_SLT, b.t_bool, added, b.s0)
            added = b.select(b.t_s32, low, b.s0, added)
            high = b.bin(OP_SGT, b.t_bool, added, b.s191)
            return b.select(b.t_s32, high, b.s191, added)
        return b.sconst(min(addr & 0xFF, 191))

    def _load_const(self, index: int) -> int:
        b = self.b
        if self.local_c:
            ptr = b.access(CLASS_FUNCTION, b.t_v4, self.local_c, index)
        else:
            ptr = b.access(CLASS_UNIFORM, b.t_v4, self.const_ubo, b.s0, index)
        return b.load(b.t_v4, ptr)

    def _operand(self, slot: Slot, operand: Operand) -> int:
        b = self.b
        if operand.mux == 1:
            ptr = self._temp(operand.reg)
            value = b.v4_0 if ptr is None else b.load(b.t_v4, ptr)
        elif operand.mux == 2:
            value = b.load(b.t_v4, self.inputs[slot.input_index & 15])
        elif operand.mux == 3:
            value = self._load_const(self._const_index(slot, slot.const_index))
        else:
            value = b.v4_0
        if operand.swizzle != (0, 1, 2, 3):
            value = b.shuffle(b.t_v4, value, value, operand.swizzle)
        if operand.neg:
            value = b.fneg(b.t_v4, value)
        return value

    def _splat(self, scalar: int) -> int:
        return self.b.construct(self.b.t_v4, scalar, scalar, scalar, scalar)

    def _xyz3(self, value: int) -> int:
        return self.b.shuffle(self.b.t_v3, value, value, (0, 1, 2))

    def _mac(self, slot: Slot) -> int:
        b = self.b
        op = slot.mac
        if op == 13:
            biased = b.fadd(b.t_f32, b.extract(b.t_f32, self._operand(slot, slot.a), 0), b.fconst(0.001))
            floored = b.ext(b.t_f32, GLSL_FLOOR, biased)
            b.store(self.a0, b.mod.result("code", OP_F_TO_S, b.t_s32, (floored,)))
            return b.v4_0
        a = self._operand(slot, slot.a)
        if op == 1:
            return a
        b_src = self._operand(slot, slot.b) if op in (2, 4, 5, 6, 7, 8, 9, 10, 11, 12) else 0
        c_src = self._operand(slot, slot.c) if op in (3, 4) else 0
        if op == 2:
            return b.fmul(b.t_v4, a, b_src)
        if op == 3:
            return b.fadd(b.t_v4, a, c_src)
        if op == 4:
            return b.fadd(b.t_v4, b.fmul(b.t_v4, a, b_src), c_src)
        if op == 5:
            return self._splat(b.dot(self._xyz3(a), self._xyz3(b_src)))
        if op == 6:
            dot = b.dot(self._xyz3(a), self._xyz3(b_src))
            return self._splat(b.fadd(b.t_f32, dot, b.extract(b.t_f32, b_src, 3)))
        if op == 7:
            return self._splat(b.dot(a, b_src))
        if op == 8:
            return b.construct(
                b.t_v4,
                b.f1,
                b.fmul(b.t_f32, b.extract(b.t_f32, a, 1), b.extract(b.t_f32, b_src, 1)),
                b.extract(b.t_f32, a, 2),
                b.extract(b.t_f32, b_src, 3),
            )
        if op == 9:
            return b.ext(b.t_v4, GLSL_FMIN, a, b_src)
        if op == 10:
            return b.ext(b.t_v4, GLSL_FMAX, a, b_src)
        cond_op = OP_F_ORD_LT if op == 11 else OP_F_ORD_GE
        cond = b.bin(cond_op, b.t_b4, a, b_src)
        return b.select(b.t_v4, cond, b.v4_1, b.v4_0)

    def _ilu(self, slot: Slot) -> int:
        b = self.b
        cs = self._operand(slot, slot.c)
        x = b.extract(b.t_f32, cs, 0)
        op = slot.ilu
        if op == 1:
            return cs
        if op == 2:
            return self._splat(b.fdiv(b.t_f32, b.f1, x))
        if op == 3:
            return self._splat(self._rcc(x))
        if op == 4:
            return self._splat(b.ext(b.t_f32, GLSL_INVSQRT, b.ext(b.t_f32, GLSL_FABS, x)))
        if op == 5:
            floor = b.ext(b.t_f32, GLSL_FLOOR, x)
            return b.construct(
                b.t_v4,
                b.ext(b.t_f32, GLSL_EXP2, floor),
                b.ext(b.t_f32, GLSL_FRACT, x),
                b.ext(b.t_f32, GLSL_EXP2, x),
                b.f1,
            )
        if op == 6:
            magnitude = b.ext(b.t_f32, GLSL_FABS, x)
            exponent = b.ext(b.t_f32, GLSL_FLOOR, b.ext(b.t_f32, GLSL_LOG2, magnitude))
            return b.construct(
                b.t_v4,
                exponent,
                b.fdiv(b.t_f32, magnitude, b.ext(b.t_f32, GLSL_EXP2, exponent)),
                b.ext(b.t_f32, GLSL_LOG2, magnitude),
                b.f1,
            )
        power = b.ext(b.t_f32, GLSL_FCLAMP, b.extract(b.t_f32, cs, 3), b.fconst(-127.9961), b.fconst(127.9961))
        base = b.ext(b.t_f32, GLSL_FMAX, b.extract(b.t_f32, cs, 1), b.f0)
        lit = b.ext(b.t_f32, GLSL_POW, base, power)
        positive = b.bin(OP_F_ORD_GT, b.t_bool, x, b.f0)
        return b.construct(
            b.t_v4,
            b.f1,
            b.ext(b.t_f32, GLSL_FMAX, x, b.f0),
            b.select(b.t_f32, positive, lit, b.f0),
            b.f1,
        )

    def _rcc(self, value: int) -> int:
        b = self.b
        magnitude = b.ext(
            b.t_f32,
            GLSL_FCLAMP,
            b.ext(b.t_f32, GLSL_FABS, b.fdiv(b.t_f32, b.f1, value)),
            b.fbits(_W_MIN),
            b.fbits(_W_MAX),
        )
        negative = self._signbit(value)
        return b.select(b.t_f32, negative, b.fneg(b.t_f32, magnitude), magnitude)

    def _signbit(self, value: int) -> int:
        b = self.b
        bits = b.mod.result("code", OP_BITCAST, b.t_u32, (value,))
        shifted = b.bin(OP_SHR, b.t_u32, bits, b.u31)
        return b.bin(OP_INE, b.t_bool, shifted, b.u0)

    def _mask_store(self, ptr: int, value: int, mask: int) -> None:
        b = self.b
        if mask == 0:
            return
        if mask == 0xF:
            b.store(ptr, value)
            return
        old = b.load(b.t_v4, ptr)
        selectors = []
        for index, bit in enumerate((8, 4, 2, 1)):
            selectors.append(4 + index if mask & bit else index)
        b.store(ptr, b.shuffle(b.t_v4, old, value, tuple(selectors)))

    def _slot(self, slot: Slot) -> None:
        b = self.b
        mac = self._mac(slot) if _uses_mac(slot) else b.v4_0
        ilu = self._ilu(slot) if _uses_ilu(slot) else b.v4_0
        if slot.mac_writes_dest:
            ptr = self._temp(slot.dest)
            if ptr is not None:
                self._mask_store(ptr, mac, slot.mac_mask)
        if slot.ilu and slot.ilu_mask:
            ptr = self._temp(slot.ilu_dest if slot.ilu_dest is not None else slot.dest)
            if ptr is not None:
                self._mask_store(ptr, ilu, slot.ilu_mask)
        if not slot.out_mask:
            return
        value = ilu if slot.out_from_ilu else mac
        if slot.mac == 0 and not slot.out_from_ilu:
            value = b.v4_0
        if slot.ilu == 0 and slot.out_from_ilu:
            value = b.v4_0
        mask = slot.out_mask
        if slot.out_is_reg:
            reg = slot.out_addr & 15
            if reg == 5:
                mask |= 8
            self._mask_store(self.outs[reg], value, mask)
        else:
            index = self._const_index(slot, slot.out_addr)
            if not self.local_c:
                raise RuntimeError("constant write without a function copy")
            ptr = b.access(CLASS_FUNCTION, b.t_v4, self.local_c, index)
            self._mask_store(ptr, value, mask)

    def _epilogue(self) -> None:
        b = self.b
        pos = b.load(b.t_v4, self.outs[0])
        xy = b.shuffle(b.t_v2, pos, pos, (0, 1))
        scale = b.repeat(b.t_v2, b.fconst(16.0), 2)
        huge = b.fbits(_FLT_MAX)
        snapped = b.fdiv(
            b.t_v2,
            b.ext(
                b.t_v2,
                GLSL_TRUNC,
                b.ext(
                    b.t_v2,
                    GLSL_FCLAMP,
                    b.fmul(b.t_v2, xy, scale),
                    b.repeat(b.t_v2, b.fbits(_FLT_MAX | 0x80000000), 2),
                    b.repeat(b.t_v2, huge, 2),
                ),
            ),
            scale,
        )
        pos = b.shuffle(b.t_v4, snapped, pos, (0, 1, 4, 5))
        b.store(self.outs[0], pos)
        w = self._clamp_w(b.extract(b.t_f32, pos, 3))
        surf = b.load(b.t_v4, b.access(CLASS_UNIFORM, b.t_v4, self.surf, b.s0))
        ox = b.extract(b.t_f32, pos, 0)
        oy = b.extract(b.t_f32, pos, 1)
        ndc_x = b.fmul(b.t_f32, b.fsub(b.t_f32, b.fdiv(b.t_f32, b.fmul(b.t_f32, ox, b.f2), b.extract(b.t_f32, surf, 0)), b.f1), w)
        ndc_y = b.fmul(b.t_f32, b.fsub(b.t_f32, b.fdiv(b.t_f32, b.fmul(b.t_f32, oy, b.f2), b.extract(b.t_f32, surf, 1)), b.f1), w)
        limit = b.fconst(1e38)
        ndc = b.ext(
            b.t_v2,
            GLSL_FCLAMP,
            b.construct(b.t_v2, ndc_x, ndc_y),
            b.repeat(b.t_v2, b.fconst(-1e38), 2),
            b.repeat(b.t_v2, limit, 2),
        )
        b.store(self.position, b.construct(b.t_v4, b.extract(b.t_f32, ndc, 0), b.extract(b.t_f32, ndc, 1), b.fmul(b.t_f32, b.f_half, w), w))
        point = b.ext(b.t_f32, GLSL_FCLAMP, b.extract(b.t_f32, b.load(b.t_v4, self.outs[6]), 0), b.f1, b.fconst(64.0))
        b.store(self.point_size, point)
        for index, local in ((0, 3), (1, 4), (2, 7), (3, 8), (4, 9), (5, 10), (6, 11), (7, 12)):
            b.store(self.exports[index], b.load(b.t_v4, self.outs[local]))
        b.store(self.exports[8], b.extract(b.t_f32, b.load(b.t_v4, self.outs[5]), 0))
        z = b.extract(b.t_f32, pos, 2)
        far = b.fconst(16777215.0)
        pair = b.construct(b.t_v2, z, b.fsub(b.t_f32, far, z))
        b.store(self.exports[9], b.fmul(b.t_v2, pair, b.construct(b.t_v2, w, w)))

    def _clamp_w(self, w: int) -> int:
        b = self.b
        low = b.fbits(_W_MIN)
        high = b.fbits(_W_MAX)
        small = b.bin(OP_F_ORD_LT, b.t_bool, b.ext(b.t_f32, GLSL_FABS, w), low)
        signed_low = b.select(b.t_f32, self._signbit(w), b.fneg(b.t_f32, low), low)
        w = b.select(b.t_f32, small, signed_low, w)
        big = b.bin(OP_F_ORD_GT, b.t_bool, b.ext(b.t_f32, GLSL_FABS, w), high)
        negative = b.bin(OP_F_ORD_LT, b.t_bool, w, b.f0)
        signed_high = b.select(b.t_f32, negative, b.fneg(b.t_f32, high), high)
        return b.select(b.t_f32, big, signed_high, w)


class _Combiner:
    def __init__(self, shader: PixelShader) -> None:
        self.shader = shader
        self.b = Builder()
        self.reg: dict[str, int] = {}
        self.tex_in: list[int] = []
        self.samplers: list[int] = []
        self.ubo = 0
        self.sampled = 0
        self.cur_c0 = 0
        self.cur_c1 = 0
        self.msb = shader.mux != 0
        self.f4 = 0
        self.f255 = 0
        self.span = 0

    def emit(self) -> bytes:
        b = self.b
        self.f4 = b.fconst(4.0)
        self.f255 = b.fconst(255.0)
        self.span = b.fconst(65536.0)
        b.v3_4 = b.repeat(b.t_v3, self.f4, 3)
        array = b.array(b.t_v4, 16, 16)
        self.ubo = b.ubo((array, b.t_v4), (256, None), 2)
        image = b.mod.result("types", OP_TYPE_IMAGE, None, (b.t_f32, DIM_2D, 0, 0, 0, 1, 0))
        self.sampled = b.mod.result("types", OP_TYPE_SAMPLED, None, (image,))
        sampled = self.sampled
        interface: list[int] = []
        locations = (("v0", 0), ("v1", 1), ("t0", 4), ("t1", 5), ("t2", 6), ("t3", 7))
        inputs: dict[str, int] = {}
        for name, location in locations:
            var = b.global_var(b.t_v4, CLASS_INPUT)
            b.decorate(var, DECO_LOCATION, location)
            inputs[name] = var
            interface.append(var)
        fog_in = b.global_var(b.t_f32, CLASS_INPUT)
        b.decorate(fog_in, DECO_LOCATION, 8)
        interface.append(fog_in)
        for binding in range(4):
            sampler = b.global_var(sampled, CLASS_UNIFORM_CONSTANT)
            b.bind(sampler, 0, 3 + binding)
            self.samplers.append(sampler)
        frag = b.global_var(b.t_v4, CLASS_OUTPUT)
        b.decorate(frag, DECO_LOCATION, 0)
        interface.append(frag)
        b.entry(MODEL_FRAGMENT, interface, True)
        b.begin()
        for name in ("v0", "v1", "t0", "t1", "t2", "t3", "r0", "r1", "fog", "spec", "ef"):
            self.reg[name] = b.func_var(b.t_v4)
        self.tex_in = [inputs[name] for name in ("t0", "t1", "t2", "t3")]
        b.store(self.reg["v0"], b.ext(b.t_v4, GLSL_FCLAMP, b.load(b.t_v4, inputs["v0"]), b.v4_0, b.v4_1))
        b.store(self.reg["v1"], b.ext(b.t_v4, GLSL_FCLAMP, b.load(b.t_v4, inputs["v1"]), b.v4_0, b.v4_1))
        for name in ("t0", "t1", "t2", "t3", "r1", "spec", "ef"):
            b.store(self.reg[name], b.v4_0)
        fog_rgb = b.shuffle(b.t_v3, b.load(b.t_v4, b.access(CLASS_UNIFORM, b.t_v4, self.ubo, b.s1)), b.v4_0, (0, 1, 2))
        factor = b.ext(b.t_f32, GLSL_FCLAMP, b.load(b.t_f32, fog_in), b.f0, b.f1)
        b.store(self.reg["fog"], b.construct(b.t_v4, b.extract(b.t_f32, fog_rgb, 0), b.extract(b.t_f32, fog_rgb, 1), b.extract(b.t_f32, fog_rgb, 2), factor))
        for stage in range(4):
            self._texture(stage)
        t0a = b.extract(b.t_f32, b.load(b.t_v4, self.reg["t0"]), 3)
        b.store(self.reg["r0"], b.construct(b.t_v4, b.f0, b.f0, b.f0, t0a))
        for stage in self.shader.shown_stages():
            self._stage(stage)
        color = self._final()
        b.store(frag, color)
        b.end()
        return b.to_bytes()

    def _texture(self, stage: int) -> None:
        mode = self.shader.texture[stage]
        if mode in _SAMPLED:
            self._sample(stage)
        elif mode == _PASS:
            coord = self.b.load(self.b.t_v4, self.tex_in[stage])
            self.b.store(self.reg[f"t{stage}"], self.b.ext(self.b.t_v4, GLSL_FCLAMP, coord, self.b.v4_0, self.b.v4_1))
        elif mode == _CLIP:
            self._clip(stage)

    def _sample(self, stage: int) -> None:
        b = self.b
        coord = b.load(b.t_v4, self.tex_in[stage])
        xy = b.shuffle(b.t_v2, coord, coord, (0, 1))
        w = b.extract(b.t_f32, coord, 3)
        uv = b.fdiv(b.t_v2, xy, b.construct(b.t_v2, w, w))
        limit = b.repeat(b.t_v2, self.span, 2)
        negative = b.repeat(b.t_v2, b.fconst(-65536.0), 2)
        uv = b.ext(b.t_v2, GLSL_FCLAMP, uv, negative, limit)
        loaded = b.load(self.sampled, self.samplers[stage])
        b.store(self.reg[f"t{stage}"], b.mod.result("code", OP_SAMPLE, b.t_v4, (loaded, uv)))

    def _clip(self, stage: int) -> None:
        b = self.b
        coord = b.load(b.t_v4, self.tex_in[stage])
        mode = (self.shader.compare_mode >> (4 * stage)) & 0xF
        for component in range(4):
            value = b.extract(b.t_f32, coord, component)
            if (mode >> component) & 1:
                cond = b.bin(OP_F_ORD_GE, b.t_bool, value, b.f0)
            else:
                cond = b.bin(OP_F_ORD_LT, b.t_bool, value, b.f0)
            b.discard_if(cond)

    def _stage_constant(self, stage: int, which: int) -> int:
        shader = self.shader
        unique = shader.unique_c0 if which == 0 else shader.unique_c1
        maps = shader.c0_map if which == 0 else shader.c1_map
        baked = shader.constant0 if which == 0 else shader.constant1
        index = stage if unique else 0
        return self._mapped(maps[index], baked[index])

    def _mapped(self, nibble: int, baked: int) -> int:
        if (nibble & 0xF) == 0xF:
            return self._color(baked)
        return self._pc(nibble & 0xF)

    def _color(self, argb: int) -> int:
        b = self.b
        parts = []
        for shift in (16, 8, 0, 24):
            parts.append(b.fconst(((argb >> shift) & 0xFF) / 255.0))
        return b.composite(b.t_v4, *parts)

    def _pc(self, index: int) -> int:
        b = self.b
        ptr = b.access(CLASS_UNIFORM, b.t_v4, self.ubo, b.s0, b.sconst(index))
        return b.load(b.t_v4, ptr)

    def _source(self, source: int, final: bool) -> int:
        b = self.b
        if source in (1, 2):
            return self.cur_c0 if source == 1 else self.cur_c1
        if source == 3:
            return b.load(b.t_v4, self.reg["fog"])
        if source in (4, 5, 8, 9, 10, 11, 12, 13):
            return b.load(b.t_v4, self.reg[_WRITABLE[source]])
        if final and source == 14:
            return b.load(b.t_v4, self.reg["spec"])
        if final and source == 15:
            return b.load(b.t_v4, self.reg["ef"])
        return b.v4_0

    def _map(self, value: int, mapping: int, type_id: int) -> int:
        b = self.b
        count = 3 if type_id == b.t_v3 else 1
        zero = b.v3_0 if count == 3 else b.f0
        one = b.v3_1 if count == 3 else b.f1
        half = b.v3_half if count == 3 else b.f_half
        two = b.v3_2 if count == 3 else b.f2
        pos = b.ext(type_id, GLSL_FMAX, value, zero)
        if mapping == 0:
            return pos
        if mapping == 1:
            return b.fsub(type_id, one, b.ext(type_id, GLSL_FCLAMP, value, zero, one))
        if mapping == 2:
            return b.fsub(type_id, b.fmul(type_id, two, pos), one)
        if mapping == 3:
            return b.fadd(type_id, b.fneg(type_id, b.fmul(type_id, two, pos)), one)
        if mapping == 4:
            return b.fsub(type_id, pos, half)
        if mapping == 5:
            return b.fsub(type_id, half, pos)
        if mapping == 6:
            return value
        return b.fneg(type_id, value)

    def _general_vec(self, item) -> int:
        base = self._source(item.source, False)
        picked = self.b.shuffle(self.b.t_v3, base, base, (3, 3, 3) if item.alpha else (0, 1, 2))
        return self._map(picked, item.mapping, self.b.t_v3)

    def _general_scalar(self, item) -> int:
        base = self._source(item.source, False)
        picked = self.b.extract(self.b.t_f32, base, 3 if item.alpha else 2)
        return self._map(picked, item.mapping, self.b.t_f32)

    def _apply(self, value: int, op: int, type_id: int) -> int:
        b = self.b
        count = 3 if type_id == b.t_v3 else 1
        half = b.v3_half if count == 3 else b.f_half
        two = b.v3_2 if count == 3 else b.f2
        four = b.v3_4 if count == 3 else self.f4
        one = b.v3_1 if count == 3 else b.f1
        neg = b.v3_n1 if count == 3 else b.fn1
        if op == 1:
            value = b.fsub(type_id, value, half)
        elif op == 2:
            value = b.fmul(type_id, value, two)
        elif op == 3:
            value = b.fmul(type_id, b.fsub(type_id, value, half), two)
        elif op == 4:
            value = b.fmul(type_id, value, four)
        elif op == 6:
            value = b.fmul(type_id, value, half)
        return b.ext(type_id, GLSL_FCLAMP, value, neg, one)

    def _mux(self, ab: int, cd: int, type_id: int, enabled: int, r0a: int) -> int:
        b = self.b
        if not enabled:
            return b.fadd(type_id, ab, cd)
        if self.msb:
            cond = b.bin(OP_F_ORD_GE, b.t_bool, r0a, b.f_half)
        else:
            bits = b.mod.result("code", OP_F_TO_S, b.t_s32, (b.fmul(b.t_f32, r0a, self.f255),))
            cond = b.bin(OP_INE, b.t_bool, b.bin(OP_AND, b.t_s32, bits, b.s1), b.s0)
        return b.select(type_id, cond, cd, ab)

    def _store_rgb(self, dest: int, value: int) -> None:
        name = _WRITABLE.get(dest)
        if name is None:
            return
        b = self.b
        old = b.load(b.t_v4, self.reg[name])
        b.store(self.reg[name], b.shuffle(b.t_v4, value, old, (0, 1, 2, 6)))

    def _store_a(self, dest: int, value: int) -> None:
        name = _WRITABLE.get(dest)
        if name is None:
            return
        b = self.b
        old = b.load(b.t_v4, self.reg[name])
        b.store(self.reg[name], b.construct(
            b.t_v4,
            b.extract(b.t_f32, old, 0),
            b.extract(b.t_f32, old, 1),
            b.extract(b.t_f32, old, 2),
            value,
        ))

    def _stage(self, stage: int) -> None:
        b = self.b
        shader = self.shader
        self.cur_c0 = self._stage_constant(stage, 0)
        self.cur_c1 = self._stage_constant(stage, 1)
        r0a = b.extract(b.t_f32, b.load(b.t_v4, self.reg["r0"]), 3)
        rgb = [self._general_vec(item) for item in shader.rgb_in[stage]]
        alpha = [self._general_scalar(item) for item in shader.alpha_in[stage]]
        rgb_out = shader.rgb_out[stage]
        alpha_out = shader.alpha_out[stage]
        cab = self._splat3(b.dot(rgb[0], rgb[1])) if rgb_out.ab_dot else b.fmul(b.t_v3, rgb[0], rgb[1])
        ccd = self._splat3(b.dot(rgb[2], rgb[3])) if rgb_out.cd_dot else b.fmul(b.t_v3, rgb[2], rgb[3])
        csum = self._mux(cab, ccd, b.t_v3, rgb_out.mux, r0a)
        cab = self._apply(cab, rgb_out.op, b.t_v3)
        ccd = self._apply(ccd, rgb_out.op, b.t_v3)
        csum = self._apply(csum, rgb_out.op, b.t_v3)
        aab = b.fmul(b.t_f32, alpha[0], alpha[1])
        acd = b.fmul(b.t_f32, alpha[2], alpha[3])
        asum = self._mux(aab, acd, b.t_f32, alpha_out.mux, r0a)
        aab = self._apply(aab, alpha_out.op, b.t_f32)
        acd = self._apply(acd, alpha_out.op, b.t_f32)
        asum = self._apply(asum, alpha_out.op, b.t_f32)
        self._store_rgb(rgb_out.ab, cab)
        self._store_rgb(rgb_out.cd, ccd)
        self._store_rgb(rgb_out.sum, csum)
        self._store_a(alpha_out.ab, aab)
        self._store_a(alpha_out.cd, acd)
        self._store_a(alpha_out.sum, asum)
        if rgb_out.blue_ab:
            self._store_a(rgb_out.ab, b.extract(b.t_f32, cab, 2))
        if rgb_out.blue_cd:
            self._store_a(rgb_out.cd, b.extract(b.t_f32, ccd, 2))

    def _splat3(self, scalar: int) -> int:
        return self.b.construct(self.b.t_v3, scalar, scalar, scalar)

    def _final_vec(self, item) -> int:
        base = self._source(item.source, True)
        picked = self.b.shuffle(self.b.t_v3, base, base, (3, 3, 3) if item.alpha else (0, 1, 2))
        clamped = self.b.ext(self.b.t_v3, GLSL_FCLAMP, picked, self.b.v3_0, self.b.v3_1)
        if item.mapping:
            return self.b.fsub(self.b.t_v3, self.b.v3_1, clamped)
        return clamped

    def _final_scalar(self, item) -> int:
        base = self._source(item.source, True)
        picked = self.b.extract(self.b.t_f32, base, 3 if item.alpha else 2)
        clamped = self.b.ext(self.b.t_f32, GLSL_FCLAMP, picked, self.b.f0, self.b.f1)
        if item.mapping:
            return self.b.fsub(self.b.t_f32, self.b.f1, clamped)
        return clamped

    def _final(self) -> int:
        b = self.b
        shader = self.shader
        self.cur_c0 = self._mapped(shader.final_c0_map, shader.final_constant0)
        self.cur_c1 = self._mapped(shader.final_c1_map, shader.final_constant1)
        summed = b.fadd(
            b.t_v3,
            b.shuffle(b.t_v3, b.load(b.t_v4, self.reg["v1"]), b.v4_0, (0, 1, 2)),
            b.shuffle(b.t_v3, b.load(b.t_v4, self.reg["r0"]), b.v4_0, (0, 1, 2)),
        )
        if shader.spec_clamp:
            summed = b.ext(b.t_v3, GLSL_FCLAMP, summed, b.v3_0, b.v3_1)
        b.store(self.reg["spec"], b.construct(
            b.t_v4,
            b.extract(b.t_f32, summed, 0),
            b.extract(b.t_f32, summed, 1),
            b.extract(b.t_f32, summed, 2),
            b.f0,
        ))
        e_vec = self._final_vec(shader.final_e)
        f_vec = self._final_vec(shader.final_f)
        product = b.fmul(b.t_v3, e_vec, f_vec)
        b.store(self.reg["ef"], b.construct(
            b.t_v4,
            b.extract(b.t_f32, product, 0),
            b.extract(b.t_f32, product, 1),
            b.extract(b.t_f32, product, 2),
            b.f0,
        ))
        a_vec = self._final_vec(shader.final_a)
        b_vec = self._final_vec(shader.final_b)
        c_vec = self._final_vec(shader.final_c)
        d_vec = self._final_vec(shader.final_d)
        rgb = b.ext(
            b.t_v3,
            GLSL_FCLAMP,
            b.fadd(b.t_v3, b.fadd(b.t_v3, b.fmul(b.t_v3, a_vec, b_vec), b.fmul(b.t_v3, b.fsub(b.t_v3, b.v3_1, a_vec), c_vec)), d_vec),
            b.v3_0,
            b.v3_1,
        )
        alpha = self._final_scalar(shader.final_g)
        return b.construct(b.t_v4, b.extract(b.t_f32, rgb, 0), b.extract(b.t_f32, rgb, 1), b.extract(b.t_f32, rgb, 2), alpha)


def emit_vertex(slots: list[Slot]) -> bytes:
    return _Vertex(slots).emit()


def emit_combiner(shader: PixelShader) -> bytes:
    return _Combiner(shader).emit()


def vertex_spirv(blob: bytes) -> bytes | None:
    slots = decode_program(blob)
    if slots is None:
        return None
    return emit_vertex(slots)


def combiner_spirv(blob: bytes) -> bytes | None:
    shader = parse_xpu(blob)
    if shader is None:
        return None
    return emit_combiner(shader)


def validate_spirv(blob: bytes) -> str | None:
    """None when spirv-val accepts the module for Vulkan 1.0 and OpenGL 4.5.

    The validator's text when either environment rejects it.
    """
    exe = shutil.which("spirv-val")
    if exe is None:
        raise FileNotFoundError("spirv-val")
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "shader.spv"
        path.write_bytes(blob)
        for env in ("vulkan1.0", "opengl4.5"):
            proc = subprocess.run(
                [exe, "--target-env", env, str(path)],
                check=False,
                capture_output=True,
                text=True,
            )
            if proc.returncode != 0:
                text = (proc.stderr or proc.stdout or "spirv-val failed").strip()
                return f"{env}: {text}"
    return None
