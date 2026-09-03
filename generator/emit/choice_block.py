"""Emit the decoded payload of a block-typed choice ([x]) field.

Why this exists
---------------
A ``value[x]`` slot on the wire is 10 bytes: an 8-byte value area and a 2-byte
tag. When the tag names a BLOCK type the value area holds that block's offset,
and the POCO deserializer used to copy that offset straight into
``ChoiceEntry::value``'s ``uint64_t`` arm.

That put a raw arena address into the public value API. Nothing in the library
ever read it back, so within one arena it was invisible -- but ``STORE`` wrote
it out verbatim, so any POCO -> stream round trip into a DIFFERENT arena emitted
a slot claiming "a Quantity lives at offset N" where N meant nothing. The offset
chain broke silently at write time and surfaced later in whatever walked the
whole graph.

Offsets are structure, not data. They must never reach a caller. So the choice
carries the DECODED VALUE instead, behind a ``unique_ptr`` so ``ChoiceEntry``
stays 32 bytes rather than growing to the width of its largest alternative
(AddressData alone is 176) and dragging every containing struct with it.
"""

from __future__ import annotations


# `__blk`, never `__block`: __block is a reserved Clang keyword on Apple
# platforms (the Objective-C block storage qualifier), so a parameter named
# __block fails to compile there with "attribute not allowed, only allowed on
# local variables". Every other generated parameter uses the __name style, so
# the collision is easy to reintroduce.
def _tag(fhir_type: str) -> str:
    return f"RECOVER_FF_{fhir_type.upper()}"


def _block(fhir_type: str) -> str:
    return f"FF_{fhir_type.upper()}"


def _data(fhir_type: str) -> str:
    return f"{fhir_type}Data"


