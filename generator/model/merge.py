# ============================================================
# FastFHIR Generator — FHIR version merge (model layer).
#
# Relocated body from tools/generator/ffc.py lines 86-119 + 915-1129.
# merge_fhir_versions + generate_cxx_for_blocks.
#
# merge_fhir_versions unifies R4/R5 StructureDefinition snapshots into a
# single master_blocks dict (one entry per dotted FHIR path).
# generate_cxx_for_blocks emits the public struct, internal vtable struct,
# zero-copy View, store/size/deserialize bridge, and TypeTraits for each
# block — the core C++ generation logic.
#
# Imports from model/ (type_map, structure) for field-kind, recovery-tag,
# and naming helpers — never the reverse.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: FastFHIR Shared Source License (FF-SSL)
# ============================================================

from __future__ import annotations

import re

from generator.emit.header import auto_header, write_if_changed
from generator.model import type_map as _tm
from generator.model import structure as _st

# ---------------------------------------------------------------------------
# Per-block field overrides (compile-time layout hints).
# Keys are (parent_path, field_name). Relocated from ffc.py line 94.
# ---------------------------------------------------------------------------
BLOCK_FIELD_OVERRIDES: dict[tuple[str, str], dict] = {
    ("Extension", "url"): {"cpp_type": "uint32_t", "raw_scalar": True, "fhir_type": "url",
                           "macro": "LOAD_U32", "size": 4, "size_const": "TYPE_SIZE_UINT32",
                           "data_type": "uint32_t", "url_idx": True},
}

# ---------------------------------------------------------------------------
# Per-block extra view methods (injected into the *View template).
# ---------------------------------------------------------------------------
VIEW_EXTRA_METHODS: dict[str, str] = {}

BASE_BLOCK_HEADER_SIZE: int = 10


# =========================================================================
# merge_fhir_versions
# =========================================================================
def merge_fhir_versions(schemas_by_version, root_resource):
    master_blocks = {}
    for v_idx, (v_name, elements) in enumerate(schemas_by_version):
        for el in elements:
            path = el.get("path", "")
            if not path.startswith(root_resource) or len(path.split(".")) == 1:
                continue
            parent_path = ".".join(path.split(".")[:-1])
            raw_field_name = path.split(".")[-1]
            field_name = raw_field_name.replace("[x]", "")
            is_choice = "[x]" in raw_field_name
            choice_types = [t.get("code") for t in el.get("type", [])] if is_choice else []

            if parent_path not in master_blocks:
                master_blocks[parent_path] = {"layout": [], "seen": set(), "sizes": {}}
            blk = master_blocks[parent_path]
            f_type = _tm.sanitize_fhir_type(
                el.get("type", [{"code": "BackboneElement"}])[0].get(
                    "code", "BackboneElement"
                )
            )
            is_array = el.get("max") == "*"
            if field_name not in blk["seen"]:
                if is_choice:
                    mapping = _tm.TYPE_MAP["CHOICE"]
                else:
                    mapping = (
                        _tm.TYPE_MAP["DEFAULT"]
                        if (is_array or f_type not in _tm.TYPE_MAP)
                        else _tm.TYPE_MAP[f_type]
                    )

                off = (
                    BASE_BLOCK_HEADER_SIZE
                    if not blk["layout"]
                    else blk["layout"][-1]["offset"] + blk["layout"][-1]["size"]
                )

                # C++ Keyword Sanitization
                cpp_safe_name = field_name.lower()
                if cpp_safe_name in {
                    "class", "template", "namespace", "operator", "new", "delete",
                    "default", "struct", "enum", "concept", "requires", "export",
                    "import", "module",
                }:
                    cpp_safe_name += "_"
                field_entry = {
                    "name": field_name.upper(),
                    "cpp_name": cpp_safe_name,
                    "orig_name": field_name,
                    "is_choice": is_choice,
                    "choice_types": choice_types,
                    "is_array": is_array,
                    "fhir_type": f_type,
                    "size": mapping["size"],
                    "size_const": mapping["size_const"],
                    "cpp_type": mapping["cpp"],
                    "data_type": mapping["data_type"],
                    "macro": mapping["macro"],
                    "first_version_name": v_name,
                    "first_version_idx": v_idx,
                    "offset": off,
                }
                override = BLOCK_FIELD_OVERRIDES.get((parent_path, field_name))
                if override:
                    field_entry.update(override)
                    field_entry["offset"] = off
                blk["layout"].append(field_entry)
                blk["seen"].add(field_name)
            blk["sizes"][v_name] = blk["layout"][-1]["offset"] + blk["layout"][-1]["size"]

    for parent_path, blk in master_blocks.items():
        for f in blk["layout"]:
            if f["fhir_type"] not in ("BackboneElement", "Element"):
                continue
            expected = parent_path + "." + f["orig_name"]
            if expected in master_blocks:
                f["resolved_path"] = expected
                continue
            direct_root = root_resource + "." + f["orig_name"]
            if direct_root in master_blocks:
                f["resolved_path"] = direct_root
                continue
            candidates = [p for p in master_blocks.keys()
                          if p.endswith("." + f["orig_name"])]
            if len(candidates) == 1:
                f["resolved_path"] = candidates[0]
    return master_blocks


