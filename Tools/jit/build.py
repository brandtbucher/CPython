"""A template JIT for CPython 3.13, based on copy-and-patch."""

import asyncio
import collections
import dataclasses
import functools
import itertools
import json
import os
import pathlib
import platform
import re
import subprocess
import sys
import tempfile
import typing

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
INCLUDE = ROOT / "Include"
INCLUDE_INTERNAL = INCLUDE / "internal"
PC = ROOT / "PC"
PYTHON = ROOT / "Python"
PYTHON_EXECUTOR_CASES_C_H = PYTHON / "executor_cases.c.h"
PYTHON_JIT_STENCILS_H = PYTHON / "jit_stencils.h"
TOOLS = ROOT / "Tools"
TOOLS_JIT = TOOLS / "jit"
TOOLS_JIT_TEMPLATE = TOOLS_JIT / "template.c"
TOOLS_JIT_TRAMPOLINE = TOOLS_JIT / "trampoline.c"

OPARGS = collections.defaultdict(
    lambda: range(1),
    {
        "BINARY_OP": range(26),
        "BUILD_CONST_KEY_MAP": range(4),
        "BUILD_LIST": range(6),
        "BUILD_MAP": range(4),
        "BUILD_SET": range(3),
        "BUILD_SLICE": range(2, 4),
        "BUILD_STRING": range(9),
        "BUILD_TUPLE": range(11),
        "CALL_INTRINSIC_1": range(12),
        "CALL_NO_KW_BUILTIN_FAST": range(4),
        "CALL_NO_KW_BUILTIN_O": range(1, 2),
        "CALL_NO_KW_ISINSTANCE": range(2, 3),
        "CALL_NO_KW_LEN": range(1, 2),
        "CALL_NO_KW_METHOD_DESCRIPTOR_FAST": range(3),
        "CALL_NO_KW_METHOD_DESCRIPTOR_O": range(1, 2),
        "CALL_NO_KW_STR_1": range(1, 2),
        "CALL_NO_KW_TUPLE_1": range(1, 2),
        "CALL_NO_KW_TYPE_1": range(1, 2),
        "COMPARE_OP": [2, 18, 42, 58, 72, 88, 103, 119, 132, 148, 172, 188],
        "COMPARE_OP_FLOAT": [2, 18, 42, 58, 72, 88, 103, 119, 132, 148, 172, 188],
        "COMPARE_OP_INT": [2, 18, 42, 58, 72, 88, 103, 119, 132, 148, 172, 188],
        "COMPARE_OP_STR": [72, 88, 103, 119],
        "CONTAINS_OP": range(2),
        "CONVERT_VALUE": range(1, 4),
        "COPY": range(1, 3),
        "COPY_FREE_VARS": range(13),
        "DELETE_ATTR": range(8),
        "DICT_MERGE": range(1, 2),
        "IS_OP": range(2),
        "LIST_APPEND": range(1, 5),
        "LIST_EXTEND": range(1, 2),
        "LOAD_ATTR": range(98),
        "LOAD_CONST": range(185),
        "LOAD_DEREF": range(34),
        "LOAD_FAST": range(260),
        "LOAD_FAST_AND_CLEAR": range(18),
        "LOAD_FAST_CHECK": range(260),
        "LOAD_GLOBAL": range(140),
        "LOAD_SUPER_ATTR_METHOD": range(52),
        "MAP_ADD": range(1, 4),
        "SET_ADD": range(1, 4),
        "SET_FUNCTION_ATTRIBUTE": [1, 2, 4, 8],
        "STORE_ATTR": range(31),
        "STORE_DEREF": range(28),
        "STORE_FAST": range(260),
        "STORE_NAME": range(172),
        "SWAP": range(2, 4),
        "UNPACK_EX": range(257),  # XXX
        "UNPACK_SEQUENCE": range(8),
        "UNPACK_SEQUENCE_LIST": range(5),
        "UNPACK_SEQUENCE_TUPLE": range(13),
        "UNPACK_SEQUENCE_TWO_TUPLE": range(2, 3),
        "_CHECK_CALL_BOUND_METHOD_EXACT_ARGS": range(4),
        "_CHECK_FUNCTION_EXACT_ARGS": range(11),
        "_CHECK_STACK_SPACE": range(11),
        "_INIT_CALL_BOUND_METHOD_EXACT_ARGS": range(4),
        "_INIT_CALL_PY_EXACT_ARGS": range(11),
        "_LOAD_ATTR_INSTANCE_VALUE": range(75),
        "_LOAD_FROM_DICT_OR_GLOBALS": range(173),
        "_LOAD_GLOBAL_BUILTINS": range(140),
        "_LOAD_GLOBAL_MODULE": range(133),
        "_POP_JUMP_IF_FALSE": range(53),
        "_POP_JUMP_IF_TRUE": range(60),
    }
)

def batched(iterable, n):
    """Batch an iterable into lists of size n."""
    it = iter(iterable)
    while True:
        batch = list(itertools.islice(it, n))
        if not batch:
            return
        yield batch

class _Value(typing.TypedDict):
    Value: str
    RawValue: int

class Flag(typing.TypedDict):
    Name: str
    Value: int

class Flags(typing.TypedDict):
    RawFlags: int
    Flags: list[Flag]

class SectionData(typing.TypedDict):
    Offset: int
    Bytes: list[int]

class _Name(typing.TypedDict):
    Value: str
    Offset: int
    Bytes: list[int]

class ELFRelocation(typing.TypedDict):
    Offset: int
    Type: _Value
    Symbol: _Value
    Addend: int

