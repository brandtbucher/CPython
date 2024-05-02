"""Core data structures for compiled code templates."""

import dataclasses
import enum
import sys

import _schema


@enum.unique
class HoleValue(enum.Enum):
    """
    Different "base" values that can be patched into holes (usually combined with the
    address of a symbol and/or an addend).
    """

    # The base address of the machine code for the current uop (exposed as _JIT_ENTRY):
    CODE = enum.auto()
    # The base address of the machine code for the next uop (exposed as _JIT_CONTINUE):
    CONTINUE = enum.auto()
    # The base address of the read-only data for this uop:
    DATA = enum.auto()
    # The address of the current executor (exposed as _JIT_EXECUTOR):
    EXECUTOR = enum.auto()
    # The base address of the "global" offset table located in the read-only data.
    # Shouldn't be present in the final stencils, since these are all replaced with
    # equivalent DATA values:
    GOT = enum.auto()
    # The current uop's oparg (exposed as _JIT_OPARG):
    OPARG = enum.auto()
    # The current uop's operand on 64-bit platforms (exposed as _JIT_OPERAND):
    OPERAND = enum.auto()
    # The current uop's operand on 32-bit platforms (exposed as _JIT_OPERAND_HI/LO):
    OPERAND_HI = enum.auto()
    OPERAND_LO = enum.auto()
    # The current uop's target (exposed as _JIT_TARGET):
    TARGET = enum.auto()
    # The base address of the machine code for the jump target (exposed as _JIT_JUMP_TARGET):
    JUMP_TARGET = enum.auto()
    # The base address of the machine code for the error jump target (exposed as _JIT_ERROR_TARGET):
    ERROR_TARGET = enum.auto()
    # The index of the exit to be jumped through (exposed as _JIT_EXIT_INDEX):
    EXIT_INDEX = enum.auto()
    # The base address of the machine code for the first uop (exposed as _JIT_TOP):
    TOP = enum.auto()
    # A hardcoded value of zero (used for symbol lookups):
    ZERO = enum.auto()


@dataclasses.dataclass
class Hole:
    """
    A "hole" in the stencil to be patched with a computed runtime value.

    Analogous to relocation records in an object file.
    """

    offset: int
    kind: _schema.HoleKind
    # Patch with this base value:
    value: HoleValue
    # ...plus the address of this symbol:
    symbol: str | None
    # ...plus this addend:
    addend: int
    # Convenience method:
    replace = dataclasses.replace

    def as_c(self) -> str:
        """Dump this hole as an initialization of a C Hole struct."""
        parts = [
            f"{self.offset:#x}",
            f"HoleKind_{self.kind}",
            f"HoleValue_{self.value.name}",
            f"&{self.symbol}" if self.symbol else "NULL",
            f"{_signed(self.addend):#x}",
        ]
        return f"{{{', '.join(parts)}}}"


@dataclasses.dataclass
class Stencil:
    """
    A contiguous block of machine code or data to be copied-and-patched.

    Analogous to a section or segment in an object file.
    """

    body: bytearray = dataclasses.field(default_factory=bytearray, init=False)
    holes: list[Hole] = dataclasses.field(default_factory=list, init=False)
    disassembly: list[str] = dataclasses.field(default_factory=list, init=False)

    def pad(self, alignment: int) -> None:
        """Pad the stencil to the given alignment."""
        offset = len(self.body)
        padding = -offset % alignment
        if padding:
            self.disassembly.append(f"{offset:x}: {' '.join(['00'] * padding)}")
            self.body.extend([0] * padding)


