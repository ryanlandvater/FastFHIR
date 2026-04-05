# =============================================================
# FastFHIR FlatBuffer Code Generator
#
# This script processes FHIR StructureDefinition bundles to generate
# C++ header and source files that define zero-copy data structures
# and accessors for FHIR resources. It extracts ValueSet information
# to create enums for code fields, and generates efficient getters
# and size calculation methods for each resource type.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: FastFHIR Shared Source License (FF-SSL)
# =============================================================

import os
import json
import re

TARGET_TYPES = [
    "Extension", 
    "Coding", "CodeableConcept", "Quantity", "Identifier",
    "Range", "Period", "Reference", "Meta", "Narrative",
    "Annotation", "HumanName", "Address", "ContactPoint",
    "Attachment", "Ratio", "SampledData", "Duration",
    "Timing", "Dosage", "Signature", "CodeableReference", "VirtualServiceDetail"
]
TARGET_RESOURCES = [
    "Observation", "Patient", "Encounter", "DiagnosticReport", "Bundle"
]

# =====================================================================
# 1. FASTFHIR TYPE MAPPING & NORMALIZATION
# =====================================================================
TYPE_MAP = {
    'boolean':      {'cpp': 'uint8_t',  'data_type': 'uint8_t', 'null': 'FF_NULL_UINT8',    'size': 1, 'size_const': 'TYPE_SIZE_UINT8',   'macro': 'LOAD_U8'},
    'integer':      {'cpp': 'uint32_t', 'data_type': 'uint32_t', 'null': 'FF_NULL_UINT32',  'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'},
    'unsignedInt':  {'cpp': 'uint32_t', 'data_type': 'uint32_t', 'null': 'FF_NULL_UINT32',  'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'},
    'positiveInt':  {'cpp': 'uint32_t', 'data_type': 'uint32_t', 'null': 'FF_NULL_UINT32',  'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'},
    'decimal':      {'cpp': 'double',   'data_type': 'double', 'null': 'FF_NULL_F64',       'size': 8, 'size_const': 'TYPE_SIZE_FLOAT64', 'macro': 'LOAD_F64'},
    'code':         {'cpp': 'uint32_t', 'data_type': 'std::string', 'null': 'FF_CODE_NULL', 'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'}, 
    'DEFAULT':      {'cpp': 'Offset',   'data_type': 'Offset', 'null': 'FF_NULL_OFFSET',    'size': 8, 'size_const': 'TYPE_SIZE_OFFSET',  'macro': 'LOAD_U64'}  
}

BASE_BLOCK_HEADER_SIZE = 10

STRING_TYPES = {
    'string', 'id', 'oid', 'uuid', 'uri', 'url', 'canonical', 
    'markdown', 'date', 'datetime', 'instant', 'time', 'base64binary', 'xhtml'
}

def sanitize_fhir_type(raw_type):
    if '/' in raw_type: raw_type = raw_type.split('/')[-1]
    if raw_type.startswith('System.'): raw_type = raw_type[7:]
    if raw_type.lower() in STRING_TYPES: return 'string'
    return raw_type

def get_store_macro(load_macro):
    return load_macro.replace('LOAD_', 'STORE_')

# Helper to annotate code fields with their corresponding ValueSet enums
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

def _field_key_constant_name(raw_name):
    snake = re.sub(r'(?<!^)(?=[A-Z])', '_', raw_name).upper()
    snake = re.sub(r'[^A-Z0-9]+', '_', snake)
    snake = re.sub(r'_+', '_', snake).strip('_')
    return f"FF_{snake}"

def _field_key_short_name(raw_name):
    snake = re.sub(r'(?<!^)(?=[A-Z])', '_', raw_name).upper()
    snake = re.sub(r'[^A-Z0-9]+', '_', snake)
    return re.sub(r'_+', '_', snake).strip('_')

def _block_key_namespace(path):
    return re.sub(r'[^A-Z0-9_]', '_', path.replace('.', '_').upper())

def _child_recovery_key_expr(f, block_struct_name):
    if f['is_array']:
        if f['fhir_type'] in ('string', 'code'):
            return 'RECOVER_FF_STRING'
        child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
        return f'RECOVER_{child_struct}'
    if f['fhir_type'] == 'string':
        return 'RECOVER_FF_STRING'
    if f['cpp_type'] == 'Offset':
        child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
        return f'RECOVER_{child_struct}'
    return 'FF_RECOVER_UNDEFINED'

def _needs_getter(f):
    """Returns True if a field requires a parent-created getter method."""
    return f['is_array'] or f['fhir_type'] == 'string' or f['cpp_type'] == 'Offset'

def _getter_return_type(f, block_struct_name):
    """Returns the C++ return type for a field's getter method."""
    if f['is_array']:
        return 'FF_ARRAY'
    elif f['fhir_type'] == 'string':
        return 'FF_STRING'
    else:
        return _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))

def _field_kind_expr(f):
    if f['is_array']:
        return 'FF_FIELD_ARRAY'
    if f['fhir_type'] == 'string':
        return 'FF_FIELD_STRING'
    if f['cpp_type'] == 'Offset':
        return 'FF_FIELD_BLOCK'
    if f['fhir_type'] == 'code':
        return 'FF_FIELD_CODE'
    if f['fhir_type'] == 'boolean':
        return 'FF_FIELD_BOOL'
    if f['data_type'] == 'double':
        return 'FF_FIELD_FLOAT64'
    if f['data_type'] == 'uint32_t':
        return 'FF_FIELD_UINT32'
    return 'FF_FIELD_UNKNOWN'

def _child_recovery_expr(f, block_struct_name):
    if f['is_array']:
        if f['fhir_type'] in ('string', 'code'):
            return 'FF_STRING::recovery'
        child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
        return f'{child_struct}::recovery'
    if f['fhir_type'] == 'string':
        return 'FF_STRING::recovery'
    if f['cpp_type'] == 'Offset':
        child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
        return f'{child_struct}::recovery'
    return 'FF_RECOVER_UNDEFINED'

def _array_entries_are_offsets_expr(f):
    if not f['is_array']:
        return '0'
    return '1' if f['fhir_type'] in ('string', 'code') else '0'

def generate_getter_declarations(layout, block_struct_name):
    """Generate getter method declarations for child data blocks (Iris pattern)."""
    hpp = ""
    for f in layout:
        if not _needs_getter(f):
            continue
        ret_type = _getter_return_type(f, block_struct_name)
        hpp += f"    {ret_type} get_{f['cpp_name']}(const BYTE* const __base) const;\n"
    return hpp

def generate_getter_implementations(layout, block_struct_name):
    """Generate getter method implementations following the Iris pattern."""
    cpp = ""
    for f in layout:
        if not _needs_getter(f):
            continue
        ret_type = _getter_return_type(f, block_struct_name)
        cpp += f"{ret_type} {block_struct_name}::get_{f['cpp_name']}(const BYTE* const __base) const {{\n"
        cpp += f"#ifdef __EMSCRIPTEN__\n    const_cast<{block_struct_name}&>(*this).check_and_fetch_remote(__base);\n#endif\n"
        if f['first_version_idx'] > 0:
            cpp += f"    if (__version < FHIR_VERSION_{f['first_version_name']}) return {ret_type}(FF_NULL_OFFSET, __size, __version);\n"
        cpp += f"    auto child = {ret_type}(LOAD_U64(__base + __offset + {block_struct_name}::{f['name']}), __size, __version);\n"
        cpp += f"    if (!child) return child;\n"
        cpp += f"    auto result = child.validate_offset(__base, {ret_type}::type, {ret_type}::recovery);\n"
        cpp += f"    if (result != FF_SUCCESS) throw std::runtime_error(\"Failed to retrieve {f['cpp_name']}: \" + result.message);\n"
        cpp += f"    return child;\n"
        cpp += f"}}\n"
    return cpp

def generate_field_info_implementation(layout, block_struct_name):
    cpp = f'const FF_FieldInfo {block_struct_name}::FIELDS[{block_struct_name}::FIELD_COUNT] = {{\n'
    for f in layout:
        cpp += (
            f'    {{"{f["orig_name"]}", {_field_kind_expr(f)}, {block_struct_name}::{f["name"]}, '
            f'{_child_recovery_expr(f, block_struct_name)}, {_array_entries_are_offsets_expr(f)}}},\n'
        )
    cpp += '};\n'
    cpp += f'const FF_FieldInfo* {block_struct_name}::find_field(std::string_view name) const {{\n'
    cpp += f'    for (size_t i = 0; i < FIELD_COUNT; ++i) {{\n'
    cpp += f'        if (name == FIELDS[i].name) return &FIELDS[i];\n'
    cpp += f'    }}\n'
    cpp += f'    return nullptr;\n'
    cpp += f'}}\n'
    return cpp