class COFFRelocation(typing.TypedDict):
    Offset: int
    Type: _Value
    Symbol: str
    SymbolIndex: int

class MachORelocation(typing.TypedDict):
    Offset: int
    PCRel: int
    Length: int
    Type: _Value
    Symbol: _Value  # XXX
    Section: _Value  # XXX

class COFFAuxSectionDef(typing.TypedDict):
    Length: int
    RelocationCount: int
    LineNumberCount: int
    Checksum: int
    Number: int
    Selection: int

class COFFSymbol(typing.TypedDict):
    Name: str
    Value: int
    Section: _Value
    BaseType: _Value
    ComplexType: _Value
    StorageClass: int
    AuxSymbolCount: int
    AuxSectionDef: COFFAuxSectionDef

class ELFSymbol(typing.TypedDict):
    Name: _Value
    Value: int
    Size: int
    Binding: _Value
    Type: _Value
    Other: int
    Section: _Value

class MachOSymbol(typing.TypedDict):
    Name: _Value
    Type: _Value
    Section: _Value
    RefType: _Value
    Flags: Flags
    Value: int

class ELFSection(typing.TypedDict):
    Index: int
    Name: _Value
    Type: _Value
    Flags: Flags
    Address: int
    Offset: int
    Size: int
    Link: int
    Info: int
    AddressAlignment: int
    EntrySize: int
    Relocations: list[dict[typing.Literal["Relocation"], ELFRelocation]]
    Symbols: list[dict[typing.Literal["Symbol"], ELFSymbol]]
    SectionData: SectionData

class COFFSection(typing.TypedDict):
    Number: int
    Name: _Name
    VirtualSize: int
    VirtualAddress: int
    RawDataSize: int
    PointerToRawData: int
    PointerToRelocations: int
    PointerToLineNumbers: int
    RelocationCount: int
    LineNumberCount: int
    Characteristics: Flags
    Relocations: list[dict[typing.Literal["Relocation"], COFFRelocation]]
    Symbols: list[dict[typing.Literal["Symbol"], COFFSymbol]]
    SectionData: SectionData  # XXX

class MachOSection(typing.TypedDict):
    Index: int
    Name: _Name
    Segment: _Name
    Address: int
    Size: int
    Offset: int
    Alignment: int
    RelocationOffset: int
    RelocationCount: int
    Type: _Value
    Attributes: Flags
    Reserved1: int
    Reserved2: int
    Reserved3: int
    Relocations: list[dict[typing.Literal["Relocation"], MachORelocation]]  # XXX
    Symbols: list[dict[typing.Literal["Symbol"], MachOSymbol]]  # XXX
    SectionData: SectionData  # XXX

S = typing.TypeVar("S", bound=str)
T = typing.TypeVar("T")


def unwrap(source: list[dict[S, T]], wrapper: S) -> list[T]:
    return [child[wrapper] for child in source]

def get_llvm_tool_version(name: str) -> int | None:
    try:
        args = [name, "--version"]
        process = subprocess.run(args, check=True, stdout=subprocess.PIPE)
    except FileNotFoundError:
        return None
    match = re.search(br"version\s+(\d+)\.\d+\.\d+\s+", process.stdout)
    return match and int(match.group(1))

def find_llvm_tool(tool: str) -> tuple[str, int]:
    versions = {14, 15, 16}
    forced_version = os.getenv("PYTHON_LLVM_VERSION")
    if forced_version:
        versions &= {int(forced_version)}
    # Unversioned executables:
    path = tool
    version = get_llvm_tool_version(tool)
    if version in versions:
        return tool, version
    for version in sorted(versions, reverse=True):
        # Versioned executables:
        path = f"{tool}-{version}"
        if get_llvm_tool_version(path) == version:
            return path, version
        # My homebrew homies:
        try:
            args = ["brew", "--prefix", f"llvm@{version}"]
            process = subprocess.run(args, check=True, stdout=subprocess.PIPE)
        except (FileNotFoundError, subprocess.CalledProcessError):
            pass
        else:
            prefix = process.stdout.decode().removesuffix("\n")
            path = f"{prefix}/bin/{tool}"
            if get_llvm_tool_version(path) == version:
                return path, version
    raise RuntimeError(f"Can't find {tool}!")

# TODO: Divide into read-only data and writable/executable text.

async def run(*args, **kwargs) -> tuple[bytes | None, bytes | None]:
    process = await asyncio.create_subprocess_exec(*args, **kwargs)
    stdout, stderr = await process.communicate()
    if process.returncode:
        raise RuntimeError(f"{args[0]} exited with {process.returncode}")
    return stdout, stderr