@dataclasses.dataclass
class StencilGroup:
    """
    Code and data corresponding to a given micro-opcode.

    Analogous to an entire object file.
    """

    code: Stencil = dataclasses.field(default_factory=Stencil, init=False)
    data: Stencil = dataclasses.field(default_factory=Stencil, init=False)
    symbols: dict[int | str, tuple[HoleValue, int]] = dataclasses.field(
        default_factory=dict, init=False
    )

    def process_relocations(self, *, alignment: int = 1) -> None:
        """Fix up all GOT and internal relocations for this stencil group."""
        got: dict[str, int] = {}
        plt: dict[str, int] = {}
        for hole in self.code.holes.copy():
            if (
                hole.kind
                in {"R_AARCH64_CALL26", "R_AARCH64_JUMP26", "ARM64_RELOC_BRANCH26"}
                and hole.value is HoleValue.ZERO
            ):
                self.code.pad(alignment)
                self._emit_aarch64_trampoline(plt, hole)
                self.code.holes.remove(hole)
        self._remove_jump(alignment=alignment)
        self.code.pad(alignment)
        self.data.pad(8)
        for stencil in [self.code, self.data]:
            for hole in stencil.holes:
                if hole.value is HoleValue.GOT:
                    assert hole.symbol is not None
                    hole.value = HoleValue.DATA
                    hole.addend += self._got_lookup(got, hole.symbol)
                    hole.symbol = None
                elif hole.symbol in self.symbols:
                    hole.value, addend = self.symbols[hole.symbol]
                    hole.addend += addend
                    hole.symbol = None
                elif (
                    hole.kind in {"IMAGE_REL_AMD64_REL32"}
                    and hole.value is HoleValue.ZERO
                ):
                    raise ValueError(
                        f"Add PyAPI_FUNC(...) or PyAPI_DATA(...) to declaration of {hole.symbol}!"
                    )
        self._emit_got(got)
        self.code.holes.sort(key=lambda hole: hole.offset)
        self.data.holes.sort(key=lambda hole: hole.offset)

    def _emit_aarch64_trampoline(self, plt: dict[str, int], hole: Hole) -> None:
        """Even with the large code model, AArch64 Linux insists on 28-bit jumps."""
        assert hole.symbol is not None
        if hole.symbol not in plt:
            base = len(self.code.body)
            self.code.disassembly += [
                f"{base + 4 * 0:x}: d2800008      mov     x8, #0x0",
                f"{base + 4 * 0:016x}:  R_AARCH64_MOVW_UABS_G0_NC    {hole.symbol}",
                f"{base + 4 * 1:x}: f2a00008      movk    x8, #0x0, lsl #16",
                f"{base + 4 * 1:016x}:  R_AARCH64_MOVW_UABS_G1_NC    {hole.symbol}",
                f"{base + 4 * 2:x}: f2c00008      movk    x8, #0x0, lsl #32",
                f"{base + 4 * 2:016x}:  R_AARCH64_MOVW_UABS_G2_NC    {hole.symbol}",
                f"{base + 4 * 3:x}: f2e00008      movk    x8, #0x0, lsl #48",
                f"{base + 4 * 3:016x}:  R_AARCH64_MOVW_UABS_G3       {hole.symbol}",
                f"{base + 4 * 4:x}: d61f0100      br      x8",
            ]
            for code in [
                0xD2800008.to_bytes(4, sys.byteorder),
                0xF2A00008.to_bytes(4, sys.byteorder),
                0xF2C00008.to_bytes(4, sys.byteorder),
                0xF2E00008.to_bytes(4, sys.byteorder),
                0xD61F0100.to_bytes(4, sys.byteorder),
            ]:
                self.code.body.extend(code)
            for i, kind in enumerate(
                [
                    "R_AARCH64_MOVW_UABS_G0_NC",
                    "R_AARCH64_MOVW_UABS_G1_NC",
                    "R_AARCH64_MOVW_UABS_G2_NC",
                    "R_AARCH64_MOVW_UABS_G3",
                ]
            ):
                self.code.holes.append(hole.replace(offset=base + 4 * i, kind=kind))
            plt[hole.symbol] = base
        where = slice(hole.offset, hole.offset + 4)
        instruction = int.from_bytes(self.code.body[where], sys.byteorder)
        instruction &= 0xFC000000
        instruction |= ((plt[hole.symbol] - hole.offset) >> 2) & 0x03FFFFFF
        self.code.body[where] = instruction.to_bytes(4, sys.byteorder)

    def _remove_jump(self, *, alignment: int = 1) -> None:
        """Remove a zero-length continuation jump, if it exists."""
        hole = max(self.code.holes, key=lambda hole: hole.offset)
        match hole:
            case Hole(
                offset=offset,
                kind="IMAGE_REL_AMD64_REL32",
                value=HoleValue.GOT,
                symbol="_JIT_CONTINUE",
                addend=-4,
            ) as hole:
                # jmp qword ptr [rip]
                jump = b"\x48\xFF\x25\x00\x00\x00\x00"
                offset -= 3
            case Hole(
                offset=offset,
                kind="IMAGE_REL_I386_REL32" | "X86_64_RELOC_BRANCH",
                value=HoleValue.CONTINUE,
                symbol=None,
                addend=-4,
            ) as hole:
                # jmp 5
                jump = b"\xE9\x00\x00\x00\x00"
                offset -= 1
            case Hole(
                offset=offset,
                kind="R_AARCH64_JUMP26",
                value=HoleValue.CONTINUE,
                symbol=None,
                addend=0,
            ) as hole:
                # b #4
                jump = b"\x00\x00\x00\x14"
            case Hole(
                offset=offset,
                kind="R_X86_64_GOTPCRELX",
                value=HoleValue.GOT,
                symbol="_JIT_CONTINUE",
                addend=addend,
            ) as hole:
                assert _signed(addend) == -4
                # jmp qword ptr [rip]
                jump = b"\xFF\x25\x00\x00\x00\x00"
                offset -= 2
            case _:
                return
        if self.code.body[offset:] == jump and offset % alignment == 0:
            self.code.body = self.code.body[:offset]
            self.code.holes.remove(hole)

    def _got_lookup(self, got: dict[str, int], symbol: str) -> int:
        return len(self.data.body) + got.setdefault(symbol, 8 * len(got))

    def _emit_got(self, got: dict[str, int]) -> None:
        got_offset = len(self.data.body)
        for s, offset in got.items():
            if s in self.symbols:
                value, addend = self.symbols[s]
                symbol = None
            else:
                value, symbol = symbol_to_value(s)
                addend = 0
            self.data.holes.append(
                Hole(got_offset + offset, "R_X86_64_64", value, symbol, addend)
            )
            value_part = value.name if value is not HoleValue.ZERO else ""
            if value_part and not symbol and not addend:
                addend_part = ""
            else:
                signed = "+" if symbol is not None else ""
                addend_part = f"&{symbol}" if symbol else ""
                addend_part += f"{_signed(addend):{signed}#x}"
                if value_part:
                    value_part += "+"
            self.data.disassembly.append(
                f"{len(self.data.body):x}: {value_part}{addend_part}"
            )
            self.data.body.extend([0] * 8)


def symbol_to_value(symbol: str) -> tuple[HoleValue, str | None]:
    """
    Convert a symbol name to a HoleValue and a symbol name.

    Some symbols (starting with "_JIT_") are special and are converted to their
    own HoleValues.
    """
    if symbol.startswith("_JIT_"):
        try:
            return HoleValue[symbol.removeprefix("_JIT_")], None
        except KeyError:
            pass
    return HoleValue.ZERO, symbol


def _signed(value: int) -> int:
    value %= 1 << 64
    if value & (1 << 63):
        value -= 1 << 64
    return value
