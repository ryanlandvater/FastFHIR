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
# 2. HL7 BUNDLE EXTRACTOR & PATH RESOLUTION
# =====================================================================
def extract_structure_definition(bundle_json, resource_name):
    entries = bundle_json.get('entry', [])
    for entry in entries:
        resource = entry.get('resource', {})
        if resource.get('resourceType') == 'StructureDefinition' and resource.get('name') == resource_name:
            return resource.get('snapshot', {}).get('element', [])
    raise ValueError(f"Resource '{resource_name}' not found.")

def _resolve_ff_struct_name(fhir_type, field_name, block_struct_name, resolved_path=None):
    if fhir_type == 'string': return 'FF_STRING'
    if fhir_type in ('BackboneElement', 'Element'):
        if resolved_path: return "FF_" + resolved_path.replace('.', '_').upper()
        return f"{block_struct_name}_{field_name}"
    return f"FF_{fhir_type.upper()}"

def _resolve_data_type_name(fhir_type, field_orig_name, parent_path, resolved_path=None):
    fhir_type_l = fhir_type.lower()
    if fhir_type_l == 'string': return 'std::string_view'
    if fhir_type_l == 'code': return 'std::string'
    if fhir_type in ('BackboneElement', 'Element'):
        if resolved_path: return resolved_path.replace('.', '') + 'Data'
        return parent_path.replace('.', '') + field_orig_name + 'Data'
    if fhir_type == 'Resource': return 'ResourceData'
    if fhir_type in TYPE_MAP and fhir_type != 'DEFAULT': return TYPE_MAP[fhir_type]['data_type']
    return fhir_type + 'Data'

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
        if f['is_array']:
            cpp += f"{indent}Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}if ({f['cpp_name']}_off != FF_NULL_OFFSET) {{\n"
            cpp += f"{indent}    FF_ARRAY __arr({f['cpp_name']}_off, __size, __version);\n"
            cpp += f"{indent}    auto STEP = __arr.entry_step(__base);\n"
            cpp += f"{indent}    auto ENTRIES = __arr.entry_count(__base);\n"
            cpp += f"{indent}    auto __item_ptr = __arr.entries(__base);\n"
            cpp += f"{indent}    for (uint32_t i = 0; i < ENTRIES; ++i, __item_ptr += STEP) {{\n"
            if f['fhir_type'] in ('string', 'code'):
                code_enum = f.get('code_enum')
                cpp += f"{indent}        Offset __str_off = LOAD_U64(__item_ptr);\n"
                cpp += f"{indent}        if (__str_off != FF_NULL_OFFSET) {{\n"
                cpp += f"{indent}            FF_STRING _blk(__str_off, __size, __version);\n"
                if code_enum:
                    cpp += f"{indent}            data.{f['cpp_name']}.push_back({code_enum['parse']}(_blk.read(__base)));\n"
                else:
                    cpp += f"{indent}            data.{f['cpp_name']}.push_back(_blk.read_view(__base));\n"
                cpp += f"{indent}        }}\n"
            else:
                type_call = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
                cpp += f"{indent}        {type_call} _blk(static_cast<Offset>(__item_ptr - __base), __size, __version);\n"
                cpp += f"{indent}        data.{f['cpp_name']}.push_back(_blk.read(__base));\n"
            cpp += f"{indent}    }}\n{indent}}}\n"
        elif f['fhir_type'] == 'string':
            cpp += f"{indent}Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}if ({f['cpp_name']}_off != FF_NULL_OFFSET) {{ FF_STRING _blk({f['cpp_name']}_off, __size, __version); data.{f['cpp_name']} = _blk.read_view(__base); }}\n"
        elif f['cpp_type'] == 'Offset':
            type_call = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
            cpp += f"{indent}Offset {f['cpp_name']}_off = LOAD_U64(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}if ({f['cpp_name']}_off != FF_NULL_OFFSET) {{ {type_call} _blk({f['cpp_name']}_off, __size, __version); data.{f['cpp_name']} = new auto(_blk.read(__base)); }}\n"
        elif f['fhir_type'] == 'code':
            code_enum = f.get('code_enum')
            if code_enum:
                cpp += f"{indent}data.{f['cpp_name']} = {code_enum['parse']}(FF_ResolveCode(LOAD_U32(__base + __offset + {block_struct_name}::{f['name']}), __version));\n"
            else:
                cpp += f"{indent}data.{f['cpp_name']} = FF_ResolveCode(LOAD_U32(__base + __offset + {block_struct_name}::{f['name']}), __version);\n"
        else:
            cpp += f"{indent}data.{f['cpp_name']} = {f['macro']}(__base + __offset + {block_struct_name}::{f['name']});\n"
        if f['first_version_idx'] > 0: cpp += f"    }}\n"
    return cpp