class ObjectParser:

    _ARGS = [
        # "--demangle",
        "--elf-output-style=JSON",
        "--expand-relocs",
        "--pretty-print",
        "--section-data",
        "--section-relocations",
        "--section-symbols",
        "--sections",
    ]

    def __init__(self, path: pathlib.Path, reader: str, symbol_prefix: str = "") -> None:
        self.path = path
        self.body = bytearray()
        self.body_symbols = {}
        self.body_offsets = {}
        self.relocations = {}
        self.dupes = set()
        self.got_entries = []
        self.relocations_todo = []
        self.symbol_prefix = symbol_prefix
        self.reader = reader

    async def parse(self):
        # subprocess.run([find_llvm_tool("llvm-objdump")[0], self.path, "-dr"], check=True)  # XXX
        stdout, stderr = await run(self.reader, *self._ARGS, self.path, stdout=subprocess.PIPE)
        assert stdout is not None, stdout
        assert stderr is None, stderr
        output = stdout
        output = output.replace(b"PrivateExtern\n", b"\n")  # XXX: MachO
        output = output.replace(b"Extern\n", b"\n")  # XXX: MachO
        start = output.index(b"[", 1)  # XXX: MachO, COFF
        end = output.rindex(b"]", 0, -1) + 1  # XXX: MachO, COFF
        self._data = json.loads(output[start:end])
        for section in unwrap(self._data, "Section"):
            self._handle_section(section)
        # if "_jit_entry" in self.body_symbols:
        #     entry = self.body_symbols["_jit_entry"]
        # else:
        #     entry = self.body_symbols["_jit_trampoline"]
        entry = 0  # XXX
        holes = []
        while len(self.body) % 8:
            self.body.append(0)
        got = len(self.body)
        for newhole in handle_relocations(self.got_entries, self.body, self.relocations_todo):
            assert newhole.symbol not in self.dupes, newhole.symbol
            if newhole.symbol in self.body_symbols:
                addend = newhole.addend + self.body_symbols[newhole.symbol] - entry
                newhole = Hole(newhole.kind, "_jit_base", newhole.offset, addend)
            holes.append(newhole)
        for i, (got_symbol, addend) in enumerate(self.got_entries):
            if got_symbol in self.body_symbols:
                holes.append(Hole("PATCH_ABS_64", "_jit_base", got + 8 * i, self.body_symbols[got_symbol] + addend))
                continue
            # XXX: PATCH_ABS_32 on 32-bit platforms?
            holes.append(Hole("PATCH_ABS_64", got_symbol, got + 8 * i, addend))
        self.body.extend([0] * 8 * len(self.got_entries))
        while len(self.body) % 16:
            self.body.append(0)
        holes.sort(key=lambda hole: hole.offset)
        return Stencil(bytes(self.body)[entry:], tuple(holes))  # XXX

@dataclasses.dataclass(frozen=True)
class Hole:
    kind: str  # XXX: Enum
    symbol: str
    offset: int
    addend: int

@dataclasses.dataclass(frozen=True)
class Stencil:
    body: bytes
    holes: tuple[Hole, ...]
    # entry: int

def sign_extend_64(value: int, bits: int) -> int:
    """Sign-extend a value to 64 bits."""
    assert 0 <= value < (1 << bits) < (1 << 64)
    return value - ((value & (1 << (bits - 1))) << 1)

