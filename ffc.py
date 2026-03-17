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
# 2. FIELD SERIALIZATION HELPER
# =====================================================================
def generate_store_fields(layout, block_struct_name, ptr_name, data_name, fhir_version):
    """Generates the C++ serialization logic for a set of fields."""
    cpp = ""
    for f in layout:
        cpp += f"    // --- Store: {f['name']} ---\n"
        if f['is_array']:
            cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
            # Call the specific array writer for this FHIR type
            cpp += f"        STORE_FF_{fhir_version.upper()}_{f['fhir_type'].upper()}_ARRAY(__base, write_head, {data_name}.{f['cpp_name']});\n"
            cpp += f"    }} else {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, NULL_OFFSET);\n"
            cpp += f"    }}\n"
        elif f['is_choice']:
            cpp += f"    STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, {data_name}.{f['cpp_name']}_raw_offset);\n"
        elif f['cpp_type'] == 'Offset':
            cpp += f"    if ({data_name}.{f['cpp_name']}.has_value()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, write_head);\n"
            cpp += f"        STORE_FF_{fhir_version.upper()}_{f['fhir_type'].upper()}(__base, write_head, {data_name}.{f['cpp_name']}.value());\n"
            cpp += f"    }} else {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, NULL_OFFSET);\n"
            cpp += f"    }}\n"
        elif f['fhir_type'] == 'code':
            cpp += f"    uint32_t {f['cpp_name']}_encoded = ENCODE_FF_CODE(__base, static_cast<Offset>({ptr_name} - __base), write_head, {data_name}.{f['cpp_name']});\n"
            cpp += f"    STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, {f['cpp_name']}_encoded);\n"
        else:
            store_macro = get_store_macro(f['macro'])
            cpp += f"    {store_macro}({ptr_name} + {block_struct_name}::{f['name']}, {data_name}.{f['cpp_name']});\n"
    return cpp

# =====================================================================
# 3. FASTFHIR C++ EMITTER
# =====================================================================
# (Omitted group_fhir_elements for brevity, use the same one from previous step)
def group_fhir_elements(elements, root_resource):
    blocks = {}
    for el in elements:
        path = el.get('path', '')
        if not path.startswith(root_resource) or len(path.split('.')) == 1: continue
        parent_path = '.'.join(path.split('.')[:-1])
        field_name = path.split('.')[-1]
        if parent_path not in blocks: blocks[parent_path] = []
        types = el.get('type', [{'code': 'BackboneElement'}])
        fhir_type = types[0].get('code', 'BackboneElement')
        is_array = el.get('max') == '*'
        is_choice = field_name.endswith('[x]')
        if is_choice: field_name = field_name.replace('[x]', '')
        mapping = TYPE_MAP['DEFAULT'] if (is_array or is_choice or fhir_type not in TYPE_MAP) else TYPE_MAP[fhir_type]
        blocks[parent_path].append({'name': field_name.upper(), 'cpp_name': field_name.lower(), 'is_array': is_array, 'is_choice': is_choice, 'fhir_type': fhir_type, 'size': mapping['size'], 'cpp_type': mapping['cpp'], 'data_type': mapping['data_type'], 'macro': mapping['macro']})
    return blocks

