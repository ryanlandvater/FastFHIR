"""FastFHIR Generator — library compiler driver.

compile_fhir_library is the hub that sequences per-resource code generation:
spec-bundle reads → merge → vtable emission → python stubs → ingest mappings.
Relocated from ffc.py lines 2009-2356 with imports repointed to the
relocated module hierarchy (model / emit / bindings).

This is the ONE function external callers invoke through the public API
(``generator.run()`` → ``generator.library.compile_fhir_library()``).

Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
"""

from __future__ import annotations

import json
import os
import re

from generator.emit.header import auto_header, write_if_changed
from generator.model import type_map as _tm
from generator.model import structure as _st
from generator.model.merge import merge_fhir_versions, generate_cxx_for_blocks
from generator.emit.views import (
    generate_lazy_view_struct,
    generate_field_info_implementation,
    generate_reflection_dispatch,
)
from generator.emit.deserialize import generate_eager_deserializer
from generator.emit.store import generate_size_fields, generate_store_fields
from generator.emit.traits import generate_resource_traits_header
from generator.bindings.python_fields import (
    emit_python_fields,
    emit_python_ast,
    emit_python_fields_stubs,
    emit_python_ast_stubs,
    emit_py_typed_marker,
)


def compile_fhir_library(
    resources: list[str],
    versions: list[str],
    input_dir: str = "fhir_specs",
    output_dir: str = "generated_src",
    code_enum_map: dict | None = None,
) -> None:
    code_enums = code_enum_map or {}
    os.makedirs(output_dir, exist_ok=True)
    all_field_names: set[str] = set()
    all_block_paths: set[str] = set()
    reflected_block_names: set[str] = set()
    block_key_defs: list[tuple[str, list[dict]]] = []
    python_resource_map: dict[str, dict[str, tuple]] = {}
    token_registry: dict[str, dict[str, tuple[int, str]]] = {}

    print("Generating FF_DataTypes...")
    type_bundles: list[tuple[str, dict]] = []
    for v in versions:
        p = os.path.join(input_dir, v, "profiles-types.json")
        if os.path.exists(p):
            with open(p, "r", encoding="utf-8") as f:
                type_bundles.append((v, json.load(f)))

    fwd_decls = {t + "Data" for t in _tm.PRODUCTION_TYPES}
    hpp_head = (
        f"{auto_header}// MARK: - Universal Data Types\n#pragma once\n"
        '#include "../include/FF_Primitives.hpp"\n'
        '#include "../include/FF_Utilities.hpp"\n'
        '#include "../include/FF_Builder.hpp"\n'
        '#include "FF_CodeSystems.hpp"\n'
        "#include <vector>\n#include <string_view>\n#include <memory>\n\n"
    )
    hpp_head += "namespace FastFHIR { template<typename T> struct TypeTraits; \n\n"
    hpp_head += _DATA_TYPES_TRAITS
    hpp_head += "// Forward Declarations\n"
    for dec in sorted(fwd_decls):
        hpp_head += f"struct {dec};\n"
    hpp_head += "\n"

    types_hpp = hpp_head
    types_cpp = f'{auto_header}#include "FF_DataTypes.hpp"\n#include "FF_DataTypes_internal.hpp"\n#include "../include/FF_Utilities.hpp"\n#include "FF_Dictionary.hpp"\n\n'
    types_int_hpp = f"{auto_header}#pragma once\n#include \"FF_DataTypes.hpp\"\n\n"
    types_all = list(_tm.PRODUCTION_TYPES)
    all_blocks: dict[str, dict] = {}
    all_type_blocks: dict[str, dict] = {}
    # Token registry and python map must cover data types too, not just resources.
    # Initialised here so the types loop below can populate them.
    python_resource_map: dict[str, dict[str, tuple]] = {}
    token_registry: dict[str, dict[str, tuple[int, str]]] = {}

    for root_type in types_all:
        schemas_by_version = []
        for v_name, bundle in type_bundles:
            try:
                elements = _st.extract_structure_definition(bundle, root_type)
                schemas_by_version.append((v_name, elements))
            except ValueError:
                continue
        if not schemas_by_version:
            continue
        master_type_blocks = merge_fhir_versions(schemas_by_version, root_type)

        for path, blk in master_type_blocks.items():
            key = path if path != root_type else root_type
            if key not in all_blocks:
                all_blocks[key] = blk
                all_block_paths.add(key)
            else:
                existing = all_blocks[key]
                existing["sizes"].update(blk["sizes"])
                for v_name in blk["sizes"]:
                    if v_name not in existing.get("_versions", set()):
                        existing.setdefault("_versions", set()).add(v_name)

        _st._annotate_code_enums(master_type_blocks, code_enums)

        # Accumulate all type blocks for a single generate_cxx_for_blocks pass
        # (matching old ffc.py, ensuring cross-type dependency ordering)
        for path, blk in master_type_blocks.items():
            if path not in all_type_blocks:
                all_type_blocks[path] = blk

        all_block_paths.update(p for p in master_type_blocks if p not in all_block_paths)

        # Add data-type blocks to reflection dispatch
        reflected_block_names.update(
            "FF_" + path.replace(".", "_").upper() for path in master_type_blocks
        )
        # Only add root type if it has actual blocks (excludes constrained types
        # like SimpleQuantity whose snapshot reuses base-type paths, e.g. Quantity.*)
        if root_type in master_type_blocks:
            reflected_block_names.add(f"FF_{root_type.upper()}")

        # Populate token_registry and python_resource_map for data-type blocks
        # so emit_python_ast can resolve fields on types like Address, HumanName, etc.
        for path, blk in master_type_blocks.items():
            for fld in blk["layout"]:
                orig = fld["orig_name"]
                all_field_names.add(orig)
                c_name = _st._field_key_constant_name(orig)
                owner_ns = _st._block_key_namespace(path)
                all_field_names.add(c_name)
                ns_name = f"{owner_ns}::{c_name}"
                owner = path.replace(".", "_").upper()
                if owner not in python_resource_map:
                    python_resource_map[owner] = {}
                token_registry.setdefault(path, {})[orig] = (len(python_resource_map[owner]), ns_name)
                python_resource_map[owner][orig] = (
                    len(python_resource_map[owner]) - 1,
                    orig,
                    owner,
                    fld.get("fhir_type", "string"),
                )

    # --- Emit FF_DataTypes (single pass, all blocks for correct dependency ordering) ---
    if all_type_blocks:
        pub_hpp, int_hpp, cpp_body = generate_cxx_for_blocks(all_type_blocks, versions)
        types_hpp += pub_hpp
        types_int_hpp += int_hpp
        types_cpp += cpp_body
    types_cpp += "} // namespace FastFHIR\n"

    write_if_changed(os.path.join(output_dir, "FF_DataTypes.hpp"), types_hpp)
    write_if_changed(os.path.join(output_dir, "FF_DataTypes_internal.hpp"), types_int_hpp)
    write_if_changed(os.path.join(output_dir, "FF_DataTypes.cpp"), types_cpp)
    print("-- Emitted FF_DataTypes")

    # --- Resources ---
    generated_resources: list[str] = []

    # Pre-load resource bundles once
    resource_bundles: list[tuple[str, dict]] = []
    for v in versions:
        p = os.path.join(input_dir, v, "profiles-resources.json")
        if os.path.exists(p):
            with open(p, "r", encoding="utf-8") as f:
                resource_bundles.append((v, json.load(f)))

    for root_resource in resources:
        print(f"  Generating {root_resource}...")
        schemas_by_version = []
        for v_name, bundle in resource_bundles:
            try:
                elements = _st.extract_structure_definition(bundle, root_resource)
                schemas_by_version.append((v_name, elements))
            except ValueError:
                continue
        if not schemas_by_version:
            continue

        master_resource_blocks = merge_fhir_versions(schemas_by_version, root_resource)
        for path, blk in master_resource_blocks.items():
            key = path if path != root_resource else root_resource
            if key not in all_blocks:
                all_blocks[key] = blk
                all_block_paths.add(key)

        _st._annotate_code_enums(master_resource_blocks, code_enums)
        pub_hpp, int_hpp, cpp_body = generate_cxx_for_blocks(master_resource_blocks, versions)

        res_hpp = f"{auto_header}#pragma once\n"
        res_hpp += f'#include "FF_DataTypes.hpp"\n\n'
        res_hpp += pub_hpp
        write_if_changed(os.path.join(output_dir, f"FF_{root_resource}.hpp"), res_hpp)

        res_int_hpp = f"{auto_header}#pragma once\n"
        res_int_hpp += f'#include "FF_{root_resource}.hpp"\n'
        res_int_hpp += f'#include "FF_DataTypes_internal.hpp"\n\n'
        res_int_hpp += int_hpp
        write_if_changed(os.path.join(output_dir, f"FF_{root_resource}_internal.hpp"), res_int_hpp)

        res_cpp = f'{auto_header}#include "FF_{root_resource}_internal.hpp"\n#include "../include/FF_Utilities.hpp"\n#include "FF_Dictionary.hpp"\n\n'
        res_cpp += cpp_body
        # Close the namespace opened by FF_DataTypes.hpp (included transitively).
        # All generated code uses FastFHIR:: fully-qualified names internally, but
        # type aliases like ExtensionData, Decode etc. in *internal.hpp are
        # unqualified and rely on being inside namespace FastFHIR.
        res_cpp += "} // namespace FastFHIR\n"
        write_if_changed(os.path.join(output_dir, f"FF_{root_resource}.cpp"), res_cpp)
        generated_resources.append(root_resource)
        reflected_block_names.update(
            "FF_" + path.replace(".", "_").upper() for path in master_resource_blocks
        )
        reflected_block_names.add(f"FF_{root_resource.upper()}")

        for path, blk in master_resource_blocks.items():
            for fld in blk["layout"]:
                orig = fld["orig_name"]
                all_field_names.add(orig)
                c_name = _st._field_key_constant_name(orig)
                owner_ns = _st._block_key_namespace(path)
                all_field_names.add(c_name)
                short = _st._field_key_short_name(orig)
                ns_name = f"{owner_ns}::{c_name}"
                owner = path.replace(".", "_").upper()
                if owner not in python_resource_map:
                    python_resource_map[owner] = {}
                token_registry.setdefault(path, {})[orig] = (len(python_resource_map[owner]), ns_name)
                python_resource_map[owner][orig] = (
                    len(python_resource_map[owner]) - 1,
                    orig,
                    owner,
                    fld.get("fhir_type", "string"),
                )

        block_key_defs.append((root_resource, []))
        for path in sorted(all_blocks, key=lambda x: (x.count("."), x)):
            if path not in [b[0] for b in block_key_defs]:
                block_key_defs.append((path, all_blocks[path]["layout"]))

    # --- Field Keys ---
    # Emit rich FF_FieldKey constants with full metadata (matching old ffc.py):
    #   1. Global string-name constants (FastFHIR::FieldKeys::FF_ACTIVE)
    #   2. Schema-specific 6-arg keys (FastFHIR::Fields::PATIENT::ACTIVE with rec, kind, offset, child, arr)
    #   3. Registry[] array for runtime iteration
    field_keys_hpp = (
        f"{auto_header}#pragma once\n"
        f'#include "../include/FF_Primitives.hpp"\n\n'
        f"namespace FastFHIR::FieldKeys {{\n"
        f"    extern const FF_FieldKey* const Registry[];\n"
        f"    extern const size_t RegistrySize;\n\n"
    )
    for field_name in sorted(all_field_names):
        # Skip c_name variants (e.g. "FF_ACTIVE") — only emit for original names
        if field_name.startswith("FF_"):
            continue
        const_name = _st._field_key_constant_name(field_name)
        field_keys_hpp += f"    inline constexpr FF_FieldKey {const_name}{{\"{field_name}\"}};\n"
    
    field_keys_hpp += "\n} // namespace FastFHIR::FieldKeys\n\n"
    field_keys_hpp += "namespace FastFHIR::Fields {\n"
    
    registry_entries: list[str] = []
    seen_blocks: set[str] = set()
    
    for path, layout in sorted(block_key_defs, key=lambda item: item[0]):
        if path in seen_blocks:
            continue
        seen_blocks.add(path)
        
        ns_name = _st._block_key_namespace(path)
        block_struct_name = "FF_" + path.replace(".", "_").upper()
        
        field_keys_hpp += f"namespace {ns_name} {{\n"
        for f in layout:
            short_name = _st._field_key_short_name(f["orig_name"])
            child_rec = _st._child_recovery_key_expr(f, block_struct_name)
            arr_offsets = _st._array_entries_are_offsets_expr(f)
            owner_rec = f"ToArrayTag(RECOVER_{block_struct_name})" if f.get("is_array") else f"RECOVER_{block_struct_name}"
            field_kind = _st._field_kind_expr(f)
            
            field_keys_hpp += (
                f"    inline constexpr FF_FieldKey {short_name}"
                f"{{{owner_rec}, {field_kind}, {f['offset']}, "
                f"{child_rec}, {arr_offsets}, \"{f['cpp_name']}\"}};\n"
            )
            registry_entries.append(f"        &FastFHIR::Fields::{ns_name}::{short_name}")
        
        field_keys_hpp += f"}} // namespace {ns_name}\n\n"
    
    field_keys_hpp += "} // namespace FastFHIR::Fields\n"
    
    field_keys_cpp = (
        f"{auto_header}\n"
        f'#include "FF_FieldKeys.hpp"\n\n'
        f"namespace FastFHIR::FieldKeys {{\n"
        f"    const FF_FieldKey* const Registry[] = {{\n"
        + ",\n".join(registry_entries)
        + f"\n    }};\n\n"
        f"    const size_t RegistrySize = {len(registry_entries)};\n"
        f"}} // namespace FastFHIR::FieldKeys\n"
    )
    
    write_if_changed(os.path.join(output_dir, "FF_FieldKeys.hpp"), field_keys_hpp)
    write_if_changed(os.path.join(output_dir, "FF_FieldKeys.cpp"), field_keys_cpp)

    # --- Python field modules + AST path builders + stubs ---
    emit_python_fields(python_resource_map, output_dir)
    emit_python_ast(all_blocks, block_key_defs, token_registry, output_dir)
    emit_python_fields_stubs(python_resource_map, output_dir)
    emit_python_ast_stubs(all_blocks, block_key_defs, output_dir)
    emit_py_typed_marker(output_dir)

    # --- Reflection dispatch ---
    reflection_hpp, reflection_cpp = generate_reflection_dispatch(
        sorted(reflected_block_names), resources
    )
    write_if_changed(os.path.join(output_dir, "FF_Reflection.hpp"), reflection_hpp)
    write_if_changed(os.path.join(output_dir, "FF_Reflection.cpp"), reflection_cpp)

    # --- Resource type traits ---
    resource_traits_hpp = generate_resource_traits_header(resources)
    write_if_changed(os.path.join(output_dir, "FF_ResourceTypes.hpp"), resource_traits_hpp)

    # --- FF_AllTypes.hpp aggregator ---
    all_types_hpp = (
        f"{auto_header}#pragma once\n"
        '#include "FF_DataTypes_internal.hpp"\n'
        '#include "FF_FieldKeys.hpp"\n'
        '#include "FF_ResourceTypes.hpp"\n'
        '#include "FF_Reflection.hpp"\n'
    )
    for res in generated_resources:
        all_types_hpp += f'#include "FF_{res}_internal.hpp"\n'
    # No namespace wrapper — FF_AllTypes.hpp is an aggregator that includes
    # internal headers. Those headers rely on already being inside
    # namespace FastFHIR (from FF_DataTypes.hpp or the .cpp file's context).
    # A closing } here would be unbalanced and break inclusion from namespace
    # contexts like namespace FastFHIR::Ingest (FF_IngestMappings).
    write_if_changed(os.path.join(output_dir, "FF_AllTypes.hpp"), all_types_hpp)

    # --- Validate RECOVERY_TAG references against permanent header ---
    _parse_recovery_tags("include/FF_Recovery.hpp")

    # --- Ingest mappings ---
    from generator.emit.ingest_mappings import generate_ingest_mappings

    generate_ingest_mappings(all_blocks, resources, output_dir)

    print("\n[Success] FastFHIR Library generation complete.")


