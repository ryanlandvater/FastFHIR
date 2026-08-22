"""FastFHIR Generator — zero-copy View, field-info, reflection emitters.

Relocated from ffc.py lines 345-553.  These generate:
  * Per-block *View template (getters over raw bytes)
  * FieldInfo table (ord + kind + recovery for each V-Table slot)
  * Reflection dispatch (name -> Node)
"""

from generator.model import structure as _st
from generator.model import type_map as _tm
from generator.model.type_map import STRING_TYPES
from generator.utilities import enclose_namespace


def generate_lazy_view_struct(layout, block_struct_name, extra_methods=""):
    view_name = block_struct_name.replace("FF_", "") + "View"

    hpp = "template <uint32_t VERSION = FHIR_VERSION_R5>\n"
    hpp += f"struct {view_name} {{\n"
    hpp += "    const BYTE* const base;\n"
    hpp += "    const Offset offset;\n\n"
    hpp += "    inline bool is_null() const { return offset == FF_NULL_OFFSET; }\n\n"

    for f in layout:
        if f["is_array"]:
            ret_type = "FF_ARRAY"
        elif f.get("is_choice"):
            ret_type = "ChoiceEntry"
        elif f.get("raw_scalar"):
            ret_type = f["cpp_type"]
        elif f["fhir_type"] in _tm.DATETIME_TYPES:
            # DT-2: the accessor resolves the packed/fallback slot to TEXT —
            # the zero-copy path stays Node/Entry (TASKS.md DT-2 decision 1).
            ret_type = "std::string"
        elif f["fhir_type"] in ("string", "code") or f["fhir_type"] in STRING_TYPES:
            ret_type = "std::string_view"
        elif f["fhir_type"] in _tm.TYPE_MAP and f["fhir_type"] not in (
            "DEFAULT",
            "Resource",
            "CHOICE",
        ):
            ret_type = f["cpp_type"]
        elif f["fhir_type"] == "Resource":
            ret_type = "ResourceReference"
        else:
            child_struct = _st._resolve_ff_struct_name(
                f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
            )
            ret_type = child_struct.replace("FF_", "") + "View"

        hpp += f"    inline auto get_{f['cpp_name']}() const {{\n"

        if f["first_version_idx"] > 0:
            hpp += f"        if constexpr (VERSION < FHIR_VERSION_{f['first_version_name']}) {{\n"
            scalar_types = [
                "FF_ARRAY",
                "std::string_view",
                "std::string",
                "ResourceReference",
                "FastFHIR::Reflective::Node",
                "ChoiceEntry",
                "uint8_t",
                "uint16_t",
                "uint32_t",
                "uint64_t",
                "int32_t",
                "int64_t",
                "double",
                "bool",
                "Offset",
            ]
            if ret_type in scalar_types or f.get("raw_scalar"):
                if ret_type == "FF_ARRAY":
                    hpp += "            return FF_ARRAY(FF_NULL_OFFSET, 0, VERSION);\n"
                else:
                    hpp += f"            return {f['cpp_type'] if f.get('raw_scalar') else ret_type}{{}};\n"
            else:
                hpp += f"            return {ret_type}<VERSION>{{base, FF_NULL_OFFSET}};\n"
            hpp += "        }\n"

        vtable_off = f"{block_struct_name}::{f['name']}"

        if f["is_array"]:
            hpp += f"        Offset child_off = LOAD_U64(base + offset + {vtable_off});\n"
            hpp += "        return FF_ARRAY(child_off, 0, VERSION);\n"
        elif f.get("is_choice"):
            hpp += f"        return Decode::choice(base, offset + {vtable_off});\n"
        elif f.get("raw_scalar"):
            hpp += f"        return {f['macro']}(base + offset + {vtable_off});\n"
        elif f["fhir_type"] in _tm.DATETIME_TYPES:
            # DT-2: packed value formats to text; a flagged relative offset
            # resolves to the FF_STRING holding the original text.
            hpp += f"        const uint64_t __dt_raw = LOAD_U64(base + offset + {vtable_off});\n"
            hpp += "        if (__dt_raw == FF_DATETIME_NULL) return std::string();\n"
            hpp += "        if (FF_DATETIME_IS_FALLBACK(__dt_raw)) {\n"
            hpp += f"            return std::string(FF_STRING(FF_ResolveDateTimeOffset(__dt_raw, offset), 0, VERSION).read_view(base));\n"
            hpp += "        }\n"
            hpp += f"        return FF_FORMAT_DATETIME(FF_UNPACK_DATETIME(__dt_raw), {_st._child_recovery_expr(f, block_struct_name)});\n"
        elif f["fhir_type"] in _tm.TYPE_MAP and f["fhir_type"] not in (
            "string",
            "code",
            "DEFAULT",
            "Resource",
            "CHOICE",
        ):
            hpp += f"        return Decode::scalar<{f['cpp_type']}>(base, offset + {vtable_off}, {_st._child_recovery_expr(f, block_struct_name)});\n"
        elif f["fhir_type"] in ("string", "code") or f["fhir_type"] in STRING_TYPES:
            hpp += f"        Offset child_off = LOAD_U64(base + offset + {vtable_off});\n"
            hpp += "        if (child_off == FF_NULL_OFFSET) return std::string_view();\n"
            hpp += "        return FF_STRING(child_off, 0, VERSION).read_view(base);\n"
        elif f["fhir_type"] == "Resource":
            hpp += f"        Offset child_off = LOAD_U64(base + offset + {vtable_off});\n"
            hpp += f"        return ResourceReference{{child_off, static_cast<RECOVERY_TAG>(LOAD_U16(base + offset + {vtable_off} + DATA_BLOCK::RECOVERY))}};\n"
        else:
            hpp += f"        Offset child_off = LOAD_U64(base + offset + {vtable_off});\n"
            hpp += f"        return {ret_type}<VERSION>{{base, child_off}};\n"

        hpp += "    }\n"

    if extra_methods:
        hpp += extra_methods
    hpp += "};\n\n"
    return hpp