def handle_relocations(
    got_entries: list[tuple[str, int]],
    body: bytearray,
    relocations: typing.Sequence[tuple[int, typing.Mapping[str, typing.Any]]],
) -> typing.Generator[Hole, None, None]:
    for base, relocation in relocations:
        match relocation:
            # aarch64-apple-darwin:
            case {
                "Length": 2 as length,
                "Offset": int(offset),
                "PCRel": 1 as pcrel,
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "ARM64_RELOC_BRANCH26"},
            }:
                offset += base
                where = slice(offset, offset + (1 << length))
                what = int.from_bytes(body[where], "little", signed=False)
                # XXX: This nonsense...
                assert what & 0xFC000000 == 0x14000000 or what & 0xFC000000 == 0x94000000, what
                addend = (what & 0x03FFFFFF) << 2
                addend = sign_extend_64(addend, 28)
                assert symbol.startswith("_"), symbol
                symbol = symbol.removeprefix("_")
                yield Hole("PATCH_REL_26", symbol, offset, addend)
            case {
                "Length": 2 as length,
                "Offset": int(offset),
                "PCRel": 1 as pcrel,
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "ARM64_RELOC_GOT_LOAD_PAGE21"},
            }:
                offset += base
                where = slice(offset, offset + (1 << length))
                what = int.from_bytes(body[where], "little", signed=False)
                # XXX: This nonsense...
                assert what & 0x9F000000 == 0x90000000, what
                addend = ((what & 0x60000000) >> 29) | ((what & 0x01FFFFE0) >> 3) << 12
                addend = sign_extend_64(addend, 33)
                # assert symbol.startswith("_"), symbol
                symbol = symbol.removeprefix("_")
                if (symbol, addend) not in got_entries:
                    got_entries.append((symbol, addend))
                addend = len(body) + got_entries.index((symbol, addend)) * 8
                yield Hole("PATCH_REL_21", "_jit_base", offset, addend)
            case {
                "Length": 2 as length,
                "Offset": int(offset),
                "PCRel": 0 as pcrel,
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "ARM64_RELOC_GOT_LOAD_PAGEOFF12"},
            }:
                offset += base
                where = slice(offset, offset + (1 << length))
                what = int.from_bytes(body[where], "little", signed=False)
                # XXX: This nonsense...
                assert what & 0x3B000000 == 0x39000000, what
                addend = (what & 0x003FFC00) >> 10
                implicit_shift = 0
                if what & 0x3B000000 == 0x39000000:
                    implicit_shift = (what >> 30) & 0x3
                    if implicit_shift == 0:
                        if what & 0x04800000 == 0x04800000:
                            implicit_shift = 4
                addend <<= implicit_shift
                # assert symbol.startswith("_"), symbol
                symbol = symbol.removeprefix("_")
                if (symbol, addend) not in got_entries:
                    got_entries.append((symbol, addend))
                addend = len(body) + got_entries.index((symbol, addend)) * 8
                yield Hole("PATCH_ABS_12", "_jit_base", offset, addend)
            case {
                "Length": 2 as length,
                "Offset": int(offset),
                "PCRel": 1 as pcrel,
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "ARM64_RELOC_PAGE21"},
            }:
                offset += base
                where = slice(offset, offset + (1 << length))
                what = int.from_bytes(body[where], "little", signed=False)
                # XXX: This nonsense...
                assert what & 0x9F000000 == 0x90000000, what
                addend = ((what & 0x60000000) >> 29) | ((what & 0x01FFFFE0) >> 3) << 12
                addend = sign_extend_64(addend, 33)
                # assert symbol.startswith("_"), symbol
                symbol = symbol.removeprefix("_")
                yield Hole("PATCH_REL_21", symbol, offset, addend)
            case {
                "Length": 2 as length,
                "Offset": int(offset),
                "PCRel": 0 as pcrel,
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "ARM64_RELOC_PAGEOFF12"},
            }:
                offset += base
                where = slice(offset, offset + (1 << length))
                what = int.from_bytes(body[where], "little", signed=False)
                # XXX: This nonsense...
                assert what & 0x3B000000 == 0x39000000 or what & 0x11C00000 == 0x11000000, what
                addend = (what & 0x003FFC00) >> 10
                implicit_shift = 0
                if what & 0x3B000000 == 0x39000000:
                    implicit_shift = (what >> 30) & 0x3
                    if implicit_shift == 0:
                        if what & 0x04800000 == 0x04800000:
                            implicit_shift = 4
                addend <<= implicit_shift
                # assert symbol.startswith("_"), symbol
                symbol = symbol.removeprefix("_")
                yield Hole("PATCH_ABS_12", symbol, offset, addend)
            case {
                "Length": 3 as length,
                "Offset": int(offset),
                "PCRel": 0 as pcrel,
                "Section": {"Value": str(section)},
                "Type": {"Value": "ARM64_RELOC_UNSIGNED"},
            }:
                offset += base
                where = slice(offset, offset + (1 << length))
                what = int.from_bytes(body[where], "little", signed=False)
                addend = what
                assert section.startswith("_"), section
                section = section.removeprefix("_")
                yield Hole("PATCH_ABS_64", section, offset, addend)
            case {
                "Length": 3 as length,
                "Offset": int(offset),
                "PCRel": 0 as pcrel,
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "ARM64_RELOC_UNSIGNED"},
            }:
                offset += base
                where = slice(offset, offset + (1 << length))
                what = int.from_bytes(body[where], "little", signed=False)
                addend = what
                assert symbol.startswith("_"), symbol
                symbol = symbol.removeprefix("_")
                yield Hole("PATCH_ABS_64", symbol, offset, addend)
            # x86_64-pc-windows-msvc:
            case {
                "Offset": int(offset),
                "Symbol": str(symbol),
                "Type": {"Value": "IMAGE_REL_AMD64_ADDR64"},
            }:
                offset += base
                where = slice(offset, offset + 8)
                what = int.from_bytes(body[where], sys.byteorder)
                # assert not what, what
                addend = what
                body[where] = [0] * 8
                yield Hole("PATCH_ABS_64", symbol, offset, addend)
            # i686-pc-windows-msvc:
            case {
                "Offset": int(offset),
                "Symbol": str(symbol),
                "Type": {"Value": "IMAGE_REL_I386_DIR32"},
            }:
                offset += base
                where = slice(offset, offset + 4)
                what = int.from_bytes(body[where], sys.byteorder)
                # assert not what, what
                addend = what
                body[where] = [0] * 4
                # assert symbol.startswith("_")
                symbol = symbol.removeprefix("_")
                yield Hole("PATCH_ABS_32", symbol, offset, addend)
            # aarch64-unknown-linux-gnu:
            case {
                "Addend": int(addend),
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_AARCH64_ABS64"},
            }:
                offset += base
                where = slice(offset, offset + 8)
                what = int.from_bytes(body[where], sys.byteorder)
                assert not what, what
                yield Hole("PATCH_ABS_64", symbol, offset, addend)
            case {
                "Addend": 0,
                "Offset": int(offset),
                "Symbol": {'Value': str(symbol)},
                "Type": {"Value": "R_AARCH64_ADR_GOT_PAGE"},
            }:
                offset += base
                where = slice(offset, offset + 4)
                what = int.from_bytes(body[where], "little", signed=False)
                # XXX: This nonsense...
                assert what & 0x9F000000 == 0x90000000, what
                addend = ((what & 0x60000000) >> 29) | ((what & 0x01FFFFE0) >> 3) << 12
                addend = sign_extend_64(addend, 33)
                if (symbol, addend) not in got_entries:
                    got_entries.append((symbol, addend))
                addend = len(body) + got_entries.index((symbol, addend)) * 8
                yield Hole("PATCH_REL_21", "_jit_base", offset, addend)
            case {
                "Addend": 0,
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_AARCH64_CALL26" | "R_AARCH64_JUMP26"},  # XXX
            }:
                offset += base
                where = slice(offset, offset + 4)
                what = int.from_bytes(body[where], "little", signed=False)
                # XXX: This nonsense...
                assert what & 0xFC000000 == 0x14000000 or what & 0xFC000000 == 0x94000000, what
                addend = (what & 0x03FFFFFF) << 2
                addend = sign_extend_64(addend, 28)
                yield Hole("PATCH_REL_26", symbol, offset, addend)
            case {
                "Addend": 0,
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_AARCH64_LD64_GOT_LO12_NC"},
            }:
                offset += base
                where = slice(offset, offset + 4)
                what = int.from_bytes(body[where], "little", signed=False)
                # XXX: This nonsense...
                assert what & 0x3B000000 == 0x39000000, what
                addend = (what & 0x003FFC00) >> 10
                implicit_shift = 0
                if what & 0x3B000000 == 0x39000000:
                    implicit_shift = (what >> 30) & 0x3
                    if implicit_shift == 0:
                        if what & 0x04800000 == 0x04800000:
                            implicit_shift = 4
                addend <<= implicit_shift
                if (symbol, addend) not in got_entries:
                    got_entries.append((symbol, addend))
                addend = len(body) + got_entries.index((symbol, addend)) * 8
                yield Hole("PATCH_ABS_12", "_jit_base", offset, addend)
            case {
                "Addend": int(addend),
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_AARCH64_MOVW_UABS_G0_NC"},
            }:
                offset += base
                where = slice(offset, offset + 4)
                what = int.from_bytes(body[where], "little", signed=False)
                assert ((what >> 5) & 0xFFFF) == 0, what
                yield Hole("PATCH_ABS_16_A", symbol, offset, addend)
            case {
                "Addend": int(addend),
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_AARCH64_MOVW_UABS_G1_NC"},
            }:
                offset += base
                where = slice(offset, offset + 4)
                what = int.from_bytes(body[where], "little", signed=False)
                assert ((what >> 5) & 0xFFFF) == 0, what
                yield Hole("PATCH_ABS_16_B", symbol, offset, addend)
            case {
                "Addend": int(addend),
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_AARCH64_MOVW_UABS_G2_NC"},
            }:
                offset += base
                where = slice(offset, offset + 4)
                what = int.from_bytes(body[where], "little", signed=False)
                assert ((what >> 5) & 0xFFFF) == 0, what
                yield Hole("PATCH_ABS_16_C", symbol, offset, addend)
            case {
                "Addend": int(addend),
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_AARCH64_MOVW_UABS_G3"},
            }:
                offset += base
                where = slice(offset, offset + 4)
                what = int.from_bytes(body[where], "little", signed=False)
                assert ((what >> 5) & 0xFFFF) == 0, what
                yield Hole("PATCH_ABS_16_D", symbol, offset, addend)
            # x86_64-unknown-linux-gnu:
            case {
                "Addend": int(addend),
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_X86_64_64"},
            }:
                offset += base
                where = slice(offset, offset + 8)
                what = int.from_bytes(body[where], sys.byteorder)
                assert not what, what
                yield Hole("PATCH_ABS_64", symbol, offset, addend)
            case {
                "Addend": int(addend),
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_X86_64_GOT64"},
            }:
                offset += base
                where = slice(offset, offset + 8)
                what = int.from_bytes(body[where], sys.byteorder)
                assert not what, what
                if (symbol, addend) not in got_entries:
                    got_entries.append((symbol, addend))
                addend = got_entries.index((symbol, addend)) * 8
                body[where] = addend.to_bytes(8, sys.byteorder)
            case {
                "Addend": int(addend),
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_X86_64_GOTOFF64"},
            }:
                offset += base
                where = slice(offset, offset + 8)
                what = int.from_bytes(body[where], sys.byteorder)
                assert not what, what
                addend += offset - len(body)
                yield Hole("PATCH_REL_64", symbol, offset, addend)
            case {
                "Addend": int(addend),
                "Offset": int(offset),
                "Symbol": {"Value": "_GLOBAL_OFFSET_TABLE_"},
                "Type": {"Value": "R_X86_64_GOTPC64"},
            }:
                offset += base
                where = slice(offset, offset + 8)
                what = int.from_bytes(body[where], sys.byteorder)
                assert not what, what
                addend += len(body) - offset
                body[where] = addend.to_bytes(8, sys.byteorder)
            case {
                "Addend": int(addend),
                "Offset": int(offset),
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "R_X86_64_PC32"},
            }:
                offset += base
                where = slice(offset, offset + 4)
                what = int.from_bytes(body[where], sys.byteorder)
                assert not what, what
                yield Hole("PATCH_REL_32", symbol, offset, addend)
            # x86_64-apple-darwin:
            case {
                "Length": 2,
                "Offset": int(offset),
                "PCRel": 1,
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "X86_64_RELOC_GOT_LOAD"},
            }:
                offset += base
                where = slice(offset, offset + 4)
                what = int.from_bytes(body[where], "little", signed=False)
                assert not what, what
                addend = what
                body[where] = [0] * 4
                assert symbol.startswith("_"), symbol
                symbol = symbol.removeprefix("_")
                if (symbol, addend) not in got_entries:
                    got_entries.append((symbol, addend))
                addend = len(body) + got_entries.index((symbol, addend)) * 8 - offset - 4
                body[where] = addend.to_bytes(4, sys.byteorder)
            case {
                "Length": 3,
                "Offset": int(offset),
                "PCRel": 0,
                "Section": {"Value": str(section)},
                "Type": {"Value": "X86_64_RELOC_UNSIGNED"},
            }:
                offset += base
                where = slice(offset, offset + 8)
                what = int.from_bytes(body[where], sys.byteorder)
                # assert not what, what
                addend = what
                body[where] = [0] * 8
                assert section.startswith("_")
                section = section.removeprefix("_")
                yield Hole("PATCH_ABS_64", section, offset, addend)
            case {
                "Length": 3,
                "Offset": int(offset),
                "PCRel": 0,
                "Symbol": {"Value": str(symbol)},
                "Type": {"Value": "X86_64_RELOC_UNSIGNED"},
            }:
                offset += base
                where = slice(offset, offset + 8)
                what = int.from_bytes(body[where], sys.byteorder)
                # assert not what, what
                addend = what
                body[where] = [0] * 8
                assert symbol.startswith("_")
                symbol = symbol.removeprefix("_")
                yield Hole("PATCH_ABS_64", symbol, offset, addend)
            case _:
                raise NotImplementedError(relocation)


