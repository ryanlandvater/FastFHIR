import os
import json

# =====================================================================
# 1. FASTFHIR TYPE MAPPING & NORMALIZATION
# =====================================================================
TYPE_MAP = {
    'boolean':      {'cpp': 'uint8_t',  'data_type': 'bool',        'size': 1, 'size_const': 'TYPE_SIZE_UINT8',   'macro': 'LOAD_U8'},
    'integer':      {'cpp': 'uint32_t', 'data_type': 'uint32_t',    'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'},
    'unsignedInt':  {'cpp': 'uint32_t', 'data_type': 'uint32_t',    'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'},
    'positiveInt':  {'cpp': 'uint32_t', 'data_type': 'uint32_t',    'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'},
    'decimal':      {'cpp': 'double',   'data_type': 'double',      'size': 8, 'size_const': 'TYPE_SIZE_FLOAT64', 'macro': 'LOAD_F64'},
    'code':         {'cpp': 'uint32_t', 'data_type': 'std::string', 'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'}, 
    'DEFAULT':      {'cpp': 'Offset',   'data_type': 'Offset',      'size': 8, 'size_const': 'TYPE_SIZE_OFFSET',  'macro': 'LOAD_U64'}  
}

# All FHIR primitives that should serialize as a standard FF_STRING
STRING_TYPES = {
    'string', 'id', 'oid', 'uuid', 'uri', 'url', 'canonical', 
    'markdown', 'date', 'datetime', 'instant', 'time', 'base64binary', 'xhtml'
}

def sanitize_fhir_type(raw_type):
    if '/' in raw_type: raw_type = raw_type.split('/')[-1]
    if raw_type.startswith('System.'): raw_type = raw_type[7:]
    if raw_type.lower() in STRING_TYPES: return 'string'
    return raw_type

def align_up(offset, alignment):
    return (offset + (alignment - 1)) & ~(alignment - 1)

def get_store_macro(load_macro):
    return load_macro.replace('LOAD_', 'STORE_')

# =====================================================================
# 2. HL7 BUNDLE EXTRACTOR
# =====================================================================
def extract_structure_definition(bundle_json, resource_name):
    entries = bundle_json.get('entry', [])
    for entry in entries:
        resource = entry.get('resource', {})
        if resource.get('resourceType') == 'StructureDefinition' and resource.get('name') == resource_name:
            return resource.get('snapshot', {}).get('element', [])
    raise ValueError(f"Resource '{resource_name}' not found.")

# =====================================================================
# 3. C++ GENERATION HELPERS
# =====================================================================
def _resolve_ff_struct_name(fhir_type, field_name, block_struct_name):
    """Resolve the FF_ struct name for read/store dispatch."""
    if fhir_type == 'string':
        return 'FF_STRING'
    if fhir_type in ('BackboneElement', 'Element'):
        return f"{block_struct_name}_{field_name}"
    return f"FF_{fhir_type.upper()}"

def _resolve_data_type_name(fhir_type, field_orig_name, parent_path):
    """Resolve the C++ Data struct name for a FHIR type."""
    fhir_type_l = fhir_type.lower()
    if fhir_type_l == 'string':
        return 'std::string'
    if fhir_type_l == 'code':
        return 'std::string'
    if fhir_type in ('BackboneElement', 'Element'):
        # Must match: d_name = path.replace('.', '') + "Data"
        # where path = parent_path + '.' + orig_field_name
        return parent_path.replace('.', '') + field_orig_name + 'Data'
    if fhir_type == 'Resource':
        return 'ResourceData'
    if fhir_type in TYPE_MAP and fhir_type != 'DEFAULT':
        return TYPE_MAP[fhir_type]['data_type']
    return fhir_type + 'Data'

