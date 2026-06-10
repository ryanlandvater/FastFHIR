"""FastFHIR Generator — simdjson ingest-mapping emitter.

Relocated from ffc.py lines 1151-1727.  Generates FF_IngestMappings.{hpp,cpp}
with per-block ``_from_json`` deserializers that map simdjson objects into
FastFHIR builder calls.  The body is the heaviest emitter in the pipeline.
"""

import os

from generator.emit.header import auto_header, write_if_changed
from generator.model import type_map as _tm
from generator.model.structure import (
    _resolve_data_type_name,
    _block_key_namespace,
    _field_key_short_name,
    _child_recovery_key_expr,
)

# Per-field JSON-ingest overrides (relocated from ffc.py line 144).
INGEST_FIELD_OVERRIDES: dict[tuple[str, str], str] = {}


def generate_ingest_mappings(master_blocks, resources, output_dir="generated_src"):
    hpp = (
        f"{auto_header}#pragma once\n"
        f'#include "FF_AllTypes.hpp"\n'
        f'#include "FF_Logger.hpp"\n'
        f"#include <simdjson.h>\n\n"
        f"namespace FastFHIR::Ingest {{\n\n"
    )

    cpp = (
        f'{auto_header}#include "FF_IngestMappings.hpp"\n\n'
        f"namespace FastFHIR::Ingest {{\n\n"
        f"// Internal Parser Forward Declarations\n"
    )

    for path, blk in master_blocks.items():
        if path == "Bundle":
            continue
        data_type = path.replace(".", "") + "Data"
        fn_name = path.replace(".", "_") + "_from_json"
        cpp += (
            f"static {data_type} {fn_name}("
            f"simdjson::ondemand::object obj, "
            f"FastFHIR::ConcurrentLogger* logger = nullptr, "
            f"std::vector<std::string_view>* concurrent_queue = nullptr, "
            f"FastFHIR::Builder* builder = nullptr);\n"
        )

    cpp += "\n"

    # --- Per-block POD parsing ---
    for path, blk in master_blocks.items():
        data_type = path.replace(".", "") + "Data"
        fn_name = path.replace(".", "_") + "_from_json"

        if path == "Bundle":
            hpp += (
                f"    {data_type} {fn_name}("
                f"simdjson::ondemand::object obj, "
                f"FastFHIR::ConcurrentLogger* logger = nullptr, "
                f"std::vector<std::string_view>* concurrent_queue = nullptr, "
                f"FastFHIR::Builder* builder = nullptr);\n\n"
            )
            cpp += (
                f"{data_type} {fn_name}("
                f"simdjson::ondemand::object obj, "
                f"FastFHIR::ConcurrentLogger* logger, "
                f"std::vector<std::string_view>* concurrent_queue, "
                f"FastFHIR::Builder* builder) {{\n"
            )
        else:
            cpp += (
                f"static {data_type} {fn_name}("
                f"simdjson::ondemand::object obj, "
                f"FastFHIR::ConcurrentLogger* logger, "
                f"std::vector<std::string_view>* concurrent_queue, "
                f"FastFHIR::Builder* builder) {{\n"
            )

        cpp += (
            f"    obj.reset();\n"
            f"    {data_type} data;\n"
            f"    for (auto field : obj) {{\n"
            f"        std::string_view key = field.unescaped_key().value_unsafe();\n"
        )

        is_first = True
        for f in blk["layout"]:
            json_key = f["orig_name"]
            cpp_name = f["cpp_name"]
            is_choice = f.get("is_choice", False)
            err_log = (
                f'if (logger) logger->log("[Warning] FastFHIR Ingestion: Malformed data at '
                f'{path}.{json_key}");'
            )

            condition = (
                f'key.starts_with("{json_key}")'
                if is_choice
                else f'key == "{json_key}"'
            )

            cpp += f"        {'if' if is_first else 'else if'} ({condition}) {{\n"
            is_first = False

            ingest_key = INGEST_FIELD_OVERRIDES.get((path, json_key))
            if ingest_key:
                json_key = ingest_key

            if is_choice:
                cpp += f"            std::string_view suffix = key.substr({len(json_key)});\n"
                is_first_choice = True
                for c_type in f.get("choice_types", []):
                    suffix_match = c_type[0].upper() + c_type[1:]
                    cond = (
                        f'if (suffix == "{suffix_match}")'
                        if is_first_choice
                        else f'else if (suffix == "{suffix_match}")'
                    )
                    cpp += f"            {cond} {{\n"
                    if c_type == "boolean":
                        cpp += (
                            f"                bool b_val;\n"
                            f"                if (field.value().get_bool().get(b_val) == simdjson::SUCCESS) {{\n"
                            f"                    data.{cpp_name}.tag = RECOVER_FF_BOOL;\n"
                            f"                    data.{cpp_name}.value = b_val;\n"
                            f"                }}\n"
                        )
                    elif c_type in ("integer", "positiveInt", "unsignedInt", "integer64"):
                        cpp += (
                            f"                uint64_t i_val;\n"
                            f"                if (field.value().get_uint64().get(i_val) == simdjson::SUCCESS) {{\n"
                            f'                    data.{cpp_name}.tag = (suffix == "Integer64") ? RECOVER_FF_UINT64 : RECOVER_FF_UINT32;\n'
                            f"                    data.{cpp_name}.value = i_val;\n"
                            f"                }}\n"
                        )
                    elif c_type == "decimal":
                        cpp += (
                            f"                double d_val;\n"
                            f"                if (field.value().get_double().get(d_val) == simdjson::SUCCESS) {{\n"
                            f"                    data.{cpp_name}.tag = RECOVER_FF_FLOAT64;\n"
                            f"                    data.{cpp_name}.value = d_val;\n"
                            f"                }}\n"
                        )
                    elif c_type == "string":
                        cpp += (
                            f"                std::string_view s_val;\n"
                            f"                if (field.value().get_string().get(s_val) == simdjson::SUCCESS) {{\n"
                            f"                    if (!s_val.empty()) {{\n"
                            f"                        data.{cpp_name}.tag = RECOVER_FF_STRING;\n"
                            f"                        data.{cpp_name}.value = s_val;\n"
                            f"                    }}\n"
                            f"                }}\n"
                        )
                    else:
                        child_fn = f"{c_type}_from_json"
                        child_data = f"FF_{c_type.upper()}Data"
                        cpp += (
                            f"                auto sub = field.value().get_object();\n"
                            f"                if (!sub.error()) {{\n"
                            f"                    auto sub_val = {child_fn}(sub.value_unsafe(), logger, concurrent_queue, builder);\n"
                            f"                    data.{cpp_name}.tag = RECOVER_FF_{c_type.upper()};\n"
                            f"                    if (builder) data.{cpp_name}.value = builder->claim(builder->store(data.{cpp_name}));\n"
                            f"                    else if (concurrent_queue) {{ /* stub */ }}\n"
                            f"                }} else {{\n"
                            f"                    {err_log}\n"
                            f"                }}\n"
                        )
                    cpp += f"            }}\n"
                    is_first_choice = False
            elif f["is_array"]:
                fhir_type = f["fhir_type"]
                err_log_line = f"            {err_log}"
                cpp += f'            for (auto elem : field.value().get_array()) {{\n'
                if fhir_type == "string":
                    cpp += (
                        f"                std::string_view s;\n"
                        f"                if (elem.get_string().get(s) == simdjson::SUCCESS) "
                        f"data.{cpp_name}.emplace_back(s);\n"
                        f"                else {{ {err_log_line} }}\n"
                    )
                elif fhir_type == "code":
                    cpp += (
                        f"                std::string_view c;\n"
                        f"                if (elem.get_string().get(c) == simdjson::SUCCESS) "
                        f"data.{cpp_name}.emplace_back(std::string(c));\n"
                        f"                else {{ {err_log_line} }}\n"
                    )
                elif fhir_type in _tm.SCALAR_PRIMITIVE_TYPES:
                    if fhir_type == "boolean":
                        cpp += (
                            f"                bool v;\n"
                            f"                if (elem.get_bool().get(v) == simdjson::SUCCESS) "
                            f"data.{cpp_name}.push_back(v);\n"
                            f"                else {{ {err_log_line} }}\n"
                        )
                    elif fhir_type in ("integer64",):
                        cpp += (
                            f"                uint64_t v;\n"
                            f"                if (elem.get_uint64().get(v) == simdjson::SUCCESS) "
                            f"data.{cpp_name}.push_back(v);\n"
                            f"                else {{ {err_log_line} }}\n"
                        )
                    else:
                        cpp += (
                            f"                uint64_t v;\n"
                            f"                if (elem.get_uint64().get(v) == simdjson::SUCCESS) "
                            f"data.{cpp_name}.push_back(static_cast<uint32_t>(v));\n"
                            f"                else {{ {err_log_line} }}\n"
                        )
                else:
                    child_fn_name = f"{fhir_type}_from_json"
                    cpp += (
                        f"                auto sub_obj = elem.get_object();\n"
                        f"                if (!sub_obj.error()) {{\n"
                        f"                    data.{cpp_name}.emplace_back("
                        f"{child_fn_name}(sub_obj.value_unsafe(), logger, concurrent_queue, builder)"
                        f");\n"
                        f"                }} else {{ {err_log_line} }}\n"
                    )
                cpp += "            }\n"
            elif f["fhir_type"] == "string":
                cpp += (
                    f"            std::string_view s;\n"
                    f"            if (field.value().get_string().get(s) == simdjson::SUCCESS) "
                    f"data.{cpp_name} = s;\n"
                    f"            else {{ {err_log} }}\n"
                )
            elif f["fhir_type"] == "code":
                cpp += (
                    f"            std::string_view c;\n"
                    f"            if (field.value().get_string().get(c) == simdjson::SUCCESS) "
                    f"data.{cpp_name} = std::string(c);\n"
                    f"            else {{ {err_log} }}\n"
                )
            elif f["fhir_type"] in _tm.SCALAR_PRIMITIVE_TYPES:
                if f["fhir_type"] == "boolean":
                    cpp += (
                        f"            bool v;\n"
                        f"            if (field.value().get_bool().get(v) == simdjson::SUCCESS) "
                        f"data.{cpp_name} = v;\n"
                        f"            else {{ {err_log} }}\n"
                    )
                elif f["fhir_type"] in ("integer64",):
                    cpp += (
                        f"            uint64_t v;\n"
                        f"            if (field.value().get_uint64().get(v) == simdjson::SUCCESS) "
                        f"data.{cpp_name} = v;\n"
                        f"            else {{ {err_log} }}\n"
                    )
                else:
                    cpp += (
                        f"            uint64_t v;\n"
                        f"            if (field.value().get_uint64().get(v) == simdjson::SUCCESS) "
                        f"data.{cpp_name} = static_cast<uint32_t>(v);\n"
                        f"            else {{ {err_log} }}\n"
                    )
            elif f["fhir_type"] == "Resource":
                cpp += (
                    f"            auto ref_obj = field.value().get_object();\n"
                    f"            if (!ref_obj.error()) {{\n"
                    f"                data.{cpp_name} = ResourceReference();\n"
                    f"            }}\n"
                )
            else:
                child_fn = f"{f['fhir_type']}_from_json"
                cpp += (
                    f"            auto sub = field.value().get_object();\n"
                    f"            if (!sub.error()) {{\n"
                    f"                data.{cpp_name} = std::make_unique<decltype(data.{cpp_name})::element_type>("
                    f"{child_fn}(sub.value_unsafe(), logger, concurrent_queue, builder)"
                    f");\n"
                    f"            }} else {{ {err_log} }}\n"
                )
            cpp += "        }\n"

        cpp += (
            "    }\n"
            "    return data;\n"
            "}\n\n"
        )

    hpp += "} // namespace FastFHIR::Ingest\n"
    cpp += "} // namespace FastFHIR::Ingest\n"

    write_if_changed(os.path.join(output_dir, "FF_IngestMappings.hpp"), hpp)
    write_if_changed(os.path.join(output_dir, "FF_IngestMappings.cpp"), cpp)
    print(f"-- Emitted {output_dir}/FF_IngestMappings.{{hpp,cpp}}")
