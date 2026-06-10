from generator.model.type_map import SCALAR_PRIMITIVE_TYPES, STRING_TYPES, TYPE_MAP
from generator.model.structure import _child_recovery_expr, _resolve_data_type_name


def _resolve_ff_struct_name(fhir_type, field_name, block_struct_name, resolved_path=None):
    """Resolve a FHIR type to its C++ struct name."""
    if fhir_type in ('string', 'code') or fhir_type in STRING_TYPES:
        return 'FF_STRING'
    if fhir_type in ('BackboneElement', 'Element'):
        if resolved_path:
            return "FF_" + resolved_path.replace('.', '_').upper()
        return f"{block_struct_name}_{field_name}"
    return f"FF_{fhir_type.upper()}"


def generate_eager_deserializer(layout, block_struct_name, data_name):
    cpp = f"    {data_name} data;\n"
    for f in layout:
        indent = "    "
        if f['first_version_idx'] > 0:
            cpp += f"    if (__version >= FHIR_VERSION_{f['first_version_name']}) {{\n"
            indent = "        "
        cpp += f"{indent}// --- Deserialize: {f['name']} ---\n"
        vtable_off = f"__offset + {block_struct_name}::{f['name']}"
        
        if f.get('is_choice'):
            cpp += f"{indent}{{\n"
            cpp += f"{indent}    RECOVERY_TAG tag = static_cast<RECOVERY_TAG>(LOAD_U16(__base + {vtable_off} + 8));\n"
            cpp += f"{indent}    data.{f['cpp_name']}.tag = tag;\n"
            cpp += f"{indent}    if ((tag & 0xFF00) == RECOVER_FF_SCALAR_BLOCK) {{\n"
            cpp += f"{indent}        if (tag == RECOVER_FF_BOOL) data.{f['cpp_name']}.value = FastFHIR::Decode::scalar<bool>(__base, {vtable_off}, tag);\n"
            cpp += f"{indent}        else if (tag == RECOVER_FF_FLOAT64) data.{f['cpp_name']}.value = FastFHIR::Decode::scalar<double>(__base, {vtable_off}, tag);\n"
            cpp += f"{indent}        else if (tag == RECOVER_FF_INT32) data.{f['cpp_name']}.value = FastFHIR::Decode::scalar<int32_t>(__base, {vtable_off}, tag);\n"
            cpp += f"{indent}        else data.{f['cpp_name']}.value = FastFHIR::Decode::scalar<uint64_t>(__base, {vtable_off}, tag);\n"
            cpp += f"{indent}    }} else if (tag != FF_RECOVER_UNDEFINED) {{\n"
            cpp += f"{indent}        Offset child_off = LOAD_U64(__base + {vtable_off});\n"
            cpp += f"{indent}        if (child_off != FF_NULL_OFFSET) {{\n"
            cpp += f"{indent}            if (tag == RECOVER_FF_STRING) data.{f['cpp_name']}.value = FF_STRING(child_off, __size, __version).read_view(__base);\n"
            cpp += f"{indent}            else data.{f['cpp_name']}.value = child_off;\n"
            cpp += f"{indent}        }}\n"
            cpp += f"{indent}    }}\n"
            cpp += f"{indent}}}\n"
            
        elif f['is_array']:
            cpp += f"{indent}Offset arr_off_{f['cpp_name']} = LOAD_U64(__base + {vtable_off});\n"
            cpp += f"{indent}if (arr_off_{f['cpp_name']} != FF_NULL_OFFSET) {{\n"
            cpp += f"{indent}    FF_ARRAY arr_{f['cpp_name']}(arr_off_{f['cpp_name']}, __size, __version);\n"
            cpp += f"{indent}    auto STEP = arr_{f['cpp_name']}.entry_step(__base);\n"
            cpp += f"{indent}    auto ENTRIES = arr_{f['cpp_name']}.entry_count(__base);\n"
            cpp += f"{indent}    auto blk_item_ptr = arr_{f['cpp_name']}.entries(__base);\n"
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
            elif f['fhir_type'] == 'Resource':
                cpp += f"{indent}        Offset res_off = LOAD_U64(blk_item_ptr);\n"
                cpp += f"{indent}        if (res_off != FF_NULL_OFFSET) {{\n"
                cpp += f"{indent}            RECOVERY_TAG res_tag = static_cast<RECOVERY_TAG>(LOAD_U16(blk_item_ptr + DATA_BLOCK::RECOVERY));\n"
                cpp += f"{indent}            data.{f['cpp_name']}.push_back(ResourceReference(res_off, res_tag));\n"
                cpp += f"{indent}        }}\n"
            elif f['fhir_type'] in SCALAR_PRIMITIVE_TYPES:
                load_macro = TYPE_MAP[f['fhir_type']]['macro']
                cpp += f"{indent}        data.{f['cpp_name']}.push_back({load_macro}(blk_item_ptr));\n"
            elif f['fhir_type'] in STRING_TYPES:
                # STRING_TYPES in arrays use FF_STRING constructor + read_view, not ::deserialize()
                cpp += f"{indent}        Offset blk_str_off = LOAD_U64(blk_item_ptr);\n"
                cpp += f"{indent}        if (blk_str_off != FF_NULL_OFFSET) {{\n"
                cpp += f"{indent}            data.{f['cpp_name']}.push_back(FF_STRING(blk_str_off, __size, __version).read_view(__base));\n"
                cpp += f"{indent}        }}\n"
            else:
                child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
                cpp += f"{indent}        data.{f['cpp_name']}.push_back({child_struct}::deserialize(__base, static_cast<Offset>(blk_item_ptr - __base), __size, __version));\n"
            cpp += f"{indent}    }}\n{indent}}}\n"
            
        elif f['fhir_type'] == 'Resource':
            cpp += f"{indent}Offset res_off_{f['cpp_name']} = LOAD_U64(__base + {vtable_off});\n"
            cpp += f"{indent}if (res_off_{f['cpp_name']} != FF_NULL_OFFSET) {{\n"
            cpp += f"{indent}    RECOVERY_TAG res_tag_{f['cpp_name']} = static_cast<RECOVERY_TAG>(LOAD_U16(__base + {vtable_off} + DATA_BLOCK::RECOVERY));\n"
            cpp += f"{indent}    data.{f['cpp_name']} = ResourceReference(res_off_{f['cpp_name']}, res_tag_{f['cpp_name']});\n"
            cpp += f"{indent}}}\n"

        elif f['fhir_type'] == 'string':
            cpp += f"{indent}Offset str_off_{f['cpp_name']} = LOAD_U64(__base + {vtable_off});\n"
            cpp += f"{indent}if (str_off_{f['cpp_name']} != FF_NULL_OFFSET) data.{f['cpp_name']} = FF_STRING(str_off_{f['cpp_name']}, __size, __version).read_view(__base);\n"
            
        elif f['cpp_type'] == 'Offset':
            data_type = _resolve_data_type_name(f['fhir_type'], f['orig_name'], block_struct_name, f.get('resolved_path'))
            child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
            cpp += f"{indent}Offset blk_off_{f['cpp_name']} = LOAD_U64(__base + {vtable_off});\n"
            # STRING_TYPES (dateTime, base64Binary, markdown, xhtml, etc.) use
            # FF_STRING constructor + read_view(), not ::deserialize().
            if f['fhir_type'] in STRING_TYPES:
                cpp += f"{indent}if (blk_off_{f['cpp_name']} != FF_NULL_OFFSET) data.{f['cpp_name']} = std::make_unique<{data_type}>(FF_STRING(blk_off_{f['cpp_name']}, __size, __version).read_view(__base));\n"
            else:
                cpp += f"{indent}if (blk_off_{f['cpp_name']} != FF_NULL_OFFSET) data.{f['cpp_name']} = std::make_unique<{data_type}>({child_struct}::deserialize(__base, blk_off_{f['cpp_name']}, __size, __version));\n"
            
        elif f['fhir_type'] == 'code':
            code_enum = f.get('code_enum')
            cpp += f"{indent}{{\n"
            cpp += f"{indent}    uint32_t raw_code = LOAD_U32(__base + {vtable_off});\n"
            cpp += f"{indent}    if (raw_code == FF_CODE_NULL) {{\n"
            cpp += f"{indent}    }} else if (const char* resolved = FF_ResolveCode(raw_code, __version)) {{\n"
            if code_enum: cpp += f"{indent}        data.{f['cpp_name']} = {code_enum['parse']}(std::string(resolved));\n"
            else:         cpp += f"{indent}        data.{f['cpp_name']} = resolved;\n"
            cpp += f"{indent}    }} else if (raw_code & FF_CUSTOM_STRING_FLAG) {{\n"
            cpp += f"{indent}        Offset relative_off = __offset + (raw_code & ~FF_CUSTOM_STRING_FLAG);\n"
            cpp += f"{indent}        std::string_view custom_str = FF_STRING(relative_off, __size, __version).read_view(__base);\n"
            if code_enum: cpp += f"{indent}        data.{f['cpp_name']} = {code_enum['parse']}(std::string(custom_str));\n"
            else:         cpp += f"{indent}        data.{f['cpp_name']} = custom_str;\n"
            cpp += f"{indent}    }}\n"
            cpp += f"{indent}}}\n"
            
        elif f['fhir_type'] in TYPE_MAP and f['fhir_type'] not in ('string', 'code', 'DEFAULT'):
            cpp += f"{indent}data.{f['cpp_name']} = FastFHIR::Decode::scalar<{f['cpp_type']}>(__base, {vtable_off}, {_child_recovery_expr(f, block_struct_name)});\n"
            
        if f['first_version_idx'] > 0: cpp += f"    }}\n"
    cpp += "    return data;\n"
    return cpp

