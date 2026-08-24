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
    emit_python_fields_init,
    emit_python_fields_stubs,
    emit_python_ast_stubs,
    emit_py_typed_marker,
)
from generator.utilities import (
    enclose_namespace,
    validate_recovery_bands,
    validate_recovery_tags,
)


def compile_fhir_library(
    resources: list[str],
    versions: list[str],
    input_dir: str = "fhir_packages",
    output_dir: str = "generated_src",
    code_enum_map: dict | None = None,
    external_system_map: dict | None = None,
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
        pkg = os.path.join(input_dir, v, "package")
        if os.path.isdir(pkg):
            type_bundles.append((v, _st.load_npm_bundle(pkg)))

    fwd_decls = {t + "Data" for t in _tm.PRODUCTION_TYPES}
    hpp_head = (
        f"{auto_header}// MARK: - Universal Data Types\n#pragma once\n"
        '#include "FF_Primitives.hpp"\n'
        '#include "FF_Utilities.hpp"\n'
        '#include "FF_Builder.hpp"\n'
        '#include "FF_CodeSystems.hpp"\n'
        "#include <vector>\n#include <string_view>\n#include <memory>\n\n"
    )
    traits_preamble = "template<typename T> struct TypeTraits; \n\n" + _DATA_TYPES_TRAITS
    hpp_head += enclose_namespace("FastFHIR", traits_preamble)
    hpp_head += "using namespace FastFHIR;\n\n"
    hpp_head += "// Forward Declarations\n"
    for dec in sorted(fwd_decls):
        hpp_head += f"struct {dec};\n"
    hpp_head += "\n"

    types_hpp = hpp_head
    types_cpp = f'{auto_header}#include "FF_DataTypes.hpp"\n#include "FF_DataTypes_internal.hpp"\n#include "FF_Utilities.hpp"\n#include "FF_Dictionary.hpp"\n\n'
    types_int_hpp = f'{auto_header}#pragma once\n#include "FF_DataTypes.hpp"\n\n'
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
        _st._annotate_external_systems(master_type_blocks, external_system_map)

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
                short = _st._field_key_short_name(orig)
                ns_name = f"{owner_ns}::{c_name}"
                owner = path.replace(".", "_").upper()
                # The map key is the FHIR class name the docs/tests import
                # (Patient, BundleEntry, ObservationComponent) — derived from the
                # path's own case, not reconstructible from the uppercase owner.
                class_key = "".join(seg[:1].upper() + seg[1:] for seg in path.split("."))
                if class_key not in python_resource_map:
                    python_resource_map[class_key] = {}
                token_registry.setdefault(path, {})[orig] = (
                    len(python_resource_map[class_key]),
                    ns_name,
                )
                python_resource_map[class_key][orig] = (
                    len(python_resource_map[class_key]) - 1,
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

    write_if_changed(os.path.join(output_dir, "FF_DataTypes.hpp"), types_hpp)
    write_if_changed(os.path.join(output_dir, "FF_DataTypes_internal.hpp"), types_int_hpp)
    write_if_changed(os.path.join(output_dir, "FF_DataTypes.cpp"), types_cpp)
    print("-- Emitted FF_DataTypes")

    # --- Resources ---
    generated_resources: list[str] = []

    # Pre-load resource bundles once. The NPM packages ship individual
    # StructureDefinition-*.json files, not the profiles-resources.json bundle
    # the old fhir_specs/ layout had, so synthesise the bundle shape.
    resource_bundles: list[tuple[str, dict]] = []
    for v in versions:
        pkg = os.path.join(input_dir, v, "package")
        if os.path.isdir(pkg):
            resource_bundles.append((v, _st.load_npm_bundle(pkg)))
    if not resource_bundles:
        raise RuntimeError(
            f"No FHIR packages under {input_dir}/<version>/package -- cannot generate "
            "resources. Run the generator with network access so specs.py can fetch them."
        )

    skipped: list[str] = []
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
            # Absent from every version we loaded. Record it and fail at the end
            # rather than silently emitting a library with holes in it.
            skipped.append(root_resource)
            continue

        master_resource_blocks = merge_fhir_versions(schemas_by_version, root_resource)
        for path, blk in master_resource_blocks.items():
            key = path if path != root_resource else root_resource
            if key not in all_blocks:
                all_blocks[key] = blk
                all_block_paths.add(key)

        _st._annotate_code_enums(master_resource_blocks, code_enums)
        _st._annotate_external_systems(master_resource_blocks, external_system_map)
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

        res_cpp = f'{auto_header}#include "FF_{root_resource}_internal.hpp"\n#include "FF_Utilities.hpp"\n#include "FF_Dictionary.hpp"\n\n'
        res_cpp += cpp_body
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
                # Same PascalCase key rule as the data-type pass above.
                class_key = "".join(seg[:1].upper() + seg[1:] for seg in path.split("."))
                if class_key not in python_resource_map:
                    python_resource_map[class_key] = {}
                token_registry.setdefault(path, {})[orig] = (
                    len(python_resource_map[class_key]),
                    ns_name,
                )
                python_resource_map[class_key][orig] = (
                    len(python_resource_map[class_key]) - 1,
                    orig,
                    owner,
                    fld.get("fhir_type", "string"),
                )

        block_key_defs.append((root_resource, all_blocks.get(root_resource, {}).get("layout", [])))
        for path in sorted(all_blocks, key=lambda x: (x.count("."), x)):
            if path not in [b[0] for b in block_key_defs]:
                block_key_defs.append((path, all_blocks[path]["layout"]))

    if skipped:
        raise RuntimeError(
            f"{len(skipped)} production resources were not found in any FHIR package "
            f"and would be silently missing from the library: {skipped}"
        )

    # --- Field Keys ---
    # Emit rich FF_FieldKey constants with full metadata (matching old ffc.py):
    #   1. Global string-name constants (FastFHIR::FieldKeys::FF_ACTIVE)
    #   2. Schema-specific 6-arg keys (FastFHIR::Fields::PATIENT::ACTIVE with rec, kind, offset, child, arr)
    #   3. Registry[] array for runtime iteration
    field_keys_hpp = f"{auto_header}#pragma once\n" f'#include "FF_Primitives.hpp"\n\n'

    # ── FastFHIR::FieldKeys ──────────────────────────────────────
    # Accumulate body first, then wrap once — no manual open/close tracking.
    fieldkeys_body = ""
    fieldkeys_body += "    extern const FF_FieldKey* const Registry[];\n"
    fieldkeys_body += "    extern const size_t RegistrySize;\n\n"
    for field_name in sorted(all_field_names):
        # Skip c_name variants (e.g. "FF_ACTIVE") — only emit for original names
        if field_name.startswith("FF_"):
            continue
        const_name = _st._field_key_constant_name(field_name)
        fieldkeys_body += f'    inline constexpr FF_FieldKey {const_name}{{"{field_name}"}};\n'
    field_keys_hpp += enclose_namespace("FastFHIR::FieldKeys", fieldkeys_body) + "\n"

    # ── FastFHIR::Fields ─────────────────────────────────────────
    # Each block gets its own sub-namespace (e.g. PATIENT, OBSERVATION)
    # wrapped inside the outer FastFHIR::Fields namespace.
    fields_inner = ""

    registry_entries: list[str] = []
    registry_index: dict[tuple[str, str], int] = {}
    seen_blocks: set[str] = set()

    for path, layout in sorted(block_key_defs, key=lambda item: item[0]):
        if path in seen_blocks:
            continue
        seen_blocks.add(path)

        ns_name = _st._block_key_namespace(path)
        block_struct_name = "FF_" + path.replace(".", "_").upper()

        # Accumulate this block's field keys, then wrap in its sub-namespace
        block_body = ""
        for f in layout:
            short_name = _st._field_key_short_name(f["orig_name"])
            child_rec = _st._child_recovery_key_expr(f, block_struct_name)
            arr_offsets = _st._array_entries_are_offsets_expr(f)
            owner_rec = (
                f"ToArrayTag(RECOVER_{block_struct_name})"
                if f.get("is_array")
                else f"RECOVER_{block_struct_name}"
            )
            field_kind = _st._field_kind_expr(f)

            block_body += (
                f"    inline constexpr FF_FieldKey {short_name}"
                f"{{{owner_rec}, {field_kind}, {f['offset']}, "
                f"{child_rec}, {arr_offsets}, \"{f['cpp_name']}\"}};\n"
            )
            registry_entries.append(f"        &FastFHIR::Fields::{ns_name}::{short_name}")
            # Record the true global Registry index (keyed by the class name the
            # Python fields import) so emit_python_fields can emit it — the
            # per-block indices assigned during map construction are off by one
            # and do not index this array.
            ck = "".join(seg[:1].upper() + seg[1:] for seg in path.split("."))
            registry_index[(ck, f["orig_name"])] = len(registry_entries) - 1

        fields_inner += enclose_namespace(ns_name, block_body) + "\n"

    field_keys_hpp += enclose_namespace("FastFHIR::Fields", fields_inner) + "\n"

    # ── Reconcile Python field indices with the C++ Registry ─────────────────
    # The bindings resolve a Python Field by indexing the generated C++
    # Registry (FF_FieldKeys.cpp), a global array. Rewrite the per-block map
    # metadata with the true global index; a field the registry does not know
    # is a generator bug and must fail loudly, not emit a wrong index.
    for _ck, _fields in python_resource_map.items():
        for _orig, _meta in _fields.items():
            _idx, _orig_name, _owner, _fhir_type = _meta
            python_resource_map[_ck][_orig] = (
                registry_index[(_ck, _orig)],
                _orig_name,
                _owner,
                _fhir_type,
            )

    cpp_body_fieldkeys = (
        f"    const FF_FieldKey* const Registry[] = {{\n"
        + ",\n".join(registry_entries)
        + f"\n    }};\n\n"
        f"    const size_t RegistrySize = {len(registry_entries)};\n"
    )
    field_keys_cpp = f"{auto_header}\n" f'#include "FF_FieldKeys.hpp"\n\n' + enclose_namespace(
        "FastFHIR::FieldKeys", cpp_body_fieldkeys
    )

    write_if_changed(os.path.join(output_dir, "FF_FieldKeys.hpp"), field_keys_hpp)
    write_if_changed(os.path.join(output_dir, "FF_FieldKeys.cpp"), field_keys_cpp)

    # --- Python field modules + AST path builders + stubs ---
    emit_python_fields(python_resource_map, output_dir)
    emit_python_ast(all_blocks, block_key_defs, token_registry, output_dir)
    emit_python_fields_init(python_resource_map, output_dir)
    emit_python_fields_stubs(python_resource_map, output_dir)
    emit_python_ast_stubs(all_blocks, block_key_defs, output_dir)
    emit_py_typed_marker(output_dir)

    # --- Reflection dispatch ---
    # Choice [x] variants name a top-level type -- mostly data types (Quantity,
    # CodeableConcept, Period), occasionally a resource. Dotless paths only:
    # a backbone element like "Observation.component" is never a variant.
    reflection_hpp, reflection_cpp = generate_reflection_dispatch(
        sorted(reflected_block_names),
        resources,
        top_level_types=sorted(p for p in all_blocks if "." not in p),
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
    # NOTE: No namespace wrapper — FF_AllTypes.hpp is a flat aggregator.
    # The internal headers define DATA_BLOCK subclasses at global scope
    # (using `using namespace FastFHIR;` from the public header chain).
    # Callers (like FF_IngestMappings) include this at global scope and
    # open their own namespace (FastFHIR::Ingest) afterwards. A namespace
    # wrapper here would double-nest and break std:: resolution in headers
    # included afterward.
    write_if_changed(os.path.join(output_dir, "FF_AllTypes.hpp"), all_types_hpp)

    # --- Validate RECOVERY_TAG references against the permanent header ---
    # generated_src/FF_Recovery.hpp is GENERATED from dictionaries/master_tags.json at
    # stage 1b, so every tag the spec needs is already declared by now. The
    # emitters build some tag names by concatenation, though, so this checks
    # every emitted name actually resolves, instead of letting a bad one surface
    # as a wall of C++ "undeclared identifier" errors.
    # Against the header just emitted into output_dir, NOT a hardcoded
    # "generated_src/FF_Recovery.hpp". The wire-format tests generate into a
    # temporary directory (tests/generator/conftest.py), so the hardcoded path
    # made a clean checkout fail outright and a dirty one validate the temporary
    # sources against an unrelated stale header -- the check silently stopped
    # being about the tree it was checking.
    recovery_hpp = os.path.join(output_dir, "FF_Recovery.hpp")
    n_tags = validate_recovery_tags(output_dir, recovery_hpp)
    print(f"-- Validated {n_tags} RECOVERY_TAG references against {recovery_hpp}")

    # --- Validate the header's own band discipline ---
    # Independent of what was emitted: every tag must sit inside a band, and no
    # two tags may share a value. Bands drive runtime classification
    # (FF_IsResourceTag), so a misplaced tag is silent, not a compile error.
    validate_recovery_bands(recovery_hpp)
    print("-- Validated RECOVERY_TAG band membership and uniqueness")

    # --- Ingest mappings ---
    from generator.emit.ingest_mappings import generate_ingest_mappings

    generate_ingest_mappings(all_blocks, resources, output_dir)

    print("\n[Success] FastFHIR Library generation complete.")


# ─── Data from ffc.py compiled-in TypeTraits preamble ─────────────────
_DATA_TYPES_TRAITS = """template<> struct TypeTraits<std::string_view> {
    static constexpr auto recovery = RECOVER_FF_STRING;
    static Size size(std::string_view d, uint32_t = FHIR_VERSION_R5) { return SIZE_FF_STRING(d); }
    static Offset store(BYTE* const base, Offset off, std::string_view d, uint32_t = FHIR_VERSION_R5) { return off + STORE_FF_STRING(base, off, d); }
};

template<> struct TypeTraits<std::vector<Offset>> {
};

template<> struct TypeTraits<std::vector<ResourceReference>> {
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_RESOURCE | RECOVER_ARRAY_BIT);
    static Size size(const std::vector<ResourceReference>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_RESOURCE); }
    static Offset store(BYTE* const base, Offset off, const std::vector<ResourceReference>& d, uint32_t = FHIR_VERSION_R5) {
        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_RESOURCE, static_cast<uint32_t>(d.size()), recovery);
        for (const auto& ref : d) {
            STORE_U64(base + off, ref.offset); STORE_U16(base + off + DATA_BLOCK::RECOVERY, ref.recovery); off += TYPE_SIZE_RESOURCE;
        }
        return off;
    }
};

template<> struct TypeTraits<std::vector<uint8_t>> {
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_BOOL | RECOVER_ARRAY_BIT);
    static Size size(const std::vector<uint8_t>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_UINT8); }
    static Offset store(BYTE* const base, Offset off, const std::vector<uint8_t>& d, uint32_t = FHIR_VERSION_R5) {
        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_UINT8, static_cast<uint32_t>(d.size()), recovery);
        for (const auto& v : d) { STORE_U8(base + off, v); off += TYPE_SIZE_UINT8; }
        return off;
    }
};

template<> struct TypeTraits<std::vector<uint32_t>> {
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_UINT32 | RECOVER_ARRAY_BIT);
    static Size size(const std::vector<uint32_t>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_UINT32); }
    static Offset store(BYTE* const base, Offset off, const std::vector<uint32_t>& d, uint32_t = FHIR_VERSION_R5) {
        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_UINT32, static_cast<uint32_t>(d.size()), recovery);
        for (const auto& v : d) { STORE_U32(base + off, v); off += TYPE_SIZE_UINT32; }
        return off;
    }
};

template<> struct TypeTraits<std::vector<double>> {
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_FLOAT64 | RECOVER_ARRAY_BIT);
    // A decimal entry is TYPE_SIZE_DECIMAL wide, not TYPE_SIZE_FLOAT64: the 9th
    // byte is the source digit count. std::vector<double> has nowhere to keep a
    // per-element count, so every entry writes the sentinel and exports
    // shortest-round-trip -- but the STRIDE must still match what
    // generate_store_fields emits for the same array, or the two writers
    // disagree by one byte per element and the reader walks off the entries.
    static Size size(const std::vector<double>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_DECIMAL); }
    static Offset store(BYTE* const base, Offset off, const std::vector<double>& d, uint32_t = FHIR_VERSION_R5) {
        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_DECIMAL, static_cast<uint32_t>(d.size()), recovery);
        for (const auto& v : d) {
            STORE_F64(base + off, v);
            STORE_U8(base + off + TYPE_SIZE_UINT64, FF_DECIMAL_SIGFIGS_UNSPECIFIED);
            off += TYPE_SIZE_DECIMAL;
        }
        return off;
    }
};

"""