def generate_reflection_dispatch(block_struct_names, resources):
    hpp = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
        "#pragma once\n"
        "#include \"../include/FF_Primitives.hpp\"\n"
        "#include <string_view>\n"
        "#include <vector>\n\n"
        "namespace FastFHIR {\n"
        "class Node;\n"
        "std::vector<FF_FieldInfo> reflected_fields(uint16_t recovery);\n"
        "std::vector<std::string_view> reflected_keys(uint16_t recovery);\n"
        "Node reflected_child_node(const BYTE* base, Size size, uint32_t version, Offset offset, uint16_t recovery, std::string_view key);\n"
        "std::string_view reflected_resource_type(uint16_t recovery);\n"
        "} // namespace FastFHIR\n"
    )

    cpp = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n\n"
        "#include \"../include/FF_Utilities.hpp\"\n"
        "#include \"../include/FF_Parser.hpp\"\n"
        "#include \"FF_AllTypes.hpp\"\n"
        "#include \"FF_Reflection.hpp\"\n\n"
        "namespace FastFHIR {\n\n"
        "template <typename T_Block>\n"
        "std::vector<FF_FieldInfo> fields_for_block() {\n"
        "    return std::vector<FF_FieldInfo>(T_Block::FIELDS, T_Block::FIELDS + T_Block::FIELD_COUNT);\n"
        "}\n\n"
        "template <typename T_Block>\n"
        "Node object_field_node(const T_Block& block, const BYTE* base, std::string_view key) {\n"
        "    const FF_FieldInfo* field = block.find_field(key);\n"
        "    if (!field) return {};\n\n"
        "    const Offset value_offset = block.__offset + field->field_offset;\n"
        "    if (field->kind == FF_FIELD_STRING) {\n"
        "        const Offset child_offset = LOAD_U64(base + value_offset);\n"
        "        if (child_offset == FF_NULL_OFFSET) return {};\n"
        "        return Node(base, block.__size, block.__version, child_offset, FF_STRING::recovery, FF_FIELD_STRING);\n"
        "    }\n"
        "    if (field->kind == FF_FIELD_ARRAY) {\n"
        "        const Offset child_offset = LOAD_U64(base + value_offset);\n"
        "        if (child_offset == FF_NULL_OFFSET) return {};\n"
        "        return Node(base, block.__size, block.__version, child_offset, FF_ARRAY::recovery, FF_FIELD_ARRAY,\n"
        "                    field->child_recovery, field->array_entries_are_offsets != 0);\n"
        "    }\n"
        "    if (field->kind == FF_FIELD_BLOCK) {\n"
        "        const Offset child_offset = LOAD_U64(base + value_offset);\n"
        "        if (child_offset == FF_NULL_OFFSET) return {};\n"
        "        return Node(base, block.__size, block.__version, child_offset, field->child_recovery, FF_FIELD_BLOCK);\n"
        "    }\n"
        "    return Node::scalar(base, block.__size, block.__version, block.__offset, value_offset, field->kind);\n"
        "}\n\n"
        "template <typename T_Block>\n"
        "std::vector<std::string_view> keys_for_block() {\n"
        "    std::vector<std::string_view> keys;\n"
        "    keys.reserve(T_Block::FIELD_COUNT);\n"
        "    for (size_t i = 0; i < T_Block::FIELD_COUNT; ++i) {\n"
        "        keys.emplace_back(T_Block::FIELDS[i].name);\n"
        "    }\n"
        "    return keys;\n"
        "}\n\n"
        "std::vector<FF_FieldInfo> reflected_fields(uint16_t recovery) {\n"
        "    switch (recovery) {\n"
    )
    for s_name in block_struct_names:
        cpp += f"        case {s_name}::recovery: return fields_for_block<{s_name}>();\n"
    cpp += (
        "        default: return {};\n"
        "    }\n"
        "}\n\n"
        "std::vector<std::string_view> reflected_keys(uint16_t recovery) {\n"
        "    switch (recovery) {\n"
    )
    for s_name in block_struct_names:
        cpp += f"        case {s_name}::recovery: return keys_for_block<{s_name}>();\n"
    cpp += (
        "        default: return {};\n"
        "    }\n"
        "}\n\n"
        "Node reflected_child_node(const BYTE* base, Size size, uint32_t version, Offset offset, uint16_t recovery, std::string_view key) {\n"
        "    switch (recovery) {\n"
    )
    for s_name in block_struct_names:
        cpp += f"        case {s_name}::recovery: return object_field_node({s_name}(offset, size, version), base, key);\n"
    cpp += (
        "        default: return {};\n"
        "    }\n"
        "}\n\n"
    )
    cpp += (
        "std::string_view reflected_resource_type(uint16_t recovery) {\n"
        "    switch (recovery) {\n"
    )
    for res in resources:
        cpp += f"        case FF_{res.upper()}::recovery: return \"{res}\";\n"
    cpp += (
        "        default: return \"\";\n"
        "    }\n"
        "}\n\n"
    )
    cpp += "} // namespace FastFHIR\n"

    return hpp, cpp

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
            # Use blk_ for the array view
            cpp += f"{indent}auto blk_{f['cpp_name']}_arr = get_{f['cpp_name']}(__base);\n"
            cpp += f"{indent}if (blk_{f['cpp_name']}_arr) {{\n"
            cpp += f"{indent}    auto STEP = blk_{f['cpp_name']}_arr.entry_step(__base);\n"
            cpp += f"{indent}    auto ENTRIES = blk_{f['cpp_name']}_arr.entry_count(__base);\n"
            cpp += f"{indent}    auto blk_item_ptr = blk_{f['cpp_name']}_arr.entries(__base);\n"
            cpp += f"{indent}    for (uint32_t i = 0; i < ENTRIES; ++i, blk_item_ptr += STEP) {{\n"
            
            if f['fhir_type'] in ('string', 'code'):
                code_enum = f.get('code_enum')
                cpp += f"{indent}        Offset blk_str_off = LOAD_U64(blk_item_ptr);\n"
                cpp += f"{indent}        if (blk_str_off != FF_NULL_OFFSET) {{\n"
                cpp += f"{indent}            FF_STRING blk_str(blk_str_off, __size, __version);\n"
                if code_enum:
                    cpp += f"{indent}            data.{f['cpp_name']}.push_back({code_enum['parse']}(blk_str.read(__base)));\n"
                else:
                    cpp += f"{indent}            data.{f['cpp_name']}.push_back(blk_str.read_view(__base));\n"
                cpp += f"{indent}        }}\n"
            else:
                type_call = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
                cpp += f"{indent}        {type_call} blk_nested(static_cast<Offset>(blk_item_ptr - __base), __size, __version);\n"
                cpp += f"{indent}        data.{f['cpp_name']}.push_back(blk_nested.read(__base));\n"
            cpp += f"{indent}    }}\n{indent}}}\n"
            
        elif f['fhir_type'] == 'string':
            cpp += f"{indent}auto blk_{f['cpp_name']} = get_{f['cpp_name']}(__base);\n"
            cpp += f"{indent}if (blk_{f['cpp_name']}) data.{f['cpp_name']} = blk_{f['cpp_name']}.read_view(__base);\n"
            
        elif f['cpp_type'] == 'Offset':
            cpp += f"{indent}auto blk_{f['cpp_name']} = get_{f['cpp_name']}(__base);\n"
            data_type = _resolve_data_type_name(f['fhir_type'], f['orig_name'], block_struct_name, f.get('resolved_path'))
            cpp += f"{indent}if (blk_{f['cpp_name']}) data.{f['cpp_name']} = std::make_unique<{data_type}>(blk_{f['cpp_name']}.read(__base));\n"
            
        elif f['fhir_type'] == 'code':
            code_enum = f.get('code_enum')
            cpp += f"{indent}{{\n"
            cpp += f"{indent}    uint32_t raw_code = LOAD_U32(__base + __offset + {block_struct_name}::{f['name']});\n"
            cpp += f"{indent}    if (raw_code == FF_CODE_NULL) {{\n"
            cpp += f"{indent}        // Keep default empty state\n"
            cpp += f"{indent}    }} else if (const char* resolved = FF_ResolveCode(raw_code, __version)) {{\n"
            if code_enum:
                cpp += f"{indent}        data.{f['cpp_name']} = {code_enum['parse']}(std::string(resolved));\n"
            else:
                cpp += f"{indent}        data.{f['cpp_name']} = resolved;\n"
            
            # Safely extract and read the custom string block
            cpp += f"{indent}    }} else if (raw_code & FF_CUSTOM_STRING_FLAG) {{\n"
            cpp += f"{indent}        Offset relative_off = __offset + (raw_code & ~FF_CUSTOM_STRING_FLAG);\n"
            cpp += f"{indent}        std::string_view custom_str = FF_STRING(relative_off, __size, __version).read_view(__base);\n"
            if code_enum:
                cpp += f"{indent}        data.{f['cpp_name']} = {code_enum['parse']}(std::string(custom_str));\n"
            else:
                cpp += f"{indent}        data.{f['cpp_name']} = custom_str;\n"
            cpp += f"{indent}    }}\n"
            cpp += f"{indent}}}\n"
        elif f['fhir_type'] in TYPE_MAP and f['fhir_type'] not in ('string', 'code', 'DEFAULT'):
            cpp += f"{indent}data.{f['cpp_name']} = {f['macro']}(__base + __offset + {block_struct_name}::{f['name']});\n"
            
        if f['first_version_idx'] > 0: cpp += f"    }}\n"
    return cpp

