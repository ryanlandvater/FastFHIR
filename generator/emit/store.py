from generator.model.type_map import (
    DATETIME_TYPES,
    DECIMAL_SIGFIGS_SUFFIX,
    SCALAR_PRIMITIVE_TYPES,
    STRING_TYPES,
    TYPE_MAP,
    _scalar_recovery_tag,
    sanitize_fhir_type,
    get_store_macro,
)
from generator.emit.deserialize import _resolve_ff_struct_name
from generator.model.structure import _child_recovery_expr
from generator.model import structure as _st

_SCALAR_KINDS = frozenset(
    {
        "FF_FIELD_BOOL",
        "FF_FIELD_INT32",
        "FF_FIELD_UINT32",
        "FF_FIELD_INT64",
        "FF_FIELD_UINT64",
        "FF_FIELD_FLOAT64",
    }
)


def generate_size_fields(layout, block_struct_name, data_name):
    cpp = f"    Size __total = {block_struct_name}::HEADER_SIZE;\n"
    for f in layout:
        kind = _st._field_kind_expr(f)
        if kind == "FF_FIELD_CHOICE":
            cpp += f"    std::visit([&](auto&& arg) {{\n"
            cpp += f"        using T = std::decay_t<decltype(arg)>;\n"
            cpp += f"        if constexpr (std::is_same_v<T, std::string_view>) {{\n"
            # No `!arg.empty()` guard: the STORE side writes STORE_FF_STRING
            # unconditionally once the variant holds a string_view, so an empty
            # one still costs a 14-byte FF_STRING header. Guarding here and not
            # there is the SIZE/STORE asymmetry that produced A23 Bug B. An
            # absent choice is std::monostate, not an empty string.
            #
            # Three variant families share that one std::string_view and are
            # separated only by the tag: plain strings, date/time, and code.
            # Each reserves a different amount of child space -- a date/time
            # that packs into 63 bits needs none, a code in the dictionary needs
            # none, and both need an FF_STRING / FF_CODEABLE_CONCEPT when they
            # do not fit. This must mirror the STORE branch below exactly or the
            # claim and the write disagree.
            _cv = f"{data_name}.{f['cpp_name']}"
            cpp += (
                f"            if (FF_IsDateTimeTag({_cv}.tag))\n"
                f"                __total += SIZE_FF_DATETIME(arg, {_cv}.tag);\n"
                f"            else if ({_cv}.tag == RECOVER_FF_CODE)\n"
                f"                __total += SIZE_FF_CODE(std::string(arg), __version);\n"
                f"            else\n"
                f"                __total += SIZE_FF_STRING(arg);\n"
            )
            cpp += f"        }}\n"
            cpp += f"    }}, {data_name}.{f['cpp_name']}.value);\n"
            # A block-typed variant lives in `.block`, not in `.value` -- the
            # visitor above cannot see it. It is a decoded struct now, so it
            # costs whatever rebuilding it costs.
            cpp += (
                f"    if ({_cv}.block)\n"
                f"        __total += FF_SizeChoiceBlock(*{_cv}.block, __version);\n"
            )
        elif kind == "FF_FIELD_ARRAY":
            if (
                f["fhir_type"] == "string"
                or f["fhir_type"] in STRING_TYPES
                or f["fhir_type"] in DATETIME_TYPES
            ):
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * TYPE_SIZE_OFFSET);\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                cpp += f"            __total += SIZE_FF_STRING(__item);\n"
                cpp += f"        }}\n    }}\n"
            elif f["fhir_type"] == "code":
                code_enum = f.get("code_enum")
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * TYPE_SIZE_OFFSET);\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                if code_enum:
                    # The store writes these as FF_STRINGs unconditionally (OFFSET
                    # array of strings, per the wire format) — the size MUST match,
                    # so SIZE_FF_STRING, never the dictionary-aware SIZE_FF_CODE.
                    # The old SIZE_FF_CODE returned 0 for dictionary hits, so every
                    # resource with a dictionary-backed code array (e.g.
                    # AllergyIntolerance.category = ["medication"]) was claimed
                    # short and the NEXT resource's write overlapped its tail
                    # (TASKS.md A23, Bug B).
                    cpp += f"            __total += SIZE_FF_STRING(std::string({code_enum['serialize']}(__item)));\n"
                else:
                    cpp += f"            __total += SIZE_FF_STRING(__item);\n"
                cpp += f"        }}\n    }}\n"
            elif f["fhir_type"] == "Resource":
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * TYPE_SIZE_RESOURCE);\n"
                cpp += f"    }}\n"
            elif f["fhir_type"] in SCALAR_PRIMITIVE_TYPES:
                size_const = TYPE_MAP[f["fhir_type"]]["size_const"]
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * {size_const});\n"
                cpp += f"    }}\n"
            else:
                child_struct = _resolve_ff_struct_name(
                    f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
                )
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE;\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                cpp += f"            __total += SIZE_{child_struct}(__item);\n"
                cpp += f"        }}\n    }}\n"

        elif kind == "FF_FIELD_STRING":
            cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
            cpp += f"        __total += SIZE_FF_STRING({data_name}.{f['cpp_name']});\n    }}\n"

        elif kind == "FF_FIELD_DATETIME":
            # DT-2: the slot is inline (already counted in HEADER_SIZE); only
            # the fallback FF_STRING (text that does not fit the packed 63
            # bits) needs reserved child space. SIZE_FF_DATETIME returns 0 for
            # empty text and for text that packs inline.
            cpp += f"    __total += SIZE_FF_DATETIME({data_name}.{f['cpp_name']}, {_scalar_recovery_tag(f['fhir_type'])});\n"

        elif kind == "FF_FIELD_BLOCK":
            # STRING_TYPES (dateTime, base64Binary, markdown, ...) need no special
            # case: _resolve_ff_struct_name maps them to FF_STRING, so the line
            # below is already SIZE_FF_STRING(*data.x) for them.
            child_struct = _resolve_ff_struct_name(
                f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
            )
            cpp += f"    if ({data_name}.{f['cpp_name']} != nullptr) {{\n"
            cpp += (
                f"        __total += SIZE_{child_struct}(*{data_name}.{f['cpp_name']});\n    }}\n"
            )

        elif kind == "FF_FIELD_CODE":
            code_enum = f.get("code_enum")
            if code_enum:
                cpp += f"    __total += SIZE_FF_CODE(std::string({code_enum['serialize']}({data_name}.{f['cpp_name']})), __version);\n"
            else:
                cpp += f"    __total += SIZE_FF_CODE({data_name}.{f['cpp_name']}, __version);\n"

    cpp += "    return __total;\n"
    return cpp


