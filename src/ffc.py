import os
import json

# =====================================================================
# 1. FASTFHIR TYPE MAPPING & NORMALIZATION
# =====================================================================
TYPE_MAP = {
    'boolean':      {'cpp': 'uint8_t',  'data_type': 'bool',        'size': 1, 'macro': 'LOAD_U8'},
    'integer':      {'cpp': 'uint32_t', 'data_type': 'uint32_t',    'size': 4, 'macro': 'LOAD_U32'},
    'unsignedInt':  {'cpp': 'uint32_t', 'data_type': 'uint32_t',    'size': 4, 'macro': 'LOAD_U32'},
    'positiveInt':  {'cpp': 'uint32_t', 'data_type': 'uint32_t',    'size': 4, 'macro': 'LOAD_U32'},
    'decimal':      {'cpp': 'double',   'data_type': 'double',      'size': 8, 'macro': 'LOAD_F64'},
    'code':         {'cpp': 'uint32_t', 'data_type': 'std::string', 'size': 4, 'macro': 'LOAD_U32'}, 
    'DEFAULT':      {'cpp': 'Offset',   'data_type': 'Offset',      'size': 8, 'macro': 'LOAD_U64'}  
}

# All FHIR primitives that should serialize as a standard FF_STRING
STRING_TYPES = {
    'string', 'id', 'oid', 'uuid', 'uri', 'url', 'canonical', 
    'markdown', 'date', 'datetime', 'instant', 'time', 'base64binary'
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
            cpp += f"{indent}    auto __arr_ptr = __base + {f['cpp_name']}_off;\n"
            cpp += f"{indent}    auto STEP = LOAD_U16(__arr_ptr + 10);\n"
            cpp += f"{indent}    auto ENTRIES = LOAD_U32(__arr_ptr + 12);\n"
            cpp += f"{indent}    auto __item_ptr = __arr_ptr + 16;\n"
            cpp += f"{indent}    for (uint32_t i = 0; i < ENTRIES; ++i, __item_ptr += STEP) {{\n"
            type_call = "FF_STRING" if f['fhir_type'] == 'string' else f"FF_{f['fhir_type'].upper()}"
            cpp += f"{indent}        auto _blk = {type_call}(static_cast<Offset>(__item_ptr - __base), __size, __version);\n"
            cpp += f"{indent}        data.{f['cpp_name']}.push_back(_blk.read(__base));\n"
            cpp += f"{indent}    }}\n{indent}}}\n"
        elif f['fhir_type'] == 'string':
            cpp += f"{indent}Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}if ({f['cpp_name']}_off != FF_NULL_OFFSET) {{ FF_STRING _blk({f['cpp_name']}_off, __size, __version); data.{f['cpp_name']} = _blk.read(__base); }}\n"
        elif f['cpp_type'] == 'Offset':
            cpp += f"{indent}Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}if ({f['cpp_name']}_off != FF_NULL_OFFSET) {{ FF_{f['fhir_type'].upper()} _blk({f['cpp_name']}_off, __size, __version); data.{f['cpp_name']} = _blk.read(__base); }}\n"
        elif f['fhir_type'] == 'code':
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
            cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
            cpp += f"        STORE_{block_struct_name}_{f['name']}_ARRAY(__base, write_head, {data_name}.{f['cpp_name']});\n"
            cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
        elif f['fhir_type'] == 'string':
            cpp += f"    if ({data_name}.{f['cpp_name']}.has_value()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
            cpp += f"        STORE_FF_STRING(__base, write_head, {data_name}.{f['cpp_name']}.value());\n"
            cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
        elif f['cpp_type'] == 'Offset':
            cpp += f"    if ({data_name}.{f['cpp_name']}.has_value()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
            cpp += f"        STORE_FF_{f['fhir_type'].upper()}(__base, write_head, {data_name}.{f['cpp_name']}.value());\n"
            cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
        elif f['fhir_type'] == 'code':
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
                blk['layout'].append({'name': field_name.upper(), 'cpp_name': field_name.lower(), 'is_array': is_array, 'fhir_type': f_type, 'size': mapping['size'], 'cpp_type': mapping['cpp'], 'data_type': mapping['data_type'], 'macro': mapping['macro'], 'first_version_name': v_name, 'first_version_idx': v_idx, 'offset': off})
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
            if f['is_array']: hpp += f"    std::vector<{'std::string' if f['fhir_type'] == 'string' else f[f'fhir_type'] + 'Data'}> {f['cpp_name']};\n"
            elif f['fhir_type'] == 'string': hpp += f"    std::optional<std::string> {f['cpp_name']};\n"
            elif f['cpp_type'] == 'Offset': hpp += f"    std::optional<{f['fhir_type']}Data> {f['cpp_name']};\n"
            else: hpp += f"    {f['data_type']} {f['cpp_name']};\n"
        hpp += f"}};\n\n"
        # Data Block
        hpp += f"struct FF_EXPORT {s_name} : DATA_BLOCK {{\n    static constexpr char type [] = \"{s_name}\";\n    static constexpr enum RECOVERY recovery = RECOVER_{s_name};\n    enum vtable_offsets {{\n        VALIDATION = 0, RECOVERY = 8, PADDING = 10,\n"
        for f in layout: hpp += f"        {f['name']} = {f['offset']},\n"
        for v, sz in sizes.items(): hpp += f"        HEADER_{v}_SIZE = {sz},\n"
        hpp += f"        HEADER_SIZE = HEADER_{versions[-1]}_SIZE\n    }};\n    inline Size get_header_size() const {{\n"
        for v in versions: hpp += f"        if (__version <= FHIR_VERSION_{v}) return HEADER_{v}_SIZE;\n"
        hpp += f"        return HEADER_SIZE;\n    }}\n    FF_Result validate_full(const BYTE* const __base) const noexcept;\n    {d_name} read(const BYTE* const __base) const;\n}};\nvoid STORE_{s_name}(BYTE* const __base, Offset& write_head, const {d_name}& data);\n\n"
        # Methods
        cpp += f"FF_Result {s_name}::validate_full(const BYTE *const __base) const noexcept {{ return validate_offset(__base, type, recovery); }}\n"
        cpp += f"{d_name} {s_name}::read(const BYTE *const __base) const {{\n#ifdef __EMSCRIPTEN__\n    const_cast<{s_name}&>(*this).check_and_fetch_remote(__base);\n#endif\n    {d_name} data;\n{generate_read_fields(layout, s_name)}    return data;\n}}\n"
        cpp += f"void STORE_{s_name}(BYTE* const __base, Offset& write_head, const {d_name}& data) {{\n    Offset my_off = write_head; auto __ptr = __base + my_off; write_head += {s_name}::HEADER_SIZE; STORE_U64(__ptr + {s_name}::VALIDATION, my_off); STORE_U16(__ptr + {s_name}::RECOVERY, {s_name}::recovery);\n{generate_store_fields(layout, s_name, '__ptr', 'data')}}}\n\n"
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
    
    target_types = ["Coding", "CodeableConcept", "Quantity", "Identifier", "Range", "Period"]
    hpp_head = f"// MARK: - Universal Data Types\n#pragma once\n#include \"FF_Primitives.hpp\"\n#include <vector>\n#include <string>\n#include <optional>\n\n"
    cpp_head = f"#include \"FF_DataTypes.hpp\"\n#include \"FF_Dictionary.hpp\"\n\n"
    
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
                f.write(f"#pragma once\n#include \"FF_DataTypes.hpp\"\n\n{hpp}")
            with open(os.path.join(output_dir, f"FF_{res}.cpp"), "w") as f: 
                f.write(f"#include \"FF_{res}.hpp\"\n\n{cpp}")

if __name__ == "__main__":
    compile_fhir_library(["Observation", "Patient", "Encounter", "DiagnosticReport"], ["R4", "R5"])
    print("\n✅ Build Complete: generated_src/ contains all FastFHIR artifacts.")