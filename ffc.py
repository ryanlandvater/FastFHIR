import os
import json

# =====================================================================
# 1. FASTFHIR TYPE MAPPING & CONFIGURATION
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
    if '/' in raw_type:
        raw_type = raw_type.split('/')[-1]
    if raw_type.startswith('System.'):
        raw_type = raw_type[7:]
    if raw_type.lower() in STRING_TYPES:
        return 'string'
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
    raise ValueError(f"Resource '{resource_name}' not found in the provided HL7 Bundle.")

# =====================================================================
# 3. C++ FIELD GENERATION HELPERS
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
            if f['fhir_type'] == 'string':
                cpp += f"{indent}        auto _blk = FF_STRING(static_cast<Offset>(__item_ptr - __base), __size, __version);\n"
            else:
                cpp += f"{indent}        auto _blk = FF_{f['fhir_type'].upper()}(static_cast<Offset>(__item_ptr - __base), __size, __version);\n"
            cpp += f"{indent}        data.{f['cpp_name']}.push_back(_blk.read(__base));\n"
            cpp += f"{indent}    }}\n"
            cpp += f"{indent}}}\n"
        elif f['is_choice']:
            cpp += f"{indent}data.{f['cpp_name']}_raw_offset = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
        elif f['fhir_type'] == 'string':
            cpp += f"{indent}Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}if ({f['cpp_name']}_off != FF_NULL_OFFSET) {{\n"
            cpp += f"{indent}    auto _blk = FF_STRING({f['cpp_name']}_off, __size, __version);\n"
            cpp += f"{indent}    data.{f['cpp_name']} = _blk.read(__base);\n"
            cpp += f"{indent}}}\n"
        elif f['cpp_type'] == 'Offset':
            cpp += f"{indent}Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}if ({f['cpp_name']}_off != FF_NULL_OFFSET) {{\n"
            cpp += f"{indent}    auto _blk = FF_{f['fhir_type'].upper()}({f['cpp_name']}_off, __size, __version);\n"
            cpp += f"{indent}    data.{f['cpp_name']} = _blk.read(__base);\n"
            cpp += f"{indent}}}\n"
        elif f['fhir_type'] == 'code':
            cpp += f"{indent}uint32_t {f['cpp_name']}_code = LOAD_U32(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}data.{f['cpp_name']} = FF_ResolveCode({f['cpp_name']}_code);\n"
        else:
            cpp += f"{indent}data.{f['cpp_name']} = {f['macro']}(__base + __offset + {block_struct_name}::{f['name']});\n"
            
        if f['first_version_idx'] > 0:
            cpp += "    }\n"
    return cpp

def generate_store_fields(layout, block_struct_name, ptr_name, data_name):
    cpp = ""
    for f in layout:
        cpp += f"    // --- Store: {f['name']} ---\n"
        if f['is_array']:
            cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
            cpp += f"        STORE_{block_struct_name}_{f['name']}_ARRAY(__base, write_head, {data_name}.{f['cpp_name']});\n"
            cpp += f"    }} else {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET);\n"
            cpp += f"    }}\n"
        elif f['is_choice']:
            cpp += f"    STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, {data_name}.{f['cpp_name']}_raw_offset);\n"
        elif f['fhir_type'] == 'string':
            cpp += f"    if ({data_name}.{f['cpp_name']}.has_value()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
            cpp += f"        STORE_FF_STRING(__base, write_head, {data_name}.{f['cpp_name']}.value());\n"
            cpp += f"    }} else {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET);\n"
            cpp += f"    }}\n"
        elif f['cpp_type'] == 'Offset':
            cpp += f"    if ({data_name}.{f['cpp_name']}.has_value()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
            cpp += f"        STORE_FF_{f['fhir_type'].upper()}(__base, write_head, {data_name}.{f['cpp_name']}.value());\n"
            cpp += f"    }} else {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET);\n"
            cpp += f"    }}\n"
        elif f['fhir_type'] == 'code':
            cpp += f"    uint32_t {f['cpp_name']}_encoded = ENCODE_FF_CODE(__base, static_cast<Offset>({ptr_name} - __base), write_head, {data_name}.{f['cpp_name']});\n"
            cpp += f"    STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, {f['cpp_name']}_encoded);\n"
        else:
            store_macro = get_store_macro(f['macro'])
            cpp += f"    {store_macro}({ptr_name} + {block_struct_name}::{f['name']}, {data_name}.{f['cpp_name']});\n"
    return cpp