def generate_size_fields(layout, block_struct_name, data_name):
    cpp = f"    Size __total = {block_struct_name}::HEADER_SIZE;\n"
    for f in layout:
        if f['is_array']:
            # 1. Handle native string arrays
            if f['fhir_type'] == 'string':
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * TYPE_SIZE_OFFSET);\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                cpp += f"            __total += SIZE_FF_STRING(__item);\n"
                cpp += f"        }}\n    }}\n"
            
            # 2. Handle code arrays (using the new C++ wrapper)
            elif f['fhir_type'] == 'code':
                code_enum = f.get('code_enum')
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * TYPE_SIZE_OFFSET);\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                if code_enum: 
                    cpp += f"            __total += SIZE_FF_CODE(std::string({code_enum['serialize']}(__item)), __version);\n"
                else:         
                    cpp += f"            __total += SIZE_FF_CODE(__item, __version);\n"
                cpp += f"        }}\n    }}\n"
            
            # 3. Handle nested objects/resources arrays
            else:
                child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE;\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                if child_struct == 'FF_RESOURCE':
                    cpp += f"            __total += FF_RESOURCE::HEADER_SIZE + SIZE_FF_STRING(__item.json);\n"
                else:
                    cpp += f"            __total += SIZE_{child_struct}(__item);\n"
                cpp += f"        }}\n    }}\n"
                cpp += f"    else if (!{data_name}.{f['cpp_name']}_handles.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}_handles.size() * TYPE_SIZE_OFFSET);\n"
                cpp += f"    }}\n"
                
        # 4. Handle scalar strings
        elif f['fhir_type'] == 'string':
            cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
            cpp += f"        __total += SIZE_FF_STRING({data_name}.{f['cpp_name']});\n    }}\n"
            
        # 5. Handle single nested objects / pointers
        elif f['cpp_type'] == 'Offset':
            child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
            cpp += f"    if ({data_name}.{f['cpp_name']} != nullptr) {{\n"
            if child_struct == 'FF_RESOURCE':
                cpp += f"        __total += FF_RESOURCE::HEADER_SIZE + SIZE_FF_STRING({data_name}.{f['cpp_name']}->json);\n    }}\n"
            else:
                cpp += f"        __total += SIZE_{child_struct}(*{data_name}.{f['cpp_name']});\n    }}\n"
                
        # 6. Handle scalar codes (using the new C++ wrapper)
        elif f['fhir_type'] == 'code':
            code_enum = f.get('code_enum')
            if code_enum: 
                cpp += f"    __total += SIZE_FF_CODE(std::string({code_enum['serialize']}({data_name}.{f['cpp_name']})), __version);\n"
            else:         
                cpp += f"    __total += SIZE_FF_CODE({data_name}.{f['cpp_name']}, __version);\n"
            
    cpp += "    return __total;\n"
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
                cpp += f"        auto blk_n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::OFFSET, TYPE_SIZE_OFFSET, blk_n);\n"
                cpp += f"        Offset blk_off_tbl = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(blk_n) * TYPE_SIZE_OFFSET;\n"
                cpp += f"        for (uint32_t blk_i = 0; blk_i < blk_n; ++blk_i) {{\n"
                cpp += f"            STORE_U64(__base + blk_off_tbl + blk_i * TYPE_SIZE_OFFSET, child_off);\n"
                if code_enum: cpp += f"            child_off += STORE_FF_STRING(__base, child_off, std::string({code_enum['serialize']}({data_name}.{f['cpp_name']}[blk_i])));\n"
                else:         cpp += f"            child_off += STORE_FF_STRING(__base, child_off, {data_name}.{f['cpp_name']}[blk_i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
            else:
                child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
                store_fn = f"STORE_{child_struct}"
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::INLINE_BLOCK, {child_struct}::HEADER_SIZE, __n);\n"
                cpp += f"        Offset __entries_start = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * {child_struct}::HEADER_SIZE;\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                if child_struct == 'FF_RESOURCE':
                    cpp += f"            {store_fn}(__base, __entries_start + __i * {child_struct}::HEADER_SIZE, child_off, {data_name}.{f['cpp_name']}[__i]);\n"
                else:
                    cpp += f"            child_off = {store_fn}(__base, __entries_start + __i * {child_struct}::HEADER_SIZE, child_off, {data_name}.{f['cpp_name']}[__i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} "
                cpp += f"    else if (!{data_name}.{f['cpp_name']}_handles.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}_handles.size());\n"
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::OFFSET, TYPE_SIZE_OFFSET, __n);\n"
                cpp += f"        Offset __entries_start = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * TYPE_SIZE_OFFSET;\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                cpp += f"            STORE_U64(__base + __entries_start + __i * TYPE_SIZE_OFFSET, {data_name}.{f['cpp_name']}_handles[__i].offset());\n"
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
            val_str = f"std::string({code_enum['serialize']}({data_name}.{f['cpp_name']}))" if code_enum else f"{data_name}.{f['cpp_name']}"
            cpp += f"    {{\n"
            cpp += f"        std::string __code_str = {val_str};\n"
            cpp += f"        STORE_U32({ptr_name} + {block_struct_name}::{f['name']}, ENCODE_FF_CODE(__base, hdr_off, child_off, __code_str, __version));\n"
            cpp += f"    }}\n"
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
                off = 10 if not blk['layout'] else blk['layout'][-1]['offset'] + blk['layout'][-1]['size']
                # C++ Keyword Sanitization
                cpp_safe_name = field_name.lower()
                if cpp_safe_name in ["class", "template", "namespace", "operator", "new", "delete", "default", "struct", "enum", "concept", "requires", "export", "import", "module"]:
                    cpp_safe_name += "_"     
                blk['layout'].append({
                    'name': field_name.upper(), 'cpp_name': cpp_safe_name, 'orig_name': field_name,
                    'is_array': is_array, 'fhir_type': f_type, 'size': mapping['size'],
                    'size_const': mapping['size_const'], 'cpp_type': mapping['cpp'],
                    'data_type': mapping['data_type'], 'macro': mapping['macro'],
                    'first_version_name': v_name, 'first_version_idx': v_idx, 'offset': off
                })
                blk['seen'].add(field_name)
            blk['sizes'][v_name] = blk['layout'][-1]['offset'] + blk['layout'][-1]['size']
            
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
    block_struct_names = sorted({"FF_" + path.replace('.', '_').upper() for path in master_blocks})
    for d_name in block_data_names: hpp += f"struct {d_name};\n"
    for s_name in block_struct_names: hpp += f"struct {s_name};\n"
    if block_data_names or block_struct_names: hpp += "\n"

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
                if f['fhir_type'] not in ('string', 'code'):
                    hpp += f"    std::vector<FastFHIR::ObjectHandle> {f['cpp_name']}_handles;\n"
            elif f['fhir_type'] == 'string': hpp += f"    std::string_view {f['cpp_name']};\n"
            elif code_enum: hpp += f"    {code_enum['enum']} {f['cpp_name']} = {code_enum['enum']}::Unknown;\n"
            elif f['cpp_type'] == 'Offset': hpp += f"    std::unique_ptr<{_resolve_data_type_name(f['fhir_type'], f['orig_name'], path, f.get('resolved_path'))}> {f['cpp_name']};\n"
            elif f['data_type'] == 'bool': hpp += f"    bool {f['cpp_name']} = false;\n"
            elif f['data_type'] == 'std::string': hpp += f"    std::string {f['cpp_name']};\n"
            elif f['fhir_type'] in TYPE_MAP and 'null' in TYPE_MAP[f['fhir_type']]:
                null_val = TYPE_MAP[f['fhir_type']]['null']
                hpp += f"    {f['data_type']} {f['cpp_name']} = {null_val};\n"
            else: hpp += f"    {f['data_type']} {f['cpp_name']}{{}};\n"
        hpp += f"}};\n\n"

        # Data Block
        hpp += f"struct FF_EXPORT {s_name} : DATA_BLOCK {{\n"
        hpp += f"    static constexpr char type [] = \"{s_name}\";\n"
        hpp += f"    static constexpr enum RECOVERY_TAG recovery = RECOVER_{s_name};\n"
        hpp += f"    enum vtable_sizes {{\n        VALIDATION_S = TYPE_SIZE_UINT64,\n        RECOVERY_S = TYPE_SIZE_UINT16,\n"
        for f in layout: hpp += f"        {f['name']}_S = {f['size_const']},\n"
        hpp += f"    }};\n    enum vtable_offsets {{\n        VALIDATION = 0,\n        RECOVERY = VALIDATION + VALIDATION_S,\n"
        prev_name, prev_size = "RECOVERY", "RECOVERY_S"
        for f in layout:
            hpp += f"        {f['name']:<20}= {prev_name} + {prev_size},\n"
            prev_name, prev_size = f['name'], f"{f['name']}_S"
        for v, sz in sizes.items(): hpp += f"        HEADER_{v}_SIZE = {sz},\n"
        # Safely determine the maximum version that actually exists for this specific block
        present_versions = [v for v in versions if v in sizes]
        max_v = max(present_versions, key=lambda x: versions.index(x))
        hpp += f"        HEADER_SIZE = HEADER_{max_v}_SIZE\n    }};\n"
        
        min_version = next((v for v in versions if v in sizes), None)
        hpp += f"    inline Size get_header_size() const {{\n"
        if min_version: hpp += f"        if (__version < FHIR_VERSION_{min_version}) return 0;\n"
        for v in versions:
            if v in sizes: hpp += f"        if (__version <= FHIR_VERSION_{v}) return HEADER_{v}_SIZE;\n"
        hpp += f"        return HEADER_SIZE;\n    }}\n"

        hpp += f"    explicit {s_name}(Offset off, Size total_size, uint32_t ver) : DATA_BLOCK(off, total_size, ver) {{}}\n"
        hpp += f"    static constexpr size_t FIELD_COUNT = {len(layout)};\n"
        hpp += f"    static const FF_FieldInfo FIELDS[FIELD_COUNT];\n"
        hpp += f"    FF_Result validate_full(const BYTE* const __base) const noexcept;\n"
        hpp += f"    {d_name} read(const BYTE* const __base) const;\n\n"
        hpp += f"    const FF_FieldInfo* find_field(std::string_view name) const;\n"
        hpp += generate_getter_declarations(layout, s_name)
        hpp += f"}};\n\n"
        
        # Lock-Free Store Signatures
        hpp += f"Size SIZE_{s_name}(const {d_name}& data, uint32_t __version = FHIR_VERSION_R5);\n"
        hpp += f"Offset STORE_{s_name}(BYTE* const __base, Offset hdr_off, Offset child_off, const {d_name}& data, uint32_t __version = FHIR_VERSION_R5);\n"
        hpp += f"inline Offset STORE_{s_name}(BYTE* const __base, Offset start_off, const {d_name}& data, uint32_t __version = FHIR_VERSION_R5) {{\n"
        hpp += f"    return STORE_{s_name}(__base, start_off, start_off + {s_name}::HEADER_SIZE, data, __version);\n"
        hpp += f"}}\n\n"

        # Type Traits Specialization
        hpp += f"namespace FastFHIR {{\n"
        hpp += f"template<> struct TypeTraits<{d_name}> {{\n"
        hpp += f"    static constexpr auto recovery = {s_name}::recovery;\n"
        hpp += f"    static Size size(const {d_name}& d, uint32_t v = FHIR_VERSION_R5) {{ return SIZE_{s_name}(d, v); }}\n"
        hpp += f"    static void store(BYTE* const base, Offset off, const {d_name}& d, uint32_t v = FHIR_VERSION_R5) {{ STORE_{s_name}(base, off, d, v); }}\n"
        hpp += f"    static {d_name} read(const BYTE* const base, Offset off, Size size, uint32_t v) {{ return {s_name}(off, size, v).read(base); }}\n"
        hpp += f"}};\n"
        hpp += f"}} // namespace FastFHIR\n\n"

        # CPP Implementations
        cpp += f"FF_Result {s_name}::validate_full(const BYTE *const __base) const noexcept {{ return validate_offset(__base, type, recovery); }}\n"
        cpp += generate_field_info_implementation(layout, s_name)
        cpp += f"{d_name} {s_name}::read(const BYTE *const __base) const {{\n"
        cpp += f"#ifdef __EMSCRIPTEN__\n    const_cast<{s_name}&>(*this).check_and_fetch_remote(__base);\n#endif\n"
        cpp += f"    {d_name} data;\n{generate_read_fields(layout, s_name)}    return data;\n}}\n"
        
        cpp += f"Size SIZE_{s_name}(const {d_name}& data, uint32_t __version) {{\n{generate_size_fields(layout, s_name, 'data')}}}\n\n"
        
        cpp += f"Offset STORE_{s_name}(BYTE* const __base, Offset hdr_off, Offset child_off, const {d_name}& data, uint32_t __version) {{\n"
        cpp += f"    auto __ptr = __base + hdr_off;\n"
        cpp += f"    STORE_U64(__ptr + {s_name}::VALIDATION, hdr_off);\n"
        cpp += f"    STORE_U16(__ptr + {s_name}::RECOVERY, {s_name}::recovery);\n"
        cpp += f"{generate_store_fields(layout, s_name, '__ptr', 'data')}\n"
        cpp += f"    return child_off;\n}}\n\n"
        cpp += generate_getter_implementations(layout, s_name)
        cpp += "\n"
    return hpp, cpp