class ObjectParserCOFF(ObjectParser):

    def _handle_section(self, section: COFFSection) -> None:
        flags = {flag["Name"] for flag in section["Characteristics"]["Flags"]}
        if "SectionData" not in section:
            return
        if flags & {"IMAGE_SCN_LINK_COMDAT", "IMAGE_SCN_MEM_EXECUTE", "IMAGE_SCN_MEM_READ", "IMAGE_SCN_MEM_WRITE"} == {"IMAGE_SCN_LINK_COMDAT", "IMAGE_SCN_MEM_READ"}:
            # XXX: Merge these
            before = self.body_offsets[section["Number"]] = len(self.body)
            section_data = section["SectionData"]
            self.body.extend(section_data["Bytes"])
        elif flags & {"IMAGE_SCN_MEM_READ"} == {"IMAGE_SCN_MEM_READ"}:
            before = self.body_offsets[section["Number"]] = len(self.body)
            section_data = section["SectionData"]
            self.body.extend(section_data["Bytes"])
        else:
            return
        for symbol in unwrap(section["Symbols"], "Symbol"):
            offset = before + symbol["Value"]
            name = symbol["Name"]
            # assert name.startswith("_")  # XXX
            name = name.removeprefix(self.symbol_prefix)  # XXX
            if name in self.body_symbols:
                self.dupes.add(name)
            self.body_symbols[name] = offset
        for relocation in unwrap(section["Relocations"], "Relocation"):
            self.relocations_todo.append((before, relocation))

