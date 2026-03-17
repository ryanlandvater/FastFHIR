import json

# =====================================================================
# 1. FASTFHIR TYPE MAPPING & CONFIGURATION
# =====================================================================
TYPE_MAP = {
    'boolean':      {'cpp': 'uint8_t',  'data_type': 'bool',        'size': 1, 'macro': 'LOAD_U8'},
    'integer':      {'cpp': 'uint32_t', 'data_type': 'uint32_t',    'size': 4, 'macro': 'LOAD_U32'},
    'decimal':      {'cpp': 'double',   'data_type': 'double',      'size': 8, 'macro': 'LOAD_F64'},
    'code':         {'cpp': 'uint32_t', 'data_type': 'std::string', 'size': 4, 'macro': 'LOAD_U32'}, 
    'DEFAULT':      {'cpp': 'Offset',   'data_type': 'Offset',      'size': 8, 'macro': 'LOAD_U64'}  
}

def align_up(offset, alignment):
    return (offset + (alignment - 1)) & ~(alignment - 1)

def get_store_macro(load_macro):
    return load_macro.replace('LOAD_', 'STORE_')

# =====================================================================
# 2. FHIR TREE FLATTENER
# =====================================================================
def group_fhir_elements(elements, root_resource):
    blocks = {}
    for el in elements:
        path = el.get('path', '')
        if not path.startswith(root_resource) or len(path.split('.')) == 1: 
            continue
            
        parent_path = '.'.join(path.split('.')[:-1])
        field_name = path.split('.')[-1]
        
        if parent_path not in blocks:
            blocks[parent_path] = []
            
        types = el.get('type', [{'code': 'BackboneElement'}])
        fhir_type = types[0].get('code', 'BackboneElement')
        is_array = el.get('max') == '*'
        is_choice = field_name.endswith('[x]')
        
        if is_choice:
            field_name = field_name.replace('[x]', '')
            
        mapping = TYPE_MAP['DEFAULT'] if (is_array or is_choice or fhir_type not in TYPE_MAP) else TYPE_MAP[fhir_type]
        
        blocks[parent_path].append({
            'name': field_name.upper(),
            'cpp_name': field_name.lower(),
            'is_array': is_array,
            'is_choice': is_choice,
            'fhir_type': fhir_type,
            'min': el.get('min', 0),
            'size': mapping['size'],
            'cpp_type': mapping['cpp'],
            'data_type': mapping['data_type'],
            'macro': mapping['macro']
        })
    return blocks