def generate_read_fields(layout, block_struct_name):
    cpp = ""
    for f in layout:
        indent = "    "
        if f['first_version_idx'] > 0:
            cpp += f"    if (__version >= FHIR_VERSION_{f['first_version_name']}) {{\n"
            indent = "        "
        cpp += f"{indent}// --- Read: {f['name']} ---\n"
        if f['is_array']:
            cpp += f"{indent}Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}if ({f['cpp_name']}_off != FF_NULL_OFFSET) {{\n"
            cpp += f"{indent}    FF_ARRAY __arr({f['cpp_name']}_off, __size, __version);\n"
            cpp += f"{indent}    auto STEP = __arr.entry_step(__base);\n"
            cpp += f"{indent}    auto ENTRIES = __arr.entry_count(__base);\n"
            cpp += f"{indent}    auto __item_ptr = __arr.entries(__base);\n"
            cpp += f"{indent}    for (uint32_t i = 0; i < ENTRIES; ++i, __item_ptr += STEP) {{\n"
            if f['fhir_type'] in ('string', 'code'):
                # String arrays: entries are 8-byte offsets to FF_STRING blocks
                cpp += f"{indent}        Offset __str_off = LOAD_U64(__item_ptr);\n"
                cpp += f"{indent}        if (__str_off != FF_NULL_OFFSET) {{\n"
                cpp += f"{indent}            FF_STRING _blk(__str_off, __size, __version);\n"
                cpp += f"{indent}            data.{f['cpp_name']}.push_back(_blk.read(__base));\n"
                cpp += f"{indent}        }}\n"
            else:
                # Complex type arrays: entries are inline DATA_BLOCK headers (Iris pattern)
                type_call = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name)
                cpp += f"{indent}        {type_call} _blk(static_cast<Offset>(__item_ptr - __base), __size, __version);\n"
                cpp += f"{indent}        data.{f['cpp_name']}.push_back(_blk.read(__base));\n"
            cpp += f"{indent}    }}\n{indent}}}\n"
        elif f['fhir_type'] == 'string':
            cpp += f"{indent}Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}if ({f['cpp_name']}_off != FF_NULL_OFFSET) {{ FF_STRING _blk({f['cpp_name']}_off, __size, __version); data.{f['cpp_name']} = _blk.read(__base); }}\n"
        elif f['cpp_type'] == 'Offset':
            type_call = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name)
            cpp += f"{indent}Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}if ({f['cpp_name']}_off != FF_NULL_OFFSET) {{ {type_call} _blk({f['cpp_name']}_off, __size, __version); data.{f['cpp_name']} = _blk.read(__base); }}\n"
        elif f['fhir_type'] == 'code':
            if block_struct_name == 'FF_NARRATIVE' and f['name'] == 'STATUS':
                cpp += f"{indent}data.{f['cpp_name']} = FF_ParseNarrativeStatus(FF_ResolveCode(LOAD_U32(__base + __offset + {block_struct_name}::{f['name']}), __version));\n"
            else:
                cpp += f"{indent}data.{f['cpp_name']} = FF_ResolveCode(LOAD_U32(__base + __offset + {block_struct_name}::{f['name']}), __version);\n"
        else:
            cpp += f"{indent}data.{f['cpp_name']} = {f['macro']}(__base + __offset + {block_struct_name}::{f['name']});\n"
        if f['first_version_idx'] > 0: cpp += f"    }}\n"
    return cpp

