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
    'code':         {'cpp': 'uint32_t', 'data_type': 'uint32_t',    'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'}, 
    'DEFAULT':      {'cpp': 'Offset',   'data_type': 'Offset',      'size': 8, 'size_const': 'TYPE_SIZE_OFFSET',  'macro': 'LOAD_U64'}  
}

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

TARGET_TYPES = [
    "Extension", 
    "Coding", "CodeableConcept", "Quantity", "Identifier",
    "Range", "Period", "Reference", "Meta", "Narrative",
    "Annotation", "HumanName", "Address", "ContactPoint",
    "Attachment", "Ratio", "SampledData", "Duration",
    "Timing", "Dosage",
]

def _annotate_code_enums(master_blocks, code_enum_map):
    for path, blk in master_blocks.items():
        for f in blk['layout']:
            if f['fhir_type'] == 'code':
                fhir_path = path + '.' + f['orig_name']
                if fhir_path in code_enum_map:
                    f['code_enum'] = code_enum_map[fhir_path]

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
# 3. ZERO-COPY C++ GENERATION HELPERS
# =====================================================================
def generate_read_fields(layout, block_struct_name):
    cpp = ""
    for f in layout:
        indent = "    "
        if f['first_version_idx'] > 0:
            cpp += f"    if (__version >= FHIR_VERSION_{f['first_version_name']}) {{\n"
            indent = "        "
            
        cpp += f"{indent}// --- Read: {f['name']} ---\n"
        
        # Flattened Zero-Copy Mapping
        if f['is_array'] or f['fhir_type'] == 'string' or f['cpp_type'] == 'Offset':
            cpp += f"{indent}info.{f['cpp_name']} = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
        elif f.get('code_enum'):
            code_enum = f['code_enum']
            cpp += f"{indent}info.{f['cpp_name']} = {code_enum['parse']}(FF_ResolveCode(LOAD_U32(__base + __offset + {block_struct_name}::{f['name']}), __version));\n"
        elif f['fhir_type'] == 'code':
            cpp += f"{indent}info.{f['cpp_name']} = LOAD_U32(__base + __offset + {block_struct_name}::{f['name']});\n"
        else:
            cpp += f"{indent}info.{f['cpp_name']} = {f['macro']}(__base + __offset + {block_struct_name}::{f['name']});\n"
            
        if f['first_version_idx'] > 0: cpp += f"    }}\n"
    return cpp