def generate_size_fields(layout, block_struct_name, data_name):
    cpp = f"    Size __total = {block_struct_name}::HEADER_SIZE;\n"
    for f in layout:
        if f['is_array']:
            if f['fhir_type'] in ('string', 'code'):
                code_enum = f.get('code_enum')
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * TYPE_SIZE_OFFSET);\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                if code_enum: cpp += f"            __total += ff_align(FF_STRING::HEADER_SIZE + std::string({code_enum['serialize']}(__item)).size());\n"
                else:         cpp += f"            __total += ff_align(FF_STRING::HEADER_SIZE + __item.size());\n"
                cpp += f"        }}\n    }}\n"
            else:
                child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE;\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                if child_struct == 'FF_RESOURCE':
                    cpp += f"            __total += FF_RESOURCE::HEADER_SIZE + ff_align(FF_STRING::HEADER_SIZE + __item.json.size());\n"
                else:
                    cpp += f"            __total += SIZE_{child_struct}(__item);\n"
                cpp += f"        }}\n    }}\n"
        elif f['fhir_type'] == 'string':
            cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
            cpp += f"        __total += ff_align(FF_STRING::HEADER_SIZE + {data_name}.{f['cpp_name']}.size());\n    }}\n"
        elif f['cpp_type'] == 'Offset':
            child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
            cpp += f"    if ({data_name}.{f['cpp_name']} != nullptr) {{\n"
            if child_struct == 'FF_RESOURCE':
                cpp += f"        __total += FF_RESOURCE::HEADER_SIZE + ff_align(FF_STRING::HEADER_SIZE + {data_name}.{f['cpp_name']}->json.size());\n    }}\n"
            else:
                cpp += f"        __total += SIZE_{child_struct}(*{data_name}.{f['cpp_name']});\n    }}\n"
        elif f['fhir_type'] == 'code':
            code_enum = f.get('code_enum')
            if code_enum: cpp += f"    __total += ff_align(FF_STRING::HEADER_SIZE + std::string({code_enum['serialize']}({data_name}.{f['cpp_name']})).size());\n"
            else:         cpp += f"    __total += ff_align(FF_STRING::HEADER_SIZE + {data_name}.{f['cpp_name']}.size());\n"
    cpp += "    return ff_align(__total);\n"
    return cpp

def generate_store_fields(layout, block_struct_name, ptr_name, data_name):
    cpp = ""
    for f in layout:
        cpp += f"    // --- Store: {f['name']} ---\n"
        if f['is_array']:
            if f['fhir_type'] in ('string', 'code'):
                code_enum = f.get('code_enum')
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                cpp += f"        FF_ArrayHeader::Store(__base, child_off, RECOVER_{block_struct_name}, TYPE_SIZE_OFFSET, __n);\n"
                cpp += f"        Offset __off_tbl = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * TYPE_SIZE_OFFSET;\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                cpp += f"            STORE_U64(__base + __off_tbl + __i * TYPE_SIZE_OFFSET, child_off);\n"
                if code_enum: cpp += f"            child_off += STORE_FF_STRING(__base, child_off, std::string({code_enum['serialize']}({data_name}.{f['cpp_name']}[__i])));\n"
                else:         cpp += f"            child_off += STORE_FF_STRING(__base, child_off, {data_name}.{f['cpp_name']}[__i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
            else:
                child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
                store_fn = f"STORE_{child_struct}"
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                cpp += f"        FF_ArrayHeader::Store(__base, child_off, RECOVER_{block_struct_name}, {child_struct}::HEADER_SIZE, __n);\n"
                cpp += f"        Offset __entries_start = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * {child_struct}::HEADER_SIZE;\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                if child_struct == 'FF_RESOURCE':
                    cpp += f"            {store_fn}(__base, __entries_start + __i * {child_struct}::HEADER_SIZE, child_off, {data_name}.{f['cpp_name']}[__i]);\n"
                else:
                    cpp += f"            child_off = {store_fn}(__base, __entries_start + __i * {child_struct}::HEADER_SIZE, child_off, {data_name}.{f['cpp_name']}[__i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
        elif f['fhir_type'] == 'string':
            cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
            cpp += f"        child_off += STORE_FF_STRING(__base, child_off, {data_name}.{f['cpp_name']});\n"
            cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
        elif f['cpp_type'] == 'Offset':
            child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
            store_fn = f"STORE_{child_struct}"
            cpp += f"    if ({data_name}.{f['cpp_name']} != nullptr) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
            cpp += f"        Offset nested_hdr = child_off;\n"
            cpp += f"        child_off += {child_struct}::HEADER_SIZE;\n"
            if child_struct == 'FF_RESOURCE':
                cpp += f"        {store_fn}(__base, nested_hdr, child_off, *{data_name}.{f['cpp_name']});\n"
            else:
                cpp += f"        child_off = {store_fn}(__base, nested_hdr, child_off, *{data_name}.{f['cpp_name']});\n"
            cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
        elif f['fhir_type'] == 'code':
            code_enum = f.get('code_enum')
            if code_enum:
                cpp += f"    STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, ENCODE_FF_CODE(__base, hdr_off, child_off, std::string({code_enum['serialize']}({data_name}.{f['cpp_name']}))));\n"
            else:
                cpp += f"    STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, ENCODE_FF_CODE(__base, hdr_off, child_off, {data_name}.{f['cpp_name']}));\n"
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
                    'name': field_name.upper(), 'cpp_name': field_name.lower(), 'orig_name': field_name,
                    'is_array': is_array, 'fhir_type': f_type, 'size': mapping['size'],
                    'size_const': mapping['size_const'], 'cpp_type': mapping['cpp'],
                    'data_type': mapping['data_type'], 'macro': mapping['macro'],
                    'first_version_name': v_name, 'first_version_idx': v_idx, 'offset': off
                })
                blk['seen'].add(field_name)
            blk['sizes'][v_name] = align_up(blk['layout'][-1]['offset'] + blk['layout'][-1]['size'], 8)
            
    for parent_path, blk in master_blocks.items():
        for f in blk['layout']:
            if f['fhir_type'] not in ('BackboneElement', 'Element'): continue
            expected = parent_path + '.' + f['orig_name']
            if expected in master_blocks:
                f['resolved_path'] = expected
                continue
            direct_root = root_resource + '.' + f['orig_name']
            if direct_root in master_blocks:
                f['resolved_path'] = direct_root
                continue
            candidates = [p for p in master_blocks.keys() if p.endswith('.' + f['orig_name'])]
            if len(candidates) == 1: f['resolved_path'] = candidates[0]
    return master_blocks