def generate_store_fields(layout, block_struct_name, ptr_name, data_name):
    cpp = ""
    for f in layout:
        cpp += f"    // --- Store: {f['name']} ---\n"
        if f['is_array']:
            if f['fhir_type'] in ('string', 'code'):
                # String arrays: offset table pointing to FF_STRING blocks
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, write_head, TYPE_SIZE_OFFSET, __n);\n"
                cpp += f"        Offset __off_tbl = write_head;\n"
                cpp += f"        write_head += static_cast<Offset>(__n) * TYPE_SIZE_OFFSET;\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                cpp += f"            STORE_U64(__base + __off_tbl + __i * TYPE_SIZE_OFFSET, write_head);\n"
                cpp += f"            STORE_FF_STRING(__base, write_head, {data_name}.{f['cpp_name']}[__i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
            else:
                # Complex type arrays: Iris-style inline DATA_BLOCK headers
                child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name)
                store_fn = child_struct.replace('FF_', 'STORE_FF_', 1)
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, write_head, {child_struct}::HEADER_SIZE, __n);\n"
                cpp += f"        Offset __entries = write_head;\n"
                cpp += f"        write_head += static_cast<Offset>(__n) * {child_struct}::HEADER_SIZE;\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                cpp += f"            {store_fn}(__base, __entries + __i * {child_struct}::HEADER_SIZE, write_head, {data_name}.{f['cpp_name']}[__i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
        elif f['fhir_type'] == 'string':
            cpp += f"    if ({data_name}.{f['cpp_name']}.has_value()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
            cpp += f"        STORE_FF_STRING(__base, write_head, {data_name}.{f['cpp_name']}.value());\n"
            cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
        elif f['cpp_type'] == 'Offset':
            store_fn = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name).replace('FF_', 'STORE_FF_', 1)
            cpp += f"    if ({data_name}.{f['cpp_name']}.has_value()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
            cpp += f"        {store_fn}(__base, write_head, {data_name}.{f['cpp_name']}.value());\n"
            cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
        elif f['fhir_type'] == 'code':
            if block_struct_name == 'FF_NARRATIVE' and f['name'] == 'STATUS':
                cpp += f"    STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, ENCODE_FF_CODE(__base, static_cast<Offset>({ptr_name} - __base), write_head, std::string(FF_NarrativeStatusToString({data_name}.{f['cpp_name']}))));\n"
            else:
                cpp += f"    STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, ENCODE_FF_CODE(__base, static_cast<Offset>({ptr_name} - __base), write_head, {data_name}.{f['cpp_name']}));\n"
        else:
            cpp += f"    {get_store_macro(f['macro'])}({ptr_name} + {block_struct_name}::{f['name']}, {data_name}.{f['cpp_name']});\n"
    return cpp

# =====================================================================
# 4. ORCHESTRATION & VERSION MERGING
# =====================================================================
def merge_fhir_versions(schemas_by_version, root_resource):
    master_blocks = {}
    for v_idx, (v_name, elements) in enumerate(schemas_by_version):
        for el in elements:
            path = el.get('path', '')
            if not path.startswith(root_resource) or len(path.split('.')) == 1: continue
            parent_path = '.'.join(path.split('.')[:-1])
            field_name = path.split('.')[-1].replace('[x]', '')
            if parent_path not in master_blocks: master_blocks[parent_path] = {'layout': [], 'seen': set(), 'sizes': {}}
            blk = master_blocks[parent_path]
            f_type = sanitize_fhir_type(el.get('type', [{'code': 'BackboneElement'}])[0].get('code', 'BackboneElement'))
            is_array = el.get('max') == '*'
            mapping = TYPE_MAP['DEFAULT'] if (is_array or f_type not in TYPE_MAP) else TYPE_MAP[f_type]
            if field_name not in blk['seen']:
                off = align_up(16 if not blk['layout'] else blk['layout'][-1]['offset'] + blk['layout'][-1]['size'], min(mapping['size'], 8))
                blk['layout'].append({
                    'name': field_name.upper(),
                    'cpp_name': field_name.lower(),
                    'orig_name': field_name,
                    'is_array': is_array,
                    'fhir_type': f_type,
                    'size': mapping['size'],
                    'size_const': mapping['size_const'],
                    'cpp_type': mapping['cpp'],
                    'data_type': mapping['data_type'],
                    'macro': mapping['macro'],
                    'first_version_name': v_name,
                    'first_version_idx': v_idx,
                    'offset': off
                })
                blk['seen'].add(field_name)
            blk['sizes'][v_name] = align_up(blk['layout'][-1]['offset'] + blk['layout'][-1]['size'], 8)
    return master_blocks

def generate_cxx_for_blocks(master_blocks, versions):
    hpp, cpp = "", ""
    for path, blk in master_blocks.items():
        s_name, d_name = "FF_" + path.replace('.', '_').upper(), path.replace('.', '') + "Data"
        layout, sizes = blk['layout'], blk['sizes']
        # POD Struct
        hpp += f"struct {d_name} {{\n"
        for f in layout:
            if f['is_array']:
                item_type = _resolve_data_type_name(f['fhir_type'], f['orig_name'], path)
                hpp += f"    std::vector<{item_type}> {f['cpp_name']};\n"
            elif f['fhir_type'] == 'string': hpp += f"    std::optional<std::string> {f['cpp_name']};\n"
            elif path == 'Narrative' and f['fhir_type'] == 'code' and f['name'] == 'STATUS': hpp += f"    NarrativeStatus {f['cpp_name']};\n"
            elif f['cpp_type'] == 'Offset': hpp += f"    std::optional<{_resolve_data_type_name(f['fhir_type'], f['orig_name'], path)}> {f['cpp_name']};\n"
            else: hpp += f"    {f['data_type']} {f['cpp_name']};\n"
        hpp += f"}};\n\n"

        # Data Block with additive vtable offsets
        hpp += f"struct FF_EXPORT {s_name} : DATA_BLOCK {{\n"
        hpp += f"    static constexpr char type [] = \"{s_name}\";\n"
        hpp += f"    static constexpr enum RECOVERY recovery = RECOVER_{s_name};\n"

        # vtable_sizes enum
        hpp += f"    enum vtable_sizes {{\n"
        hpp += f"        VALIDATION_S    = TYPE_SIZE_UINT64,\n"
        hpp += f"        RECOVERY_S      = TYPE_SIZE_UINT16,\n"
        hpp += f"        PADDING_S       = 6,  // align to 16\n"
        for f in layout:
            hpp += f"        {f['name']}_S      = {f['size_const']},  // since {f['first_version_name']}\n"
        hpp += f"    }};\n"

        # vtable_offsets enum (Iris-style additive)
        hpp += f"    enum vtable_offsets {{\n"
        hpp += f"        VALIDATION      = 0,\n"
        hpp += f"        RECOVERY        = VALIDATION + VALIDATION_S,\n"
        hpp += f"        PADDING         = RECOVERY + RECOVERY_S,\n"

        prev_name = "PADDING"
        prev_size = "PADDING_S"
        for f in layout:
            hpp += f"        {f['name']:<20}= {prev_name} + {prev_size},  // since {f['first_version_name']}\n"
            prev_name = f['name']
            prev_size = f"{f['name']}_S"

        for v, sz in sizes.items():
            hpp += f"        HEADER_{v}_SIZE  = {sz},\n"
        hpp += f"        HEADER_SIZE     = HEADER_{versions[-1]}_SIZE\n"
        hpp += f"    }};\n"

        # get_header_size
        hpp += f"    inline Size get_header_size() const {{\n"
        for v in versions: hpp += f"        if (__version <= FHIR_VERSION_{v}) return HEADER_{v}_SIZE;\n"
        hpp += f"        return HEADER_SIZE;\n    }}\n"

        hpp += f"    FF_Result validate_full(const BYTE* const __base) const noexcept;\n"
        hpp += f"    {d_name} read(const BYTE* const __base) const;\n"
        hpp += f"}};\n"
        # Two STORE overloads: 3-arg (array context) + 2-arg (standalone convenience)
        hpp += f"void STORE_{s_name}(BYTE* const __base, Offset entry_off, Offset& write_head, const {d_name}& data);\n"
        hpp += f"inline void STORE_{s_name}(BYTE* const __base, Offset& write_head, const {d_name}& data) {{\n"
        hpp += f"    Offset hdr = write_head; write_head += {s_name}::HEADER_SIZE;\n"
        hpp += f"    STORE_{s_name}(__base, hdr, write_head, data);\n"
        hpp += f"}}\n\n"

        # Methods
        cpp += f"FF_Result {s_name}::validate_full(const BYTE *const __base) const noexcept {{ return validate_offset(__base, type, recovery); }}\n"
        cpp += f"{d_name} {s_name}::read(const BYTE *const __base) const {{\n"
        cpp += f"#ifdef __EMSCRIPTEN__\n    const_cast<{s_name}&>(*this).check_and_fetch_remote(__base);\n#endif\n"
        cpp += f"    {d_name} data;\n{generate_read_fields(layout, s_name)}    return data;\n}}\n"
        # 3-arg STORE: header at entry_off, overflow at write_head
        cpp += f"void STORE_{s_name}(BYTE* const __base, Offset entry_off, Offset& write_head, const {d_name}& data) {{\n"
        cpp += f"    auto __ptr = __base + entry_off;\n"
        cpp += f"    STORE_U64(__ptr + {s_name}::VALIDATION, entry_off);\n"
        cpp += f"    STORE_U16(__ptr + {s_name}::RECOVERY, {s_name}::recovery);\n"
        cpp += f"{generate_store_fields(layout, s_name, '__ptr', 'data')}}}\n\n"
    return hpp, cpp

# =====================================================================
# 5. MASTER BUILD ORCHESTRATOR
# =====================================================================
def compile_fhir_library(resources, versions, input_dir="fhir_specs", output_dir="generated_src"):
    os.makedirs(output_dir, exist_ok=True)
    
    # --- FF_DataTypes.hpp/cpp ---
    print("Generating FF_DataTypes...")
    type_bundles = []
    for v in versions:
        p = os.path.join(input_dir, v, "profiles-types.json")
        if os.path.exists(p): 
            with open(p, 'r') as f: type_bundles.append((v, json.load(f)))
    
    target_types = [
        "Extension",       # Must be first — referenced by all other types
        "Coding", "CodeableConcept", "Quantity", "Identifier",
        "Range", "Period", "Reference", "Meta", "Narrative",
        "Annotation", "HumanName", "Address", "ContactPoint",
        "Attachment", "Ratio", "SampledData", "Duration",
        "Timing", "Dosage",
    ]

    # Collect all forward-declared data struct names for circular references
    fwd_decls = set()
    for t in target_types:
        fwd_decls.add(t + "Data")

    auto_header = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
    )

    hpp_head = f"{auto_header}// MARK: - Universal Data Types\n#pragma once\n#include \"../include/FF_Primitives.hpp\"\n#include <vector>\n#include <string>\n#include <optional>\n\n"
    hpp_head += "enum class NarrativeStatus : uint8_t { Generated, Extensions, Additional, Empty, Unknown };\n"
    hpp_head += "inline NarrativeStatus FF_ParseNarrativeStatus(const std::string& s) {\n"
    hpp_head += "    if (s == \"generated\") return NarrativeStatus::Generated;\n"
    hpp_head += "    if (s == \"extensions\") return NarrativeStatus::Extensions;\n"
    hpp_head += "    if (s == \"additional\") return NarrativeStatus::Additional;\n"
    hpp_head += "    if (s == \"empty\") return NarrativeStatus::Empty;\n"
    hpp_head += "    return NarrativeStatus::Unknown;\n"
    hpp_head += "}\n"
    hpp_head += "inline const char* FF_NarrativeStatusToString(NarrativeStatus s) {\n"
    hpp_head += "    switch (s) {\n"
    hpp_head += "        case NarrativeStatus::Generated: return \"generated\";\n"
    hpp_head += "        case NarrativeStatus::Extensions: return \"extensions\";\n"
    hpp_head += "        case NarrativeStatus::Additional: return \"additional\";\n"
    hpp_head += "        case NarrativeStatus::Empty: return \"empty\";\n"
    hpp_head += "        default: return \"\";\n"
    hpp_head += "    }\n"
    hpp_head += "}\n\n"

    # Forward declarations (needed for self-referential types like Extension.extension)
    for decl in sorted(fwd_decls):
        hpp_head += f"struct {decl};\n"
    # Stub for Resource (contained resources) - full definition elsewhere
    hpp_head += "struct ResourceData { std::string resourceType; std::string json; };\n"
    hpp_head += "\n"

    cpp_head = f"{auto_header}\n#include \"../include/FF_utilities.hpp\"\n#include \"FF_DataTypes.hpp\"\n#include \"FF_Dictionary.hpp\"\n\n"
    
    for t in target_types:
        sch = []
        for v, bun in type_bundles:
            try: sch.append((v, extract_structure_definition(bun, t)))
            except: pass
        if sch:
            h, c = generate_cxx_for_blocks(merge_fhir_versions(sch, t), versions)
            hpp_head += f"// --- {t} ---\n{h}"
            cpp_head += f"// --- {t} ---\n{c}"
    
    with open(os.path.join(output_dir, "FF_DataTypes.hpp"), "w") as f: f.write(hpp_head)
    with open(os.path.join(output_dir, "FF_DataTypes.cpp"), "w") as f: f.write(cpp_head)

    # --- FF_{Resource}.hpp/cpp ---
    for res in resources:
        print(f"Generating FF_{res}...")
        sch = []
        for v in versions:
            p = os.path.join(input_dir, v, "profiles-resources.json")
            if os.path.exists(p):
                with open(p, 'r') as f:
                    try: sch.append((v, extract_structure_definition(json.load(f), res)))
                    except: pass
        if sch:
            hpp, cpp = generate_cxx_for_blocks(merge_fhir_versions(sch, res), versions)
            with open(os.path.join(output_dir, f"FF_{res}.hpp"), "w") as f: 
                f.write(f"{auto_header}#pragma once\n#include \"FF_DataTypes.hpp\"\n\n{hpp}")
            with open(os.path.join(output_dir, f"FF_{res}.cpp"), "w") as f: 
                f.write(f"{auto_header}\n#include \"../include/FF_utilities.hpp\"\n#include \"FF_{res}.hpp\"\n\n{cpp}")

if __name__ == "__main__":
    compile_fhir_library(["Observation", "Patient", "Encounter", "DiagnosticReport"], ["R4", "R5"])
    print("\n✅ Build Complete: generated_src/ contains all FastFHIR artifacts.")