# =====================================================================
# 5. RECOVERY TAG GENERATOR
# =====================================================================
def generate_recovery_header(target_types, resources, all_block_paths, output_dir="generated_src"):
    """
    Auto-generate FF_Recovery.hpp containing the RECOVERY enum from
    the discovered data types, resources, and backbone elements.
    """
    auto_header = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
    )

    # Categorize block paths into data types, resources, and backbone elements
    type_set = set(target_types)
    resource_set = set(resources)
    data_type_paths = []
    resource_paths = []
    backbone_paths = []

    for path in all_block_paths:
        root = path.split('.')[0]
        is_nested = '.' in path
        if not is_nested and root in type_set:
            data_type_paths.append(path)
        elif not is_nested and root in resource_set:
            resource_paths.append(path)
        else:
            backbone_paths.append(path)

    # Preserve TARGET_TYPES / RESOURCES ordering for stable ABI
    dt_order = {t: i for i, t in enumerate(target_types)}
    res_order = {r: i for i, r in enumerate(resources)}
    data_type_paths.sort(key=lambda p: dt_order.get(p, len(target_types)))
    resource_paths.sort(key=lambda p: res_order.get(p, len(resources)))
    backbone_paths.sort()

    lines = [auto_header]
    lines.append("#pragma once\n")
    lines.append("#include <cstdint>\n")
    lines.append("// =====================================================================")
    lines.append("// RECOVERY TAG REGISTRY (auto-generated from FHIR StructureDefinitions)")
    lines.append("// =====================================================================")
    lines.append("enum RECOVERY_TAG : uint16_t {")
    lines.append("    // Undefined / Sentinel")
    lines.append("    FF_RECOVER_UNDEFINED                  = 0x0000,")
    lines.append("")
    lines.append("    // Core Primitives (0x0001 range)")
    lines.append("    RECOVER_FF_HEADER                     = 0x0001,")
    lines.append("    RECOVER_FF_STRING                     = 0x0002,")
    lines.append("    RECOVER_FF_ARRAY                      = 0x0003,")
    lines.append("    RECOVER_FF_RESOURCE                   = 0x0004,")
    lines.append("    RECOVER_FF_CHECKSUM                   = 0x0005,")

    if data_type_paths:
        lines.append("")
        lines.append("    // Data Types (0x0100 range)")
        for i, path in enumerate(data_type_paths):
            tag_name = f"RECOVER_FF_{path.replace('.', '_').upper()}"
            lines.append(f"    {tag_name:<44}= 0x{0x0100 + i:04X},")

    if resource_paths:
        lines.append("")
        lines.append("    // Resources (0x0200 range)")
        for i, path in enumerate(resource_paths):
            tag_name = f"RECOVER_FF_{path.replace('.', '_').upper()}"
            lines.append(f"    {tag_name:<44}= 0x{0x0200 + i:04X},")

    if backbone_paths:
        lines.append("")
        lines.append("    // Sub-elements / BackboneElements (0x0300 range)")
        for i, path in enumerate(backbone_paths):
            tag_name = f"RECOVER_FF_{path.replace('.', '_').upper()}"
            lines.append(f"    {tag_name:<44}= 0x{0x0300 + i:04X},")

    lines.append("};")
    lines.append("")

    out_path = os.path.join(output_dir, "FF_Recovery.hpp")
    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    print(f"Generated {out_path}")

