"""Xbox pixel-shader files (.xpu).

A file is the four bytes ``PSB0`` followed by the 240-byte combiner
definition the driver copies into the NV2A register combiners. There are
eight general stages and one final stage. Each general stage multiplies
A·B and C·D, then sums those products or muxes them on r0.a. The final
stage computes A·B + (1−A)·C + D, and its alpha output is G.

An input byte is a 3-bit mapping, a channel bit, and a 4-bit source.
The final stage only uses the channel bit and the complement bit. R0's
alpha is the mux selector. A mapping nibble of 15 means the stage keeps
the color stored in this file. Any other nibble selects a pixel-shader
constant.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

PSB_MAGIC = b"PSB0"
PSB_SIZE = 244

_SOURCES = (
    "zero", "c0", "c1", "fog", "v0", "v1", "?6", "?7",
    "t0", "t1", "t2", "t3", "r0", "r1", "v1r0", "ef",
)
_WRITABLE = {4, 5, 8, 9, 10, 11, 12, 13}
_MAPS = ("", "1-", "expand ", "-expand ", "bias ", "-bias ", "signed ", "-signed ")
_ZERO = ("0", "1", "-1", "1", "-0.5", "0.5", "0", "0")
_OPS = ("", " bias", " *2", " *2bias", " *4", " ?5", " /2", " ?7")
_TEXTURES = (
    "NONE", "2D_PROJECTIVE", "3D_PROJECTIVE", "CUBE_MAP", "PASS_THROUGH", "CLIP_PLANE",
    "BUMPENVMAP", "BUMPENVMAP_LUMINANCE", "BRDF", "DOT_ST", "DOT_ZW", "DOT_REFLECT_DIFFUSE",
    "DOT_REFLECT_SPECULAR", "DOT_STR_3D", "DOT_STR_CUBE", "DEPENDENT_AR", "DEPENDENT_GB",
    "DOT_PRODUCT", "DOT_REFLECT_SPECULAR_CONST",
)


def _field(word: int, lo: int, width: int) -> int:
    return (word >> lo) & ((1 << width) - 1)


@dataclass(frozen=True)
class Input:
    source: int
    alpha: int
    mapping: int


@dataclass(frozen=True)
class Output:
    cd: int
    ab: int
    sum: int
    cd_dot: int
    ab_dot: int
    mux: int
    op: int
    blue_cd: int
    blue_ab: int


@dataclass(frozen=True)
class PixelShader:
    alpha_in: tuple[tuple[Input, Input, Input, Input], ...]
    rgb_in: tuple[tuple[Input, Input, Input, Input], ...]
    alpha_out: tuple[Output, ...]
    rgb_out: tuple[Output, ...]
    constant0: tuple[int, ...]
    constant1: tuple[int, ...]
    compare_mode: int
    final_constant0: int
    final_constant1: int
    final_a: Input
    final_b: Input
    final_c: Input
    final_d: Input
    final_e: Input
    final_f: Input
    final_g: Input
    spec_clamp: int
    final_extra: int
    stages: int
    mux: int
    unique_c0: int
    unique_c1: int
    count_extra: int
    texture: tuple[int, int, int, int]
    texture_extra: int
    dot_map: tuple[int, int, int]
    dot_extra: int
    input_tex2: int
    input_tex3: int
    input_tex_extra: int
    c0_map: tuple[int, ...]
    c1_map: tuple[int, ...]
    final_c0_map: int
    final_c1_map: int
    final_map_extra: int

    def shown_stages(self) -> range:
        count = self.stages
        if count < 1:
            count = 1
        if count > 8:
            count = 8
        return range(count)

    def extra_stages(self) -> tuple[int, ...]:
        start = self.shown_stages().stop
        return tuple(index for index in range(start, 8) if _stage_used(self, index))


def _general(byte: int) -> Input:
    return Input(byte & 0xF, _field(byte, 4, 1), _field(byte, 5, 3))


def _final(byte: int) -> Input:
    return Input(byte & 0xF, _field(byte, 4, 1), _field(byte, 5, 1))


def _inputs(word: int, final: bool) -> tuple[Input, Input, Input, Input]:
    decode = _final if final else _general
    return (
        decode(_field(word, 24, 8)),
        decode(_field(word, 16, 8)),
        decode(_field(word, 8, 8)),
        decode(_field(word, 0, 8)),
    )


def _output(word: int) -> Output:
    return Output(
        cd=word & 0xF,
        ab=_field(word, 4, 4),
        sum=_field(word, 8, 4),
        cd_dot=_field(word, 12, 1),
        ab_dot=_field(word, 13, 1),
        mux=_field(word, 14, 1),
        op=_field(word, 15, 3),
        blue_cd=_field(word, 18, 1),
        blue_ab=_field(word, 19, 1),
    )


def _nibbles(word: int) -> tuple[int, ...]:
    return tuple(_field(word, 4 * index, 4) for index in range(8))


def parse_xpu(blob: bytes) -> PixelShader | None:
    if len(blob) != PSB_SIZE or blob[:4] != PSB_MAGIC:
        return None
    words = struct.unpack_from("<60I", blob, 4)
    count = words[53]
    textures = words[54]
    dots = words[55]
    tex_in = words[56]
    final_map = words[59]
    cw1 = words[9]
    return PixelShader(
        alpha_in=tuple(_inputs(words[index], False) for index in range(8)),
        rgb_in=tuple(_inputs(words[34 + index], False) for index in range(8)),
        alpha_out=tuple(_output(words[26 + index]) for index in range(8)),
        rgb_out=tuple(_output(words[45 + index]) for index in range(8)),
        constant0=words[10:18],
        constant1=words[18:26],
        compare_mode=words[42],
        final_constant0=words[43],
        final_constant1=words[44],
        final_a=_final(_field(words[8], 24, 8)),
        final_b=_final(_field(words[8], 16, 8)),
        final_c=_final(_field(words[8], 8, 8)),
        final_d=_final(_field(words[8], 0, 8)),
        final_e=_final(_field(cw1, 24, 8)),
        final_f=_final(_field(cw1, 16, 8)),
        final_g=_final(_field(cw1, 8, 8)),
        spec_clamp=_field(cw1, 7, 1),
        final_extra=cw1 & 0x7F,
        stages=count & 0xFF,
        mux=_field(count, 8, 4),
        unique_c0=_field(count, 12, 4),
        unique_c1=_field(count, 16, 4),
        count_extra=_field(count, 20, 12),
        texture=tuple(_field(textures, 5 * index, 5) for index in range(4)),
        texture_extra=_field(textures, 20, 12),
        dot_map=tuple(_field(dots, 4 * index, 4) for index in range(3)),
        dot_extra=_field(dots, 12, 20),
        input_tex2=_field(tex_in, 16, 4),
        input_tex3=_field(tex_in, 20, 4),
        input_tex_extra=tex_in & ~(0xF << 16) & ~(0xF << 20),
        c0_map=_nibbles(words[57]),
        c1_map=_nibbles(words[58]),
        final_c0_map=final_map & 0xF,
        final_c1_map=_field(final_map, 4, 4),
        final_map_extra=_field(final_map, 8, 24),
    )


def _output_used(item: Output) -> bool:
    return bool(
        item.ab or item.cd or item.sum or item.op or item.mux
        or item.ab_dot or item.cd_dot or item.blue_ab or item.blue_cd
    )


def _stage_used(shader: PixelShader, index: int) -> bool:
    if _output_used(shader.rgb_out[index]) or _output_used(shader.alpha_out[index]):
        return True
    for group in (shader.rgb_in[index], shader.alpha_in[index]):
        for item in group:
            if item.source or item.alpha or item.mapping:
                return True
    return False


def _source(item: Input, portion: str, final: bool) -> str:
    if item.source == 0:
        if final:
            return "1" if item.mapping else "0"
        return _ZERO[item.mapping]
    name = _SOURCES[item.source]
    if portion == "rgb":
        channel = ".aaa" if item.alpha else ".rgb"
    else:
        channel = ".a" if item.alpha else ".b"
    if final:
        prefix = "1-" if item.mapping else ""
    else:
        prefix = _MAPS[item.mapping]
    return prefix + name + channel


def _dest(register: int) -> str:
    if register == 0:
        return "discard"
    return _SOURCES[register]


def _map_name(nibble: int) -> str:
    if nibble == 15:
        return "local"
    return f"c{nibble}"


def _flag(nibble: int, off: str, on: str) -> str:
    if nibble == 0:
        return off
    if nibble == 1:
        return on
    return str(nibble)


def _texture_name(mode: int) -> str:
    if mode < len(_TEXTURES):
        return _TEXTURES[mode]
    return f"?{mode}"


def format_stage(index: int, portion: str, inputs: tuple[Input, Input, Input, Input], output: Output) -> str:
    names = ("A", "B", "C", "D")
    text = f"{index} {portion}"
    for name, item in zip(names, inputs):
        text += f"  {name}={_source(item, portion, False)}"
    ab = "dot(A,B)" if output.ab_dot else "A*B"
    cd = "dot(C,D)" if output.cd_dot else "C*D"
    total = f"mux({ab},{cd})" if output.mux else f"{ab}+{cd}"
    op = _OPS[output.op]
    text += f"  ab={ab}{op} -> {_dest(output.ab)}"
    text += f"  cd={cd}{op} -> {_dest(output.cd)}"
    text += f"  sum={total}{op} -> {_dest(output.sum)}"
    if output.blue_ab:
        text += f"  blue ab -> {_dest(output.ab)}.a"
    if output.blue_cd:
        text += f"  blue cd -> {_dest(output.cd)}.a"
    return text


def format_program(shader: PixelShader) -> str:
    lines = [
        "stages {0}  mux={1}  c0={2}  c1={3}".format(
            shader.stages,
            _flag(shader.mux, "lsb", "msb"),
            _flag(shader.unique_c0, "shared", "unique"),
            _flag(shader.unique_c1, "shared", "unique"),
        )
    ]
    if shader.count_extra:
        lines[-1] += f"  count-extra={shader.count_extra:#x}"
    modes = "  ".join(_texture_name(mode) for mode in shader.texture)
    lines.append(f"tex  {modes}")
    if shader.texture_extra:
        lines[-1] += f"  extra={shader.texture_extra:#x}"
    if any(shader.dot_map) or shader.dot_extra:
        lines.append(
            "dot  "
            + "  ".join(f"{index + 1}:{value:#x}" for index, value in enumerate(shader.dot_map))
            + (f"  extra={shader.dot_extra:#x}" if shader.dot_extra else "")
        )
    if shader.input_tex2 or shader.input_tex3 or shader.input_tex_extra:
        lines.append(
            f"tex-src  stage2={shader.input_tex2}  stage3={shader.input_tex3}"
            + (f"  extra={shader.input_tex_extra:#x}" if shader.input_tex_extra else "")
        )
    if shader.compare_mode:
        clips = "  ".join(f"{index}:{_field(shader.compare_mode, 4 * index, 4):#x}" for index in range(4))
        lines.append(f"clip  {clips}")
    for index in (*shader.shown_stages(), *shader.extra_stages()):
        lines.append(format_stage(index, "rgb", shader.rgb_in[index], shader.rgb_out[index]))
        lines.append(format_stage(index, "a", shader.alpha_in[index], shader.alpha_out[index]))
    final = (
        ("A", shader.final_a),
        ("B", shader.final_b),
        ("C", shader.final_c),
        ("D", shader.final_d),
        ("E", shader.final_e),
        ("F", shader.final_f),
        ("G", shader.final_g),
    )
    text = "final"
    for name, item in final:
        portion = "a" if name == "G" else "rgb"
        text += f"  {name}={_source(item, portion, True)}"
    text += "  spec=" + ("clamp(v1+r0)" if shader.spec_clamp else "v1+r0")
    if shader.final_extra:
        text += f"  extra={shader.final_extra:#x}"
    lines.append(text)
    lines.append(
        "map c0  " + " ".join(_map_name(item) for item in shader.c0_map)
    )
    lines.append(
        "map c1  " + " ".join(_map_name(item) for item in shader.c1_map)
    )
    lines.append(
        f"map final  c0={_map_name(shader.final_c0_map)}  c1={_map_name(shader.final_c1_map)}"
        + (f"  extra={shader.final_map_extra:#x}" if shader.final_map_extra else "")
    )
    for label, values in (("C0", shader.constant0), ("C1", shader.constant1)):
        for index, color in enumerate(values):
            if color:
                lines.append(f"{label}[{index}] #{color:08X}")
    if shader.final_constant0:
        lines.append(f"final C0 #{shader.final_constant0:08X}")
    if shader.final_constant1:
        lines.append(f"final C1 #{shader.final_constant1:08X}")
    return "\n".join(lines)


def list_xpu(directory: Path) -> tuple[list[tuple[Path, PixelShader]], list[Path]]:
    found: list[tuple[Path, PixelShader]] = []
    bad: list[Path] = []
    for path in sorted(directory.iterdir()):
        if not path.is_file() or path.suffix.lower() != ".xpu":
            continue
        shader = parse_xpu(path.read_bytes())
        if shader is None:
            bad.append(path)
        else:
            found.append((path, shader))
    return found, bad