class ObjectParserMachO(ObjectParser):

    def _handle_section(self, section: MachOSection) -> None:
        assert section["Address"] >= len(self.body)
        self.body.extend([0] * (section["Address"] - len(self.body)))
        before = self.body_offsets[section["Index"]] = section["Address"]
        section_data = section["SectionData"]
        self.body.extend(section_data["Bytes"])
        name = section["Name"]["Value"]
        # assert name.startswith("_")  # XXX
        name = name.removeprefix(self.symbol_prefix)  # XXX
        if name == "_eh_frame":
            return
        if name in self.body_symbols:
            self.dupes.add(name)
        self.body_symbols[name] = 0  # before
        for symbol in unwrap(section["Symbols"], "Symbol"):
            offset = symbol["Value"]
            name = symbol["Name"]["Value"]
            # assert name.startswith("_")  # XXX
            name = name.removeprefix(self.symbol_prefix)  # XXX
            if name in self.body_symbols:
                self.dupes.add(name)
            self.body_symbols[name] = offset
        for relocation in unwrap(section["Relocations"], "Relocation"):
            self.relocations_todo.append((before, relocation))


class ObjectParserELF(ObjectParser):

    def _handle_section(self, section: ELFSection) -> None:
        type = section["Type"]["Value"]
        flags = {flag["Name"] for flag in section["Flags"]["Flags"]}
        if type == "SHT_RELA":
            assert "SHF_INFO_LINK" in flags, flags
            before = self.body_offsets[section["Info"]]
            assert not section["Symbols"]
            for relocation in unwrap(section["Relocations"], "Relocation"):
                self.relocations_todo.append((before, relocation))
        elif type == "SHT_PROGBITS":
            before = self.body_offsets[section["Index"]] = len(self.body)
            if "SHF_ALLOC" not in flags:
                return
            elif flags & {"SHF_EXECINSTR", "SHF_MERGE", "SHF_WRITE"} == {"SHF_MERGE"}:
                # XXX: Merge these
                section_data = section["SectionData"]
                self.body.extend(section_data["Bytes"])
            else:
                section_data = section["SectionData"]
                self.body.extend(section_data["Bytes"])
            assert not section["Relocations"]
            for symbol in unwrap(section["Symbols"], "Symbol"):
                offset = before + symbol["Value"]
                name = symbol["Name"]["Value"]
                # assert name.startswith("_")  # XXX
                name = name.removeprefix(self.symbol_prefix)  # XXX
                assert name not in self.body_symbols
                self.body_symbols[name] = offset
        else:
            assert type in {"SHT_LLVM_ADDRSIG", "SHT_NULL", "SHT_STRTAB", "SHT_SYMTAB"}, type


CFLAGS = [
    f"-DNDEBUG",  # XXX
    f"-DPy_BUILD_CORE",
    f"-D_PyJIT_ACTIVE",
    f"-I{ROOT}",  # XXX
    f"-I{INCLUDE}",
    f"-I{INCLUDE_INTERNAL}",
    f"-I{PC}",  # XXX
    f"-O3",
    f"-Wno-unreachable-code",
    f"-Wno-unused-but-set-variable",
    f"-Wno-unused-command-line-argument",
    f"-Wno-unused-label",
    f"-Wno-unused-variable",
    # Keep library calls from sneaking in:
    f"-ffreestanding",  # XXX
    # We don't need this (and it causes weird relocations):
    f"-fno-asynchronous-unwind-tables",  # XXX
    # # Don't need the overhead of position-independent code, if posssible:
    # "-fno-pic",
    # # Disable stack-smashing canaries, which use magic symbols:
    # f"-fno-stack-protector",  # XXX
    # The GHC calling convention uses %rbp as an argument-passing register:
    f"-fomit-frame-pointer",  # XXX
    # # Disable debug info:
    # f"-g0",  # XXX
    # Need this to leave room for patching our 64-bit pointers:
    f"-mcmodel=large",  # XXX
]

if sys.platform == "darwin":
    ObjectParserDefault = functools.partial(ObjectParserMachO, symbol_prefix="_")  # XXX
elif sys.platform == "linux":
    ObjectParserDefault = ObjectParserELF