# =====================================================================
# 6. INGESTOR MAPPINGS GENERATOR (simdjson bridge)
# =====================================================================
def generate_ingest_mappings(master_blocks, resources, output_dir="generated_src"):
    """
    Generates the standalone FF_IngestMappings.hpp and .cpp files bridging
    simdjson text to FastFHIR POD structs, complete with location-aware error logging.
    """
    import os
    
    auto_header = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
    )

    hpp = f"{auto_header}#pragma once\n#include \"FF_AllTypes.hpp\"\n"
    hpp += "#include \"FF_Logger.hpp\"\n"
    hpp += "#include <simdjson.h>\n\nnamespace FastFHIR::Ingest {\n\n"

    cpp = f"{auto_header}#include \"FF_IngestMappings.hpp\"\n\n"
    cpp += "namespace FastFHIR::Ingest {\n\n"

    # =====================================================================
    # 1. FORWARD DECLARATIONS (CPP Only for Strict Encapsulation)
    # =====================================================================
    cpp += "// ============================================================\n"
    cpp += "// Internal Parser Forward Declarations\n"
    cpp += "// ============================================================\n"
    for path, blk in master_blocks.items():
        # Exception: Bundle must be public for the Ingestor's concurrent queue slicing
        if path == 'Bundle':
            continue

        data_type = path.replace('.', '') + 'Data'
        fn_name = path.replace('.', '_') + "_from_json"
        cpp += f"static {data_type} {fn_name}(simdjson::ondemand::object obj, FastFHIR::ConcurrentLogger* logger = nullptr, std::vector<std::string_view>* concurrent_queue = nullptr);\n"
    cpp += "\n"

    # =====================================================================
    # 2. PARSER IMPLEMENTATIONS
    # =====================================================================
    for path, blk in master_blocks.items():
        data_type = path.replace('.', '') + 'Data'
        fn_name = path.replace('.', '_') + "_from_json"
        
        # Exception: Expose Bundle_from_json in the Header
        if path == 'Bundle':
            hpp += f"    // Exposed explicitly for Top-Down concurrent ingestion queueing\n"
            hpp += f"    {data_type} {fn_name}(simdjson::ondemand::object obj, FastFHIR::ConcurrentLogger* logger = nullptr, std::vector<std::string_view>* concurrent_queue = nullptr);\n\n"
            
            cpp += f"{data_type} {fn_name}(simdjson::ondemand::object obj, FastFHIR::ConcurrentLogger* logger, std::vector<std::string_view>* concurrent_queue) {{\n"
        else:
            # Standard Encapsulation: Keep everything else static
            cpp += f"static {data_type} {fn_name}(simdjson::ondemand::object obj, FastFHIR::ConcurrentLogger* logger, std::vector<std::string_view>* concurrent_queue) {{\n"

        cpp += f"    obj.reset(); // Reset simdjson reel else will error.\n"
        cpp += f"    {data_type} data;\n"
        cpp += f"    for (auto field : obj) {{\n"
        cpp += f"        std::string_view key = field.unescaped_key().value_unsafe();\n"

        is_first = True
        for f in blk['layout']:
            json_key = f['orig_name']
            cpp_name = f['cpp_name']
            fhir_location = f"{path}.{json_key}"
            
            err_log = f'if (logger) logger->log("[Warning] FastFHIR Ingestion: Malformed data at {fhir_location}");'

            if is_first:
                cpp += f'        if (key == "{json_key}") {{\n'
                is_first = False
            else:
                cpp += f'        else if (key == "{json_key}") {{\n'

            # --- Array Logic ---
            if f['is_array']:
                cpp += f'            simdjson::ondemand::array arr;\n'
                cpp += f'            if (field.value().get_array().get(arr) == simdjson::SUCCESS) {{\n'
                if path == 'Bundle' and f['orig_name'] == 'entry':
                    cpp += f'                if (concurrent_queue) {{\n'
                    cpp += f'                    for (auto item : arr) {{\n'
                    cpp += f'                        std::string_view full_entry;\n'
                    cpp += f'                        if (item.raw_json().get(full_entry) == simdjson::SUCCESS) {{\n'
                    cpp += f'                            concurrent_queue->push_back(full_entry);\n'
                    cpp += f'                        }}\n'
                    cpp += f'                    }}\n'
                    cpp += f'                }} else {{\n'
                cpp += f'                for (auto item : arr) {{\n'
                if f['fhir_type'] in ('string', 'code'):
                    code_enum = f.get('code_enum')
                    cpp += f'                    std::string_view val;\n'
                    cpp += f'                    if (item.get_string().get(val) == simdjson::SUCCESS) {{\n'
                    if code_enum:
                        cpp += f'                        data.{cpp_name}.push_back({code_enum["parse"]}(std::string(val)));\n'
                    else:
                        cpp += f'                        data.{cpp_name}.push_back(val);\n'
                    cpp += f'                    }} else {{ {err_log} }}\n'
                elif f['fhir_type'] == 'Resource':
                    cpp += f'                    // [Polymorphic Resource array {cpp_name} skipped]\n'
                else:
                    if f['fhir_type'] in ('BackboneElement', 'Element'):
                        child_fn = f.get('resolved_path', f"{path}.{json_key}").replace('.', '_') + "_from_json"
                    else:
                        child_fn = f"{f['fhir_type']}_from_json"

                    cpp += f'                    simdjson::ondemand::object obj_val;\n'
                    cpp += f'                    if (item.get_object().get(obj_val) == simdjson::SUCCESS) {{\n'
                    cpp += f'                        data.{cpp_name}.push_back({child_fn}(obj_val, logger));\n'
                    cpp += f'                    }} else {{ {err_log} }}\n'
                cpp += f'                }}\n'
                if path == 'Bundle' and f['orig_name'] == 'entry':
                    cpp += f'                }}\n'
                cpp += f'            }} else {{ {err_log} }}\n'
                cpp += f'        }}\n'
                continue

            # --- String Logic ---
            if f['fhir_type'] == 'string':
                cpp += f'            std::string_view val;\n'
                cpp += f'            if (field.value().get_string().get(val) == simdjson::SUCCESS) {{\n'
                cpp += f'                data.{cpp_name} = val;\n'
                cpp += f'            }} else {{ {err_log} }}\n'
            
            # --- Nested Object Logic ---
            elif f['cpp_type'] == 'Offset':
                if f['fhir_type'] == 'Resource':
                    cpp += f'            // [Polymorphic Resource \'{cpp_name}\' skipped. Handled via ObjectHandle bypass]\n'
                else:
                    child_data_type = _resolve_data_type_name(f['fhir_type'], f['orig_name'], path, f.get('resolved_path'))
                    if f['fhir_type'] in ('BackboneElement', 'Element'):
                        child_fn = f.get('resolved_path', f"{path}.{json_key}").replace('.', '_') + "_from_json"
                    else:
                        child_fn = f"{f['fhir_type']}_from_json"

                    # Generate nested object parsing logic with unique_ptr assignment
                    cpp += f'            simdjson::ondemand::object obj_val;\n'
                    cpp += f'            if (field.value().get_object().get(obj_val) == simdjson::SUCCESS) {{\n'
                    cpp += f'                data.{cpp_name} = std::make_unique<{child_data_type}>({child_fn}(obj_val, logger));\n'
                    cpp += f'            }} else {{ {err_log} }}\n'
            
            # --- Enum / Code Logic ---
            elif f['fhir_type'] == 'code':
                code_enum = f.get('code_enum')
                cpp += f'            std::string_view val;\n'
                cpp += f'            if (field.value().get_string().get(val) == simdjson::SUCCESS) {{\n'
                if code_enum:
                    cpp += f'                data.{cpp_name} = {code_enum["parse"]}(std::string(val));\n'
                else:
                    cpp += f'                data.{cpp_name} = val;\n'
                cpp += f'            }} else {{ {err_log} }}\n'
            
            # --- Primitives ---
            elif f['fhir_type'] == 'boolean':
                cpp += f'            bool val;\n'
                cpp += f'            if (field.value().get_bool().get(val) == simdjson::SUCCESS) {{\n'
                cpp += f'                data.{cpp_name} = val ? 1 : 0;\n'
                cpp += f'            }} else {{ {err_log} }}\n'
            elif f['data_type'] == 'double':
                cpp += f'            double val;\n'
                cpp += f'            if (field.value().get_double().get(val) == simdjson::SUCCESS) {{\n'
                cpp += f'                data.{cpp_name} = val;\n'
                cpp += f'            }} else {{ {err_log} }}\n'
            else: # uint32_t / integers
                cpp += f'            uint64_t val;\n'
                cpp += f'            if (field.value().get_uint64().get(val) == simdjson::SUCCESS) {{\n'
                cpp += f'                data.{cpp_name} = static_cast<{f["cpp_type"]}>(val);\n'
                cpp += f'            }} else {{ {err_log} }}\n'

            cpp += f'        }}\n'
            
        cpp += f"    }}\n    return data;\n}}\n\n"

        # =====================================================================
        # 3. CONCURRENT PATCHING GENERATOR (Zero-Copy Inline Array Support)
        # =====================================================================
        patch_fn_name = f"patch_{path.replace('.', '_')}_from_json"
        owner_struct = f"FF_{path.replace('.', '_').upper()}"
        owner_ns = _block_key_namespace(path)

        # Declaration (REMAINS IN HPP as it is used by the Ingestor worker threads)
        hpp += f"void {patch_fn_name}(simdjson::ondemand::object& obj, FastFHIR::MutableEntry& wrapper, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger = nullptr);\n"

        # Implementation
        cpp += f"void {patch_fn_name}(simdjson::ondemand::object& obj, FastFHIR::MutableEntry& wrapper, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger) {{\n"
        cpp += f"    for (auto field : obj) {{\n"
        cpp += f"        std::string_view key = field.unescaped_key().value_unsafe();\n"

        is_first_patch = True
        for f in blk['layout']:
            json_key = f['orig_name']
            cpp_name = f['cpp_name']
            field_key_enum = f"FastFHIR::Fields::{owner_ns}::{_field_key_short_name(json_key)}"
            
            err_log = f'if (logger) logger->log("[Warning] FastFHIR Patching: Malformed data at {path}.{json_key}");'

            if is_first_patch:
                cpp += f'        if (key == "{json_key}") {{\n'
                is_first_patch = False
            else:
                cpp += f'        else if (key == "{json_key}") {{\n'

            # --- String Logic ---
            if f['fhir_type'] == 'string':
                cpp += f'            std::string_view val;\n'
                cpp += f'            if (field.value().get_string().get(val) == simdjson::SUCCESS) {{\n'
                cpp += f'                // Naturally resolved via TypeTraits<std::string_view>!\n'
                cpp += f'                wrapper[{field_key_enum}] = builder.append_obj(val);\n'
                cpp += f'            }} else {{ {err_log} }}\n'

            # --- Nested Object Logic (e.g., request, response, resource) ---
            elif f['cpp_type'] == 'Offset' and not f['is_array']:
                if f['fhir_type'] == 'Resource':
                    cpp += f'            simdjson::ondemand::object res_obj;\n'
                    cpp += f'            if (field.value().get_object().get(res_obj) == simdjson::SUCCESS) {{\n'
                    cpp += f'                std::string_view child_type;\n'
                    cpp += f'                if (res_obj["resourceType"].get_string().get(child_type) == simdjson::SUCCESS) {{\n'
                    cpp += f'                    FastFHIR::ObjectHandle child = dispatch_resource(child_type, res_obj, builder, logger);\n'
                    cpp += f'                    if (child.offset() != FF_NULL_OFFSET) {{\n'
                    cpp += f'                        wrapper[{field_key_enum}] = child;\n'
                    cpp += f'                    }}\n'
                    cpp += f'                }}\n'
                    cpp += f'            }} else {{ {err_log} }}\n'
                else:
                    # Leverage the existing builder function to create the sub-block
                    if f['fhir_type'] in ('BackboneElement', 'Element'):
                        child_fn = f.get('resolved_path', f"{path}.{json_key}").replace('.', '_') + "_from_json"
                    else:
                        child_fn = f"{f['fhir_type']}_from_json"

                    cpp += f'            simdjson::ondemand::object obj_val;\n'
                    cpp += f'            if (field.value().get_object().get(obj_val) == simdjson::SUCCESS) {{\n'
                    cpp += f'                auto child_data = {child_fn}(obj_val, logger);\n'
                    cpp += f'                FastFHIR::ObjectHandle child_handle = builder.append_obj(child_data);\n'
                    cpp += f'                wrapper[{field_key_enum}] = child_handle;\n'
                    cpp += f'            }} else {{ {err_log} }}\n'

            # --- Array Logic ---
            elif f['is_array']:
                cpp += f'            simdjson::ondemand::array arr;\n'
                cpp += f'            if (field.value().get_array().get(arr) == simdjson::SUCCESS) {{\n'
                cpp += f'                std::vector<FastFHIR::ObjectHandle> handles;\n'
                
                if f['fhir_type'] in ('string', 'code'):
                    cpp += f'                for (auto item : arr) {{\n'
                    cpp += f'                    std::string_view val;\n'
                    cpp += f'                    if (item.get_string().get(val) == simdjson::SUCCESS) {{\n'
                    cpp += f'                        handles.push_back(builder.append_obj(val));\n'
                    cpp += f'                    }}\n'
                    cpp += f'                }}\n'
                elif f['fhir_type'] == 'Resource':
                    cpp += f'                for (auto item : arr) {{\n'
                    cpp += f'                    simdjson::ondemand::object res_obj;\n'
                    cpp += f'                    if (item.get_object().get(res_obj) == simdjson::SUCCESS) {{\n'
                    cpp += f'                        std::string_view child_type;\n'
                    cpp += f'                        if (res_obj["resourceType"].get_string().get(child_type) == simdjson::SUCCESS) {{\n'
                    cpp += f'                            FastFHIR::ObjectHandle child = dispatch_resource(child_type, res_obj, builder, logger);\n'
                    cpp += f'                            if (child.offset() != FF_NULL_OFFSET) handles.push_back(child);\n'
                    cpp += f'                        }}\n'
                    cpp += f'                    }}\n'
                    cpp += f'                }}\n'
                else:
                    if f['fhir_type'] in ('BackboneElement', 'Element'):
                        child_fn = f.get('resolved_path', f"{path}.{json_key}").replace('.', '_') + "_from_json"
                    else:
                        child_fn = f"{f['fhir_type']}_from_json"
                        
                    cpp += f'                for (auto item : arr) {{\n'
                    cpp += f'                    simdjson::ondemand::object obj_val;\n'
                    cpp += f'                    if (item.get_object().get(obj_val) == simdjson::SUCCESS) {{\n'
                    cpp += f'                        auto child_data = {child_fn}(obj_val, logger);\n'
                    cpp += f'                        handles.push_back(builder.append_obj(child_data));\n'
                    cpp += f'                    }}\n'
                    cpp += f'                }}\n'
                    
                cpp += f'                // Naturally resolved via TypeTraits<std::vector<ObjectHandle>>!\n'
                cpp += f'                wrapper[{field_key_enum}] = builder.append_obj(handles);\n'
                cpp += f'            }} else {{ {err_log} }}\n'

            # --- Array / Primitive bypass for wrappers ---
            else:
                cpp += f'            // [Arrays and inline primitives inside wrappers currently bypass concurrent patch generation]\n'

            cpp += f'        }}\n'
            
        cpp += f"    }}\n}}\n\n"

    # =====================================================================
    # 4. AUTO-GENERATED DISPATCHERS
    # =====================================================================

    hpp += "\n    // Auto-generated routing for top-level JSON Resources\n"
    hpp += "    FastFHIR::ObjectHandle dispatch_resource(std::string_view resource_type, simdjson::ondemand::object obj, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger = nullptr);\n"

    cpp += "\nFastFHIR::ObjectHandle dispatch_resource(std::string_view resource_type, simdjson::ondemand::object obj, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger) {\n"
    
    is_first_dispatch = True
    for res in resources:
        if is_first_dispatch:
            cpp += f'    if (resource_type == "{res}") {{\n'
            is_first_dispatch = False
        else:
            cpp += f'    else if (resource_type == "{res}") {{\n'
        cpp += f'        return builder.append_obj({res}_from_json(obj, logger));\n    }}\n'

    cpp += '    if (logger) logger->log("[Warning] FastFHIR Ingestion: Unknown root resource type encountered.");\n'
    cpp += '    return FastFHIR::ObjectHandle(&builder, FF_NULL_OFFSET);\n'
    cpp += f'}}\n\n'

    # --- Inner Block Dispatcher (For O(1) field insertion) ---
    hpp += "    // Auto-generated O(1) inner-block routing using Compile-Time Recovery Tags\n"
    hpp += "    FastFHIR::ObjectHandle dispatch_block(RECOVERY_TAG expected_tag, simdjson::ondemand::value& json_val, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger = nullptr);\n"

    cpp += "\nFastFHIR::ObjectHandle dispatch_block(RECOVERY_TAG expected_tag, simdjson::ondemand::value& json_val, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger) {\n"
    
    cpp += "    simdjson::ondemand::object obj;\n"
    cpp += "    if (json_val.get_object().get(obj) != simdjson::SUCCESS) {\n"
    cpp += '        if (logger) logger->log("[Error] dispatch_block: Expected JSON object for inner block parsing.");\n'
    cpp += "        return FastFHIR::ObjectHandle(&builder, FF_NULL_OFFSET);\n"
    cpp += "    }\n\n"
    
    cpp += "    switch (expected_tag) {\n"
    
    for path, blk in master_blocks.items():
        tag_name = "RECOVER_FF_" + path.replace('.', '_').upper()
        fn_name = path.replace('.', '_') + "_from_json"
        
        cpp += f"        case {tag_name}:\n"
        cpp += f"            return builder.append_obj({fn_name}(obj, logger));\n"
        
    cpp += "        default:\n"
    cpp += '            if (logger) logger->log("[Error] FastFHIR Ingestion: dispatch_block encountered an unknown or unsupported recovery tag.");\n'
    cpp += "            return FastFHIR::ObjectHandle(&builder, FF_NULL_OFFSET);\n"
    cpp += "    }\n"
    cpp += "}\n\n"

    # =====================================================================
    # End of file closures
    # =====================================================================

    hpp += "\n} // namespace FastFHIR::Ingest\n"
    cpp += "} // namespace FastFHIR::Ingest\n"

    out_hpp = os.path.join(output_dir, "FF_IngestMappings.hpp")
    out_cpp = os.path.join(output_dir, "FF_IngestMappings.cpp")
    
    with open(out_hpp, "w") as f: f.write(hpp)
    with open(out_cpp, "w") as f: f.write(cpp)
    print(f"Generated {out_hpp} and {out_cpp}")