# =====================================================================
# 3. FASTFHIR C++ EMITTER
# =====================================================================
def emit_domain_file(schema_json, resource_name, fhir_version="R5"):
    elements = schema_json.get('snapshot', {}).get('element', [])
    blocks = group_fhir_elements(elements, resource_name)
    version_prefix = f"FF_{fhir_version.upper()}_"
    
    hpp_out = f"// MARK: - {resource_name} Domain ({fhir_version})\n#pragma once\n#include \"FF_{fhir_version.upper()}_DataTypes.hpp\"\n#include <vector>\n#include <string>\n#include <optional>\n\n"
    cpp_out = f"// MARK: - Implementation: {resource_name} Domain ({fhir_version})\n#include \"FF_{fhir_version.upper()}_{resource_name}.hpp\"\n\n"
    
    for block_path, fields in blocks.items():
        block_struct_name = version_prefix + block_path.replace('.', '_').upper()
        data_struct_name = block_path.replace('.', '') + "Data"
        
        # Calculate Aligned Offsets
        current_off = 16 
        layout = []
        for f in fields:
            current_off = align_up(current_off, min(f['size'], 8))
            layout.append({**f, 'offset': current_off})
            current_off += f['size']
            
        total_header_size = align_up(current_off, 8)
        
        # --- 1. Generate STORE Signatures (.hpp) ---
        hpp_out += f"struct {data_struct_name} {{ /* omitted for brevity */ }};\n"
        hpp_out += f"struct IFE_EXPORT {block_struct_name} : DATA_BLOCK {{ /* omitted for brevity */ }};\n"
        hpp_out += f"void STORE_{block_struct_name}(BYTE* const __base, Offset& write_head, const {data_struct_name}& data);\n"
        hpp_out += f"void STORE_{block_struct_name}_ARRAY(BYTE* const __base, Offset& write_head, const std::vector<{data_struct_name}>& items);\n\n"
        
        # --- 2. Generate Scalar STORE (.cpp) ---
        cpp_out += f"void STORE_{block_struct_name}(BYTE* const __base, Offset& write_head, const {data_struct_name}& data) {{\n"
        cpp_out += f"    Offset my_offset = write_head;\n"
        cpp_out += f"    auto __ptr = __base + my_offset;\n"
        cpp_out += f"    write_head += {block_struct_name}::HEADER_SIZE;\n\n"
        cpp_out += f"    STORE_U64(__ptr + {block_struct_name}::VALIDATION, my_offset);\n"
        cpp_out += f"    STORE_U16(__ptr + {block_struct_name}::RECOVERY, {block_struct_name}::recovery);\n\n"
        cpp_out += generate_store_fields(layout, block_struct_name, "__ptr", "data", fhir_version)
        cpp_out += f"}}\n\n"
        
        # --- 3. Generate Array STORE (.cpp) ---
        cpp_out += f"void STORE_{block_struct_name}_ARRAY(BYTE* const __base, Offset& write_head, const std::vector<{data_struct_name}>& items) {{\n"
        cpp_out += f"    if (items.empty()) return;\n\n"
        cpp_out += f"    Offset array_offset = write_head;\n"
        cpp_out += f"    auto __ptr = __base + array_offset;\n"
        cpp_out += f"    write_head += 16; // Standard Array Header Size\n\n"
        
        cpp_out += f"    STORE_U64(__ptr + 0, array_offset);\n"
        cpp_out += f"    STORE_U16(__ptr + 8, RECOVER_{block_struct_name}_ARRAY);\n"
        cpp_out += f"    STORE_U16(__ptr + 10, {block_struct_name}::HEADER_SIZE);\n"
        cpp_out += f"    STORE_U32(__ptr + 12, static_cast<uint32_t>(items.size()));\n\n"
        
        cpp_out += f"    Offset vtable_start = write_head;\n"
        cpp_out += f"    write_head += items.size() * {block_struct_name}::HEADER_SIZE;\n"
        cpp_out += f"    write_head = align_up(write_head, 8); // Align for trailing variable data\n\n"
        
        cpp_out += f"    auto __item_ptr = __base + vtable_start;\n"
        cpp_out += f"    for (const auto& item : items) {{\n"
        cpp_out += f"        Offset current_item_offset = static_cast<Offset>(__item_ptr - __base);\n"
        cpp_out += f"        STORE_U64(__item_ptr + {block_struct_name}::VALIDATION, current_item_offset);\n"
        cpp_out += f"        STORE_U16(__item_ptr + {block_struct_name}::RECOVERY, {block_struct_name}::recovery);\n\n"
        
        # Call the helper again, but target __item_ptr and the loop variable 'item'
        cpp_out += generate_store_fields(layout, block_struct_name, "__item_ptr", "item", fhir_version)
        
        cpp_out += f"\n        __item_ptr += {block_struct_name}::HEADER_SIZE;\n"
        cpp_out += f"    }}\n"
        cpp_out += f"}}\n\n"
        
    return hpp_out, cpp_out