elif sys.platform == "win32":
    assert sys.argv[1] == "--windows", sys.argv[1]
    if sys.argv[2] == "Debug|Win32":
        ObjectParserDefault = functools.partial(ObjectParserCOFF, symbol_prefix="_")  # XXX
        CFLAGS += ["-D_DEBUG", "-m32"]
    elif sys.argv[2] == "Debug|x64":
        ObjectParserDefault = ObjectParserCOFF
        CFLAGS += ["-D_DEBUG"]
    elif sys.argv[2] in {"PGInstrument|Win32", "PGUpdate|Win32", "Release|Win32"}:
        ObjectParserDefault = functools.partial(ObjectParserCOFF, symbol_prefix="_")  # XXX
        # CFLAGS += ["-DNDEBUG", "-m32"]  # XXX
        CFLAGS += ["-m32"]
    elif sys.argv[2] in {"PGInstrument|x64", "PGUpdate|x64", "Release|x64"}:
        ObjectParserDefault = ObjectParserCOFF
        # CFLAGS += ["-DNDEBUG"]  # XXX
        pass
    else:
        assert False, sys.argv[2]
else:
    raise NotImplementedError(sys.platform)

class Compiler:

    def __init__(
        self,
        *,
        verbose: bool = False,
        jobs: int = os.cpu_count() or 1,
        ghccc: bool = True,
    )-> None:
        self._stencils_built = {}
        self._verbose = verbose
        self._clang, clang_version = find_llvm_tool("clang")
        self._readobj, readobj_version = find_llvm_tool("llvm-readobj")
        self._stderr(f"Using {self._clang} ({clang_version}) and {self._readobj} ({readobj_version}).")
        self._semaphore = asyncio.BoundedSemaphore(jobs)
        self._ghccc = ghccc

    def _stderr(self, *args, **kwargs) -> None:
        if self._verbose:
            print(*args, **kwargs, file=sys.stderr)

    def _use_ghccc(self, ll: pathlib.Path) -> None:
        if self._ghccc:
            ir = before = ll.read_text()
            for name in ["_jit_branch", "_jit_continue", "_jit_entry", "_jit_loop"]:
                for ptr in ["ptr", "%struct._PyInterpreterFrame*"]:
                    ir = ir.replace(f"{ptr} @{name}", f"ghccc {ptr} @{name}")
            assert ir != before, ir
            ll.write_text(ir)

    async def _compile(self, opname, oparg, c) -> None:
        defines = [f"-D_JIT_OPCODE={opname}", f"-D_JIT_OPARG={oparg}"]
        with tempfile.TemporaryDirectory() as tempdir:
            ll = pathlib.Path(tempdir, f"{opname}_{oparg}.ll").resolve()
            o = pathlib.Path(tempdir, f"{opname}_{oparg}.o").resolve()
            async with self._semaphore:
                self._stderr(f"Compiling {opname}({oparg})...")
                stdout, stderr = await run(self._clang, *CFLAGS, "-emit-llvm", "-S", *defines, "-o", ll, c)
                assert stdout is None, stdout
                assert stderr is None, stderr
                self._use_ghccc(ll)
                self._stderr(f"Recompiling {opname}({oparg})...")
                stdout, stderr = await run(self._clang, *CFLAGS, "-c", "-o", o, ll)
                assert stdout is None, stdout
                assert stderr is None, stderr
                self._stderr(f"Parsing {opname}({oparg})...")
                self._stencils_built[opname, oparg] = await ObjectParserDefault(o, self._readobj).parse()
        self._stderr(f"Built {opname}({oparg})!")

    async def build(self) -> None:
        generated_cases = PYTHON_EXECUTOR_CASES_C_H.read_text()
        opnames = sorted(re.findall(r"\n {8}case (\w+): \{\n", generated_cases))
        await asyncio.gather(
            self._compile("trampoline", 0, TOOLS_JIT_TRAMPOLINE),
            *[self._compile(opname, oparg, TOOLS_JIT_TEMPLATE) for opname in opnames for oparg in OPARGS[opname]],
        )
        self._stderr(f"Built {sum(map(len, OPARGS.values()))} stencils ({len({stencil.body for stencil in self._stencils_built.values()})} unique)!")

    def dump(self) -> str:
        # XXX: Rework these to use Enums:
        kinds = {
            "PATCH_ABS_12",
            "PATCH_ABS_16_A",
            "PATCH_ABS_16_B",
            "PATCH_ABS_16_C",
            "PATCH_ABS_16_D",
            "PATCH_ABS_32",
            "PATCH_ABS_64",
            "PATCH_REL_21",
            "PATCH_REL_26",
            "PATCH_REL_32",
            "PATCH_REL_64",
        }
        values = {
            "HOLE_base",
            "HOLE_branch",
            "HOLE_continue",
            "HOLE_loop",
            "HOLE_next_trace",
            "HOLE_operand_plus_one",
        }
        lines = []
        lines.append(f"// Don't be scared... this entire file is generated by {__file__}!")
        lines.append(f"")
        lines.append(f"#define OPARG_MAX {max(map(max, filter(None, OPARGS.values())))}")  # XXX
        lines.append(f"")
        lines.append(f"typedef enum {{")
        for kind in sorted(kinds):
            lines.append(f"    {kind},")
        lines.append(f"}} HoleKind;")
        lines.append(f"")
        lines.append(f"typedef enum {{")
        for value in sorted(values):
            lines.append(f"    {value},")
        lines.append(f"}} HoleValue;")
        lines.append(f"")
        lines.append(f"typedef struct {{")
        lines.append(f"    const HoleKind kind;")
        lines.append(f"    const uintptr_t offset;")
        lines.append(f"    const uintptr_t addend;")
        lines.append(f"    const HoleValue value;")
        lines.append(f"}} Hole;")
        lines.append(f"")
        lines.append(f"typedef struct {{")
        lines.append(f"    const HoleKind kind;")
        lines.append(f"    const uintptr_t offset;")
        lines.append(f"    const uintptr_t addend;")
        lines.append(f"    const int symbol;")
        lines.append(f"}} SymbolLoad;")
        lines.append(f"")
        lines.append(f"typedef struct {{")
        lines.append(f"    const size_t nbytes;")
        lines.append(f"    const unsigned char * const bytes;")
        lines.append(f"    const size_t nholes;")
        lines.append(f"    const Hole * const holes;")
        lines.append(f"    const size_t nloads;")
        lines.append(f"    const SymbolLoad * const loads;")
        lines.append(f"}} Stencil;")
        lines.append(f"")
        opnames = collections.defaultdict(set)
        symbols = set()
        for stencil in self._stencils_built.values():
            for hole in stencil.holes:
                if not hole.symbol.startswith("_jit_"):
                    symbols.add(hole.symbol)
        symbols = sorted(symbols)
        for (opname, oparg), stencil in sorted(self._stencils_built.items()):
            opnames[opname].add(oparg)
            lines.append(f"// {opname}({oparg})")
            assert stencil.body, (opname, oparg)
            lines.append(f"static const unsigned char {opname}_{oparg}_stencil_bytes[] = {{")
            for chunk in batched(stencil.body, 8):
                lines.append(f"    {', '.join(f'0x{byte:02X}' for byte in chunk)},")
            lines.append(f"}};")
            holes = []
            loads = []
            for hole in stencil.holes:
                assert hole.kind in kinds, hole.kind
                if hole.symbol.startswith("_jit_"):
                    value = f"HOLE_{hole.symbol.removeprefix('_jit_')}"
                    assert value in values, value
                    holes.append(f"    {{.kind = {hole.kind}, .offset = {hole.offset:4}, .addend = {hole.addend % (1 << 64):4}, .value = {value}}},")
                else:
                    loads.append(f"    {{.kind = {hole.kind}, .offset = {hole.offset:4}, .addend = {hole.addend % (1 << 64):4}, .symbol = {symbols.index(hole.symbol):3}}},  // {hole.symbol}")
            lines.append(f"static const Hole {opname}_{oparg}_stencil_holes[] = {{")
            for hole in holes:
                lines.append(hole)
            lines.append(f"    {{.kind =            0, .offset =    0, .addend =    0, .value = 0}},")
            lines.append(f"}};")
            lines.append(f"static const SymbolLoad {opname}_{oparg}_stencil_loads[] = {{")
            for  load in loads:
                lines.append(load)
            lines.append(f"    {{.kind =            0, .offset =    0, .addend =    0, .symbol =   0}},")
            lines.append(f"}};")
            lines.append(f"")
        lines.append(f"")
        lines.append(f"static const char *const symbols[] = {{")
        for symbol in symbols:
            lines.append(f"    \"{symbol}\",")
        lines.append(f"}};")
        lines.append(f"")
        lines.append(f"static uintptr_t symbol_addresses[{len(symbols)}];")
        lines.append(f"")
        lines.append(f"#define INIT_STENCIL(OPCODE, OPARG) {{                                \\")
        lines.append(f"    .nbytes = Py_ARRAY_LENGTH(OPCODE##_##OPARG##_stencil_bytes),     \\")
        lines.append(f"    .bytes = OPCODE##_##OPARG##_stencil_bytes,                       \\")
        lines.append(f"    .nholes = Py_ARRAY_LENGTH(OPCODE##_##OPARG##_stencil_holes) - 1, \\")
        lines.append(f"    .holes = OPCODE##_##OPARG##_stencil_holes,                       \\")
        lines.append(f"    .nloads = Py_ARRAY_LENGTH(OPCODE##_##OPARG##_stencil_loads) - 1, \\")
        lines.append(f"    .loads = OPCODE##_##OPARG##_stencil_loads,                       \\")
        lines.append(f"}}")
        lines.append(f"")
        lines.append(f"static const Stencil trampoline_stencil = INIT_STENCIL(trampoline, 0);")
        lines.append(f"")
        lines.append(f"static const Stencil stencils[512][OPARG_MAX + 1] = {{")
        del opnames["trampoline"]
        for opname, opargs in opnames.items():
            lines.append(f"    [{opname}] = {{")
            for oparg in sorted(opargs):
                lines.append(f"        [{oparg}] = INIT_STENCIL({opname}, {oparg}),")
            lines.append(f"    }},")
        lines.append(f"}};")
        lines.append(f"")
        lines.append(f"#define INIT_HOLE(NAME) [HOLE_##NAME] = (uintptr_t)0xBAD0BAD0BAD0BAD0")
        lines.append(f"")
        lines.append(f"#define GET_PATCHES() {{ \\")
        for value in sorted(values):
            if value.startswith("HOLE_"):
                name = value.removeprefix("HOLE_")
                lines.append(f"    INIT_HOLE({name}), \\")
        lines.append(f"}}")
        lines.append(f"")
        return "\n".join(lines)

if __name__ == "__main__":
    # Clang internal error with musttail + ghccc + aarch64:
    ghccc = platform.machine() not in {"aarch64", "arm64"}
    engine = Compiler(verbose=True, ghccc=ghccc)
    asyncio.run(engine.build())
    with PYTHON_JIT_STENCILS_H.open("w") as file:
        file.write(engine.dump())