# =====================================================================
# 5. PYTHON FIELD TOKEN EMITTER (Modular File-Per-Resource)
# =====================================================================

def emit_python_fields(python_resource_map, output_dir="generated_src"):
    """
    Generates a modular Python fields directory.
    Each resource/block gets its own file (e.g., patient.py, bundle_entry.py)
    to prevent massive RAM spikes and slow imports on AWS Lambda cold starts.
    """
    import os
    
    # Create the nested module directory
    target_dir = os.path.join(output_dir, "python", "fields")
    os.makedirs(target_dir, exist_ok=True)
    
    # 1. Generate an __init__.py so Python treats the folder as a package
    init_path = os.path.join(target_dir, "__init__.py")
    with open(init_path, "w") as f:
        f.write("# Auto-generated FastFHIR fields module.\n")
        f.write("# Import resources lazily to save memory.\n")
        
    # 2. Emit a separate file for every single FHIR resource/block
    for class_name, fields in python_resource_map.items():
        # Convert class names like BUNDLE_ENTRY to bundle_entry.py
        file_name = f"{class_name.lower()}.py"
        file_path = os.path.join(target_dir, file_name)
        
        with open(file_path, "w") as f:
            f.write("# Auto-generated by FastFHIR pipeline. Do not edit.\n")
            # Assumes the C++ extension (_core.so) sits one directory up
            f.write("from .. import _core\n\n")
            
            f.write(f"class {class_name}:\n")
            if not fields:
                f.write("    pass\n\n")
                continue
            
            for field_name, metadata in fields.items():
                idx, orig_name, owner = metadata
                safe_name = field_name.upper()
                
                # Python keyword safety
                if safe_name in ["CLASS", "IMPORT", "GLOBAL", "FOR", "WHILE", "IN", "IS", "AS"]:
                    safe_name += "_"
                
                # Instantiate the C++ PyField wrapper
                f.write(f"    {safe_name} = _core.Field({idx}, \"{orig_name}\", \"{owner}\")\n")
                
    print(f"-- Emitted {len(python_resource_map)} Python field modules into: {target_dir}/")