# =====================================================================
# 4. VERSION MERGING ENGINE
# =====================================================================
def merge_fhir_versions(schemas_by_version, root_resource):
    master_blocks = {}
    
    for v_idx, (v_name, elements) in enumerate(schemas_by_version):
        for el in elements:
            path = el.get('path', '')
            if not path.startswith(root_resource) or len(path.split('.')) == 1: 
                continue
                
            parent_path = '.'.join(path.split('.')[:-1])
            field_name = path.split('.')[-1]
            
            if parent_path not in master_blocks:
                master_blocks[parent_path] = {'layout': [], 'seen': set(), 'sizes': {}}
                
            blk = master_blocks[parent_path]
            
            types = el.get('type', [{'code': 'BackboneElement'}])
            raw_type = types[0].get('code', 'BackboneElement')
            fhir_type = sanitize_fhir_type(raw_type)
            
            is_array = el.get('max') == '*'
            is_choice = field_name.endswith('[x]')
            if is_choice: field_name = field_name.replace('[x]', '')
            
            mapping = TYPE_MAP['DEFAULT'] if (is_array or is_choice or fhir_type not in TYPE_MAP) else TYPE_MAP[fhir_type]
            
            if field_name not in blk['seen']:
                current_off = 16 if not blk['layout'] else blk['layout'][-1]['offset'] + blk['layout'][-1]['size']
                current_off = align_up(current_off, min(mapping['size'], 8))
                
                blk['layout'].append({
                    'name': field_name.upper(),
                    'cpp_name': field_name.lower(),
                    'is_array': is_array,
                    'is_choice': is_choice,
                    'fhir_type': fhir_type,
                    'size': mapping['size'],
                    'cpp_type': mapping['cpp'],
                    'data_type': mapping['data_type'],
                    'macro': mapping['macro'],
                    'first_version_name': v_name,
                    'first_version_idx': v_idx,
                    'offset': current_off
                })
                blk['seen'].add(field_name)
                
            latest_off = 16 if not blk['layout'] else blk['layout'][-1]['offset'] + blk['layout'][-1]['size']
            blk['sizes'][v_name] = align_up(latest_off, 8)
            
    return master_blocks

# =====================================================================
# 5. FASTFHIR CORE C++ EMITTER
# =====================================================================
def generate_cxx_for_blocks(master_blocks, versions):
    hpp_out = ""
    cpp_out = ""
    
    for block_path, blk in master_blocks.items():
        block_struct_name = "FF_" + block_path.replace('.', '_').upper()
        data_struct_name = block_path.replace('.', '') + "Data"
        layout = blk['layout']
        sizes = blk['sizes']
        
        # --- HPP: POD Struct ---
        hpp_out += f"struct {data_struct_name} {{\n"
        for f in layout:
            if f['is_array']: 
                if f['fhir_type'] == 'string':
                    hpp_out += f"    std::vector<std::string> {f['cpp_name']};\n"
                else:
                    hpp_out += f"    std::vector<{f['fhir_type']}Data> {f['cpp_name']};\n"
            elif f['is_choice']: 
                hpp_out += f"    Offset {f['cpp_name']}_raw_offset = FF_NULL_OFFSET;\n"
            elif f['fhir_type'] == 'string':
                hpp_out += f"    std::optional<std::string> {f['cpp_name']};\n"
            elif f['cpp_type'] == 'Offset': 
                hpp_out += f"    std::optional<{f['fhir_type']}Data> {f['cpp_name']};\n"
            else: 
                hpp_out += f"    {f['data_type']} {f['cpp_name']};\n"
        hpp_out += "};\n\n"

        # --- HPP: DATA_BLOCK ---
        hpp_out += f"struct FF_EXPORT {block_struct_name} : DATA_BLOCK {{\n"
        hpp_out += f"    static constexpr char type [] = \"{block_struct_name}\";\n"
        hpp_out += f"    static constexpr enum RECOVERY recovery = RECOVER_{block_struct_name};\n"
        hpp_out += "    enum vtable_offsets {\n        VALIDATION = 0, RECOVERY = 8, PADDING = 10,\n"
        for f in layout: 
            hpp_out += f"        {f['name']} = {f['offset']}, // Intro: {f['first_version_name']}\n"
        
        hpp_out += "\n        // Versioned Block Sizes\n"
        for v_name, size in sizes.items():
            hpp_out += f"        HEADER_{v_name}_SIZE = {size},\n"
        latest_v = versions[-1]
        hpp_out += f"        HEADER_SIZE = HEADER_{latest_v}_SIZE\n    }};\n\n"
        
        hpp_out += "    inline Size get_header_size() const {\n"
        for v_name, size in sizes.items():
            hpp_out += f"        if (__version <= FHIR_VERSION_{v_name}) return HEADER_{v_name}_SIZE;\n"
        hpp_out += "        return HEADER_SIZE;\n    }\n\n"

        hpp_out += "    FF_Result validate_full(const BYTE* const __base) const noexcept;\n"
        hpp_out += f"    {data_struct_name} read(const BYTE* const __base) const;\n}};\n"
        hpp_out += f"void STORE_{block_struct_name}(BYTE* const __base, Offset& write_head, const {data_struct_name}& data);\n"
        hpp_out += f"void STORE_{block_struct_name}_ARRAY(BYTE* const __base, Offset& write_head, const std::vector<{data_struct_name}>& items);\n\n"

        # --- CPP: Validate & Read Methods ---
        cpp_out += f"FF_Result {block_struct_name}::validate_full(const BYTE *const __base) const noexcept {{\n"
        cpp_out += "    auto result = validate_offset(__base, type, recovery);\n    if (result != FF_SUCCESS) return result;\n    return {FF_SUCCESS, \"\"};\n}\n\n"
        
        cpp_out += f"{data_struct_name} {block_struct_name}::read(const BYTE *const __base) const {{\n"
        cpp_out += f"#ifdef __EMSCRIPTEN__\n    const_cast<{block_struct_name}&>(*this).check_and_fetch_remote(__base);\n#endif\n"
        cpp_out += f"    {data_struct_name} data;\n"
        cpp_out += generate_read_fields(layout, block_struct_name)
        cpp_out += "    return data;\n}\n\n"
        
        # --- CPP: Store Methods ---
        cpp_out += f"void STORE_{block_struct_name}(BYTE* const __base, Offset& write_head, const {data_struct_name}& data) {{\n"
        cpp_out += f"    Offset my_offset = write_head;\n    auto __ptr = __base + my_offset;\n"
        cpp_out += f"    write_head += {block_struct_name}::HEADER_SIZE;\n"
        cpp_out += f"    STORE_U64(__ptr + {block_struct_name}::VALIDATION, my_offset);\n"
        cpp_out += f"    STORE_U16(__ptr + {block_struct_name}::RECOVERY, {block_struct_name}::recovery);\n"
        cpp_out += generate_store_fields(layout, block_struct_name, "__ptr", "data")
        cpp_out += "}\n\n"
        
        # --- CPP: Array Store Methods ---
        cpp_out += f"void STORE_{block_struct_name}_ARRAY(BYTE* const __base, Offset& write_head, const std::vector<{data_struct_name}>& items) {{\n"
        cpp_out += f"    if (items.empty()) return;\n"
        cpp_out += f"    Offset array_offset = write_head;\n"
        cpp_out += f"    auto __ptr = __base + array_offset;\n"
        cpp_out += f"    write_head += 16;\n"
        cpp_out += f"    STORE_U64(__ptr + 0, array_offset);\n"
        cpp_out += f"    STORE_U16(__ptr + 8, RECOVER_{block_struct_name}_ARRAY);\n"
        cpp_out += f"    STORE_U16(__ptr + 10, {block_struct_name}::HEADER_SIZE);\n"
        cpp_out += f"    STORE_U32(__ptr + 12, static_cast<uint32_t>(items.size()));\n\n"
        cpp_out += f"    Offset vtable_start = write_head;\n"
        cpp_out += f"    write_head += items.size() * {block_struct_name}::HEADER_SIZE;\n"
        cpp_out += f"    write_head = align_up(write_head, 8);\n\n"
        cpp_out += f"    auto __item_ptr = __base + vtable_start;\n"
        cpp_out += f"    for (const auto& item : items) {{\n"
        cpp_out += f"        Offset current_item_offset = static_cast<Offset>(__item_ptr - __base);\n"
        cpp_out += f"        STORE_U64(__item_ptr + {block_struct_name}::VALIDATION, current_item_offset);\n"
        cpp_out += f"        STORE_U16(__item_ptr + {block_struct_name}::RECOVERY, {block_struct_name}::recovery);\n"
        cpp_out += generate_store_fields(layout, block_struct_name, "__item_ptr", "item")
        cpp_out += f"        __item_ptr += {block_struct_name}::HEADER_SIZE;\n"
        cpp_out += f"    }}\n"
        cpp_out += "}\n\n"

    return hpp_out, cpp_out

