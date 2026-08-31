from generator.model.type_map import (
    DATETIME_TYPES,
    DECIMAL_SIGFIGS_SUFFIX,
    SCALAR_PRIMITIVE_TYPES,
    STRING_TYPES,
    TYPE_MAP,
)
from generator.model import structure as _st
from generator.model.structure import _child_recovery_expr, _resolve_data_type_name


def _resolve_ff_struct_name(fhir_type, field_name, block_struct_name, resolved_path=None):
    """Resolve a FHIR type to its C++ struct name."""
    if fhir_type in ("string", "code") or fhir_type in STRING_TYPES:
        return "FF_STRING"
    if fhir_type in ("BackboneElement", "Element"):
        if resolved_path:
            return "FF_" + resolved_path.replace(".", "_").upper()
        return f"{block_struct_name}_{field_name}"
    return f"FF_{fhir_type.upper()}"


def generate_eager_deserializer(layout, block_struct_name, data_name):
    cpp = f"    {data_name} data;\n"
    for f in layout:
        kind = _st._field_kind_expr(f)
        indent = "    "
        if f["first_version_idx"] > 0:
            cpp += f"    if (__version >= FHIR_VERSION_{f['first_version_name']}) {{\n"
            indent = "        "
        cpp += f"{indent}// --- Deserialize: {f['name']} ---\n"
        vtable_off = f"__offset + {block_struct_name}::{f['name']}"

        if kind == "FF_FIELD_CHOICE":
            cpp += f"{indent}{{\n"
            cpp += f"{indent}    RECOVERY_TAG tag = static_cast<RECOVERY_TAG>(LOAD_U16(__base + {vtable_off} + 8));\n"
            cpp += f"{indent}    data.{f['cpp_name']}.tag = tag;\n"
            cpp += f"{indent}    if ((tag & 0xFF00) == RECOVER_FF_SCALAR_BLOCK) {{\n"
            cpp += f"{indent}        switch (tag) {{\n"
            cpp += f"{indent}            case RECOVER_FF_BOOL:   data.{f['cpp_name']}.value = FastFHIR::Decode::scalar<bool>(__base, {vtable_off}, tag); break;\n"
            cpp += f"{indent}            case RECOVER_FF_FLOAT64: data.{f['cpp_name']}.value = FastFHIR::Decode::scalar<double>(__base, {vtable_off}, tag); break;\n"
            cpp += f"{indent}            case RECOVER_FF_INT32:   data.{f['cpp_name']}.value = FastFHIR::Decode::scalar<int32_t>(__base, {vtable_off}, tag); break;\n"
            cpp += f"{indent}            default:                 data.{f['cpp_name']}.value = FastFHIR::Decode::scalar<uint64_t>(__base, {vtable_off}, tag); break;\n"
            cpp += f"{indent}        }}\n"
            cpp += f"{indent}    }} else if (tag != FF_RECOVER_UNDEFINED) {{\n"
            cpp += f"{indent}        Offset child_off = LOAD_U64(__base + {vtable_off});\n"
            cpp += f"{indent}        if (child_off != FF_NULL_OFFSET) {{\n"
            cpp += f"{indent}            if (tag == RECOVER_FF_STRING) data.{f['cpp_name']}.value = FF_STRING(child_off, __size, __version).read_view(__base);\n"
            cpp += f"{indent}            else data.{f['cpp_name']}.value = child_off;\n"
            cpp += f"{indent}        }}\n"
            cpp += f"{indent}    }}\n"
            cpp += f"{indent}}}\n"

        elif kind == "FF_FIELD_ARRAY":
            cpp += f"{indent}Offset arr_off_{f['cpp_name']} = LOAD_U64(__base + {vtable_off});\n"
            cpp += f"{indent}if (arr_off_{f['cpp_name']} != FF_NULL_OFFSET) {{\n"
            cpp += f"{indent}    FF_ARRAY arr_{f['cpp_name']}(arr_off_{f['cpp_name']}, __size, __version);\n"
            cpp += f"{indent}    auto STEP = arr_{f['cpp_name']}.entry_step(__base);\n"
            cpp += f"{indent}    auto ENTRIES = arr_{f['cpp_name']}.entry_count(__base);\n"
            cpp += f"{indent}    auto blk_item_ptr = arr_{f['cpp_name']}.entries(__base);\n"
            cpp += f"{indent}    for (uint32_t i = 0; i < ENTRIES; ++i, blk_item_ptr += STEP) {{\n"

            if (
                f["fhir_type"] in ("string", "code")
                or f["fhir_type"] in STRING_TYPES
                or f["fhir_type"] in DATETIME_TYPES
            ):
                code_enum = f.get("code_enum")
                # DT-2 datetime arrays hold std::vector<std::string> (the data
                # member became std::string); plain string arrays keep
                # string_view, so only datetime needs the explicit conversion.
                push_expr = (
                    "std::string(blk_str.read_view(__base))"
                    if f["fhir_type"] in DATETIME_TYPES
                    else "blk_str.read_view(__base)"
                )
                cpp += f"{indent}        Offset blk_str_off = LOAD_U64(blk_item_ptr);\n"
                cpp += f"{indent}        if (blk_str_off != FF_NULL_OFFSET) {{\n"
                cpp += f"{indent}            FF_STRING blk_str(blk_str_off, __size, __version);\n"
                if code_enum:
                    cpp += f"{indent}            data.{f['cpp_name']}.push_back({code_enum['parse']}(blk_str.read(__base)));\n"
                else:
                    cpp += f"{indent}            data.{f['cpp_name']}.push_back({push_expr});\n"
                cpp += f"{indent}        }}\n"
            elif f["fhir_type"] == "Resource":
                cpp += f"{indent}        Offset res_off = LOAD_U64(blk_item_ptr);\n"
                cpp += f"{indent}        if (res_off != FF_NULL_OFFSET) {{\n"
                cpp += f"{indent}            RECOVERY_TAG res_tag = FF_GET_RECOVERY_TAG(__base, static_cast<Offset>(blk_item_ptr - __base));\n"
                cpp += f"{indent}            data.{f['cpp_name']}.push_back(ResourceReference(res_off, res_tag));\n"
                cpp += f"{indent}        }}\n"
            elif f["fhir_type"] in SCALAR_PRIMITIVE_TYPES:
                load_macro = TYPE_MAP[f["fhir_type"]]["macro"]
                cpp += (
                    f"{indent}        data.{f['cpp_name']}.push_back({load_macro}(blk_item_ptr));\n"
                )
            elif f["fhir_type"] in STRING_TYPES:
                # STRING_TYPES in arrays use FF_STRING constructor + read_view, not ::deserialize()
                cpp += f"{indent}        Offset blk_str_off = LOAD_U64(blk_item_ptr);\n"
                cpp += f"{indent}        if (blk_str_off != FF_NULL_OFFSET) {{\n"
                cpp += f"{indent}            data.{f['cpp_name']}.push_back(FF_STRING(blk_str_off, __size, __version).read_view(__base));\n"
                cpp += f"{indent}        }}\n"
            else:
                child_struct = _resolve_ff_struct_name(
                    f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
                )
                cpp += f"{indent}        data.{f['cpp_name']}.push_back({child_struct}::deserialize(__base, static_cast<Offset>(blk_item_ptr - __base), __size, __version));\n"
            cpp += f"{indent}    }}\n{indent}}}\n"

        elif kind == "FF_FIELD_RESOURCE":
            cpp += f"{indent}Offset res_off_{f['cpp_name']} = LOAD_U64(__base + {vtable_off});\n"
            cpp += f"{indent}if (res_off_{f['cpp_name']} != FF_NULL_OFFSET) {{\n"
            cpp += f"{indent}    RECOVERY_TAG res_tag_{f['cpp_name']} = FF_GET_RECOVERY_TAG(__base, {vtable_off});\n"
            cpp += f"{indent}    data.{f['cpp_name']} = ResourceReference(res_off_{f['cpp_name']}, res_tag_{f['cpp_name']});\n"
            cpp += f"{indent}}}\n"

        elif kind == "FF_FIELD_STRING":
            cpp += f"{indent}Offset str_off_{f['cpp_name']} = LOAD_U64(__base + {vtable_off});\n"
            cpp += f"{indent}if (str_off_{f['cpp_name']} != FF_NULL_OFFSET) data.{f['cpp_name']} = FF_STRING(str_off_{f['cpp_name']}, __size, __version).read_view(__base);\n"

        elif kind == "FF_FIELD_CODE":
            code_enum = f.get("code_enum")
            cpp += f"{indent}{{\n"
            cpp += f"{indent}    uint32_t raw_code = LOAD_U32(__base + {vtable_off});\n"
            cpp += f"{indent}    if (raw_code == FF_CODE_NULL) {{\n"
            # FF_ResolveCode returns `const char*` (null when the ID is not a
            # dictionary entry), so the resolve and the test are one condition.
            cpp += (
                f"{indent}    }} else if (const char* _cc_label = "
                f"FF_ResolveCode(raw_code, __version)) {{\n"
            )
            if code_enum:
                cpp += f"{indent}        data.{f['cpp_name']} = {code_enum['parse']}(std::string(_cc_label));\n"
            else:
                cpp += f"{indent}        data.{f['cpp_name']} = _cc_label;\n"
            cpp += f"{indent}    }} else if (raw_code & FF_CODEABLE_CONCEPT_FLAG) {{\n"
            cpp += f"{indent}        Offset abs_off = FF_ResolveCodeableConceptOffset(raw_code, __offset);\n"
            if code_enum:
                cpp += f"{indent}        data.{f['cpp_name']} = {code_enum['parse']}(std::string(FF_DECODE_CODEABLE_CONCEPT(__base, abs_off, __version).label));\n"
            else:
                cpp += f"{indent}        data.{f['cpp_name']} = FF_DECODE_CODEABLE_CONCEPT(__base, abs_off, __version).label;\n"
            cpp += f"{indent}    }}\n"
            cpp += f"{indent}}}\n"

        elif kind == "FF_FIELD_BLOCK":
            # A SINGULAR block-typed field. Its slot holds the ABSOLUTE arena
            # offset of the child's header -- the same value store.py wrote
            # (`STORE_U64(slot, child_off)`), NOT the entry-relative offset the
            # ARRAY branch computes. Do not copy the array form here.
            #
            # This branch did not exist until 2026-08-26 (TASKS.md P0-1 /
            # CAPI-13): a singular CodeableConcept/Reference/BackboneElement
            # matched no arm of this chain and emitted NOTHING, so 565 fields
            # across every generated resource file deserialized as null while
            # the reflective lens read them correctly. The `else` at the end of
            # this chain is what stops the next one.
            #
            # The POCO member is std::unique_ptr<T> (model/merge.py); naming T
            # via decltype rather than re-deriving it means the two cannot
            # disagree.
            child_struct = _resolve_ff_struct_name(
                f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
            )
            _p = f"blk_off_{f['cpp_name']}"
            _elem = f"decltype(data.{f['cpp_name']})::element_type"
            cpp += f"{indent}Offset {_p} = LOAD_U64(__base + {vtable_off});\n"
            cpp += f"{indent}if ({_p} != FF_NULL_OFFSET) {{\n"
            if child_struct == "FF_STRING":
                # STRING_TYPES stored as unique_ptr<std::string_view> -- FF_STRING
                # owns its header, so there is no ::deserialize to call.
                cpp += (
                    f"{indent}    data.{f['cpp_name']} = std::make_unique<{_elem}>("
                    f"FF_STRING({_p}, __size, __version).read_view(__base));\n"
                )
            else:
                cpp += (
                    f"{indent}    data.{f['cpp_name']} = std::make_unique<{_elem}>("
                    f"{child_struct}::deserialize(__base, {_p}, __size, __version));\n"
                )
            cpp += f"{indent}}}\n"

        elif kind == "FF_FIELD_URL":
            # A 4-byte FF_URL_DIRECTORY index (Extension.url). The POCO member
            # is the index itself (uint32_t), so the read is the plain scalar
            # load that mirrors store.py's terminal STORE_U32. Absent stays
            # FF_NULL_UINT32. Like FF_FIELD_BLOCK above, this emitted nothing
            # before 2026-08-26, so every hydrated ExtensionData lost its url.
            cpp += f"{indent}data.{f['cpp_name']} = LOAD_U32(__base + {vtable_off});\n"

        elif f["fhir_type"] in DATETIME_TYPES:
            # DT-2: packed inline u64 decodes to text; a flagged relative
            # offset resolves to an FF_STRING holding the ORIGINAL text (so the
            # round trip is byte-exact for values that do not pack).
            cpp += f"{indent}{{\n"
            cpp += f"{indent}    const uint64_t __dt_raw = LOAD_U64(__base + {vtable_off});\n"
            cpp += f"{indent}    if (__dt_raw != FF_DATETIME_NULL) {{\n"
            cpp += f"{indent}        if (FF_DATETIME_IS_FALLBACK(__dt_raw)) {{\n"
            cpp += f"{indent}            FF_STRING __dt_str(FF_ResolveDateTimeOffset(__dt_raw, __offset), __size, __version);\n"
            cpp += f"{indent}            data.{f['cpp_name']} = std::string(__dt_str.read_view(__base));\n"
            cpp += f"{indent}        }} else {{\n"
            cpp += f"{indent}            data.{f['cpp_name']} = FF_FORMAT_DATETIME(FF_UNPACK_DATETIME(__dt_raw), {_child_recovery_expr(f, block_struct_name)});\n"
            cpp += f"{indent}        }}\n"
            cpp += f"{indent}    }}\n"
            cpp += f"{indent}}}\n"

        elif f["fhir_type"] == "decimal" and not f["is_array"]:
            # Mirror of the store: the value from +0, the source scale from +8.
            # Dropping the scale here would make a POD round trip (deserialize
            # -> store) silently downgrade every decimal to "no scale recorded".
            cpp += f"{indent}data.{f['cpp_name']} = FastFHIR::Decode::scalar<double>(__base, {vtable_off}, {_child_recovery_expr(f, block_struct_name)});\n"
            cpp += (
                f"{indent}data.{f['cpp_name']}{DECIMAL_SIGFIGS_SUFFIX} = "
                f"LOAD_U8(__base + {vtable_off} + TYPE_SIZE_UINT64);\n"
            )

        elif (
            f["fhir_type"] in TYPE_MAP
            and f["fhir_type"] not in ("string", "code", "DEFAULT")
            and f["fhir_type"] not in DATETIME_TYPES
        ):
            cpp += f"{indent}data.{f['cpp_name']} = FastFHIR::Decode::scalar<{f['cpp_type']}>(__base, {vtable_off}, {_child_recovery_expr(f, block_struct_name)});\n"

        else:
            # A field kind with no branch above emits NO CODE and silently
            # drops the field -- which is exactly how CAPI-13 survived a green
            # suite. Never replace this with a `pass`: an unhandled kind is a
            # generator bug, and the configure step is the only place it is
            # still cheap to find.
            raise NotImplementedError(
                f"deserialize: no branch for kind={kind} "
                f"field={block_struct_name}.{f['name']} type={f['fhir_type']} "
                f"cpp_type={f['cpp_type']} -- would emit no code and silently "
                f"drop the field"
            )

        if f["first_version_idx"] > 0:
            cpp += f"    }}\n"
    cpp += "    return data;\n"
    return cpp