# =====================================================================
# 6. PYTHON DEFERRED PATH BUILDER (AST GENERATOR)
# =====================================================================
def emit_python_ast(master_blocks, block_key_defs, token_registry, output_dir="generated_src"):
    import os
    target_dir = os.path.join(output_dir, "python", "fields")
    os.makedirs(target_dir, exist_ok=True)
    
    # 1. Base AST Nodes: Split between Scalars/Structs and Arrays
    with open(os.path.join(target_dir, "base.py"), "w") as f:
        f.write("class ASTNode:\n")
        f.write("    def __init__(self, current_path=None):\n")
        f.write("        self.path = current_path or tuple()\n")
        f.write("    def cast(self, target_class):\n")
        f.write("        return target_class(self.path)\n\n")

        f.write("class ASTArrayNode(ASTNode):\n")
        f.write("    def __init__(self, current_path, item_class):\n")
        f.write("        super().__init__(current_path)\n")
        f.write("        self.item_class = item_class\n")
        f.write("    def __getitem__(self, index):\n")
        f.write("        if isinstance(index, int):\n")
        f.write("            return self.item_class(self.path + (index,))\n")
        f.write("        raise TypeError('FastFHIR: Only integer indexing is allowed for arrays.')\n")

    # 2. Emit the Strongly-Typed Path Builders
    for path, layout in block_key_defs:
        class_name = path.replace('.', '_').upper()
        file_name = f"{class_name.lower()}.py"
        
        with open(os.path.join(target_dir, file_name), "w") as f:
            f.write("from .. import _core\n")
            f.write("from .base import ASTNode\n\n")
            
            f.write(f"class {class_name}_PATH(ASTNode):\n")
            if not layout:
                f.write("    pass\n")
            
            for f_def in layout:
                orig_name = f_def['orig_name']
                token_id, ns_name = token_registry[path][orig_name]
                
                safe_name = orig_name.upper()
                if safe_name in ["CLASS", "IMPORT", "GLOBAL", "FOR", "WHILE", "IN", "IS", "AS"]:
                    safe_name += "_"
                
                f.write(f"    @property\n")
                f.write(f"    def {safe_name}(self):\n")
                f.write(f"        tok = _core.Field({token_id}, \"{orig_name}\", \"{ns_name}\")\n")
                
                # Check if this field points to another known FHIR block
                target_path = f_def.get('resolved_path', f"{path}.{orig_name}")
                is_array = f_def.get('is_array', False)
                
                target_class_name = "ASTNode"
                if target_path in master_blocks:
                    base_name = target_path.replace('.', '_')
                    target_class_name = f"{base_name.upper()}_PATH"
                    f.write(f"        from .{base_name.lower()} import {target_class_name}\n")
                
                # STUB: Enforce array geometry at compile time
                if is_array:
                    f.write(f"        from .base import ASTArrayNode\n")
                    f.write(f"        return ASTArrayNode(self.path + (tok,), {target_class_name})\n")
                else:
                    f.write(f"        return {target_class_name}(self.path + (tok,))\n")

    # 3. Auto-Generate __init__.py for Clean Namespace Imports
    init_path = os.path.join(target_dir, "__init__.py")
    with open(init_path, "w") as f:
        f.write('"""Auto-generated FastFHIR AST Namespace"""\n\n')
        f.write("from .base import ASTNode, ASTArrayNode\n\n")
        
        for block_name in master_blocks.keys():
            # Only alias top-level resources (ignore nested structs like Patient.contact)
            if '.' not in block_name:
                module_name = block_name.lower()
                class_name = f"{block_name.upper()}_PATH"
                
                # Aliases BUNDLE_PATH to Bundle, PATIENT_PATH to Patient
                f.write(f"from .{module_name} import {class_name} as {block_name}\n")