def generate_cxx_for_blocks(master_blocks, versions):
    hpp, cpp = "", ""
    block_data_names = sorted({path.replace('.', '') + "Data" for path in master_blocks})
    for d_name in block_data_names: hpp += f"struct {d_name};\n"
    if block_data_names: hpp += "\n"

    def get_deps(layout):
        deps = set()
        for f in layout:
            if f['fhir_type'] in master_blocks: deps.add(f['fhir_type'])
            if f.get('resolved_path') and f['resolved_path'] in master_blocks: deps.add(f['resolved_path'])
        return deps
    
    ordered_paths = []
    visited = set()
    def visit(path):
        if path in visited: return
        visited.add(path)
        for dep in get_deps(master_blocks[path]['layout']):
            if dep in master_blocks: visit(dep)
        ordered_paths.append(path)
    for path in master_blocks: visit(path)
    
    for path in ordered_paths:
        blk = master_blocks[path]
        s_name, d_name = "FF_" + path.replace('.', '_').upper(), path.replace('.', '') + "Data"
        layout, sizes = blk['layout'], blk['sizes']
        
        # POD Struct
        hpp += f"struct {d_name} {{\n"
        for f in layout:
            code_enum = f.get('code_enum')
            if f['is_array']:
                item_type = code_enum['enum'] if code_enum else _resolve_data_type_name(f['fhir_type'], f['orig_name'], path, f.get('resolved_path'))
                hpp += f"    std::vector<{item_type}> {f['cpp_name']};\n"
            elif f['fhir_type'] == 'string': hpp += f"    std::string_view {f['cpp_name']};\n"
            elif code_enum: hpp += f"    {code_enum['enum']} {f['cpp_name']} = {code_enum['enum']}::Unknown;\n"
            elif f['cpp_type'] == 'Offset': hpp += f"    {_resolve_data_type_name(f['fhir_type'], f['orig_name'], path, f.get('resolved_path'))}* {f['cpp_name']} = nullptr;\n"
            elif f['data_type'] == 'bool': hpp += f"    bool {f['cpp_name']} = false;\n"
            else: hpp += f"    {f['data_type']} {f['cpp_name']} = 0;\n"
        hpp += f"}};\n\n"

        # Data Block
        hpp += f"struct FF_EXPORT {s_name} : DATA_BLOCK {{\n"
        hpp += f"    static constexpr char type [] = \"{s_name}\";\n"
        hpp += f"    static constexpr enum RECOVERY recovery = RECOVER_{s_name};\n"
        hpp += f"    enum vtable_sizes {{\n        VALIDATION_S = TYPE_SIZE_UINT64,\n        RECOVERY_S = TYPE_SIZE_UINT16,\n        PADDING_S = 6,\n"
        for f in layout: hpp += f"        {f['name']}_S = {f['size_const']},\n"
        hpp += f"    }};\n    enum vtable_offsets {{\n        VALIDATION = 0,\n        RECOVERY = VALIDATION + VALIDATION_S,\n        PADDING = RECOVERY + RECOVERY_S,\n"
        prev_name, prev_size = "PADDING", "PADDING_S"
        for f in layout:
            hpp += f"        {f['name']:<20}= {prev_name} + {prev_size},\n"
            prev_name, prev_size = f['name'], f"{f['name']}_S"
        for v, sz in sizes.items(): hpp += f"        HEADER_{v}_SIZE = {sz},\n"
        hpp += f"        HEADER_SIZE = HEADER_{versions[-1]}_SIZE\n    }};\n"
        
        min_version = next((v for v in versions if v in sizes), None)
        hpp += f"    inline Size get_header_size() const {{\n"
        if min_version: hpp += f"        if (__version < FHIR_VERSION_{min_version}) return 0;\n"
        for v in versions:
            if v in sizes: hpp += f"        if (__version <= FHIR_VERSION_{v}) return HEADER_{v}_SIZE;\n"
        hpp += f"        return HEADER_SIZE;\n    }}\n"

        hpp += f"    explicit {s_name}(Offset off, Size total_size, uint32_t ver) : DATA_BLOCK(off, total_size, ver) {{}}\n"
        hpp += f"    FF_Result validate_full(const BYTE* const __base) const noexcept;\n"
        hpp += f"    {d_name} read(const BYTE* const __base) const;\n}};\n\n"
        
        # Lock-Free Store Signatures
        hpp += f"Size SIZE_{s_name}(const {d_name}& data);\n"
        hpp += f"Offset STORE_{s_name}(BYTE* const __base, Offset hdr_off, Offset child_off, const {d_name}& data);\n"
        hpp += f"inline Offset STORE_{s_name}(BYTE* const __base, Offset start_off, const {d_name}& data) {{\n"
        hpp += f"    return STORE_{s_name}(__base, start_off, start_off + {s_name}::HEADER_SIZE, data);\n}}\n\n"

        # CPP Implementations
        cpp += f"FF_Result {s_name}::validate_full(const BYTE *const __base) const noexcept {{ return validate_offset(__base, type, recovery); }}\n"
        cpp += f"{d_name} {s_name}::read(const BYTE *const __base) const {{\n"
        cpp += f"#ifdef __EMSCRIPTEN__\n    const_cast<{s_name}&>(*this).check_and_fetch_remote(__base);\n#endif\n"
        cpp += f"    {d_name} data;\n{generate_read_fields(layout, s_name)}    return data;\n}}\n"
        
        cpp += f"Size SIZE_{s_name}(const {d_name}& data) {{\n{generate_size_fields(layout, s_name, 'data')}}}\n\n"
        
        cpp += f"Offset STORE_{s_name}(BYTE* const __base, Offset hdr_off, Offset child_off, const {d_name}& data) {{\n"
        cpp += f"    auto __ptr = __base + hdr_off;\n"
        cpp += f"    STORE_U64(__ptr + {s_name}::VALIDATION, hdr_off);\n"
        cpp += f"    STORE_U16(__ptr + {s_name}::RECOVERY, {s_name}::recovery);\n"
        cpp += f"{generate_store_fields(layout, s_name, '__ptr', 'data')}\n"
        cpp += f"    return child_off;\n}}\n\n"
    return hpp, cpp

# =====================================================================
# 5. MASTER BUILD ORCHESTRATOR
# =====================================================================
def compile_fhir_library(resources, versions, input_dir="fhir_specs", output_dir="generated_src", code_enum_map=None):
    code_enums = code_enum_map or {}
    os.makedirs(output_dir, exist_ok=True)
    
    print("Generating FF_DataTypes...")
    type_bundles = []
    for v in versions:
        p = os.path.join(input_dir, v, "profiles-types.json")
        if os.path.exists(p): 
            with open(p, 'r') as f: type_bundles.append((v, json.load(f)))
    
    fwd_decls = set([t + "Data" for t in TARGET_TYPES])
    auto_header = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
    )

    hpp_head = f"{auto_header}// MARK: - Universal Data Types\n#pragma once\n#include \"../include/FF_Primitives.hpp\"\n#include \"FF_CodeSystems.hpp\"\n#include <vector>\n#include <string_view>\n\n"
    for decl in sorted(fwd_decls): hpp_head += f"struct {decl};\n"
    hpp_head += "\n"

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