def generate_store_fields(layout, block_struct_name, ptr_name, data_name):
    cpp = ""
    for f in layout:
        cpp += f"    // --- Store: {f['name']} ---\n"
        
        if f['is_array'] or f['fhir_type'] == 'string' or f['cpp_type'] == 'Offset':
            cpp += f"    STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, {data_name}.{f['cpp_name']});\n"
        elif f.get('code_enum'):
            code_enum = f['code_enum']
            # Safely converts the enum back to string for dictionary lookup encoding
            cpp += f"    STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, ENCODE_FF_CODE(__base, static_cast<Offset>({ptr_name} - __base), write_head, std::string({code_enum['serialize']}({data_name}.{f['cpp_name']}))));\n"
        elif f['fhir_type'] == 'code':
            cpp += f"    STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, {data_name}.{f['cpp_name']});\n"
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

    # Since we are flattening the structs to just hold Offsets, 
    # we don't need any topological sorting or forward declarations!
    for path, blk in master_blocks.items():
        s_name = "FF_" + path.replace('.', '_').upper()
        info_name = path.replace('.', '') + "Info"
        layout, sizes = blk['layout'], blk['sizes']

        # POD Struct (Flat, Zero-Copy Architecture)
        hpp += f"struct {info_name} {{\n"
        for f in layout:
            code_enum = f.get('code_enum')
            if f['is_array'] or f['fhir_type'] == 'string' or f['cpp_type'] == 'Offset':
                hpp += f"    Offset {f['cpp_name']} = FF_NULL_OFFSET;\n"
            elif code_enum:
                hpp += f"    {code_enum['enum']} {f['cpp_name']} = {code_enum['enum']}::Unknown;\n"
            elif f['fhir_type'] == 'code':
                hpp += f"    uint32_t {f['cpp_name']} = 0;\n"
            elif f['data_type'] == 'bool':
                hpp += f"    bool {f['cpp_name']} = false;\n"
            elif f['data_type'] == 'double':
                hpp += f"    double {f['cpp_name']} = 0.0;\n"
            else:
                hpp += f"    {f['data_type']} {f['cpp_name']} = 0;\n"
        hpp += f"}};\n\n"

        # Data Block
        hpp += f"struct FF_EXPORT {s_name} : DATA_BLOCK {{\n"
        hpp += f"    static constexpr char type [] = \"{s_name}\";\n"
        hpp += f"    static constexpr enum RECOVERY recovery = RECOVER_{s_name};\n"

        hpp += f"    enum vtable_sizes {{\n"
        hpp += f"        VALIDATION_S    = TYPE_SIZE_UINT64,\n"
        hpp += f"        RECOVERY_S      = TYPE_SIZE_UINT16,\n"
        hpp += f"        PADDING_S       = 6,\n"
        for f in layout:
            hpp += f"        {f['name']}_S      = {f['size_const']},\n"
        hpp += f"    }};\n"

        hpp += f"    enum vtable_offsets {{\n"
        hpp += f"        VALIDATION      = 0,\n"
        hpp += f"        RECOVERY        = VALIDATION + VALIDATION_S,\n"
        hpp += f"        PADDING         = RECOVERY + RECOVERY_S,\n"
        prev_name, prev_size = "PADDING", "PADDING_S"
        for f in layout:
            hpp += f"        {f['name']:<20}= {prev_name} + {prev_size},\n"
            prev_name, prev_size = f['name'], f"{f['name']}_S"

        for v, sz in sizes.items():
            hpp += f"        HEADER_{v}_SIZE  = {sz},\n"
        hpp += f"        HEADER_SIZE     = HEADER_{versions[-1]}_SIZE\n"
        hpp += f"    }};\n"

        min_version = next((v for v in versions if v in sizes), None)
        hpp += f"    inline Size get_header_size() const {{\n"
        if min_version: hpp += f"        if (__version < FHIR_VERSION_{min_version}) return 0;\n"
        for v in versions:
            if v in sizes: hpp += f"        if (__version <= FHIR_VERSION_{v}) return HEADER_{v}_SIZE;\n"
        hpp += f"        return HEADER_SIZE;\n    }}\n"

        hpp += f"    explicit {s_name}(Offset off, Size total_size, uint32_t ver) : DATA_BLOCK(off, total_size, ver) {{}}\n"
        hpp += f"    FF_Result validate_full(const BYTE* const __base) const noexcept;\n"
        hpp += f"    {info_name} read(const BYTE* const __base) const;\n"
        hpp += f"}};\n"
        
        hpp += f"void STORE_{s_name}(BYTE* const __base, Offset entry_off, Offset& write_head, const {info_name}& info);\n"
        hpp += f"inline void STORE_{s_name}(BYTE* const __base, Offset& write_head, const {info_name}& info) {{\n"
        hpp += f"    Offset hdr = write_head; write_head += {s_name}::HEADER_SIZE;\n"
        hpp += f"    STORE_{s_name}(__base, hdr, write_head, info);\n"
        hpp += f"}}\n\n"

        cpp += f"FF_Result {s_name}::validate_full(const BYTE *const __base) const noexcept {{ return validate_offset(__base, type, recovery); }}\n"
        cpp += f"{info_name} {s_name}::read(const BYTE *const __base) const {{\n"
        cpp += f"#ifdef __EMSCRIPTEN__\n    const_cast<{s_name}&>(*this).check_and_fetch_remote(__base);\n#endif\n"
        cpp += f"    {info_name} info;\n{generate_read_fields(layout, s_name)}    return info;\n}}\n"
        
        cpp += f"void STORE_{s_name}(BYTE* const __base, Offset entry_off, Offset& write_head, const {info_name}& info) {{\n"
        cpp += f"    auto __ptr = __base + entry_off;\n"
        cpp += f"    STORE_U64(__ptr + {s_name}::VALIDATION, entry_off);\n"
        cpp += f"    STORE_U16(__ptr + {s_name}::RECOVERY, {s_name}::recovery);\n"
        cpp += f"{generate_store_fields(layout, s_name, '__ptr', 'info')}}}\n\n"
    return hpp, cpp

