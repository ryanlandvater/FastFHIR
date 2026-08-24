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

# FHIR primitive types stored as strings (no _from_json function, no vtable).
# These parse as string_view with RECOVER_FF_STRING, exactly like "string".
_STRING_LIKE_TYPES: frozenset = frozenset(
    {
        "string",
        "code",
        "id",
        "markdown",
        "uri",
        "url",
        "canonical",
        "oid",
        "base64Binary",
        "date",
        "dateTime",
        "instant",
        "time",
        "uuid",
    }
)

# How each scalar primitive is pulled out of simdjson: the accessor, the type of
# the staging local, and the expression stored into the POD member.
#
# ONE table for both the 0..1 and the 0..* emitters, which previously carried
# separate three-arm if/elif chains ending in a `get_uint64()` catch-all. That
# catch-all silently swallowed `decimal`: every Quantity.value, Timing.repeat.*
# and Attachment.duration in a bundle hit get_uint64() on a fractional literal,
# failed INCORRECT_TYPE, and left the slot at its null sentinel -- 302 warnings
# and 302 dropped values on a single Synthea fixture, because a catch-all cannot
# tell "type I planned for" from "type nobody wired up".
#
# Keyed on SCALAR_PRIMITIVE_TYPES exactly: a type added there without an entry
# here raises KeyError at generate time instead of quietly emitting the wrong
# accessor again.
_SCALAR_INGEST: dict[str, tuple[str, str, str]] = {
    # fhir_type: (simdjson accessor, staging type, expression stored)
    "boolean": ("get_bool", "bool", "v"),
    "integer": ("get_uint64", "uint64_t", "static_cast<uint32_t>(v)"),
    "unsignedInt": ("get_uint64", "uint64_t", "static_cast<uint32_t>(v)"),
    "positiveInt": ("get_uint64", "uint64_t", "static_cast<uint32_t>(v)"),
    "integer64": ("get_uint64", "uint64_t", "v"),
    "decimal": ("get_double", "double", "v"),
}

# The RECOVERY_TAG a choice ([x]) variant of each integer-family type carries.
#
# A choice slot has nothing but this tag to say which FHIR type it holds, so the
# exporter reads the variant's name straight off it. One tag standing in for
# several types therefore RENAMES the field rather than corrupting its value:
# every one of these used to emit RECOVER_FF_UINT32 (except integer64), and
# get_choice_suffix maps that to "UnsignedInt", so `valueInteger` left as
# `valueUnsignedInt` -- 4,396 pairs in Synthea, silent and well-formed.
#
# Every value here MUST be in the scalar band (0x0100-0x01FF). A choice slot
# holds its value inline, and Recovery_to_Kind sends anything >= 0x0200 down the
# FF_FIELD_BLOCK path, where the reader would treat the raw integer as a block
# offset. That is why RECOVER_FF_POSITIVEINT (0x0230) and RECOVER_FF_UNSIGNEDINT
# (0x0237) are NOT used despite their names -- they are datatype-band tags, and
# `positiveInt` consequently shares "UnsignedInt" until a scalar-band tag is
# appended for it (TASKS.md AR-5).
_CHOICE_INT_TAGS: dict[str, str] = {
    "integer": "RECOVER_FF_INT32",
    "unsignedInt": "RECOVER_FF_UINT32",
    "positiveInt": "RECOVER_FF_UINT32",
    "integer64": "RECOVER_FF_UINT64",
}