# ─── Data from ffc.py compiled-in TypeTraits preamble ─────────────────
_DATA_TYPES_TRAITS = """template<> struct TypeTraits<std::string_view> {
    static constexpr auto recovery = RECOVER_FF_STRING;
    static Size size(std::string_view d, uint32_t = FHIR_VERSION_R5) { return SIZE_FF_STRING(d); }
    static void store(BYTE* const base, Offset off, std::string_view d, uint32_t = FHIR_VERSION_R5) { STORE_FF_STRING(base, off, d); }
};

template<> struct TypeTraits<std::vector<Offset>> {
};

template<> struct TypeTraits<std::vector<ResourceReference>> {
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_RESOURCE | RECOVER_ARRAY_BIT);
    static Size size(const std::vector<ResourceReference>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_RESOURCE); }
    static void store(BYTE* const base, Offset off, const std::vector<ResourceReference>& d, uint32_t = FHIR_VERSION_R5) {
        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_RESOURCE, static_cast<uint32_t>(d.size()), recovery);
        for (const auto& ref : d) {
            STORE_U64(base + off, ref.offset); STORE_U16(base + off + DATA_BLOCK::RECOVERY, ref.recovery); off += TYPE_SIZE_RESOURCE;
        }
    }
};

template<> struct TypeTraits<std::vector<uint8_t>> {
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_BOOL | RECOVER_ARRAY_BIT);
    static Size size(const std::vector<uint8_t>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_UINT8); }
    static void store(BYTE* const base, Offset off, const std::vector<uint8_t>& d, uint32_t = FHIR_VERSION_R5) {
        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_UINT8, static_cast<uint32_t>(d.size()), recovery);
        for (const auto& v : d) { STORE_U8(base + off, v); off += TYPE_SIZE_UINT8; }
    }
};

template<> struct TypeTraits<std::vector<uint32_t>> {
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_UINT32 | RECOVER_ARRAY_BIT);
    static Size size(const std::vector<uint32_t>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_UINT32); }
    static void store(BYTE* const base, Offset off, const std::vector<uint32_t>& d, uint32_t = FHIR_VERSION_R5) {
        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_UINT32, static_cast<uint32_t>(d.size()), recovery);
        for (const auto& v : d) { STORE_U32(base + off, v); off += TYPE_SIZE_UINT32; }
    }
};

template<> struct TypeTraits<std::vector<double>> {
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_FLOAT64 | RECOVER_ARRAY_BIT);
    static Size size(const std::vector<double>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_FLOAT64); }
    static void store(BYTE* const base, Offset off, const std::vector<double>& d, uint32_t = FHIR_VERSION_R5) {
        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_FLOAT64, static_cast<uint32_t>(d.size()), recovery);
        for (const auto& v : d) { STORE_F64(base + off, v); off += TYPE_SIZE_FLOAT64; }
    }
};

"""


# ─── Shared helper (also lives in emit/dictionary.py) ─────────────────
def _parse_recovery_tags(recovery_path: str = "include/FF_Recovery.hpp") -> dict[str, int]:
    tags: dict[str, int] = {}
    with open(recovery_path, "r") as f:
        for line in f:
            m = re.match(r"\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", line)
            if m:
                tags[m.group(1)] = int(m.group(2), 16)
    return tags