def generate_field_info_implementation(layout, block_struct_name):
    cpp = f"const FF_FieldInfo {block_struct_name}::FIELDS[{block_struct_name}::FIELD_COUNT] = {{\n"
    for f in layout:
        cpp += (
            f'    {{"{f["orig_name"]}", {_st._field_kind_expr(f)}, '
            f'{block_struct_name}::{f["name"]}, '
            f"{_st._child_recovery_expr(f, block_struct_name)}, "
            f"{_st._array_entries_are_offsets_expr(f)}}},\n"
        )
    cpp += "};\n"
    # Compact slot widths. The generator emits the field KIND and lets
    # ff_slot_width() (FF_Primitives.hpp) compute the byte count at compile
    # time -- it never computes a width itself. That is what keeps this table
    # and the compactor's dense-slot arithmetic on one definition instead of
    # two that have to be kept in step.
    #
    # Zero-padded to a multiple of 8 so the SIMD dense-offset walk can do a
    # single unconditional load.
    stride = ((len(layout) + 7) // 8) * 8
    vals = [f"ff_slot_width({_st._field_kind_expr(f)})" for f in layout]
    vals += ["0"] * (stride - len(layout))
    cpp += f"alignas(8) const uint8_t {block_struct_name}::COMPACT_SLOT_SIZES"
    cpp += f'[{block_struct_name}::COMPACT_SIZES_STRIDE] = {{{", ".join(vals)}}};\n'

    # The V-Table slot widths (<FIELD>_S) are a separate, PERMANENT wire
    # constant -- they fix every field offset in the standard layout. They are
    # not derived from ff_slot_width(), because a future compact-only packing
    # change must not silently move a V-Table offset. But they agree today, and
    # a silent divergence would corrupt data, so the compiler checks it: if you
    # ever need them to differ, you have to delete the assertion deliberately.
    for f in layout:
        cpp += (
            f"static_assert({block_struct_name}::{f['name']}_S == "
            f"ff_slot_width({_st._field_kind_expr(f)}),\n"
            f'              "{block_struct_name}.{f["orig_name"]}: V-Table slot width '
            f'disagrees with ff_slot_width() for its FF_FieldKind");\n'
        )
    cpp += f"const FF_FieldInfo* {block_struct_name}::find_field(std::string_view name) const {{\n"
    cpp += "    const FF_FieldInfo* fallback_choice = nullptr;\n"
    cpp += "    for (size_t i = 0; i < FIELD_COUNT; ++i) {\n"
    cpp += "        if (FIELDS[i].kind == FF_FIELD_CHOICE) fallback_choice = &FIELDS[i];\n"
    cpp += "        if (FIELDS[i].name == name) return &FIELDS[i];\n"
    cpp += "    }\n"
    cpp += "    return fallback_choice;\n"
    cpp += "}\n"
    return cpp


def generate_reflection_dispatch(block_struct_names, resources, top_level_types=()):
    """Reflection dispatch matching old ffc.py API.

    ``top_level_types`` are the dotless FHIR paths (``Quantity``,
    ``CodeableConcept``, ``Observation`` ...) that a choice ``[x]`` variant can
    name. They feed ``reflected_choice_suffix``; ``resources`` alone is not
    enough, because most choice variants are data types.
    """
    # ── HPP: banner + includes + namespace body ────────────────────────
    hpp_banner = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
        "#pragma once\n"
        '#include "FF_Primitives.hpp"\n'
        "#include <string_view>\n"
        "#include <vector>\n"
        "#include <span>\n\n"
    )
    hpp_body = (
        "namespace Reflective { class Node; }\n"
        "// Zero-copy view over a block's static FF_FieldInfo table: FIELDS is\n"
        "// a static array, so the span is a pointer and a length — no\n"
        "// allocation, no copy. (The by-value reflected_fields() was removed\n"
        "// 2026-08-19 after every caller migrated to this view.)\n"
        "std::span<const FF_FieldInfo> reflected_fields_view(uint16_t recovery);\n"
        "std::vector<std::string_view> reflected_keys(uint16_t recovery);\n"
        "Reflective::Node reflected_child_node(const BYTE* base, Size size, uint32_t version, Offset offset, uint16_t recovery, std::string_view key);\n"
        "std::string_view reflected_resource_type(uint16_t recovery);\n"
        "// FHIR type name for a choice ([x]) variant tag, e.g.\n"
        '// RECOVER_FF_QUANTITY -> "Quantity", which the exporter appends to the\n'
        "// base field name to rebuild `valueQuantity`. Covers DATA TYPES as well\n"
        "// as resources -- reflected_resource_type above deliberately does not,\n"
        "// and using it here printed a bare `value` for every complex variant.\n"
        "std::string_view reflected_choice_suffix(uint16_t recovery);\n"
        "const uint8_t* compact_field_sizes(uint16_t recovery);\n"
    )
    hpp = hpp_banner + enclose_namespace("FastFHIR", hpp_body)

    # ── CPP: banner + includes + namespace body (accumulated) ──────────
    cpp_banner = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
        '#include "FF_Utilities.hpp"\n'
        '#include "FF_Parser.hpp"\n'
        '#include "FF_AllTypes.hpp"\n'
        '#include "FF_Reflection.hpp"\n\n'
    )
    cpp_body = ""
    cpp_body += (
        "template <typename T_Block>\n"
        "const uint8_t* compact_sizes_for_block() {\n"
        "    return T_Block::COMPACT_SLOT_SIZES;\n"
        "}\n\n"
        "template <typename T_Block>\n"
        "Reflective::Node object_field_node(const T_Block& block, const BYTE* base, std::string_view key) {\n"
        "    const FF_FieldInfo* field = block.find_field(key);\n"
        "    if (!field) return {};\n"
        "    const Offset value_offset = block.__offset + field->field_offset;\n"
        "    if (FF_IsFieldEmpty(base, value_offset, field->kind)) return {};\n"
        "    Reflective::Entry entry{base, block.__offset, field->field_offset, field->child_recovery, field->kind};\n"
        "    return entry.as_node(block.__size, block.__version, field->child_recovery, field->kind);\n"
        "}\n\n"
        "template <typename T_Block>\n"
        "std::vector<std::string_view> keys_for_block() {\n"
        "    std::vector<std::string_view> keys;\n"
        "    keys.reserve(T_Block::FIELD_COUNT);\n"
        "    for (size_t i = 0; i < T_Block::FIELD_COUNT; ++i) {\n"
        "        keys.emplace_back(T_Block::FIELDS[i].name);\n"
        "    }\n"
        "    return keys;\n"
        "}\n\n"
        "std::span<const FF_FieldInfo> reflected_fields_view(uint16_t recovery) {\n"
        "    switch (recovery) {\n"
    )
    for s_name in sorted(block_struct_names):
        cpp_body += (
            f"        case {s_name}::recovery: "
            f"return {{{s_name}::FIELDS, {s_name}::FIELD_COUNT}};\n"
        )
    cpp_body += (
        "        default: return {};\n"
        "    }\n"
        "}\n\n"
        "std::vector<std::string_view> reflected_keys(uint16_t recovery) {\n"
        "    switch (recovery) {\n"
    )
    for s_name in sorted(block_struct_names):
        cpp_body += f"        case {s_name}::recovery: return keys_for_block<{s_name}>();\n"
    cpp_body += (
        "        default: return {};\n"
        "    }\n"
        "}\n\n"
        "Reflective::Node reflected_child_node(const BYTE* base, Size size, uint32_t version, Offset offset, uint16_t recovery, std::string_view key) {\n"
        "    switch (recovery) {\n"
    )
    for s_name in sorted(block_struct_names):
        cpp_body += f"        case {s_name}::recovery: return object_field_node({s_name}(offset, size, version), base, key);\n"
    cpp_body += (
        "        default: return {};\n"
        "    }\n"
        "}\n\n"
        "std::string_view reflected_resource_type(uint16_t recovery) {\n"
        "    switch (recovery) {\n"
    )
    for res in sorted(resources):
        cpp_body += f'        case FF_{res.upper()}::recovery: return "{res}";\n'
    cpp_body += (
        '        default: return "";\n'
        "    }\n"
        "}\n\n"
        "// Choice ([x]) variant tag -> FHIR type name. Every top-level block gets\n"
        "// a case, data types included: a choice slot's runtime tag is the ONLY\n"
        "// thing naming its active variant, so an unmapped tag is a field exported\n"
        "// as bare `value` instead of `valueQuantity` -- syntactically fine JSON\n"
        "// that no FHIR server will accept.\n"
        "std::string_view reflected_choice_suffix(uint16_t recovery) {\n"
        "    switch (recovery) {\n"
    )
    for _t in sorted(top_level_types):
        cpp_body += f'        case FF_{_t.upper()}::recovery: return "{_t}";\n'
    cpp_body += (
        '        default: return "";\n'
        "    }\n"
        "}\n\n"
        "const uint8_t* compact_field_sizes(uint16_t recovery) {\n"
        "    switch (recovery) {\n"
    )
    for s_name in sorted(block_struct_names):
        cpp_body += (
            f"        case {s_name}::recovery: return compact_sizes_for_block<{s_name}>();\n"
        )
    cpp_body += "        default: return nullptr;\n" "    }\n" "}\n"
    cpp = cpp_banner + enclose_namespace("FastFHIR", cpp_body)
    return hpp, cpp