def generate_store_fields(layout, block_struct_name, ptr_name, data_name):
    cpp = ""
    for f in layout:
        kind = _st._field_kind_expr(f)
        cpp += f"    // --- Store: {f['name']} ---\n"

        # --- Polymorphic Choice [x] Handling ---
        if kind == "FF_FIELD_CHOICE":
            vtable_off = f"{ptr_name} + {block_struct_name}::{f['name']}"

            _bv = f"{data_name}.{f['cpp_name']}"
            # A BLOCK-TYPED VARIANT IS REBUILT, NOT REPOINTED.
            #
            # This is the whole point of carrying the decoded value: the block
            # is written into THIS arena and the slot names the address it got
            # here. The old code put the SOURCE arena's offset in the slot
            # verbatim, so a stream built from a POCO read elsewhere claimed a
            # block at an address that meant nothing in it.
            cpp += f"    if ({_bv}.block) {{\n"
            cpp += f"        STORE_U64({vtable_off}, child_off);\n"
            # `=`, not `+=`. The two STORE families return different things and
            # the RETURN TYPE is the signal: `Size STORE_FF_STRING(...)` gives
            # bytes written (so `+=`), while `Offset STORE_FF_<BLOCK>(...)`
            # gives the END OFFSET (so `=`). FF_StoreChoiceBlock forwards
            # TypeTraits<T>::store and returns Offset. Using `+=` here added an
            # absolute address to the cursor: STORE consumed 13,917 bytes where
            # SIZE had claimed 4,847, and the arena's own SIZE/STORE contract
            # check caught it.
            cpp += (
                f"        child_off = FF_StoreChoiceBlock("
                f"__base, child_off, *{_bv}.block, __version);\n"
            )
            cpp += f"        STORE_U16({vtable_off} + 8, {_bv}.tag);\n"
            cpp += f"    }} else\n"
            cpp += f"    std::visit([&](auto&& arg) {{\n"
            cpp += f"        using T = std::decay_t<decltype(arg)>;\n"

            # 1. Monostate (Empty)
            cpp += f"        if constexpr (std::is_same_v<T, std::monostate>) {{\n"
            cpp += f"            STORE_U64({vtable_off}, FF_NULL_OFFSET);\n"
            cpp += f"            STORE_U16({vtable_off} + 8, FF_RECOVER_UNDEFINED);\n"
            cpp += f"        }}\n"

            # 2. Inline Scalars (Big-Endian Safe)
            cpp += f"        else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t> || std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t> || std::is_same_v<T, double>) {{\n"
            cpp += f"            STORE_U64({vtable_off}, FF_NULL_OFFSET);\n\n"
            cpp += f"            if constexpr (std::is_same_v<T, bool>) STORE_U8({vtable_off}, arg ? 1 : 0);\n"
            cpp += f"            else if constexpr (std::is_same_v<T, int32_t>) STORE_U32({vtable_off}, static_cast<uint32_t>(arg));\n"
            cpp += f"            else if constexpr (std::is_same_v<T, uint32_t>) STORE_U32({vtable_off}, arg);\n"
            cpp += f"            else if constexpr (std::is_same_v<T, int64_t>) STORE_U64({vtable_off}, static_cast<uint64_t>(arg));\n"
            cpp += f"            else if constexpr (std::is_same_v<T, uint64_t>) STORE_U64({vtable_off}, arg);\n"
            cpp += f"            else if constexpr (std::is_same_v<T, double>) STORE_F64({vtable_off}, arg);\n"
            cpp += f"            STORE_U16({vtable_off} + 8, {data_name}.{f['cpp_name']}.tag);\n"
            cpp += f"        }}\n"

            # 3. Variable-Length String Primitives — and date/time, which
            #    arrives in the same std::string_view and is separated only by
            #    the tag. ENCODE_FF_DATETIME returns the 8-byte slot word
            #    directly (packed civil value, or a flagged relative offset to
            #    an FF_STRING it writes into child space) and advances child_off
            #    itself, so there is no STORE_U64(slot, child_off) here: writing
            #    an absolute FF_STRING offset into a date/time slot is what the
            #    reader would unpack as a garbage civil date.
            cpp += f"        else if constexpr (std::is_same_v<T, std::string_view>) {{\n"
            cpp += f"            const RECOVERY_TAG __ct = {data_name}.{f['cpp_name']}.tag;\n"
            cpp += f"            if (FF_IsDateTimeTag(__ct)) {{\n"
            cpp += (
                f"                STORE_U64({vtable_off}, ENCODE_FF_DATETIME("
                f"__base, hdr_off, child_off, arg, __ct));\n"
            )
            cpp += f"            }} else if (__ct == RECOVER_FF_CODE) {{\n"
            # A code is 4 bytes; the choice VALUE AREA is 8 (the slot is 10 --
            # 8 for the value, 2 for the tag at +8 -- because it must fit the
            # widest inline variant: uint64, double, or a packed date/time).
            # So a code occupies the low half and the upper half is padding.
            #
            # Fill, then overwrite: identical to the arithmetic branch above,
            # which pre-fills FF_NULL_OFFSET before STORE_U32 for int32/uint32/
            # bool. Two padding conventions in adjacent arms of one visitor
            # would be a difference a reader has to explain to themselves and
            # cannot.
            #
            # The padding has to be determinate at all because finalize hashes
            # EVERY byte from base() to the hash slot (FF_StreamFinalize in
            # FF_Memory.hpp), so an unwritten half makes two logically identical
            # documents digest differently. It reads as zero today only because
            # claim_space hands back untouched sparse-mmap pages -- the OS's
            # guarantee, not the format's.
            #
            # No reader looks at the upper half: code_node does LOAD_U32 at the
            # slot start, and STORE_* is little-endian on the wire, so the low
            # four wire bytes are the word it wants on either host endianness.
            cpp += f"                STORE_U64({vtable_off}, FF_NULL_OFFSET);\n"
            cpp += (
                f"                STORE_U32({vtable_off}, ENCODE_FF_CODE("
                f"__base, hdr_off, child_off, std::string(arg), __version));\n"
            )
            cpp += f"            }} else {{\n"
            cpp += f"                STORE_U64({vtable_off}, child_off);\n"
            cpp += f"                child_off += STORE_FF_STRING(__base, child_off, arg);\n"
            cpp += f"            }}\n"
            cpp += f"            STORE_U16({vtable_off} + 8, __ct);\n"
            cpp += f"        }}\n"

            # 4. Immediate Serialization Offsets (Quantity, CodeableConcept, etc.)
            cpp += f"        else if constexpr (std::is_same_v<T, Offset>) {{\n"
            cpp += f"            STORE_U64({vtable_off}, arg);\n"
            cpp += f"            STORE_U16({vtable_off} + 8, {data_name}.{f['cpp_name']}.tag);\n"
            cpp += f"        }}\n"
            cpp += f"    }}, {data_name}.{f['cpp_name']}.value);\n"

        # --- Existing Array / Legacy Logic ---
        elif kind == "FF_FIELD_ARRAY":
            # PATTERN 1: ARRAY OF STRING/CODE OFFSETS -- 8-byte pointers to
            # variable-length strings stored elsewhere. This must be an
            # `in STRING_TYPES` test, never `== "string"`: dateTime, markdown,
            # uri and id share this layout and silently take the wrong branch
            # otherwise.
            if (
                f["fhir_type"] in ("string", "code")
                or f["fhir_type"] in STRING_TYPES
                or f["fhir_type"] in DATETIME_TYPES
            ):
                code_enum = f.get("code_enum")
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                # Uses OFFSET with 8-byte hops
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::OFFSET, TYPE_SIZE_OFFSET, __n, ToArrayTag({_child_recovery_expr(f, block_struct_name)}));\n"
                cpp += f"        Offset blk_off_tbl = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * TYPE_SIZE_OFFSET;\n"
                cpp += f"        for (uint32_t blk_i = 0; blk_i < __n; ++blk_i) {{\n"
                cpp += f"            STORE_U64(__base + blk_off_tbl + blk_i * TYPE_SIZE_OFFSET, child_off);\n"
                if code_enum:
                    cpp += f"            child_off += STORE_FF_STRING(__base, child_off, std::string({code_enum['serialize']}({data_name}.{f['cpp_name']}[blk_i])));\n"
                else:
                    cpp += f"            child_off += STORE_FF_STRING(__base, child_off, {data_name}.{f['cpp_name']}[blk_i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"

            # --- PATTERN 2: ARRAY OF POLYMORPHIC RESOURCE TUPLES ---
            # Physically: 10-byte tuples (8-byte Offset + 2-byte Recovery Tag)
            # stored inline. This is a `fhir_type` test on purpose -- `kind` is
            # FF_FIELD_ARRAY for the whole enclosing branch, so testing
            # FF_FIELD_RESOURCE here would be unreachable and Resource arrays
            # would fall through to the complex-struct branch below.
            elif f["fhir_type"] == "Resource":
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_RESOURCE, __n, ToArrayTag({_child_recovery_expr(f, block_struct_name)}));\n"
                cpp += f"        Offset __entries_start = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * TYPE_SIZE_RESOURCE;\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                cpp += f"            STORE_U64(__base + __entries_start + __i * TYPE_SIZE_RESOURCE, {data_name}.{f['cpp_name']}[__i].offset);\n"
                cpp += f"            STORE_U16(__base + __entries_start + __i * TYPE_SIZE_RESOURCE + DATA_BLOCK::RECOVERY, {data_name}.{f['cpp_name']}[__i].recovery);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"

            # --- PATTERN 4: ARRAY OF INLINE SCALAR PRIMITIVES ---
            elif f["fhir_type"] in SCALAR_PRIMITIVE_TYPES:
                size_const = TYPE_MAP[f["fhir_type"]]["size_const"]
                recovery = _scalar_recovery_tag(f["fhir_type"])
                store_mac = {"boolean": "STORE_U8", "decimal": "STORE_F64"}.get(
                    f["fhir_type"], "STORE_U32"
                )
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::INLINE_BLOCK, {size_const}, __n, ToArrayTag({recovery}));\n"
                cpp += f"        Offset __entries_start = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * {size_const};\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                cpp += f"            {store_mac}(__base + __entries_start + __i * {size_const}, {data_name}.{f['cpp_name']}[__i]);\n"
                if f["fhir_type"] == "decimal":
                    # A decimal entry is 9 bytes wide; the vector element is a
                    # bare double, so there is no per-element source scale to
                    # record. Write the sentinel rather than leaving the byte
                    # unset -- the exporter falls back to shortest-round-trip.
                    cpp += (
                        f"            STORE_U8(__base + __entries_start + __i * {size_const}"
                        f" + TYPE_SIZE_UINT64, FF_DECIMAL_SIGFIGS_UNSPECIFIED);\n"
                    )
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
            # --- PATTERN 3: ARRAY OF COMPLEX STRUCTS ---
            # Self-referential arrays (e.g. Extension.extension → ExtensionData)
            # fall through to the generic array-of-blocks branch below — the type
            # is complete, so STORE_{child_struct} is available for recursive store.
            else:
                child_struct = _resolve_ff_struct_name(
                    f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
                )
                store_fn = f"STORE_{child_struct}"

                # Branch 3a: Inline Data (Bundle.entry, Patient.name)
                # Physically: Full structs stored back-to-back.
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                # Uses INLINE_BLOCK with full struct header-size hops
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::INLINE_BLOCK, {child_struct}::HEADER_SIZE, __n, ToArrayTag({_child_recovery_expr(f, block_struct_name)}));\n"
                cpp += f"        Offset __entries_start = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * {child_struct}::HEADER_SIZE;\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                cpp += f"            child_off = {store_fn}(__base, __entries_start + __i * {child_struct}::HEADER_SIZE, child_off, {data_name}.{f['cpp_name']}[__i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"

        elif f["fhir_type"] == "Resource":
            cpp += f"    if ({data_name}.{f['cpp_name']}.offset != FF_NULL_OFFSET) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, {data_name}.{f['cpp_name']}.offset);\n"
            cpp += f"        STORE_U16({ptr_name} + {block_struct_name}::{f['name']} + DATA_BLOCK::RECOVERY, {data_name}.{f['cpp_name']}.recovery);\n"
            cpp += f"    }} else {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET);\n"
            cpp += f"        STORE_U16({ptr_name} + {block_struct_name}::{f['name']} + DATA_BLOCK::RECOVERY, FF_RECOVER_UNDEFINED);\n"
            cpp += f"    }}\n"

        elif f["fhir_type"] == "string":
            cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
            cpp += f"        child_off += STORE_FF_STRING(__base, child_off, {data_name}.{f['cpp_name']});\n"
            cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"

        elif f["fhir_type"] in STRING_TYPES and f["cpp_type"] == "Offset":
            # STRING_TYPES (dateTime, base64Binary, markdown, etc.) stored as
            # unique_ptr<string_view>. Use STORE_FF_STRING with 3 args: (base, off, *data).
            # Only applies when cpp_type is 'Offset' — some STRING_TYPES fields
            # (e.g. Extension.url) are overridden to scalar storage (uint32_t code).
            cpp += f"    if ({data_name}.{f['cpp_name']} != nullptr) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
            cpp += f"        child_off += STORE_FF_STRING(__base, child_off, *{data_name}.{f['cpp_name']});\n"
            cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"

        elif f["cpp_type"] == "Offset":
            child_struct = _resolve_ff_struct_name(
                f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
            )
            if child_struct == "FF_STRING":
                # FF_STRING manages its own header internally — use 3-arg STORE_FF_STRING directly
                cpp += f"    if ({data_name}.{f['cpp_name']} != nullptr) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        child_off += STORE_FF_STRING(__base, child_off, *{data_name}.{f['cpp_name']});\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
            else:
                store_fn = f"STORE_{child_struct}"
                cpp += f"    if ({data_name}.{f['cpp_name']} != nullptr) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        Offset nested_hdr = child_off;\n"
                cpp += f"        child_off += {child_struct}::HEADER_SIZE;\n"
                cpp += f"        child_off = {store_fn}(__base, nested_hdr, child_off, *{data_name}.{f['cpp_name']});\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"

        elif f["fhir_type"] == "code":
            code_enum = f.get("code_enum")
            val_str = (
                f"std::string({code_enum['serialize']}({data_name}.{f['cpp_name']}))"
                if code_enum
                else f"std::string({data_name}.{f['cpp_name']})"
            )
            ext_sys = f.get("external_system", "")
            cpp += f"    {{\n"
            cpp += f"        std::string __code_str = {val_str};\n"
            if ext_sys:
                cpp += f"        STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, ENCODE_FF_CODE(__base, hdr_off, child_off, __code_str, __version, {ext_sys}));\n"
            else:
                cpp += f"        STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, ENCODE_FF_CODE(__base, hdr_off, child_off, __code_str, __version));\n"
            cpp += f"    }}\n"
        elif f["fhir_type"] in DATETIME_TYPES:
            # DT-2: packed inline u64, or a flagged relative offset to an
            # FF_STRING fallback written by ENCODE_FF_DATETIME into child
            # space (the SIZE pass above reserves it).
            cpp += f"    STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, ENCODE_FF_DATETIME(__base, hdr_off, child_off, {data_name}.{f['cpp_name']}, {_scalar_recovery_tag(f['fhir_type'])}));\n"
        elif f["fhir_type"] == "decimal":
            # [ double @ +0 | scale @ +8 ]. Two stores, not one -- the 9th byte
            # is part of the slot and an unwritten one reads as arena garbage,
            # which the exporter would take for a digit count.
            slot = f"{ptr_name} + {block_struct_name}::{f['name']}"
            cpp += f"    STORE_F64({slot}, {data_name}.{f['cpp_name']});\n"
            cpp += (
                f"    STORE_U8({slot} + TYPE_SIZE_UINT64, "
                f"{data_name}.{f['cpp_name']}{DECIMAL_SIGFIGS_SUFFIX});\n"
            )
        else:
            cpp += f"    {get_store_macro(f['macro'])}({ptr_name} + {block_struct_name}::{f['name']}, {data_name}.{f['cpp_name']});\n"

    return cpp