def generate_choice_block(production_types: list[str], auto_header: str) -> tuple[str, str]:
    """Return (header fragment, source) for ChoiceBlock and its decoder.

    The fragment is APPENDED to FF_DataTypes.hpp rather than given its own
    header: ChoiceBlock is a variant over the structs defined there, so it has
    to come after them, and a separate header would have to include
    FF_DataTypes.hpp while every generated .cpp needs both -- a cycle for no
    gain.

    `production_types` is the datatype set this profile generates; a choice
    target outside it has no struct to decode into and is reported rather than
    guessed at.
    """
    types = sorted(production_types)

    hpp = (
        "// The DECODED value of a block-typed choice ([x]) field.\n"
        "//\n"
        "// Held behind a unique_ptr by ChoiceEntry: std::variant is sized by its\n"
        "// largest alternative, so storing this inline would take ChoiceEntry from\n"
        "// 32 bytes to ~184 and grow every struct that contains one -- and they\n"
        "// nest, and live in vectors. The indirection costs one allocation on a\n"
        "// path that is already allocation-heavy (these structs are full of\n"
        "// std::vector) and is explicitly NOT the zero-copy lens.\n"
        "struct ChoiceBlock {\n"
        "    std::variant<\n        std::monostate"
    )
    for t in types:
        hpp += f",\n        {_data(t)}"
    hpp += "\n    > value;\n};\n\n"
    hpp += (
        "// Decode the block a choice slot names into its typed value.\n"
        "//\n"
        "// Returns nullptr when `tag` names a type this build does not generate:\n"
        "// there is no struct to decode into, and inventing one would put\n"
        "// plausible bytes under the right tag, which is worse than an absent\n"
        "// value. The caller reports it; the profile decides it.\n"
        "std::unique_ptr<ChoiceBlock> FF_DecodeChoiceBlock(\n"
        "    const BYTE* const __base, Offset __offset, Size __size,\n"
        "    uint32_t __version, RECOVERY_TAG __tag);\n\n"
        "// An EMPTY ChoiceBlock of the alternative `tag` names -- the inverse of\n"
        "// FF_DecodeChoiceBlock's switch, for a caller building a choice up from\n"
        "// parts rather than reading one off a wire. Without it a decoder can name\n"
        "// a block-typed choice but cannot construct one, so `value{Quantity}.code`\n"
        "// is addressable in a path and unwritable in fact.\n"
        "//\n"
        "// nullptr for a tag this profile does not generate -- the same answer, and\n"
        "// the same reason, as FF_DecodeChoiceBlock.\n"
        "std::unique_ptr<ChoiceBlock> FF_MakeChoiceBlock(RECOVERY_TAG __tag);\n\n"
        "// SIZE/STORE for a decoded choice value. Named FF_Size.../FF_Store...\n"
        "// rather than SIZE_FF_.../STORE_FF_...: that prefix is the convention for a\n"
        "// WIRE BLOCK STRUCT (SIZE_FF_ADDRESS pairs with struct FF_ADDRESS), and\n"
        "// ChoiceBlock is a POCO variant with no block of its own. The generator\n"
        "// gate checks exactly that, and was right to reject the first spelling.\n"
        "// THIS is what makes a POCO safe\n"
        "// to re-serialize: the block is rebuilt in the DESTINATION arena, so the\n"
        "// slot names a local address instead of one measured in an arena the\n"
        "// reader has never seen. Same contract as every other SIZE/STORE pair --\n"
        "// a disagreement between them overlaps the next claim.\n"
        "Size FF_SizeChoiceBlock(const ChoiceBlock& __blk, uint32_t __version);\n"
        "// RETURNS THE END OFFSET, not the byte count -- the Offset return type\n"
        "// is the convention (Size STORE_FF_STRING gives a count and pairs with\n"
        "// `+=`; Offset STORE_FF_<BLOCK> gives a cursor and pairs with `=`).\n"
        "Offset FF_StoreChoiceBlock(BYTE* const __base, Offset __start_off,\n"
        "                             const ChoiceBlock& __blk, uint32_t __version);\n"
    )

    cpp = f'{auto_header}#include "FF_DataTypes.hpp"\n'
    cpp += '#include "FF_DataTypes_internal.hpp"\n\n'
    cpp += (
        "std::unique_ptr<ChoiceBlock> FF_DecodeChoiceBlock(\n"
        "    const BYTE* const __base, Offset __offset, Size __size,\n"
        "    uint32_t __version, RECOVERY_TAG __tag) {\n"
        "    if (__offset == FF_NULL_OFFSET) return nullptr;\n"
        "    auto out = std::make_unique<ChoiceBlock>();\n"
        "    switch (__tag) {\n"
    )
    for t in types:
        cpp += (
            f"    case {_tag(t)}:\n"
            f"        out->value = {_block(t)}::deserialize(__base, __offset, __size, __version);\n"
            f"        return out;\n"
        )
    cpp += (
        "    default:\n"
        "        return nullptr;  // not generated by this profile\n"
        "    }\n"
        "}\n\n"
        "std::unique_ptr<ChoiceBlock> FF_MakeChoiceBlock(RECOVERY_TAG __tag) {\n"
        "    auto out = std::make_unique<ChoiceBlock>();\n"
        "    switch (__tag) {\n"
    )
    for t in types:
        cpp += (
            f"    case {_tag(t)}:\n"
            f"        out->value = {_data(t)}{{}};\n"
            f"        return out;\n"
        )
    cpp += (
        "    default:\n"
        "        return nullptr;  // not generated by this profile\n"
        "    }\n"
        "}\n\n"
        "Size FF_SizeChoiceBlock(const ChoiceBlock& __blk, uint32_t __version) {\n"
        "    return std::visit([&](const auto& __v) -> Size {\n"
        "        using T = std::decay_t<decltype(__v)>;\n"
        "        if constexpr (std::is_same_v<T, std::monostate>) return 0;\n"
        "        else return TypeTraits<T>::size(__v, __version);\n"
        "    }, __blk.value);\n"
        "}\n\n"
        "Offset FF_StoreChoiceBlock(BYTE* const __base, Offset __start_off,\n"
        "                             const ChoiceBlock& __blk, uint32_t __version) {\n"
        "    return std::visit([&](const auto& __v) -> Offset {\n"
        "        using T = std::decay_t<decltype(__v)>;\n"
        "        if constexpr (std::is_same_v<T, std::monostate>) return 0;\n"
        "        else return TypeTraits<T>::store(__base, __start_off, __v, __version);\n"
        "    }, __blk.value);\n"
        "}\n\n"
        "// ChoiceEntry's special members. Out of line because the header only\n"
        "// forward-declares ChoiceBlock; this is the TU that sees it complete.\n"
        "// Move-only: ChoiceBlock wraps datatype structs that hold unique_ptr\n"
        "// members, so no copy exists to give.\n"
        "ChoiceEntry::ChoiceEntry() = default;\n"
        "ChoiceEntry::~ChoiceEntry() = default;\n"
        "ChoiceEntry::ChoiceEntry(ChoiceEntry &&) noexcept = default;\n"
        "ChoiceEntry &ChoiceEntry::operator=(ChoiceEntry &&) noexcept = default;\n"
    )
    return hpp, cpp