def emit_domain_file(master_blocks, resource_name, versions):
    hpp_out = f"// MARK: - {resource_name} Domain (Versions: {', '.join(versions)})\n#pragma once\n"
    hpp_out += f"#include \"FF_DataTypes.hpp\"\n#include <vector>\n#include <string>\n#include <optional>\n\n"
    
    cpp_out = f"// MARK: - Implementation: {resource_name} Domain\n"
    cpp_out += f"#include \"FF_{resource_name}.hpp\"\n\n"
    
    h_snip, c_snip = generate_cxx_for_blocks(master_blocks, versions)
    return hpp_out + h_snip, cpp_out + c_snip

def emit_data_types_library(bundle_schemas, target_types, versions):
    hpp_out = f"// MARK: - Universal Data Types (Versions: {', '.join(versions)})\n#pragma once\n"
    hpp_out += "#include \"FF_Primitives.hpp\"\n#include <vector>\n#include <string>\n#include <optional>\n\n"
    
    cpp_out = f"// MARK: - Universal Data Types Implementation\n"
    cpp_out += f"#include \"FF_DataTypes.hpp\"\n"
    cpp_out += "#include \"FF_Dictionary.hpp\"\n\n"
    
    for type_name in target_types:
        schemas_by_version = []
        for v_name, bundle in bundle_schemas:
            try:
                elements = extract_structure_definition(bundle, type_name)
                schemas_by_version.append((v_name, elements))
            except ValueError:
                pass # Type might not exist in an older version, that's okay
                
        if schemas_by_version:
            master_blocks = merge_fhir_versions(schemas_by_version, type_name)
            h_snip, c_snip = generate_cxx_for_blocks(master_blocks, versions)
            
            hpp_out += f"// --- {type_name.upper()} ---\n" + h_snip
            cpp_out += f"// --- {type_name.upper()} ---\n" + c_snip
            
    return hpp_out, cpp_out
