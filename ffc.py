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
    hpp_out += f"#include \"FF_{fhir_version.upper()}_DataTypes.hpp\"\n#include <vector>\n#include <string>\n\n"
    
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
                hpp_out += f"    // Polymorphic choice type\n"
                hpp_out += f"    Offset {f['cpp_name']}_raw_offset = NULL_OFFSET;\n"
            elif f['cpp_type'] == 'Offset':
                hpp_out += f"    {f['fhir_type']}Data {f['cpp_name']};\n"
            else:
                hpp_out += f"    {f['data_type']} {f['cpp_name']};\n"
        hpp_out += "};\n\n"

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
        
        # --- 3. Generate the Implementation (.cpp) ---
        cpp_out += f"Result {block_struct_name}::validate_full(const BYTE *const __base) const noexcept {{\n"
        cpp_out += "    auto result = validate_offset(__base, type, recovery);\n    if (result & IRIS_FAILURE) return result;\n"
        cpp_out += "    return IRIS_SUCCESS;\n}\n\n"
        
        # --- 4. Generate the Read Method (.cpp) ---
        cpp_out += f"{data_struct_name} {block_struct_name}::read(const BYTE *const __base) const {{\n"
        cpp_out += f"#ifdef __EMSCRIPTEN__\n    const_cast<{block_struct_name}&>(*this).check_and_fetch_remote(__base);\n#endif\n\n"
        cpp_out += f"    {data_struct_name} data;\n"
        
        for f in layout:
            cpp_out += f"\n    // --- Read: {f['name']} --- \n"
            if f['is_array']:
                cpp_out += f"    Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {f['name']});\n"
                cpp_out += f"    if ({f['cpp_name']}_off != NULL_OFFSET) {{\n"
                cpp_out += f"        // Emits stride-based array looping logic here\n"
                cpp_out += f"        // data.{f['cpp_name']} = read_array_{f['cpp_name']}(__base);\n"
                cpp_out += f"    }}\n"
            elif f['is_choice']:
                cpp_out += f"    data.{f['cpp_name']}_raw_offset = LOAD_U64(__base + __offset + {f['name']});\n"
            elif f['cpp_type'] == 'Offset':
                cpp_out += f"    Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {f['name']});\n"
                cpp_out += f"    if ({f['cpp_name']}_off != NULL_OFFSET) {{\n"
                cpp_out += f"        auto _blk = FF_{fhir_version.upper()}_{f['fhir_type'].upper()}({f['cpp_name']}_off, __size, __version);\n"
                cpp_out += f"        data.{f['cpp_name']} = _blk.read(__base);\n"
                cpp_out += f"    }}\n"
            elif f['macro'] == 'LOAD_U32' and f['fhir_type'] == 'code':
                cpp_out += f"    uint32_t {f['cpp_name']}_code = {f['macro']}(__base + __offset + {f['name']});\n"
                cpp_out += f"    data.{f['cpp_name']} = FF_ResolveCode({f['cpp_name']}_code); // Uses dictionary\n"
            else:
                cpp_out += f"    data.{f['cpp_name']} = {f['macro']}(__base + __offset + {f['name']});\n"
                
        cpp_out += "\n    return data;\n}\n\n"
        
    return hpp_out, cpp_out