# =========================================================================
# generate_cxx_for_blocks  (exports referenced by compile_fhir_library)
# =========================================================================
# Notation: the function body is extremely long because it generates the
# full public + internal + cpp for every resource block.  The logic is
# preserved character-for-character from ffc.py lines 978–1129, with only
# the helper calls re-routed to local imports.

# Forward-declare the imports that compile_fhir_library and generate_cxx_for_blocks need.
# Actual emitter functions live in emit/{views,deserialize,store,traits}.py
# and are imported here to avoid circular deps.
from generator.emit.views import (  # noqa: E402
    generate_lazy_view_struct,
    generate_field_info_implementation,
    generate_reflection_dispatch,
)
from generator.emit.deserialize import generate_eager_deserializer  # noqa: E402
from generator.emit.store import generate_size_fields, generate_store_fields  # noqa: E402
from generator.emit.traits import generate_resource_traits_header  # noqa: E402


def generate_cxx_for_blocks(master_blocks, versions):
    """Returns (public_hpp, internal_hpp, cpp)."""
    public_hpp, internal_hpp, cpp = "", "", ""
    traits_hpp = ""
    block_data_names = sorted({path.replace(".", "") + "Data" for path in master_blocks})
    block_struct_names = sorted({
        "FF_" + path.replace(".", "_").upper() for path in master_blocks
    })
    block_view_names = sorted({
        s_name.replace("FF_", "") + "View" for s_name in block_struct_names
    })
    for d_name in block_data_names:
        public_hpp += f"struct {d_name};\n"
    if block_data_names:
        public_hpp += "\n"
    for s_name in block_struct_names:
        internal_hpp += f"struct {s_name};\n"
    for v_name in block_view_names:
        internal_hpp += f"template <uint32_t VERSION> struct {v_name};\n"
    if block_struct_names:
        internal_hpp += "\n"

    # Emit ALL view forward declarations at once (before any struct definitions)
    # to ensure forward declarations precede use even in template contexts.
    for v_name in sorted(block_view_names):
        internal_hpp += f"template <uint32_t VERSION> struct {v_name};\n"
    if block_view_names:
        internal_hpp += "\n"

    def get_deps(layout):
        deps = set()
        for f in layout:
            if f["fhir_type"] in master_blocks:
                deps.add(f["fhir_type"])
            if f.get("resolved_path") and f["resolved_path"] in master_blocks:
                deps.add(f["resolved_path"])
        return deps

    ordered_paths: list[str] = []
    visited: set[str] = set()

    def visit(path):
        if path in visited:
            return
        visited.add(path)
        for dep in get_deps(master_blocks[path]["layout"]):
            if dep in master_blocks:
                visit(dep)
        ordered_paths.append(path)

    for path in master_blocks:
        visit(path)

    for path in ordered_paths:
        blk = master_blocks[path]
        s_name = "FF_" + path.replace(".", "_").upper()
        d_name = path.replace(".", "") + "Data"
        layout = blk["layout"]
        sizes = blk["sizes"]

        # ── PUBLIC: POD Data Struct ──────────────────────────────
        public_hpp += f"struct {d_name} {{\n"
        for f in layout:
            code_enum = f.get("code_enum")
            if f.get("is_choice"):
                public_hpp += f"    ChoiceEntry {f['cpp_name']};\n"
            elif f["is_array"]:
                item_type = (
                    code_enum["enum"]
                    if code_enum
                    else _st._resolve_data_type_name(
                        f["fhir_type"], f["orig_name"], path, f.get("resolved_path")
                    )
                )
                public_hpp += f"    std::vector<{item_type}> {f['cpp_name']};\n"
            elif f["fhir_type"] == "string":
                public_hpp += f"    std::string_view {f['cpp_name']};\n"
            elif code_enum:
                public_hpp += (
                    f"    {code_enum['enum']} {f['cpp_name']}"
                    f" = static_cast<{code_enum['enum']}>(0);\n"
                )
            elif f["cpp_type"] == "Offset":
                public_hpp += (
                    f"    std::unique_ptr<{_st._resolve_data_type_name(f['fhir_type'], f['orig_name'], path, f.get('resolved_path'))}>"
                    f" {f['cpp_name']};\n"
                )
            elif f["data_type"] == "bool":
                public_hpp += f"    bool {f['cpp_name']} = false;\n"
            elif f["data_type"] == "std::string":
                public_hpp += f"    std::string {f['cpp_name']};\n"
            elif f["fhir_type"] in _tm.TYPE_MAP and "null" in _tm.TYPE_MAP[f["fhir_type"]]:
                null_val = _tm.TYPE_MAP[f["fhir_type"]]["null"]
                public_hpp += f"    {f['data_type']} {f['cpp_name']} = {null_val};\n"
            else:
                public_hpp += f"    {f['data_type']} {f['cpp_name']}{{}};\n"
        public_hpp += "};\n\n"

        # ── INTERNAL: Data Block Sentinel (vtable enums) ───────
        internal_hpp += f"struct FF_EXPORT {s_name} : DATA_BLOCK {{\n"
        internal_hpp += f'    static constexpr char type [] = "{s_name}";\n'
        internal_hpp += f"    static constexpr enum RECOVERY_TAG recovery = RECOVER_{s_name};\n"
        internal_hpp += (
            "    enum vtable_sizes {\n"
            "        VALIDATION_S = TYPE_SIZE_UINT64,\n"
            "        RECOVERY_S = TYPE_SIZE_UINT16,\n"
        )
        for f in layout:
            internal_hpp += f"        {f['name']}_S = {f['size_const']},\n"
        internal_hpp += (
            "    };\n    enum vtable_offsets {\n"
            "        VALIDATION = 0,\n"
            "        RECOVERY = VALIDATION + VALIDATION_S,\n"
        )
        prev_name, prev_size = "RECOVERY", "RECOVERY_S"
        for f in layout:
            internal_hpp += f"        {f['name']:<20}= {prev_name} + {prev_size},\n"
            prev_name, prev_size = f["name"], f"{f['name']}_S"
        for v, sz in sizes.items():
            internal_hpp += f"        HEADER_{v}_SIZE = {sz},\n"
        present_versions = [v for v in versions if v in sizes]
        max_v = max(present_versions, key=lambda x: versions.index(x))
        internal_hpp += f"        HEADER_SIZE = HEADER_{max_v}_SIZE\n    }};\n"

        min_version = next((v for v in versions if v in sizes), None)
        internal_hpp += "    inline Size get_header_size() const {\n"
        if min_version:
            internal_hpp += f"        if (__version < FHIR_VERSION_{min_version}) return 0;\n"
        for v in versions:
            if v in sizes:
                internal_hpp += f"        if (__version <= FHIR_VERSION_{v}) return HEADER_{v}_SIZE;\n"
        internal_hpp += "        return HEADER_SIZE;\n    }\n"

        internal_hpp += (
            f"    explicit {s_name}(Offset off, Size total_size, uint32_t ver)"
            f" : DATA_BLOCK(off, total_size, ver) {{}}\n"
        )
        internal_hpp += f"    static constexpr size_t FIELD_COUNT = {len(layout)};\n"
        internal_hpp += "    static const FF_FieldInfo FIELDS[FIELD_COUNT];\n"
        internal_hpp += (
            f"    static constexpr size_t COMPACT_SIZES_STRIDE"
            f" = {((len(layout) + 7) // 8) * 8};\n"
        )
        internal_hpp += (
            "    static const uint8_t COMPACT_SLOT_SIZES[COMPACT_SIZES_STRIDE];"
            "  // pre-baked, 8-aligned\n"
        )
        internal_hpp += (
            "    FF_Result validate_full(const BYTE* const __base) const noexcept;\n\n"
        )
        internal_hpp += (
            f"    static {d_name} deserialize(const BYTE* const __base,"
            f" Offset __offset, Size __size, uint32_t __version);\n"
        )
        internal_hpp += "    const FF_FieldInfo* find_field(std::string_view name) const;\n"
        internal_hpp += "};\n\n"

        # ── INTERNAL: Zero-copy View Template ─────────────────
        internal_hpp += generate_lazy_view_struct(
            layout, s_name,
            extra_methods=VIEW_EXTRA_METHODS.get(path, ""),
        )

        # ── INTERNAL: 3-arg STORE ─────────────────────────────
        internal_hpp += (
            f"Offset STORE_{s_name}(BYTE* const __base, Offset hdr_off,"
            f" Offset child_off, const {d_name}& data,"
            f" uint32_t __version = FHIR_VERSION_R5);\n\n"
        )

        bridge_name = s_name[3:] if s_name.startswith("FF_") else s_name

        # ── PUBLIC: Free-function declarations ────────────────
        public_hpp += (
            f"Size SIZE_{s_name}(const {d_name}& data,"
            f" uint32_t __version = FHIR_VERSION_R5);\n"
        )
        public_hpp += (
            f"Offset STORE_{s_name}(BYTE* const __base, Offset start_off,"
            f" const {d_name}& data, uint32_t __version = FHIR_VERSION_R5);\n"
        )
        public_hpp += (
            f"{d_name} FF_DESERIALIZE_{bridge_name}(const BYTE* const __base,"
            f" Offset __offset, Size __size, uint32_t __version);\n\n"
        )

        # ── PUBLIC: TypeTraits ─────────────────────────────────
        
        traits_hpp += f"template<> struct TypeTraits<{d_name}> {{\n"
        traits_hpp += f"    static constexpr auto recovery = RECOVER_{s_name};\n"
        traits_hpp += (
            f"    static Size size(const {d_name}& d, uint32_t v = FHIR_VERSION_R5)"
            f" {{ return SIZE_{s_name}(d, v); }}\n"
        )
        traits_hpp += (
            f"    static void store(BYTE* const base, Offset off,"
            f" const {d_name}& d, uint32_t v = FHIR_VERSION_R5)"
            f" {{ STORE_{s_name}(base, off, d, v); }}\n"
        )
        traits_hpp += (
            f"    static {d_name} read(const BYTE* const base, Offset off,"
            f" Size size, uint32_t v)"
            f" {{ return FF_DESERIALIZE_{bridge_name}(base, off, size, v); }}\n"
        )
        traits_hpp += "};\n"
        

        # ── CPP: Implementations ──────────────────────────────
        cpp += (
            f"FF_Result {s_name}::validate_full(const BYTE *const __base)"
            f" const noexcept {{ return validate_offset(__base, type, recovery); }}\n"
        )
        cpp += generate_field_info_implementation(layout, s_name)

        cpp += (
            f"{d_name} {s_name}::deserialize(const BYTE *const __base,"
            f" Offset __offset, Size __size, uint32_t __version) {{\n"
            f"{generate_eager_deserializer(layout, s_name, d_name)}}}\n\n"
        )

        cpp += (
            f"Size SIZE_{s_name}(const {d_name}& data, uint32_t __version) {{\n"
            f"{generate_size_fields(layout, s_name, 'data')}}}\n\n"
        )

        cpp += (
            f"Offset STORE_{s_name}(BYTE* const __base, Offset hdr_off,"
            f" Offset child_off, const {d_name}& data, uint32_t __version) {{\n"
            f"    auto __ptr = __base + hdr_off;\n"
            f"    STORE_U64(__ptr + {s_name}::VALIDATION, hdr_off);\n"
            f"    STORE_U16(__ptr + {s_name}::RECOVERY, {s_name}::recovery);\n"
            f"{generate_store_fields(layout, s_name, '__ptr', 'data')}\n"
            f"    return child_off;\n}}\n\n"
        )

        cpp += (
            f"Offset STORE_{s_name}(BYTE* const __base, Offset start_off,"
            f" const {d_name}& data, uint32_t __version) {{\n"
            f"    return STORE_{s_name}(__base, start_off,"
            f" start_off + {s_name}::HEADER_SIZE, data, __version);\n}}\n\n"
        )
        cpp += (
            f"{d_name} FF_DESERIALIZE_{bridge_name}(const BYTE* const __base,"
            f" Offset __offset, Size __size, uint32_t __version) {{\n"
            f"    return {s_name}::deserialize(__base, __offset, __size, __version);\n}}\n\n"
        )

    public_hpp += traits_hpp
    return public_hpp, internal_hpp, cpp