# =====================================================================
# 3. FASTFHIR C++ EMITTER
# =====================================================================
def emit_domain_file(schema_json, resource_name, fhir_version="R5"):
    elements = schema_json.get('snapshot', {}).get('element', [])
    blocks = group_fhir_elements(elements, resource_name)
    version_prefix = f"FF_{fhir_version.upper()}_"
    
    hpp_out = f"// MARK: - {resource_name} Domain ({fhir_version})\n#pragma once\n"
    hpp_out += f"#include \"FF_{fhir_version.upper()}_DataTypes.hpp\"\n"
    hpp_out += "#include <vector>\n#include <string>\n#include <optional>\n\n"
    
    cpp_out = f"// MARK: - Implementation: {resource_name} Domain ({fhir_version})\n"
    cpp_out += f"#include \"FF_{fhir_version.upper()}_{resource_name}.hpp\"\n\n"
    
    for block_path, fields in blocks.items():
        block_struct_name = version_prefix + block_path.replace('.', '_').upper()
        data_struct_name = block_path.replace('.', '') + "Data"
        
        # --- 1. Generate the POD Data Struct (.hpp) ---
        hpp_out += f"struct {data_struct_name} {{\n"
        for f in fields:
            if f['is_array']:
                hpp_out += f"    std::vector<{f['fhir_type']}Data> {f['cpp_name']};\n"
            elif f['is_choice']:
                hpp_out += f"    Offset {f['cpp_name']}_raw_offset = NULL_OFFSET;\n"
            elif f['cpp_type'] == 'Offset':
                # Use std::optional for nested objects to easily check if they are present
                hpp_out += f"    std::optional<{f['fhir_type']}Data> {f['cpp_name']};\n"
            else:
                hpp_out += f"    {f['data_type']} {f['cpp_name']};\n"
        hpp_out += f"}};\n\n"

        # Calculate Aligned Offsets
        current_off = 16 
        layout = []
        for f in fields:
            current_off = align_up(current_off, min(f['size'], 8))
            layout.append({**f, 'offset': current_off})
            current_off += f['size']
            
        total_header_size = align_up(current_off, 8)
        
        # --- 2. Generate the DATA_BLOCK Class (.hpp) ---
        hpp_out += f"struct IFE_EXPORT {block_struct_name} : DATA_BLOCK {{\n"
        hpp_out += f"    static constexpr char type [] = \"{block_struct_name}\";\n"
        hpp_out += f"    static constexpr enum RECOVERY recovery = RECOVER_{block_struct_name};\n\n"
        hpp_out += "    enum vtable_offsets {\n"
        hpp_out += "        VALIDATION = 0,\n        RECOVERY = 8,\n        PADDING = 10,\n"
        for f in layout:
            hpp_out += f"        {f['name']} = {f['offset']},\n"
        hpp_out += f"        HEADER_V1_0_SIZE = {total_header_size},\n        HEADER_SIZE = HEADER_V1_0_SIZE\n    }};\n\n"
        hpp_out += "    Result validate_full(const BYTE* const __base) const noexcept;\n"
        hpp_out += f"    {data_struct_name} read(const BYTE* const __base) const;\n"
        hpp_out += "};\n\n"
        
        # Add STORE signature to header
        hpp_out += f"void STORE_{block_struct_name}(BYTE* const __base, Offset& write_head, const {data_struct_name}& data);\n\n"
        
        # --- 3. Generate the Implementation (.cpp) ---
        # (Omitted validate_full and read() here for brevity, keeping focus on STORE)
        
        # --- 4. Generate the STORE Method (.cpp) ---
        cpp_out += f"void STORE_{block_struct_name}(BYTE* const __base, Offset& write_head, const {data_struct_name}& data) {{\n"
        cpp_out += f"    Offset my_offset = write_head;\n"
        cpp_out += f"    auto __ptr = __base + my_offset;\n"
        cpp_out += f"    write_head += {block_struct_name}::HEADER_SIZE;\n\n"
        cpp_out += f"    STORE_U64(__ptr + {block_struct_name}::VALIDATION, my_offset);\n"
        cpp_out += f"    STORE_U16(__ptr + {block_struct_name}::RECOVERY, {block_struct_name}::recovery);\n\n"
        
        for f in layout:
            cpp_out += f"    // --- Store: {f['name']} ---\n"
            if f['is_array']:
                cpp_out += f"    if (!data.{f['cpp_name']}.empty()) {{\n"
                cpp_out += f"        STORE_U64(__ptr + {block_struct_name}::{f['name']}, write_head);\n"
                cpp_out += f"        // Call custom array writer:\n"
                cpp_out += f"        // STORE_{block_struct_name}_{f['name']}_ARRAY(__base, write_head, data.{f['cpp_name']});\n"
                cpp_out += f"    }} else {{\n"
                cpp_out += f"        STORE_U64(__ptr + {block_struct_name}::{f['name']}, NULL_OFFSET);\n"
                cpp_out += f"    }}\n"
            elif f['is_choice']:
                cpp_out += f"    STORE_U64(__ptr + {block_struct_name}::{f['name']}, data.{f['cpp_name']}_raw_offset);\n"
            elif f['cpp_type'] == 'Offset':
                # Use std::optional to check presence
                cpp_out += f"    if (data.{f['cpp_name']}.has_value()) {{\n"
                cpp_out += f"        STORE_U64(__ptr + {block_struct_name}::{f['name']}, write_head);\n"
                cpp_out += f"        STORE_FF_{fhir_version.upper()}_{f['fhir_type'].upper()}(__base, write_head, data.{f['cpp_name']}.value());\n"
                cpp_out += f"    }} else {{\n"
                cpp_out += f"        STORE_U64(__ptr + {block_struct_name}::{f['name']}, NULL_OFFSET);\n"
                cpp_out += f"    }}\n"
            elif f['fhir_type'] == 'code':
                # Apply the MSB Dictionary Logic
                cpp_out += f"    uint32_t {f['cpp_name']}_encoded = ENCODE_FF_CODE(__base, my_offset, write_head, data.{f['cpp_name']});\n"
                cpp_out += f"    STORE_U32(__ptr + {block_struct_name}::{f['name']}, {f['cpp_name']}_encoded);\n"
            else:
                store_macro = get_store_macro(f['macro'])
                cpp_out += f"    {store_macro}(__ptr + {block_struct_name}::{f['name']}, data.{f['cpp_name']});\n"
                
        cpp_out += f"}}\n\n"
        
    return hpp_out, cpp_out 