# =====================================================================
# 5. MASTER BUILD ORCHESTRATOR
# =====================================================================
def compile_fhir_library(resources, versions, input_dir="fhir_specs", output_dir="generated_src", code_enum_map=None):
    code_enums = code_enum_map or {}
    os.makedirs(output_dir, exist_ok=True)
    
    # --- FF_DataTypes.hpp/cpp ---
    print("Generating FF_DataTypes...")
    type_bundles = []
    for v in versions:
        p = os.path.join(input_dir, v, "profiles-types.json")
        if os.path.exists(p): 
            with open(p, 'r') as f: type_bundles.append((v, json.load(f)))
    
    auto_header = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
    )

    # Removed forward declarations entirely. All headers are independent!
    hpp_head = f"{auto_header}// MARK: - Universal Data Types\n#pragma once\n#include \"../include/FF_Primitives.hpp\"\n#include \"FF_CodeSystems.hpp\"\n\n"
    cpp_head = f"{auto_header}\n#include \"../include/FF_Utilities.hpp\"\n#include \"FF_DataTypes.hpp\"\n#include \"FF_Dictionary.hpp\"\n\n"
    
    all_blocks = {}
    for t in TARGET_TYPES:
        sch = []
        for v, bun in type_bundles:
            try: sch.append((v, extract_structure_definition(bun, t)))
            except: pass
        if sch:
            blocks = merge_fhir_versions(sch, t)
            _annotate_code_enums(blocks, code_enums)
            all_blocks.update(blocks)
    
    if all_blocks:
        h, c = generate_cxx_for_blocks(all_blocks, versions)
        hpp_head += h
        cpp_head += c
    
    with open(os.path.join(output_dir, "FF_DataTypes.hpp"), "w") as f: f.write(hpp_head)
    with open(os.path.join(output_dir, "FF_DataTypes.cpp"), "w") as f: f.write(cpp_head)

    # --- FF_{Resource}.hpp/cpp ---
    generated_resources = []
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
            generated_resources.append(res)
            blocks = merge_fhir_versions(sch, res)
            _annotate_code_enums(blocks, code_enums)
            hpp, cpp = generate_cxx_for_blocks(blocks, versions)
            
            with open(os.path.join(output_dir, f"FF_{res}.hpp"), "w") as f: 
                f.write(f"{auto_header}#pragma once\n#include \"FF_DataTypes.hpp\"\n\n{hpp}")
            with open(os.path.join(output_dir, f"FF_{res}.cpp"), "w") as f: 
                f.write(f"{auto_header}\n#include \"../include/FF_Utilities.hpp\"\n#include \"FF_Dictionary.hpp\"\n#include \"FF_AllTypes.hpp\"\n\n{cpp}")
    
    # --- FF_AllTypes.hpp: aggregate header with all types ---
    all_types_hpp = f"{auto_header}#pragma once\n#include \"FF_DataTypes.hpp\"\n"
    for res in generated_resources:
        all_types_hpp += f"#include \"FF_{res}.hpp\"\n"
    all_types_hpp += "\n"
    with open(os.path.join(output_dir, "FF_AllTypes.hpp"), "w") as f: f.write(all_types_hpp)

if __name__ == "__main__":
    from ffcs import generate_code_systems
    resources = ["Observation", "Patient", "Encounter", "DiagnosticReport"]
    versions = ["R4", "R5"]
    enum_map = generate_code_systems(TARGET_TYPES, resources, versions)
    compile_fhir_library(resources, versions, code_enum_map=enum_map)
    print("\n[Success] Build Complete: generated_src/ contains all FastFHIR artifacts.")