# =====================================================================
# 7. MASTER BUILD ORCHESTRATOR
# =====================================================================
def compile_fhir_library(resources, versions, input_dir="fhir_specs", output_dir="generated_src", code_enum_map=None):
    code_enums = code_enum_map or {}
    os.makedirs(output_dir, exist_ok=True)
    all_field_names = set()
    all_block_paths = set()
    reflected_block_names = set()
    block_key_defs = []
    
    print("Generating FF_DataTypes...")
    type_bundles = []
    for v in versions:
        p = os.path.join(input_dir, v, "profiles-types.json")
        if os.path.exists(p): 
            with open(p, 'r', encoding='utf-8') as f: type_bundles.append((v, json.load(f)))
    
    fwd_decls = set([t + "Data" for t in TARGET_TYPES])
    auto_header = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
    )
    hpp_head = f"{auto_header}// MARK: - Universal Data Types\n#pragma once\n#include \"../include/FF_Primitives.hpp\"\n#include \"../include/FF_Builder.hpp\"\n#include \"FF_CodeSystems.hpp\"\n#include <vector>\n#include <string_view>\n#include <memory>\n\n"
    hpp_head += "namespace FastFHIR { template<typename T> struct TypeTraits; \n\n"
    hpp_head += "template<> struct TypeTraits<std::string_view> {\n"
    hpp_head += "    static constexpr auto recovery = RECOVER_FF_STRING;\n"
    hpp_head += "    static Size size(std::string_view d, uint32_t = FHIR_VERSION_R5) { return SIZE_FF_STRING(d); }\n"
    hpp_head += "    static void store(BYTE* const base, Offset off, std::string_view d, uint32_t = FHIR_VERSION_R5) { STORE_FF_STRING(base, off, d); }\n"
    hpp_head += "};\n\n"
    hpp_head += "template<> struct TypeTraits<std::vector<ObjectHandle>> {\n"
    hpp_head += "    static constexpr auto recovery = RECOVER_FF_ARRAY;\n"
    hpp_head += "    static Size size(const std::vector<ObjectHandle>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * sizeof(Offset)); }\n"
    hpp_head += "    static void store(BYTE* const base, Offset off, const std::vector<ObjectHandle>& d, uint32_t = FHIR_VERSION_R5) {\n"
    hpp_head += "        std::vector<Offset>h(d.size()); int i = 0;\n"
    hpp_head += "        for (auto&&obj:d) { h[i++]=obj.offset(); }\n"
    hpp_head += "        STORE_FF_POINTER_ARRAY(base, off, h);\n"
    hpp_head += "    }\n"
    hpp_head += "};\n"
    hpp_head += "} // namespace FastFHIR\n\n"
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
            all_block_paths.update(blocks.keys())
            reflected_block_names.update({"FF_" + path.replace('.', '_').upper() for path in blocks})
            for path, blk in blocks.items():
                block_key_defs.append((path, blk['layout']))
            for blk in blocks.values():
                for field in blk['layout']:
                    all_field_names.add(field['orig_name'])
    
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
            all_blocks.update(blocks)
            all_block_paths.update(blocks.keys())
            reflected_block_names.update({"FF_" + path.replace('.', '_').upper() for path in blocks})
            for path, blk in blocks.items():
                block_key_defs.append((path, blk['layout']))
            for blk in blocks.values():
                for field in blk['layout']:
                    all_field_names.add(field['orig_name'])
            hpp, cpp = generate_cxx_for_blocks(blocks, versions)
            with open(os.path.join(output_dir, f"FF_{res}.hpp"), "w") as f: 
                f.write(f"{auto_header}#pragma once\n#include \"FF_DataTypes.hpp\"\n\n{hpp}")
            with open(os.path.join(output_dir, f"FF_{res}.cpp"), "w") as f: 
                f.write(f"{auto_header}\n#include \"FF_{res}.hpp\"\n#include \"../include/FF_Utilities.hpp\"\n#include \"FF_Dictionary.hpp\"\n\n{cpp}")

    field_keys_hpp = (
        f"{auto_header}#pragma once\n"
        "#include \"../include/FF_Primitives.hpp\"\n\n"
        "namespace FastFHIR::FieldKeys {\n"
        "    extern const FF_FieldKey* const Registry[];\n"
        "    extern const size_t RegistrySize;\n\n"
    )

    # 1. Global Field Key Constants (e.g. FF_ACCOUNT)
    for field_name in sorted(all_field_names):
        field_keys_hpp += f"    inline constexpr FF_FieldKey {_field_key_constant_name(field_name)}{{\"{field_name.lower()}\"}};\n"
    
    # 2. Schema-specific Fields (Namespace based)
    field_keys_hpp += "\n} // namespace FastFHIR::FieldKeys\n\nnamespace FastFHIR::Fields {\n"
    
    registry_entries = []
    token_registry = {}
    python_resource_map = {}
    token_id = 0
    seen_blocks = set()

    for path, layout in sorted(block_key_defs, key=lambda item: item[0]):
        if path in seen_blocks: continue
        seen_blocks.add(path)
        
        ns_name = _block_key_namespace(path) # e.g. "Patient" or "Patient_Contact"
        class_name = path.replace('.', '_').upper()
        python_resource_map[class_name] = {}
        
        field_keys_hpp += f"namespace {ns_name} {{\n"
        for f in layout:
            short_name = _field_key_short_name(f['orig_name'])
            
            # 1. Calculate the missing arguments
            block_struct_name = "FF_" + path.replace('.', '_').upper()
            child_rec = _child_recovery_key_expr(f, block_struct_name)
            arr_offsets = _array_entries_are_offsets_expr(f)
            
            # 2. Generate C++ Field Definition with all 6 arguments
            field_keys_hpp += (
                f"    inline constexpr FF_FieldKey {short_name}"
                f"{{RECOVER_{block_struct_name}, {_field_kind_expr(f)}, {f['offset']}, "
                f"{child_rec}, {arr_offsets}, \"{f['cpp_name']}\"}};\n"
            )
            
            # Track for Registry and Python
            registry_entries.append(f"        &FastFHIR::Fields::{ns_name}::{short_name}")
            
            if path not in token_registry: 
                token_registry[path] = {}
            token_registry[path][f['orig_name']] = (token_id, ns_name)
            token_id += 1
            
        field_keys_hpp += f"}} // namespace {ns_name}\n\n"
    field_keys_hpp += "} // namespace FastFHIR::Fields\n"

    # 3. Generate FF_FieldKeys.cpp
    field_keys_cpp = (
        f"{auto_header}\n#include \"FF_FieldKeys.hpp\"\n\n"
        "namespace FastFHIR::FieldKeys {\n"
        "    const FF_FieldKey* const Registry[] = {\n"
        + ",\n".join(registry_entries) +
        "\n    };\n\n"
        f"    const size_t RegistrySize = {len(registry_entries)};\n"
        "} // namespace FastFHIR::FieldKeys\n"
    )

    # Write Field Keys to map serialized field names to their recovery tags, offsets, and metadata
    with open(os.path.join(output_dir, "FF_FieldKeys.hpp"), "w") as f: f.write(field_keys_hpp)
    with open(os.path.join(output_dir, "FF_FieldKeys.cpp"), "w") as f: f.write(field_keys_cpp)

    # Emit the Deferred Path Builder
    emit_python_ast(all_blocks, block_key_defs, token_registry, output_dir)

    # Generate reflection dispatch files
    reflection_hpp, reflection_cpp = generate_reflection_dispatch(sorted(reflected_block_names), resources)
    with open(os.path.join(output_dir, "FF_Reflection.hpp"), "w") as f:
        f.write(reflection_hpp)
    with open(os.path.join(output_dir, "FF_Reflection.cpp"), "w") as f:
        f.write(reflection_cpp)
    
    all_types_hpp = f"{auto_header}#pragma once\n#include \"FF_DataTypes.hpp\"\n#include \"FF_FieldKeys.hpp\"\n#include \"FF_Reflection.hpp\"\n"
    for res in generated_resources:
        all_types_hpp += f"#include \"FF_{res}.hpp\"\n"
    all_types_hpp += "\n"
    with open(os.path.join(output_dir, "FF_AllTypes.hpp"), "w") as f: f.write(all_types_hpp)

    # Generate the RECOVERY enum from all discovered block paths
    generate_recovery_header(TARGET_TYPES, resources, all_block_paths, output_dir)

    # Generate the simdjson ingestion mappings for all discovered blocks
    generate_ingest_mappings(all_blocks, resources, output_dir)

def _version_sort_key(label):
	"""Sort labels like R4, R4B, R5, R65 deterministically."""
	m = re.match(r"^R(\d+)([A-Za-z]*)$", label)
	if not m:
		return (10**9, label)
	major = int(m.group(1))
	minor = m.group(2).upper()
	return (major, minor)

def _discover_versions(specs_dir="fhir_specs"):
	"""Discover available FHIR versions from extracted spec folders."""
	if not os.path.isdir(specs_dir):
		return []
	versions = []
	for name in os.listdir(specs_dir):
		full = os.path.join(specs_dir, name)
		if os.path.isdir(full) and re.match(r"^R\d+[A-Za-z]*$", name):
			versions.append(name)
	return sorted(set(versions), key=_version_sort_key)

def main (spec_dir="fhir_specs", output_dir="generated_src"):
    versions = _discover_versions(spec_dir)
    print(f"Discovered FHIR Versions: {', '.join(versions)}")
    enum_map = {}
    if versions:
        from generator.ffcs import generate_code_systems
        enum_map = generate_code_systems(TARGET_TYPES, TARGET_RESOURCES, versions)
    compile_fhir_library(TARGET_RESOURCES, versions, code_enum_map=enum_map, output_dir=output_dir)
    print("\n[Success] Build Complete: generated_src/ contains all FastFHIR artifacts."
)
    
if __name__ == "__main__":
    main()