# Scalars whose SOURCE TEXT carries something the parsed value cannot.
#
# Only `decimal`: FHIR counts trailing zeros as significant, and a binary64 has
# no spare bit pattern to record how many there were, so the slot keeps the
# scale in a 9th byte and the ingest has to read it off the raw token. Peeking
# is safe -- raw_json_token() reports the token without advancing the on-demand
# cursor, so the get_double() that follows still succeeds.
#
# Arrays are excluded: a std::vector<double> element has nowhere to put a
# per-item scale, so those store FF_DECIMAL_SIGFIGS_UNSPECIFIED and export via
# shortest-round-trip.
_SCALAR_NEEDS_TOKEN: frozenset = frozenset({"decimal"})


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

            condition = f'key.starts_with("{json_key}")' if is_choice else f'key == "{json_key}"'

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
                    elif c_type in _CHOICE_INT_TAGS:
                        # The tag is the ONLY thing naming a choice variant on the
                        # way out, so one tag standing in for several FHIR types
                        # renames the field: RECOVER_FF_UINT32 made every
                        # `valueInteger` export as `valueUnsignedInt` (4,396 pairs
                        # in Synthea alone). Emit the tag for THIS c_type instead.
                        #
                        # The tag must stay in the scalar band (0x01xx): a choice
                        # slot holds its value inline, and Recovery_to_Kind maps
                        # anything >= 0x0200 to FF_FIELD_BLOCK, which would make
                        # the reader treat the raw integer as a block offset. That
                        # rules out RECOVER_FF_POSITIVEINT (0x0230) and
                        # RECOVER_FF_UNSIGNEDINT (0x0237) despite their names --
                        # they are datatype-band tags. `positiveInt` therefore
                        # still exports as `unsignedInt`; giving it its own
                        # spelling needs a new scalar-band tag, which is a
                        # permanent ledger append (TASKS.md AR-5).
                        cpp += (
                            f"                uint64_t i_val;\n"
                            f"                if (field.value().get_uint64().get(i_val) == simdjson::SUCCESS) {{\n"
                            f"                    data.{cpp_name}.tag = {_CHOICE_INT_TAGS[c_type]};\n"
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
                    elif c_type == "code":
                        # A code variant carries RECOVER_FF_CODE, not
                        # RECOVER_FF_STRING: the tag is what routes the reader
                        # through code_node, which resolves a dictionary ID or a
                        # flagged FF_CODEABLE_CONCEPT fallback. Tagged as a
                        # string it exported as `valueString` and lost the
                        # dictionary encoding entirely. The POD value stays the
                        # raw text; the STORE side runs ENCODE_FF_CODE on it.
                        cpp += (
                            f"                std::string_view s_val;\n"
                            f"                if (field.value().get_string().get(s_val) == simdjson::SUCCESS) {{\n"
                            f"                    if (!s_val.empty()) {{\n"
                            f"                        data.{cpp_name}.tag = RECOVER_FF_CODE;\n"
                            f"                        data.{cpp_name}.value = s_val;\n"
                            f"                    }}\n"
                            f"                }}\n"
                        )
                    elif c_type in _tm.DATETIME_TYPES:
                        # DT-2: the four date/time types carry their OWN tag,
                        # never RECOVER_FF_STRING. In a choice slot the tag is
                        # the only thing naming the active variant, so tagging a
                        # dateTime as a string exported it as `effectiveString`
                        # -- 1,072 diffs on one Synthea fixture. The POD value
                        # stays the raw text; the STORE side packs it (or falls
                        # back to an FF_STRING) via ENCODE_FF_DATETIME, keyed on
                        # this tag.
                        cpp += (
                            f"                std::string_view s_val;\n"
                            f"                if (field.value().get_string().get(s_val) == simdjson::SUCCESS) {{\n"
                            f"                    if (!s_val.empty()) {{\n"
                            f"                        data.{cpp_name}.tag = {_tm.DATETIME_TYPES[c_type]};\n"
                            f"                        data.{cpp_name}.value = s_val;\n"
                            f"                    }}\n"
                            f"                }}\n"
                        )
                    elif c_type in _STRING_LIKE_TYPES:
                        # String-like primitives (url, base64Binary, canonical,
                        # etc.) have no _from_json function and no vtable —
                        # they're stored as string_view with the
                        # RECOVER_FF_STRING tag, exactly like "string".
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
                        # Complex type (HumanName, Address, CodeableConcept, etc.)
                        # Only emit _from_json call when the block is actually
                        # in master_blocks (some types like TriggerDefinition,
                        # UsageContext are not in the spec bundles).
                        # Age/Distance/Duration/Count are NOT rewritten to
                        # Quantity here, though FHIR does define them as
                        # profiles on it. They are in PRODUCTION_TYPES, so the
                        # generator already emits a full block, _from_json,
                        # SIZE/STORE and TypeTraits for each -- collapsing them
                        # discarded all of that and stamped the slot
                        # RECOVER_FF_QUANTITY, which is the only thing naming a
                        # choice variant. A source `valueAge` then exported as
                        # `valueQuantity`: the right number under a type name
                        # the element does not permit. The four layouts are
                        # byte-identical to Quantity's, so keeping them distinct
                        # costs nothing on the wire and keeps block tag and slot
                        # tag in agreement.
                        target_block = c_type
                        if target_block in master_blocks:
                            child_fn = f"{target_block}_from_json"
                            tag_name = f"RECOVER_FF_{target_block.upper()}"
                            cpp += (
                                f"                simdjson::ondemand::object obj_val;\n"
                                f"                if (field.value().get_object().get(obj_val) == simdjson::SUCCESS) {{\n"
                                f"                    if (builder) {{\n"
                                f"                        auto child_data = {child_fn}(obj_val, logger, concurrent_queue, builder);\n"
                                f"                        data.{cpp_name}.value = builder->append(child_data);\n"
                                f"                        data.{cpp_name}.tag = {tag_name};\n"
                                f"                    }} else if (logger) {{\n"
                                f'                        logger->log("[Warning] FastFHIR Ingestion: Cannot stage choice block {c_type} without a Builder.");\n'
                                f"                    }}\n"
                                f"                }} else {{\n"
                                f"                    {err_log}\n"
                                f"                }}\n"
                            )
                        else:
                            cpp += f'                if (logger) logger->log("[Warning] FastFHIR Ingestion: Unsupported choice type {c_type}");\n'
                    cpp += f"            }}\n"
                    is_first_choice = False
            elif f["is_array"]:
                fhir_type = f["fhir_type"]
                err_log_line = f"            {err_log}"
                cpp += f"            for (auto elem : field.value().get_array()) {{\n"
                if fhir_type == "string":
                    cpp += (
                        f"                std::string_view s;\n"
                        f"                if (elem.get_string().get(s) == simdjson::SUCCESS) "
                        f"data.{cpp_name}.emplace_back(s);\n"
                        f"                else {{ {err_log_line} }}\n"
                    )
                elif fhir_type == "code":
                    code_enum = f.get("code_enum")
                    cpp += (
                        f"                std::string_view c;\n"
                        f"                if (elem.get_string().get(c) == simdjson::SUCCESS) "
                    )
                    if code_enum:
                        cpp += (
                            f"data.{cpp_name}.emplace_back({code_enum['parse']}(std::string(c)));\n"
                        )
                    else:
                        # Same dangling-view hazard as the scalar case above:
                        # the element type is std::string_view.
                        cpp += f"data.{cpp_name}.emplace_back(c);\n"
                    cpp += f"                else {{ {err_log_line} }}\n"
                elif fhir_type in _tm.SCALAR_PRIMITIVE_TYPES:
                    accessor, staging, stored = _SCALAR_INGEST[fhir_type]
                    cpp += (
                        f"                {staging} v;\n"
                        f"                if (elem.{accessor}().get(v) == simdjson::SUCCESS) "
                        f"data.{cpp_name}.push_back({stored});\n"
                        f"                else {{ {err_log_line} }}\n"
                    )
                elif fhir_type in _STRING_LIKE_TYPES:
                    # Array of string-like primitives (e.g. dateTime[])
                    cpp += (
                        f"                std::string_view sv;\n"
                        f"                if (elem.get_string().get(sv) == simdjson::SUCCESS) "
                        f"data.{cpp_name}.emplace_back(sv);\n"
                        f"                else {{ {err_log_line} }}\n"
                    )
                elif fhir_type == "Resource":
                    # Contained resources — dispatch by resourceType
                    cpp += (
                        f"                auto res_obj = elem.get_object();\n"
                        f"                if (!res_obj.error()) {{\n"
                        f"                    if (builder) {{\n"
                        f"                        std::string_view child_type;\n"
                        f'                        if (res_obj["resourceType"].get_string().get(child_type) == simdjson::SUCCESS) {{\n'
                        f"                            FastFHIR::Reflective::ObjectHandle child = dispatch_resource(child_type, res_obj.value_unsafe(), *builder, logger);\n"
                        f"                            if (child.offset() != FF_NULL_OFFSET) {{\n"
                        f"                                data.{cpp_name}.emplace_back(child.offset(), child.recovery());\n"
                        f"                            }}\n"
                        f"                        }}\n"
                        f"                    }} else if (logger) {{\n"
                        f'                        logger->log("[Warning] FastFHIR Ingestion: Contained resource requires Builder context.");\n'
                        f"                    }}\n"
                        f"                }} else {{ {err_log_line} }}\n"
                    )
                else:
                    # BackboneElement/Element use resolved_path for unique name
                    if fhir_type in ("BackboneElement", "Element"):
                        child_fn_name = (
                            f.get("resolved_path", f"{path}.{f['orig_name']}").replace(".", "_")
                            + "_from_json"
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
                code_enum = f.get("code_enum")
                cpp += (
                    f"            std::string_view c;\n"
                    f"            if (field.value().get_string().get(c) == simdjson::SUCCESS) {{\n"
                )
                if code_enum:
                    # The temporary is consumed producing an enum value, so it
                    # is safe to materialise a std::string here.
                    cpp += (
                        f"                data.{cpp_name} = {code_enum['parse']}(std::string(c));\n"
                    )
                else:
                    # The field is a std::string_view. Assigning std::string(c)
                    # binds it to a temporary that dies at the end of the
                    # statement, leaving a dangling view that the store pass
                    # later reads as garbage. Keep the view into the JSON
                    # buffer, exactly as string/uri/markdown fields do.
                    cpp += f"                data.{cpp_name} = c;\n"
                cpp += (
                    f"            }} else {{\n" f"                {err_log}\n" f"            }}\n"
                )
            elif f["fhir_type"] in _tm.SCALAR_PRIMITIVE_TYPES:
                accessor, staging, stored = _SCALAR_INGEST[f["fhir_type"]]
                if f["fhir_type"] in _SCALAR_NEEDS_TOKEN and not f["is_array"]:
                    cpp += (
                        f"            auto __val = field.value();\n"
                        f"            std::string_view __tok = __val.raw_json_token();\n"
                        f"            {staging} v;\n"
                        f"            if (__val.{accessor}().get(v) == simdjson::SUCCESS) {{\n"
                        f"                data.{cpp_name} = {stored};\n"
                        f"                data.{cpp_name}{_tm.DECIMAL_SIGFIGS_SUFFIX}"
                        f" = FF_DecimalSigfigsFromToken(__tok);\n"
                        f"            }} else {{ {err_log} }}\n"
                    )
                else:
                    cpp += (
                        f"            {staging} v;\n"
                        f"            if (field.value().{accessor}().get(v) == simdjson::SUCCESS) "
                        f"data.{cpp_name} = {stored};\n"
                        f"            else {{ {err_log} }}\n"
                    )
            elif f["fhir_type"] == "Resource":
                cpp += (
                    f"            auto ref_obj = field.value().get_object();\n"
                    f"            if (!ref_obj.error()) {{\n"
                    f"                data.{cpp_name} = ResourceReference();\n"
                    f"            }}\n"
                )
            elif f["fhir_type"] in _STRING_LIKE_TYPES:
                # String-like primitives (url, dateTime, base64Binary,
                # canonical, date, etc.) — parse as string_view.
                # These have no _from_json function.
                if f.get("data_type") in ("std::string_view", "std::string"):
                    cpp += (
                        f"            std::string_view sv;\n"
                        f"            if (field.value().get_string().get(sv) == simdjson::SUCCESS) "
                        f"data.{cpp_name} = sv;\n"
                        f"            else {{ {err_log} }}\n"
                    )
                elif f.get("data_type") == "Offset":
                    # 0..1 string-like field (dateTime/date/instant/time/...).
                    # dateTime is absent from TYPE_MAP, so it falls through to
                    # the DEFAULT mapping: the POD member is
                    # unique_ptr<std::string_view> and the wire slot is an
                    # FF_STRING pointer. Parse the string and allocate; the
                    # store pass writes it. Previously this branch emitted a
                    # "requires Builder context" stub, so EVERY timestamp field
                    # was dropped and every Period block was born empty
                    # (TASKS.md A23).
                    cpp += (
                        f"            std::string_view sv;\n"
                        f"            if (field.value().get_string().get(sv) == simdjson::SUCCESS)\n"
                        f"                data.{cpp_name} = std::make_unique<std::string_view>(sv);\n"
                        f"            else {{ {err_log} }}\n"
                    )
                elif f.get("url_idx"):
                    # URL-directory ref: resolve the URL string through the
                    # Builder's interned registry (populated by predigestion
                    # before the workers run). Unknown/absent stays
                    # FF_NULL_UINT32 — the reader prints null.
                    cpp += (
                        f"            std::string_view sv;\n"
                        f"            if (field.value().get_string().get(sv) == simdjson::SUCCESS)\n"
                        f"                data.{cpp_name} = builder ? builder->resolve_extension_url(sv) : FF_NULL_UINT32;\n"
                        f"            else {{ {err_log} }}\n"
                    )
                else:
                    # other non-string storage — needs Builder context for the
                    # conversion; warn and skip.
                    cpp += (
                        f'            if (logger) logger->log("[Warning] FastFHIR Ingestion: '
                        f"Field {f['orig_name']} ({f['fhir_type']}) requires Builder context; skipping.\");\n"
                    )
            elif f["cpp_type"] == "Offset":
                # Complex block type — has a generated _from_json function.
                # BackboneElement/Element use resolved_path to get a unique
                # function name (e.g. Availability_availabletime_from_json).
                if f["fhir_type"] in ("BackboneElement", "Element"):
                    child_fn = (
                        f.get("resolved_path", f"{path}.{f['orig_name']}").replace(".", "_")
                        + "_from_json"
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
            else:
                cpp += f"            if (logger) logger->log(\"[Warning] FastFHIR Ingestion: Unsupported field type {f['fhir_type']} for {path}.{f['orig_name']}\");\n"
            cpp += "        }\n"

        cpp += "    }\n" "    return data;\n" "}\n\n"

    # ── Auto-generated dispatch_resource for contained / inline resources ──
    hpp += (
        "\n    FastFHIR::Reflective::ObjectHandle dispatch_resource("
        "std::string_view resource_type, simdjson::ondemand::object obj, "
        "FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger = nullptr);\n"
    )

    dispatch_is_first = True
    cpp += "\nFastFHIR::Reflective::ObjectHandle dispatch_resource("
    cpp += "std::string_view resource_type, simdjson::ondemand::object obj, "
    cpp += "FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger) {\n"
    for res in resources:
        prefix = "if" if dispatch_is_first else "else if"
        cpp += f'    {prefix} (resource_type == "{res}") '
        cpp += f"return builder.append_obj({res}_from_json(obj, logger, nullptr, &builder));\n"
        dispatch_is_first = False
    # Out-of-profile resource types land here. They used to be DISCARDED, which
    # cost a bundle 41 of 250 clinical records at exit 0 (TASKS.md A26); the log
    # line was added then, but a named loss is still a loss.
    #
    # Now the raw JSON is retained verbatim as an opaque block, so a resource the
    # profile cannot TYPE is still carried, exported and round-tripped. A profile
    # decides what this build can index, not what it may hold.
    #
    # "[Retained]" is the tag Ingestor::ingest_fhir_json greps for to summarise
    # these into the returned FF_Result; keep the two in sync.
    cpp += (
        "    // simdjson's raw_json() spans from the object's recorded start token,\n"
        '    // so the caller having already read "resourceType" off this object does\n'
        "    // not truncate it; consume() then skips to the closing brace from\n"
        "    // wherever the field iterator stands.\n"
        "    std::string_view __raw;\n"
        "    if (obj.raw_json().get(__raw) != simdjson::SUCCESS) {\n"
        '        if (logger) logger->log(std::string("[Warning] FastFHIR Ingestion: '
        "resource type '\") + std::string(resource_type) + \"' is outside this build's "
        'profile AND its raw JSON could not be re-read; the entry was discarded.");\n'
        "        return FastFHIR::Reflective::ObjectHandle(&builder, FF_NULL_OFFSET);\n"
        "    }\n"
        "    // The span ends at the NEXT structural token, so it can carry trailing\n"
        "    // whitespace from a pretty-printed document. Harmless when spliced back\n"
        "    // in, but it would be stored forever.\n"
        "    while (!__raw.empty() && static_cast<unsigned char>(__raw.back()) <= ' ')\n"
        "        __raw.remove_suffix(1);\n"
        '    if (logger) logger->log(std::string("[Retained] FastFHIR Ingestion: '
        "resource type '\") + std::string(resource_type) + \"' is not compiled into "
        "this build's resource profile (see FASTFHIR_PRODUCTION_PROFILE); it was stored "
        'as opaque JSON and round-trips intact, but its fields are not typed-queryable.");\n'
        "    return builder.append_opaque_json(__raw);\n"
        "}\n\n"
    )

    # ── dispatch_block — routes RECOVERY_TAG to the correct _from_json ──
    hpp += (
        "\n    FastFHIR::Reflective::ObjectHandle dispatch_block("
        "RECOVERY_TAG expected_tag, simdjson::ondemand::value& json_val, "
        "FastFHIR::Builder& builder, "
        "FastFHIR::ConcurrentLogger* logger = nullptr);\n"
    )
    cpp += (
        "\nFastFHIR::Reflective::ObjectHandle dispatch_block("
        "RECOVERY_TAG expected_tag, simdjson::ondemand::value& json_val, "
        "FastFHIR::Builder& builder, "
        "FastFHIR::ConcurrentLogger* logger) {\n"
        "    simdjson::ondemand::object obj;\n"
        "    if (json_val.get_object().get(obj) != simdjson::SUCCESS)\n"
        "        return FastFHIR::Reflective::ObjectHandle(&builder, FF_NULL_OFFSET);\n"
        "    switch (GetTypeFromTag(expected_tag)) {\n"
    )
    for path in sorted(master_blocks, key=lambda p: (p.count("."), p)):
        tag_name = "RECOVER_FF_" + path.replace(".", "_").upper()
        fn_name = path.replace(".", "_") + "_from_json"
        cpp += f"        case {tag_name}: return builder.append_obj({fn_name}(obj, logger, nullptr, &builder));\n"
    cpp += (
        "        default: return FastFHIR::Reflective::ObjectHandle(&builder, FF_NULL_OFFSET);\n"
        "    }\n"
        "}\n\n"
    )

    # ── Bundle patch function (used by concurrent ingest workers) ──
    bundle_entry_path = "Bundle.entry"
    if bundle_entry_path in master_blocks:
        patch_fn = f"patch_{bundle_entry_path.replace('.', '_')}_from_json"
        hpp += (
            f"void {patch_fn}(simdjson::ondemand::object& obj, "
            f"FastFHIR::Reflective::MutableEntry& wrapper, "
            f"FastFHIR::Builder& builder, "
            f"FastFHIR::ConcurrentLogger* logger = nullptr);\n"
        )
        entry_struct = "FF_" + bundle_entry_path.replace(".", "_").upper()
        entry_data = bundle_entry_path.replace(".", "") + "Data"
        entry_from_json = bundle_entry_path.replace(".", "_") + "_from_json"
        cpp += (
            f"// Fill one Bundle.entry whose V-Table header is already placed inline\n"
            f"// in the Bundle's entry array (the concurrent ingest preallocates the\n"
            f"// whole array, then hands each element to a worker).\n"
            f"//\n"
            f'// This used to be a hand-written loop that matched only "resource", so\n'
            f"// EVERY other field on EVERY entry of EVERY bundle was silently dropped\n"
            f"// -- fullUrl and request alone accounted for 1,444 of the round-trip\n"
            f"// diffs on a single Synthea fixture. The loop could not be extended\n"
            f"// field-by-field either: `MutableEntry::operator=` needs a TypeTraits\n"
            f"// specialization, and the array members (extension, modifierExtension,\n"
            f"// link) are std::vector<...Data>, which has none.\n"
            f"//\n"
            f"// So the block's OWN generated store writes it, exactly as it would\n"
            f"// anywhere else. One parser, one store, no second spelling of the\n"
            f"// layout to drift out of sync when a field is added.\n"
            f"void {patch_fn}(simdjson::ondemand::object& obj, "
            f"FastFHIR::Reflective::MutableEntry& wrapper, "
            f"FastFHIR::Builder& builder, "
            f"FastFHIR::ConcurrentLogger* logger) {{\n"
            f"    {entry_data} data = {entry_from_json}(obj, logger, nullptr, &builder);\n"
            f"\n"
            f"    // `resource` is the one field the generic parser cannot fill: the\n"
            f'    // concrete type is known only from the runtime "resourceType"\n'
            f"    // string, so it needs dispatch_resource. Everything else already\n"
            f"    // came back in `data` above.\n"
            f"    obj.reset();\n"
            f"    for (auto field : obj) {{\n"
            f'        if (field.unescaped_key().value_unsafe() != "resource") continue;\n'
            f"        simdjson::ondemand::object res_obj;\n"
            f"        if (field.value().get_object().get(res_obj) != simdjson::SUCCESS) break;\n"
            f"        std::string_view child_type;\n"
            f'        if (res_obj["resourceType"].get_string().get(child_type) != simdjson::SUCCESS) break;\n'
            f"        FastFHIR::Reflective::ObjectHandle child =\n"
            f"            dispatch_resource(child_type, res_obj, builder, logger);\n"
            f"        if (child.offset() != FF_NULL_OFFSET)\n"
            f"            data.resource = ResourceReference{{child.offset(), child.recovery()}};\n"
            f"        break;\n"
            f"    }}\n"
            f"\n"
            f"    // The header is already allocated; only the variable-length tail\n"
            f"    // needs arena space. Claim exactly what SIZE reports beyond the\n"
            f"    // header and hand STORE both offsets.\n"
            f"    const uint32_t __version = static_cast<uint32_t>(builder.FhirVersion());\n"
            f"    const Offset __hdr = wrapper.offset();\n"
            f"    const Size __total = SIZE_{entry_struct}(data, __version);\n"
            f"    const Size __child_bytes = __total - {entry_struct}::HEADER_SIZE;\n"
            f"    const Offset __child = __child_bytes\n"
            f"        ? builder.claim_child_space(__child_bytes)\n"
            f"        : __hdr + {entry_struct}::HEADER_SIZE;\n"
            f"    const Offset __end = STORE_{entry_struct}(\n"
            f"        builder.memory().base(), __hdr, __child, data, __version);\n"
            f"\n"
            f"    // Same SIZE/STORE contract Builder::append enforces. It has to be\n"
            f"    // repeated here because this call site claims the space itself: a\n"
            f"    // store that overruns its claim silently overwrites whichever\n"
            f"    // worker claimed next.\n"
            f"    if (__child_bytes && __end != __child + __child_bytes) {{\n"
            f"        throw std::runtime_error(\n"
            f'            "FastFHIR: SIZE/STORE contract violated patching {bundle_entry_path}: claimed " +\n'
            f'            std::to_string(__child_bytes) + " child bytes but store consumed " +\n'
            f'            std::to_string(__end - __child) + ". This is a generator bug.");\n'
            f"    }}\n"
            f"}}\n\n"
        )

    hpp += "} // namespace FastFHIR::Ingest\n"
    cpp += "} // namespace FastFHIR::Ingest\n"

    write_if_changed(os.path.join(output_dir, "FF_IngestMappings.hpp"), hpp)
    write_if_changed(os.path.join(output_dir, "FF_IngestMappings.cpp"), cpp)
    print(f"-- Emitted {output_dir}/FF_IngestMappings.{{hpp,cpp}}")
