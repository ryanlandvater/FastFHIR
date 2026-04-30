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

def _write_if_changed(path: str, content: str, encoding: str = "utf-8") -> None:
    """Write `content` to `path` only when the content has actually changed.
    Skipping the write preserves the file's mtime so the C++ build system
    does not consider dependent translation units stale on a no-op regeneration."""
    if os.path.exists(path):
        try:
            with open(path, "r", encoding=encoding) as fh:
                if fh.read() == content:
                    return
        except OSError:
            pass
    with open(path, "w", encoding=encoding) as fh:
        fh.write(content)

PRODUCTION_TYPES = [
    "Extension", 
    "Coding", "CodeableConcept", "Quantity", "Identifier",
    "Age", "Count", "Distance", "SimpleQuantity",
    "Range", "Period", "Reference", "Meta", "Narrative",
    "Annotation", "HumanName", "Address", "ContactPoint",
    "Attachment", "Ratio", "SampledData", "Duration", "Availability", "ExtendedContactDetail",
    "Timing", "Dosage", "Signature", "CodeableReference", "VirtualServiceDetail"
]

# US Core baseline resource set commonly used in Epic/US interoperability flows.
# Keep this curated and update as your target US Core version evolves.
US_CORE_RESOURCES = [
    "AllergyIntolerance", "Bundle", "CarePlan", "CareTeam", "Condition", "Coverage",
    "Device", "DiagnosticReport", "DocumentReference", "Encounter", "Goal",
    "Immunization", "Location", "Medication", "MedicationDispense",
    "MedicationRequest", "MedicationStatement", "Observation", "Organization",
    "Patient", "Practitioner", "PractitionerRole", "Procedure", "Provenance",
    "QuestionnaireResponse", "RelatedPerson", "ServiceRequest", "Specimen"
]

# UK Core baseline resource set for typical NHS/UK profile implementations.
# Keep this curated and update as your target UK Core version evolves.
UK_CORE_RESOURCES = [
    "AllergyIntolerance", "Appointment", "Bundle", "CarePlan", "CareTeam", "Condition",
    "DiagnosticReport", "Encounter", "Immunization", "Location", "Medication",
    "MedicationDispense", "MedicationRequest", "MedicationStatement", "Observation",
    "Organization", "Patient", "Practitioner", "Procedure", "QuestionnaireResponse",
    "RelatedPerson", "ServiceRequest", "Specimen"
]

# Production profile selector:
# - us (default): use curated US Core resource set
# - uk: use curated UK Core resource set
# - all: discover all concrete FHIR resources from profiles-resources.json
PRODUCTION_PROFILE_ENV = "FASTFHIR_PRODUCTION_PROFILE"

# =====================================================================
# 1. FASTFHIR TYPE MAPPING & NORMALIZATION
# =====================================================================
TYPE_MAP = {
    'boolean':      {'cpp': 'uint8_t',  'data_type': 'uint8_t', 'null': 'FF_NULL_UINT8',    'size': 1, 'size_const': 'TYPE_SIZE_UINT8',   'macro': 'LOAD_U8'},
    'integer':      {'cpp': 'uint32_t', 'data_type': 'uint32_t', 'null': 'FF_NULL_UINT32',  'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'},
    'unsignedInt':  {'cpp': 'uint32_t', 'data_type': 'uint32_t', 'null': 'FF_NULL_UINT32',  'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'},
    'positiveInt':  {'cpp': 'uint32_t', 'data_type': 'uint32_t', 'null': 'FF_NULL_UINT32',  'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'},
    'integer64':    {'cpp': 'uint64_t', 'data_type': 'uint64_t', 'null': 'FF_NULL_UINT64',  'size': 8, 'size_const': 'TYPE_SIZE_UINT64',  'macro': 'LOAD_U64'},
    'decimal':      {'cpp': 'double',   'data_type': 'double', 'null': 'FF_NULL_F64',       'size': 8, 'size_const': 'TYPE_SIZE_FLOAT64', 'macro': 'LOAD_F64'},
    'code':         {'cpp': 'uint32_t', 'data_type': 'std::string_view', 'null': '""',      'size': 4, 'size_const': 'TYPE_SIZE_UINT32',  'macro': 'LOAD_U32'}, 
    'Resource':     {'cpp': 'ResourceReference', 'data_type': 'ResourceReference', 'null': '{}', 'size': 10, 'size_const': 'TYPE_SIZE_RESOURCE', 'macro': 'LOAD_RESOURCE'},
    'CHOICE':       {'cpp': 'ChoiceEntry', 'data_type': 'ChoiceEntry', 'null': '{}', 'size': 10, 'size_const': 'TYPE_SIZE_CHOICE', 'macro': 'LOAD_VARIANT'},
    'DEFAULT':      {'cpp': 'Offset',   'data_type': 'Offset', 'null': 'FF_NULL_OFFSET',    'size': 8, 'size_const': 'TYPE_SIZE_OFFSET',  'macro': 'LOAD_U64'}  
}

BASE_BLOCK_HEADER_SIZE = 10

# Per-(type_path, field_name) layout overrides applied at merge time.
# Each entry fully replaces the field's name/size/type mapping so that
# the generator emits the overridden layout instead of the spec-derived one.
# Keys: (type_path, spec_field_name)
# Values: dict of attrs to overlay on the field dict; 'name' overrides the
#         vtable constant name; 'cpp_name' overrides the C++ member name.
BLOCK_FIELD_OVERRIDES = {
    # Extension.url: replace 8-byte Offset→FF_STRING with 4-byte uint32_t EXT_REF.
    # EXT_REF is a discriminated-union routing word:
    #   MSB=0 → URL_IDX   → FF_URL_DIRECTORY    (Path B, passive raw-JSON blob)
    #   MSB=1 → MODULE_IDX → FF_MODULE_REGISTRY (Path A, active WASM codec)
    # See FF_Primitives.hpp for FF_EXT_REF_MSB / ff_ext_ref_is_module() helpers.
    ('Extension', 'url'): {
        'name':       'EXT_REF',
        'cpp_name':   'ext_ref',
        'fhir_type':  'uint32_t',   # sentinel — not a real FHIR type
        'cpp_type':   'uint32_t',
        'data_type':  'uint32_t',
        'null':       'FF_NULL_UINT32',
        'size':       4,
        'size_const': 'TYPE_SIZE_UINT32',
        'macro':      'LOAD_U32',
        'raw_scalar': True,          # sentinel: emit direct LOAD_* in view getter
        'is_array':   False,
        'is_choice':  False,
    },
}

# Extra methods injected into specific *View<> template bodies.
# Key: FHIR type path (e.g. 'Extension').  Value: raw C++ method string.
VIEW_EXTRA_METHODS = {
    # Resolve the full extension URL via the stream-level FF_URL_DIRECTORY.
    # Honours the EXT_REF MSB: a module ref still has a URL recoverable from
    # the FF_MODULE_REGISTRY's url_idx field, but the simple form here only
    # handles Path B; Path A callers should use FF_WasmExtensionHost.
    'Extension': (
        "    // Path-B URL accessor (MSB=0).  Returns empty string for module refs (MSB=1).\n"
        "    inline std::string get_url(const BYTE* url_base, const FF_URL_DIRECTORY& dir) const {\n"
        "        uint32_t ref = LOAD_U32(url_base + offset + FF_EXTENSION::EXT_REF);\n"
        "        if (ref == FF_EXT_REF_NULL || ff_ext_ref_is_module(ref)) return {};\n"
        "        return dir.get_url(url_base, ff_ext_ref_index(ref));\n"
        "    }\n"
        "    inline bool     is_active_module() const {\n"
        "        return ff_ext_ref_is_module(LOAD_U32(base + offset + FF_EXTENSION::EXT_REF));\n"
        "    }\n"
        "    inline uint32_t module_idx() const {\n"
        "        uint32_t ref = LOAD_U32(base + offset + FF_EXTENSION::EXT_REF);\n"
        "        return ff_ext_ref_is_module(ref) ? ff_ext_ref_index(ref) : FF_NULL_UINT32;\n"
        "    }\n"
    ),
}

# Per-(type_path, field_name) custom ingest code snippets.
# These override the auto-generated ingest branch for the named field.
# Variables available in the snippet: field, data, builder, logger,
#   concurrent_queue, err_log, cpp_name (the C++ member name).
INGEST_FIELD_OVERRIDES = {
    # Extension.url is a JSON string but is stored as a 4-byte EXT_REF word.
    # Predigestion has already loaded Builder::m_url_retrieve with either a
    # URL_IDX (MSB=0, Path B) or a MODULE_IDX with the MSB pre-set (Path A).
    # The ingest worker calls resolve_extension_url() directly — no branching.
    ('Extension', 'url'): (
        "            std::string_view url_sv;\n"
        "            if (field.value().get_string().get(url_sv) == simdjson::SUCCESS) {{\n"
        "                data.ext_ref = builder ? builder->resolve_extension_url(url_sv) : FF_EXT_REF_NULL;\n"
        "            }} else {{\n"
        "                {err_log}\n"
        "            }}\n"
    ),
}

# Scalar FHIR types that are stored inline (fixed-size, no child block).
SCALAR_PRIMITIVE_TYPES = {'boolean', 'integer', 'unsignedInt', 'positiveInt', 'integer64', 'decimal'}

def _scalar_recovery_tag(fhir_type):
    """Return the RECOVERY_TAG constant for a scalar primitive type used in arrays."""
    if fhir_type == 'boolean': return 'RECOVER_FF_BOOL'
    if fhir_type == 'decimal': return 'RECOVER_FF_FLOAT64'
    if fhir_type == 'integer64': return 'RECOVER_FF_UINT64'
    return 'RECOVER_FF_UINT32'  # integer, unsignedInt, positiveInt

STRING_TYPES = {
    'string', 'id', 'oid', 'uuid', 'uri', 'url', 'canonical', 
    'markdown', 'date', 'datetime', 'instant', 'time', 'base64binary', 'xhtml'
}

auto_header = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
    )

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
    if fhir_type in TYPE_MAP and fhir_type != 'DEFAULT': 
        return TYPE_MAP[fhir_type]['data_type']
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
    if f['fhir_type'] == 'Resource':
        return 'RECOVER_FF_RESOURCE'
    elif f['fhir_type'] == 'code':
        return 'RECOVER_FF_CODE'
    elif f['fhir_type'] == 'boolean':
        return 'RECOVER_FF_BOOL'
    elif f['is_array']:
        if f['fhir_type'] in ('string', 'code'):
            return 'RECOVER_FF_STRING'
        if f['fhir_type'] in SCALAR_PRIMITIVE_TYPES:
            return _scalar_recovery_tag(f['fhir_type'])
        child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
        return f'RECOVER_{child_struct}'
    elif f['fhir_type'] == 'string':
        return 'RECOVER_FF_STRING'
    elif f['cpp_type'] == 'Offset':
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
    # Choice must be checked first — a choice field may have a fallback fhir_type
    # (e.g. 'string') that would otherwise match an earlier branch incorrectly.
    if f.get('is_choice'):
        return 'FF_FIELD_CHOICE'
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
    if f['data_type'] == 'uint64_t':
        return 'FF_FIELD_UINT64'
    if f['fhir_type'] == 'Resource':
        return 'FF_FIELD_RESOURCE'
    return 'FF_FIELD_UNKNOWN'

def _compact_slot_size(f):
    """Return the compact binary slot size (bytes) for a field, matching compact_slot_size() in FF_Parser.cpp."""
    if f.get('is_choice'):                              return 10  # TYPE_SIZE_CHOICE
    if f['fhir_type'] == 'Resource':                   return 10  # TYPE_SIZE_RESOURCE
    if f['fhir_type'] == 'boolean':                    return 1   # TYPE_SIZE_UINT8
    if f['fhir_type'] == 'code' and not f.get('is_array'): return 4  # TYPE_SIZE_UINT32 (code)
    if f.get('data_type') == 'uint32_t':               return 4   # TYPE_SIZE_UINT32/INT32
    if f.get('data_type') == 'double':                 return 8   # TYPE_SIZE_FLOAT64
    return 8  # string / array / block / offset -> TYPE_SIZE_OFFSET

def _compact_slot_size(f):
    """Return the compact binary slot size (bytes) for a field, matching compact_slot_size() in FF_Parser.cpp."""
    if f.get('is_choice'):               return 10  # TYPE_SIZE_CHOICE
    if f['fhir_type'] == 'Resource':     return 10  # TYPE_SIZE_RESOURCE
    if f['fhir_type'] == 'boolean':      return 1   # TYPE_SIZE_UINT8
    if f['fhir_type'] == 'code' and not f.get('is_array'): return 4  # TYPE_SIZE_UINT32 (code enum)
    if f.get('data_type') == 'uint32_t': return 4   # TYPE_SIZE_UINT32/INT32
    if f.get('data_type') == 'double':   return 8   # TYPE_SIZE_FLOAT64
    return 8  # string / array / block / offset → TYPE_SIZE_OFFSET

def _child_recovery_expr(f, block_struct_name):
    if f['fhir_type'] == 'Resource':
        return 'RECOVER_FF_RESOURCE'
    if f['is_array']:
        if f['fhir_type'] == 'code':
            return 'RECOVER_FF_CODE'
        if f['fhir_type'] in ('string', 'code'):
            return 'FF_STRING::recovery'
        if f['fhir_type'] in SCALAR_PRIMITIVE_TYPES:
            return _scalar_recovery_tag(f['fhir_type'])
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

def generate_lazy_view_struct(layout, block_struct_name, extra_methods=""):
    view_name = block_struct_name.replace("FF_", "") + "View"
    
    hpp = f"template <uint32_t VERSION = FHIR_VERSION_R5>\n"
    hpp += f"struct {view_name} {{\n"
    hpp += f"    const BYTE* const base;\n"
    hpp += f"    const Offset offset;\n\n"
    hpp += f"    inline bool is_null() const {{ return offset == FF_NULL_OFFSET; }}\n\n"
    
    for f in layout:
        if f['is_array']:
            ret_type = 'FF_ARRAY'
        elif f.get('is_choice'):
            ret_type = 'ChoiceEntry'
        elif f.get('raw_scalar'):
            ret_type = f['cpp_type']  # raw scalar override (e.g. uint32_t URL_IDX)
        elif f['fhir_type'] in ('string', 'code'):
            ret_type = 'std::string_view'
        elif f['fhir_type'] in TYPE_MAP and f['fhir_type'] not in ('DEFAULT', 'Resource', 'CHOICE'):
            ret_type = f['cpp_type']
        elif f['fhir_type'] == 'Resource':
            ret_type = 'ResourceReference'
        else:
            child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
            ret_type = child_struct.replace("FF_", "") + "View"
            
        hpp += f"    inline auto get_{f['cpp_name']}() const {{\n"
        
        # Compile-time schema drift bounds
        if f['first_version_idx'] > 0:
            hpp += f"        if constexpr (VERSION < FHIR_VERSION_{f['first_version_name']}) {{\n"
            scalar_types = ['FF_ARRAY', 'std::string_view', 'ResourceReference',
                            'FastFHIR::Reflective::Node', 'ChoiceEntry',
                            'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t',
                            'int32_t', 'int64_t', 'double', 'bool', 'Offset']
            if ret_type in scalar_types or f.get('raw_scalar'):
                if ret_type == 'FF_ARRAY':
                    hpp += f"            return FF_ARRAY(FF_NULL_OFFSET, 0, VERSION);\n"
                else:
                    hpp += f"            return {f['cpp_type'] if f.get('raw_scalar') else ret_type}{{}};\n"
            else:
                hpp += f"            return {ret_type}<VERSION>{{base, FF_NULL_OFFSET}};\n"
            hpp += f"        }}\n"
            
        vtable_off = f"{block_struct_name}::{f['name']}"
        
        if f['is_array']:
            hpp += f"        Offset child_off = LOAD_U64(base + offset + {vtable_off});\n"
            hpp += f"        return FF_ARRAY(child_off, 0, VERSION);\n"
        elif f.get('is_choice'):
            hpp += f"        return FastFHIR::Decode::choice(base, offset + {vtable_off});\n"
        elif f.get('raw_scalar'):
            hpp += f"        return {f['macro']}(base + offset + {vtable_off});\n"
        elif f['fhir_type'] in TYPE_MAP and f['fhir_type'] not in ('string', 'code', 'DEFAULT', 'Resource', 'CHOICE'):
            hpp += f"        return FastFHIR::Decode::scalar<{f['cpp_type']}>(base, offset + {vtable_off}, {_child_recovery_expr(f, block_struct_name)});\n"
        elif f['fhir_type'] in ('string', 'code'):
            hpp += f"        Offset child_off = LOAD_U64(base + offset + {vtable_off});\n"
            hpp += f"        if (child_off == FF_NULL_OFFSET) return std::string_view();\n"
            hpp += f"        return FF_STRING(child_off, 0, VERSION).read_view(base);\n"
        elif f['fhir_type'] == 'Resource':
            hpp += f"        Offset child_off = LOAD_U64(base + offset + {vtable_off});\n"
            hpp += f"        return ResourceReference{{child_off, static_cast<RECOVERY_TAG>(LOAD_U16(base + offset + {vtable_off} + DATA_BLOCK::RECOVERY))}};\n"
        else:
            hpp += f"        Offset child_off = LOAD_U64(base + offset + {vtable_off});\n"
            hpp += f"        return {ret_type}<VERSION>{{base, child_off}};\n"
            
        hpp += f"    }}\n"
        
    if extra_methods:
        hpp += extra_methods
    hpp += f"}};\n\n"
    return hpp

def generate_field_info_implementation(layout, block_struct_name):
    cpp = f'const FF_FieldInfo {block_struct_name}::FIELDS[{block_struct_name}::FIELD_COUNT] = {{\n'
    for f in layout:
        cpp += (
            f'    {{"{f["orig_name"]}", {_field_kind_expr(f)}, {block_struct_name}::{f["name"]}, '
            f'{_child_recovery_expr(f, block_struct_name)}, {_array_entries_are_offsets_expr(f)}, {_compact_slot_size(f)}}},\n'
        )
    cpp += '};\n'
    # Pre-baked compact slot sizes table; zero-padded to next multiple of 8 for SIMD single-shot load safety.
    stride = ((len(layout) + 7) // 8) * 8
    vals   = [str(_compact_slot_size(f)) for f in layout] + ['0'] * (stride - len(layout))
    cpp += f'alignas(8) const uint8_t {block_struct_name}::COMPACT_SLOT_SIZES[{block_struct_name}::COMPACT_SIZES_STRIDE] = {{{", ".join(vals)}}};\n'
    cpp += f'const FF_FieldInfo* {block_struct_name}::find_field(std::string_view name) const {{\n'
    cpp += f'    const FF_FieldInfo* fallback_choice = nullptr;\n'
    cpp += f'    for (size_t i = 0; i < FIELD_COUNT; ++i) {{\n'
    cpp += f'        // 1. Exact match ALWAYS wins (Protects against collisions)\n'
    cpp += f'        if (name == FIELDS[i].name) return &FIELDS[i];\n'
    cpp += f'        \n'
    cpp += f'        // 2. Cache a potential polymorphic prefix match\n'
    cpp += f'        if (FIELDS[i].kind == FF_FIELD_CHOICE && name.starts_with(FIELDS[i].name)) {{\n'
    cpp += f'            fallback_choice = &FIELDS[i];\n'
    cpp += f'        }}\n'
    cpp += f'    }}\n'
    cpp += f'    // 3. Return the choice field only if no exact match claimed it\n'
    cpp += f'    return fallback_choice;\n'
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
        "namespace Reflective { class Node; }\n"
        "std::vector<FF_FieldInfo> reflected_fields(uint16_t recovery);\n"
        "std::vector<std::string_view> reflected_keys(uint16_t recovery);\n"
        "Reflective::Node reflected_child_node(const BYTE* base, Size size, uint32_t version, Offset offset, uint16_t recovery, std::string_view key);\n"
        "std::string_view reflected_resource_type(uint16_t recovery);\n"
        "const uint8_t* compact_field_sizes(uint16_t recovery);\n"
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
        "const uint8_t* compact_sizes_for_block() {\n"
        "    return T_Block::COMPACT_SLOT_SIZES;\n"
        "}\n\n"
        "template <typename T_Block>\n"
        "Reflective::Node object_field_node(const T_Block& block, const BYTE* base, std::string_view key) {\n"
        "    const FF_FieldInfo* field = block.find_field(key);\n"
        "    if (!field) return {};\n\n"
        "    const Offset value_offset = block.__offset + field->field_offset;\n"
        "    if (FF_IsFieldEmpty(base, value_offset, field->kind)) return {};\n\n"
        "    Reflective::Entry entry{base, block.__offset, field->field_offset, field->child_recovery, field->kind};\n"
        "    return entry.as_node(block.__size, block.__version, field->child_recovery, field->kind);\n"
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
        "Reflective::Node reflected_child_node(const BYTE* base, Size size, uint32_t version, Offset offset, uint16_t recovery, std::string_view key) {\n"
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
        "const uint8_t* compact_field_sizes(uint16_t recovery) {\n"
        "    switch (recovery) {\n"
    )
    for s_name in block_struct_names:
        cpp += f"        case {s_name}::recovery: return compact_sizes_for_block<{s_name}>();\n"
    cpp += (
        "        default: return nullptr;\n"
        "    }\n"
        "}\n\n"
    )
    cpp += "} // namespace FastFHIR\n"

    return hpp, cpp

# =====================================================================
# 3. ZERO-COPY C++ GENERATION HELPERS
# =====================================================================
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

def generate_size_fields(layout, block_struct_name, data_name):
    cpp = f"    Size __total = {block_struct_name}::HEADER_SIZE;\n"
    for f in layout:
        if f.get('is_choice'):
            cpp += f"    std::visit([&](auto&& arg) {{\n"
            cpp += f"        using T = std::decay_t<decltype(arg)>;\n"
            cpp += f"        if constexpr (std::is_same_v<T, std::string_view>) {{\n"
            cpp += f"            if (!arg.empty()) __total += SIZE_FF_STRING(arg);\n"
            cpp += f"        }}\n"
            cpp += f"    }}, {data_name}.{f['cpp_name']}.value);\n"
        elif f['is_array']:
            if f['fhir_type'] == 'string':
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * TYPE_SIZE_OFFSET);\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                cpp += f"            __total += SIZE_FF_STRING(__item);\n"
                cpp += f"        }}\n    }}\n"
            elif f['fhir_type'] == 'code':
                code_enum = f.get('code_enum')
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * TYPE_SIZE_OFFSET);\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                if code_enum: cpp += f"            __total += SIZE_FF_CODE(std::string({code_enum['serialize']}(__item)), __version);\n"
                else:         cpp += f"            __total += SIZE_FF_CODE(__item, __version);\n"
                cpp += f"        }}\n    }}\n"
            elif f['fhir_type'] == 'Resource':
                # The primary array IS the array of ResourceReferences, so .size() works perfectly
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * TYPE_SIZE_RESOURCE);\n"
                cpp += f"    }}\n"
            elif f['fhir_type'] in SCALAR_PRIMITIVE_TYPES:
                size_const = TYPE_MAP[f['fhir_type']]['size_const']
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE + ({data_name}.{f['cpp_name']}.size() * {size_const});\n"
                cpp += f"    }}\n"
            else:
                child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        __total += FF_ARRAY::HEADER_SIZE;\n"
                cpp += f"        for (const auto& __item : {data_name}.{f['cpp_name']}) {{\n"
                cpp += f"            __total += SIZE_{child_struct}(__item);\n"
                cpp += f"        }}\n    }}\n"
                
        elif f['fhir_type'] == 'string':
            cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
            cpp += f"        __total += SIZE_FF_STRING({data_name}.{f['cpp_name']});\n    }}\n"
            
        elif f['cpp_type'] == 'Offset' and f['fhir_type'] != 'Resource':
            child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
            cpp += f"    if ({data_name}.{f['cpp_name']} != nullptr) {{\n"
            cpp += f"        __total += SIZE_{child_struct}(*{data_name}.{f['cpp_name']});\n    }}\n"
            
        elif f['fhir_type'] == 'code':
            code_enum = f.get('code_enum')
            if code_enum: cpp += f"    __total += SIZE_FF_CODE(std::string({code_enum['serialize']}({data_name}.{f['cpp_name']})), __version);\n"
            else:         cpp += f"    __total += SIZE_FF_CODE({data_name}.{f['cpp_name']}, __version);\n"
            
    cpp += "    return __total;\n"
    return cpp

def generate_store_fields(layout, block_struct_name, ptr_name, data_name):
    cpp = ""
    for f in layout:
        cpp += f"    // --- Store: {f['name']} ---\n"
        
        # --- NEW: Polymorphic Choice [x] Handling ---
        if f.get('is_choice'):
            vtable_off = f"{ptr_name} + {block_struct_name}::{f['name']}"
            
            cpp += f"    std::visit([&](auto&& arg) {{\n"
            cpp += f"        using T = std::decay_t<decltype(arg)>;\n"
            
            # 1. Monostate (Empty)
            cpp += f"        if constexpr (std::is_same_v<T, std::monostate>) {{\n"
            cpp += f"            STORE_U64({vtable_off}, FF_NULL_OFFSET);\n"
            cpp += f"            STORE_U16({vtable_off} + 8, FF_RECOVER_UNDEFINED);\n"
            cpp += f"        }}\n"
            
            # 2. Inline Scalars (Big-Endian Safe)
            cpp += f"        else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t> || std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t> || std::is_same_v<T, double>) {{\n"
            cpp += f"            STORE_U64({vtable_off}, FF_NULL_OFFSET);\n\n"
            cpp += f"            if constexpr (std::is_same_v<T, bool>) STORE_U8({vtable_off}, arg ? 1 : 0);\n"
            cpp += f"            else if constexpr (std::is_same_v<T, int32_t>) STORE_U32({vtable_off}, static_cast<uint32_t>(arg));\n"
            cpp += f"            else if constexpr (std::is_same_v<T, uint32_t>) STORE_U32({vtable_off}, arg);\n"
            cpp += f"            else if constexpr (std::is_same_v<T, int64_t>) STORE_U64({vtable_off}, static_cast<uint64_t>(arg));\n"
            cpp += f"            else if constexpr (std::is_same_v<T, uint64_t>) STORE_U64({vtable_off}, arg);\n"
            cpp += f"            else if constexpr (std::is_same_v<T, double>) STORE_F64({vtable_off}, arg);\n"
            cpp += f"            STORE_U16({vtable_off} + 8, {data_name}.{f['cpp_name']}.tag);\n"
            cpp += f"        }}\n"
            
            # 3. Variable-Length String Primitives
            cpp += f"        else if constexpr (std::is_same_v<T, std::string_view>) {{\n"
            cpp += f"            STORE_U64({vtable_off}, child_off);\n"
            cpp += f"            child_off += STORE_FF_STRING(__base, child_off, arg);\n"
            cpp += f"            STORE_U16({vtable_off} + 8, {data_name}.{f['cpp_name']}.tag);\n"
            cpp += f"        }}\n"
            
            # 4. Immediate Serialization Offsets (Quantity, CodeableConcept, etc.)
            cpp += f"        else if constexpr (std::is_same_v<T, Offset>) {{\n"
            cpp += f"            STORE_U64({vtable_off}, arg);\n"
            cpp += f"            STORE_U16({vtable_off} + 8, {data_name}.{f['cpp_name']}.tag);\n"
            cpp += f"        }}\n"
            cpp += f"    }}, {data_name}.{f['cpp_name']}.value);\n"

        # --- Existing Array / Legacy Logic ---
        elif f['is_array']:
            # --- PATTERN 1: ARRAY OF STRING/CODE OFFSETS ---
            # Physically: 8-byte pointers (Offsets) to variable-length strings elsewhere.
            if f['fhir_type'] in ('string', 'code'):
                code_enum = f.get('code_enum')
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                # Uses OFFSET with 8-byte hops
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::OFFSET, TYPE_SIZE_OFFSET, __n, ToArrayTag({_child_recovery_expr(f, block_struct_name)}));\n"
                cpp += f"        Offset blk_off_tbl = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * TYPE_SIZE_OFFSET;\n"
                cpp += f"        for (uint32_t blk_i = 0; blk_i < __n; ++blk_i) {{\n"
                cpp += f"            STORE_U64(__base + blk_off_tbl + blk_i * TYPE_SIZE_OFFSET, child_off);\n"
                if code_enum: cpp += f"            child_off += STORE_FF_STRING(__base, child_off, std::string({code_enum['serialize']}({data_name}.{f['cpp_name']}[blk_i])));\n"
                else:         cpp += f"            child_off += STORE_FF_STRING(__base, child_off, {data_name}.{f['cpp_name']}[blk_i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
            
            # --- PATTERN 2: ARRAY OF POLYMORPHIC RESOURCE TUPLES ---
            # Physically: 10-byte tuples (8-byte Offset + 2-byte Recovery Tag) stored inline.
            elif f['fhir_type'] == 'Resource':
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                # Uses INLINE_BLOCK with 10-byte hops
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_RESOURCE, __n, ToArrayTag({_child_recovery_expr(f, block_struct_name)}));\n"
                cpp += f"        Offset __entries_start = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * TYPE_SIZE_RESOURCE;\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                cpp += f"            STORE_U64(__base + __entries_start + __i * TYPE_SIZE_RESOURCE, {data_name}.{f['cpp_name']}[__i].offset);\n"
                cpp += f"            STORE_U16(__base + __entries_start + __i * TYPE_SIZE_RESOURCE + DATA_BLOCK::RECOVERY, {data_name}.{f['cpp_name']}[__i].recovery);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
            
            # --- PATTERN 3: ARRAY OF COMPLEX STRUCTS ---
            # --- PATTERN 4: ARRAY OF INLINE SCALAR PRIMITIVES ---
            elif f['fhir_type'] in SCALAR_PRIMITIVE_TYPES:
                size_const = TYPE_MAP[f['fhir_type']]['size_const']
                recovery   = _scalar_recovery_tag(f['fhir_type'])
                store_mac  = {'boolean': 'STORE_U8', 'decimal': 'STORE_F64'}.get(f['fhir_type'], 'STORE_U32')
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::INLINE_BLOCK, {size_const}, __n, ToArrayTag({recovery}));\n"
                cpp += f"        Offset __entries_start = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * {size_const};\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                cpp += f"            {store_mac}(__base + __entries_start + __i * {size_const}, {data_name}.{f['cpp_name']}[__i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
            # --- PATTERN 3: ARRAY OF COMPLEX STRUCTS ---
            else:
                child_struct = _resolve_ff_struct_name(f['fhir_type'], f['name'], block_struct_name, f.get('resolved_path'))
                store_fn = f"STORE_{child_struct}"
                
                # Branch 3a: Inline Data (Bundle.entry, Patient.name)
                # Physically: Full structs stored back-to-back.
                cpp += f"    if (!{data_name}.{f['cpp_name']}.empty()) {{\n"
                cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, child_off);\n"
                cpp += f"        auto __n = static_cast<uint32_t>({data_name}.{f['cpp_name']}.size());\n"
                # Uses INLINE_BLOCK with full struct header-size hops
                cpp += f"        STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::INLINE_BLOCK, {child_struct}::HEADER_SIZE, __n, ToArrayTag({_child_recovery_expr(f, block_struct_name)}));\n"
                cpp += f"        Offset __entries_start = child_off;\n"
                cpp += f"        child_off += static_cast<Offset>(__n) * {child_struct}::HEADER_SIZE;\n"
                cpp += f"        for (uint32_t __i = 0; __i < __n; ++__i) {{\n"
                cpp += f"            child_off = {store_fn}(__base, __entries_start + __i * {child_struct}::HEADER_SIZE, child_off, {data_name}.{f['cpp_name']}[__i]);\n"
                cpp += f"        }}\n"
                cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
        
        elif f['fhir_type'] == 'Resource':
            cpp += f"    if ({data_name}.{f['cpp_name']}.offset != FF_NULL_OFFSET) {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, {data_name}.{f['cpp_name']}.offset);\n"
            cpp += f"        STORE_U16({ptr_name} + {block_struct_name}::{f['name']} + DATA_BLOCK::RECOVERY, {data_name}.{f['cpp_name']}.recovery);\n"
            cpp += f"    }} else {{\n"
            cpp += f"        STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET);\n"
            cpp += f"        STORE_U16({ptr_name} + {block_struct_name}::{f['name']} + DATA_BLOCK::RECOVERY, FF_RECOVER_UNDEFINED);\n"
            cpp += f"    }}\n"
            
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
            cpp += f"        child_off = {store_fn}(__base, nested_hdr, child_off, *{data_name}.{f['cpp_name']});\n"
            cpp += f"    }} else {{ STORE_U64({ptr_name} + {block_struct_name}::{f['name']}, FF_NULL_OFFSET); }}\n"
            
        elif f['fhir_type'] == 'code':
            code_enum = f.get('code_enum')
            val_str = f"std::string({code_enum['serialize']}({data_name}.{f['cpp_name']}))" if code_enum else f"std::string({data_name}.{f['cpp_name']})"
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
            raw_field_name = path.split('.')[-1]
            field_name = raw_field_name.replace('[x]', '')
            is_choice = '[x]' in raw_field_name
            choice_types = [t.get('code') for t in el.get('type', [])] if is_choice else []
            
            if parent_path not in master_blocks: master_blocks[parent_path] = {'layout': [], 'seen': set(), 'sizes': {}}
            blk = master_blocks[parent_path]
            f_type = sanitize_fhir_type(el.get('type', [{'code': 'BackboneElement'}])[0].get('code', 'BackboneElement'))
            is_array = el.get('max') == '*'
            mapping = TYPE_MAP['DEFAULT'] if (is_array or f_type not in TYPE_MAP) else TYPE_MAP[f_type]
            if field_name not in blk['seen']:

                if is_choice: mapping = TYPE_MAP['CHOICE']
                else: mapping = TYPE_MAP['DEFAULT'] if (is_array or f_type not in TYPE_MAP) else TYPE_MAP[f_type]

                off = 10 if not blk['layout'] else blk['layout'][-1]['offset'] + blk['layout'][-1]['size']

                # C++ Keyword Sanitization
                cpp_safe_name = field_name.lower()
                if cpp_safe_name in ["class", "template", "namespace", "operator", "new", "delete", "default", "struct", "enum", "concept", "requires", "export", "import", "module"]:
                    cpp_safe_name += "_"
                field_entry = {
                    'name': field_name.upper(), 'cpp_name': cpp_safe_name, 'orig_name': field_name,
                    'is_choice': is_choice,             # Ensure these are passed
                    'choice_types': choice_types,       # for the generator
                    'is_array': is_array, 'fhir_type': f_type, 'size': mapping['size'],
                    'size_const': mapping['size_const'], 'cpp_type': mapping['cpp'],
                    'data_type': mapping['data_type'], 'macro': mapping['macro'],
                    'first_version_name': v_name, 'first_version_idx': v_idx, 'offset': off
                }
                # Apply per-field layout overrides (e.g. Extension.url → URL_IDX uint32_t)
                override = BLOCK_FIELD_OVERRIDES.get((parent_path, field_name))
                if override:
                    field_entry.update(override)
                    # Recalculate size from the override so the offset chain is correct
                    field_entry['offset'] = off
                blk['layout'].append(field_entry)
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
    """Returns (public_hpp, internal_hpp, cpp).

    public_hpp  – Data structs, free-function declarations, bridge decls, TypeTraits.
                  Safe to ship in the public API; contains NO vtable offset enums.
    internal_hpp – FF_* DATA_BLOCK structs (vtable_sizes/vtable_offsets enums) and
                   *View<VERSION> zero-copy templates.  Include via FF_AllTypes.hpp only.
    cpp          – All implementations plus two bridge functions per block:
                     • STORE_{s_name}(base, start_off, data, version)  — 2-param, non-inline
                     • FF_DESERIALIZE_{s_name}(...)                     — thin wrapper around ::deserialize
    """
    public_hpp, internal_hpp, cpp = "", "", ""
    traits_hpp = ""
    block_data_names = sorted({path.replace('.', '') + "Data" for path in master_blocks})
    block_struct_names = sorted({"FF_" + path.replace('.', '_').upper() for path in master_blocks})
    block_view_names = sorted({s_name.replace("FF_", "") + "View" for s_name in block_struct_names})
    # Only Data struct forward declarations belong in the public header
    for d_name in block_data_names: public_hpp += f"struct {d_name};\n"
    if block_data_names: public_hpp += "\n"
    # FF_* vtable structs and *View templates are internal-only
    for s_name in block_struct_names: internal_hpp += f"struct {s_name};\n"
    for v_name in block_view_names: internal_hpp += f"template <uint32_t VERSION> struct {v_name};\n"
    if block_struct_names: internal_hpp += "\n"

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
        
        # ── PUBLIC: POD Data Struct ──────────────────────────────────────────
        public_hpp += f"struct {d_name} {{\n"
        for f in layout:
            code_enum = f.get('code_enum')
            if f.get('is_choice'): public_hpp += f"    ChoiceEntry {f['cpp_name']};\n"
            elif f['is_array']:
                item_type = code_enum['enum'] if code_enum else _resolve_data_type_name(f['fhir_type'], f['orig_name'], path, f.get('resolved_path'))
                public_hpp += f"    std::vector<{item_type}> {f['cpp_name']};\n"
            elif f['fhir_type'] == 'string': public_hpp += f"    std::string_view {f['cpp_name']};\n"
            elif code_enum: public_hpp += f"    {code_enum['enum']} {f['cpp_name']} = {code_enum['enum']}::Unknown;\n"
            elif f['cpp_type'] == 'Offset': public_hpp += f"    std::unique_ptr<{_resolve_data_type_name(f['fhir_type'], f['orig_name'], path, f.get('resolved_path'))}> {f['cpp_name']};\n"
            elif f['data_type'] == 'bool': public_hpp += f"    bool {f['cpp_name']} = false;\n"
            elif f['data_type'] == 'std::string': public_hpp += f"    std::string {f['cpp_name']};\n"
            elif f['fhir_type'] in TYPE_MAP and 'null' in TYPE_MAP[f['fhir_type']]:
                null_val = TYPE_MAP[f['fhir_type']]['null']
                public_hpp += f"    {f['data_type']} {f['cpp_name']} = {null_val};\n"
            else: public_hpp += f"    {f['data_type']} {f['cpp_name']}{{}};\n"
        public_hpp += f"}};\n\n"

        # ── INTERNAL: Data Block Sentinel (vtable enums) ─────────────────────
        internal_hpp += f"struct FF_EXPORT {s_name} : DATA_BLOCK {{\n"
        internal_hpp += f"    static constexpr char type [] = \"{s_name}\";\n"
        internal_hpp += f"    static constexpr enum RECOVERY_TAG recovery = RECOVER_{s_name};\n"
        internal_hpp += f"    enum vtable_sizes {{\n        VALIDATION_S = TYPE_SIZE_UINT64,\n        RECOVERY_S = TYPE_SIZE_UINT16,\n"
        for f in layout: internal_hpp += f"        {f['name']}_S = {f['size_const']},\n"
        internal_hpp += f"    }};\n    enum vtable_offsets {{\n        VALIDATION = 0,\n        RECOVERY = VALIDATION + VALIDATION_S,\n"
        prev_name, prev_size = "RECOVERY", "RECOVERY_S"
        for f in layout:
            internal_hpp += f"        {f['name']:<20}= {prev_name} + {prev_size},\n"
            prev_name, prev_size = f['name'], f"{f['name']}_S"
        for v, sz in sizes.items(): internal_hpp += f"        HEADER_{v}_SIZE = {sz},\n"
        present_versions = [v for v in versions if v in sizes]
        max_v = max(present_versions, key=lambda x: versions.index(x))
        internal_hpp += f"        HEADER_SIZE = HEADER_{max_v}_SIZE\n    }};\n"
        
        min_version = next((v for v in versions if v in sizes), None)
        internal_hpp += f"    inline Size get_header_size() const {{\n"
        if min_version: internal_hpp += f"        if (__version < FHIR_VERSION_{min_version}) return 0;\n"
        for v in versions:
            if v in sizes: internal_hpp += f"        if (__version <= FHIR_VERSION_{v}) return HEADER_{v}_SIZE;\n"
        internal_hpp += f"        return HEADER_SIZE;\n    }}\n"

        internal_hpp += f"    explicit {s_name}(Offset off, Size total_size, uint32_t ver) : DATA_BLOCK(off, total_size, ver) {{}}\n"
        internal_hpp += f"    static constexpr size_t FIELD_COUNT = {len(layout)};\n"
        internal_hpp += f"    static const FF_FieldInfo FIELDS[FIELD_COUNT];\n"
        internal_hpp += f"    static constexpr size_t COMPACT_SIZES_STRIDE = {((len(layout)+7)//8)*8};\n"
        internal_hpp += f"    static const uint8_t COMPACT_SLOT_SIZES[COMPACT_SIZES_STRIDE];  // pre-baked, 8-aligned\n"
        internal_hpp += f"    FF_Result validate_full(const BYTE* const __base) const noexcept;\n\n"
        internal_hpp += f"    static {d_name} deserialize(const BYTE* const __base, Offset __offset, Size __size, uint32_t __version);\n"
        internal_hpp += f"    const FF_FieldInfo* find_field(std::string_view name) const;\n"
        internal_hpp += f"}};\n\n"
        
        # ── INTERNAL: Zero-copy View Template ───────────────────────────────
        internal_hpp += generate_lazy_view_struct(layout, s_name, extra_methods=VIEW_EXTRA_METHODS.get(path, ""))

        # ── INTERNAL: 3-arg STORE (requires layout knowledge of hdr_off/child_off split) ─
        internal_hpp += f"Offset STORE_{s_name}(BYTE* const __base, Offset hdr_off, Offset child_off, const {d_name}& data, uint32_t __version = FHIR_VERSION_R5);\n\n"

        # Bridge name strips the FF_ prefix to avoid FF_DESERIALIZE_FF_OBSERVATION noise
        bridge_name = s_name[3:] if s_name.startswith("FF_") else s_name

        # ── PUBLIC: Free-function declarations (no vtable references) ────────
        public_hpp += f"Size SIZE_{s_name}(const {d_name}& data, uint32_t __version = FHIR_VERSION_R5);\n"
        # 2-param convenience overload — clean entry point for API callers
        public_hpp += f"Offset STORE_{s_name}(BYTE* const __base, Offset start_off, const {d_name}& data, uint32_t __version = FHIR_VERSION_R5);\n"
        # Bridge deserializer: decouples TypeTraits from the vtable struct
        public_hpp += f"{d_name} FF_DESERIALIZE_{bridge_name}(const BYTE* const __base, Offset __offset, Size __size, uint32_t __version);\n\n"

        # ── PUBLIC: TypeTraits — references only RECOVER_* and bridge functions
        traits_hpp += f"namespace FastFHIR {{\n"
        traits_hpp += f"template<> struct TypeTraits<{d_name}> {{\n"
        traits_hpp += f"    static constexpr auto recovery = RECOVER_{s_name};\n"
        traits_hpp += f"    static Size size(const {d_name}& d, uint32_t v = FHIR_VERSION_R5) {{ return SIZE_{s_name}(d, v); }}\n"
        traits_hpp += f"    static void store(BYTE* const base, Offset off, const {d_name}& d, uint32_t v = FHIR_VERSION_R5) {{ STORE_{s_name}(base, off, d, v); }}\n"
        traits_hpp += f"    static {d_name} read(const BYTE* const base, Offset off, Size size, uint32_t v) {{ return FF_DESERIALIZE_{bridge_name}(base, off, size, v); }}\n"
        traits_hpp += f"}};\n"
        traits_hpp += f"}} // namespace FastFHIR\n\n"

        # ── CPP: Implementations ─────────────────────────────────────────────
        cpp += f"FF_Result {s_name}::validate_full(const BYTE *const __base) const noexcept {{ return validate_offset(__base, type, recovery); }}\n"
        cpp += generate_field_info_implementation(layout, s_name)
        
        cpp += f"{d_name} {s_name}::deserialize(const BYTE *const __base, Offset __offset, Size __size, uint32_t __version) {{\n"
        cpp += f"{generate_eager_deserializer(layout, s_name, d_name)}}}\n\n"
        
        cpp += f"Size SIZE_{s_name}(const {d_name}& data, uint32_t __version) {{\n{generate_size_fields(layout, s_name, 'data')}}}\n\n"
        
        cpp += f"Offset STORE_{s_name}(BYTE* const __base, Offset hdr_off, Offset child_off, const {d_name}& data, uint32_t __version) {{\n"
        cpp += f"    auto __ptr = __base + hdr_off;\n"
        cpp += f"    STORE_U64(__ptr + {s_name}::VALIDATION, hdr_off);\n"
        cpp += f"    STORE_U16(__ptr + {s_name}::RECOVERY, {s_name}::recovery);\n"
        cpp += f"{generate_store_fields(layout, s_name, '__ptr', 'data')}\n"
        cpp += f"    return child_off;\n}}\n\n"

        # ── CPP: Bridge implementations (no #include of internal header needed by callers)
        cpp += f"Offset STORE_{s_name}(BYTE* const __base, Offset start_off, const {d_name}& data, uint32_t __version) {{\n"
        cpp += f"    return STORE_{s_name}(__base, start_off, start_off + {s_name}::HEADER_SIZE, data, __version);\n"
        cpp += f"}}\n\n"
        cpp += f"{d_name} FF_DESERIALIZE_{bridge_name}(const BYTE* const __base, Offset __offset, Size __size, uint32_t __version) {{\n"
        cpp += f"    return {s_name}::deserialize(__base, __offset, __size, __version);\n"
        cpp += f"}}\n\n"

    public_hpp += traits_hpp
    return public_hpp, internal_hpp, cpp

# =====================================================================
# 5. RECOVERY TAG GENERATOR
# =====================================================================
def generate_recovery_header(target_types, resources, all_block_paths, output_dir="generated_src"):
    """
    Auto-generate FF_Recovery.hpp containing the RECOVERY enum from
    the discovered data types, resources, and backbone elements.
    """

    # Categorize block paths into data types, resources, and backbone elements
    type_set = set(target_types)
    resource_set = set(resources)
    data_type_paths = []
    resource_paths = []
    backbone_paths = []

    candidate_paths = set(all_block_paths)

    # Some profiled datatypes (e.g., Availability in R5 profiles-types) can be
    # materialized in generated code while missing from all_block_paths in some
    # spec bundle shapes. Ensure their recovery tags are always present.
    if "Availability" in type_set:
        candidate_paths.update({
            "Availability",
            "Availability.availableTime",
            "Availability.notAvailableTime",
        })

    # Ensure all resource top-level names are always present as candidates.
    # merge_fhir_versions only adds parent paths derived from child elements, so
    # if schema extraction silently fails (except: pass) for a resource, its name
    # never enters all_block_paths and resource_paths stays empty.
    candidate_paths.update(resources)

    for path in candidate_paths:
        root = path.split('.')[0]
        is_nested = '.' in path
        if not is_nested and root in type_set:
            data_type_paths.append(path)
        elif not is_nested and root in resource_set:
            resource_paths.append(path)
        else:
            backbone_paths.append(path)

    # Preserve type/resource ordering for stable ABI
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
    lines.append("    RECOVER_FF_CODE                       = 0x0003,")
    lines.append("    RECOVER_FF_RESOURCE                   = 0x0004,")
    lines.append("    RECOVER_FF_CHECKSUM                   = 0x0005,")
    lines.append("    RECOVER_FF_URL_DIRECTORY              = 0x0006, // Stream-level URL intern table")
    lines.append("    RECOVER_FF_MODULE_REGISTRY            = 0x0007, // WASM extension module registry")
    lines.append("    RECOVER_FF_OPAQUE_JSON                = 0x0008, // Path B passive raw-JSON extension blob")
    lines.append("    RECOVER_FF_WASM_PAYLOAD               = 0x0009, // Path A WASM-encoded extension payload")
    
    lines.append("")
    lines.append("    // --- Inline Scalars (0x0100 Block) ---")
    lines.append("    RECOVER_FF_SCALAR_BLOCK               = 0x0100,")
    lines.append("    RECOVER_FF_BOOL                       = 0x0101,")
    lines.append("    RECOVER_FF_INT32                      = 0x0102,")
    lines.append("    RECOVER_FF_UINT32                     = 0x0103,")
    lines.append("    RECOVER_FF_INT64                      = 0x0104,")
    lines.append("    RECOVER_FF_UINT64                     = 0x0105,")
    lines.append("    RECOVER_FF_FLOAT64                    = 0x0106,")
    lines.append("    RECOVER_FF_DATE                       = 0x0107, // Reserved for bit-packing")
    lines.append("    RECOVER_FF_DATETIME                   = 0x0108, // Reserved for bit-packing")
    lines.append("    RECOVER_FF_TIME                       = 0x0109, // Reserved for bit-packing")
    lines.append("    RECOVER_FF_INSTANT                    = 0x010A, // Reserved for bit-packing")

    if data_type_paths:
        lines.append("")
        lines.append("    // Data Types (0x0200 Block)")
        lines.append("    RECOVER_FF_DATA_TYPE_BLOCK            = 0x0200,")
        for i, path in enumerate(data_type_paths):
            tag_name = f"RECOVER_FF_{path.replace('.', '_').upper()}"
            lines.append(f"    {tag_name:<44}= 0x{0x0200 + i+1:04X},")

    # Handwritten FastFHIR structural types are emitted in the Core Primitives block above
    if resource_paths:
        lines.append("")
        lines.append("    // Resources (0x0300 Block)")
        lines.append("    RECOVER_FF_RESOURCE_BLOCK            = 0x0300,")
        for i, path in enumerate(resource_paths):
            tag_name = f"RECOVER_FF_{path.replace('.', '_').upper()}"
            lines.append(f"    {tag_name:<44}= 0x{0x0300 + i+1:04X},")

    if backbone_paths:
        lines.append("")
        lines.append("    // Sub-elements / BackboneElements (0x0400 Block)")
        lines.append("    RECOVER_FF_BACKBONE_BLOCK             = 0x0400,")
        for i, path in enumerate(backbone_paths):
            tag_name = f"RECOVER_FF_{path.replace('.', '_').upper()}"
            lines.append(f"    {tag_name:<44}= 0x{0x0400 + i+1:04X},")

    lines.append("};")
    lines.append("")
    lines.append("constexpr uint16_t RECOVER_ARRAY_BIT = 0x8000;")
    lines.append("constexpr uint16_t RECOVER_TYPE_MASK = 0x7FFF;")
    lines.append("inline constexpr bool IsArrayTag(RECOVERY_TAG tag) {return(tag & RECOVER_ARRAY_BIT)!= 0;}")
    lines.append("inline constexpr RECOVERY_TAG GetTypeFromTag(RECOVERY_TAG tag) {return static_cast<RECOVERY_TAG>(tag & RECOVER_TYPE_MASK);}")
    lines.append("inline constexpr RECOVERY_TAG ToArrayTag(RECOVERY_TAG base_tag) {return static_cast<RECOVERY_TAG>(base_tag | RECOVER_ARRAY_BIT);}")

    out_path = os.path.join(output_dir, "FF_Recovery.hpp")
    _write_if_changed(out_path, "\n".join(lines))
    print(f"Generated {out_path}")

# =====================================================================
# 6. INGESTOR MAPPINGS GENERATOR (simdjson bridge)
# =====================================================================
def generate_ingest_mappings(master_blocks, resources, output_dir="generated_src"):
    import os

    hpp = (
        f"{auto_header}"
        f"#pragma once\n"
        f"#include \"FF_AllTypes.hpp\"\n"
        f"#include \"FF_Logger.hpp\"\n"
        f"#include <simdjson.h>\n\n"
        f"namespace FastFHIR::Ingest {{\n\n"
    )
    
    cpp = (
        f"{auto_header}"
        f"#include \"FF_IngestMappings.hpp\"\n\n"
        f"namespace FastFHIR::Ingest {{\n\n"
        f"// Internal Parser Forward Declarations\n"
    )

    for path, blk in master_blocks.items():
        if path == "Bundle": continue
        data_type = path.replace(".", "") + "Data"
        fn_name = path.replace(".", "_") + "_from_json"
        # The signature allows for metadata scraping (concurrent_queue) or full building (builder)
        cpp += (
            f"static {data_type} {fn_name}("
            f"simdjson::ondemand::object obj, "
            f"FastFHIR::ConcurrentLogger* logger = nullptr, "
            f"std::vector<std::string_view>* concurrent_queue = nullptr, "
            f"FastFHIR::Builder* builder = nullptr);\n"
        )
    
    cpp += "\n"

    # --- 1. POD PARSING (Main Thread Skeleton / Worker Thread Builder) ---
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
            json_key, cpp_name = f["orig_name"], f["cpp_name"]
            is_choice = f.get("is_choice", False)
            err_log = f"if (logger) logger->log(\"[Warning] FastFHIR Ingestion: Malformed data at {path}.{json_key}\");"

            condition = f"key.starts_with(\"{json_key}\")" if is_choice else f"key == \"{json_key}\""

            if is_first:
                cpp += f"        if ({condition}) {{\n"
            else:
                cpp += f"        else if ({condition}) {{\n"
            is_first = False
            
            if is_choice:
                cpp += f"            std::string_view suffix = key.substr({len(json_key)});\n"
                is_first_choice = True
                
                for c_type in f.get("choice_types", []):
                    suffix_match = c_type[0].upper() + c_type[1:]
                    cond = f"if (suffix == \"{suffix_match}\")" if is_first_choice else f"else if (suffix == \"{suffix_match}\")"
                    
                    cpp += f"            {cond} {{\n"
                    
                    # Route primitive scalars
                    if c_type == "boolean":
                        cpp += f"                bool b_val;\n"
                        cpp += f"                if (field.value().get_bool().get(b_val) == simdjson::SUCCESS) {{\n"
                        cpp += f"                    data.{cpp_name}.tag = RECOVER_FF_BOOL;\n"
                        cpp += f"                    data.{cpp_name}.value = b_val;\n"
                        cpp += f"                }}\n"
                    elif c_type in ["integer", "positiveInt", "unsignedInt", "integer64"]:
                        cpp += f"                uint64_t i_val;\n"
                        cpp += f"                if (field.value().get_uint64().get(i_val) == simdjson::SUCCESS) {{\n"
                        cpp += f"                    data.{cpp_name}.tag = (suffix == \"Integer64\") ? RECOVER_FF_UINT64 : RECOVER_FF_UINT32;\n"
                        cpp += f"                    data.{cpp_name}.value = i_val;\n"
                        cpp += f"                }}\n"
                    elif c_type == "decimal":
                        cpp += f"                double d_val;\n"
                        cpp += f"                if (field.value().get_double().get(d_val) == simdjson::SUCCESS) {{\n"
                        cpp += f"                    data.{cpp_name}.tag = RECOVER_FF_FLOAT64;\n"
                        cpp += f"                    data.{cpp_name}.value = d_val;\n"
                        cpp += f"                }}\n"
                    
                    # Route string-like primitives (Expanded)
                    elif c_type in ["string", "code", "id", "markdown", "uri", "url", "canonical", "oid", "base64Binary", "date", "dateTime", "instant", "time"]:
                        cpp += f"                std::string_view s_val;\n"
                        cpp += f"                if (field.value().get_string().get(s_val) == simdjson::SUCCESS) {{\n"
                        cpp += f"                    data.{cpp_name}.tag = RECOVER_FF_STRING;\n"
                        cpp += f"                    data.{cpp_name}.value = s_val;\n"
                        cpp += f"                }}\n"
                    
                    # Route complex blocks (Immediate Serialization with Fallbacks)
                    else:
                        target_block = c_type
                        if c_type in ["Age", "Distance", "Duration", "Count"] and "Quantity" in master_blocks:
                            target_block = "Quantity"
                            
                        if target_block in master_blocks:
                            child_fn = f"{target_block}_from_json"
                            tag_name = f"RECOVER_FF_{target_block.upper()}"
                            cpp += f"                simdjson::ondemand::object obj_val;\n"
                            cpp += f"                if (field.value().get_object().get(obj_val) == simdjson::SUCCESS) {{\n"
                            cpp += f"                    if (builder) {{\n"
                            cpp += f"                        auto child_data = {child_fn}(obj_val, logger, concurrent_queue, builder);\n"
                            cpp += f"                        data.{cpp_name}.value = builder->append(child_data);\n"
                            cpp += f"                        data.{cpp_name}.tag = {tag_name};\n"
                            cpp += f"                    }} else if (logger) {{\n"
                            cpp += f"                        logger->log(\"[Warning] FastFHIR Ingestion: Cannot stage choice block {c_type} without a Builder context.\");\n"
                            cpp += f"                    }}\n"
                            cpp += f"                }}\n"
                        else:
                            cpp += f"                if (logger) logger->log(\"[Warning] FastFHIR Ingestion: Unsupported choice type {c_type}\");\n"
                        
                    cpp += f"            }}\n"
                    is_first_choice = False

            # --- 2. Standard Field Handling (Exclusive) ---
            else:
                # Check for per-field ingest override first
                ingest_override = INGEST_FIELD_OVERRIDES.get((path, json_key))
                if ingest_override:
                    cpp += ingest_override.format(err_log=err_log)
                elif f["is_array"]:
                    cpp += (
                        f"            simdjson::ondemand::array arr;\n"
                        f"            if (field.value().get_array().get(arr) == simdjson::SUCCESS) {{\n"
                    )
                    
                    # SPECIAL CASE: Bundle.entry is sliced into strings for independent worker builds
                    if path == "Bundle" and f["orig_name"] == "entry":
                        cpp += (
                            f"                if (concurrent_queue) {{\n"
                            f"                    for (auto item : arr) {{\n"
                            f"                        std::string_view full_entry;\n"
                            f"                        if (item.raw_json().get(full_entry) == simdjson::SUCCESS) {{\n"
                            f"                            concurrent_queue->push_back(full_entry);\n"
                            f"                        }}\n"
                            f"                    }}\n"
                            f"                }} else {{\n"
                        )
                        
                    cpp += f"                for (auto item : arr) {{\n"
                    
                    if f["fhir_type"] in ("string", "code"):
                        code_enum = f.get("code_enum")
                        cpp += (
                            f"                    std::string_view val;\n"
                            f"                    if (item.get_string().get(val) == simdjson::SUCCESS) {{\n"
                        )
                        
                        if code_enum:
                            cpp += f"                        data.{cpp_name}.push_back({code_enum['parse']}(std::string(val)));\n"
                        else:
                            cpp += f"                        data.{cpp_name}.push_back(val);\n"
                            
                        cpp += (
                            f"                    }} else {{\n"
                            f"                        {err_log}\n"
                            f"                    }}\n"
                        )
                        
                    elif f["fhir_type"] == "Resource":
                        # INDIRECTION: If a builder is present (Worker/Fast Path), build it.
                        # Otherwise, it must be handled by the patching phase.
                        cpp += (
                            f"                    if (builder) {{\n"
                            f"                        simdjson::ondemand::object res_obj;\n"
                            f"                        if (item.get_object().get(res_obj) == simdjson::SUCCESS) {{\n"
                            f"                            std::string_view child_type;\n"
                            f"                            if (res_obj[\"resourceType\"].get_string().get(child_type) == simdjson::SUCCESS) {{\n"
                            f"                                FastFHIR::Reflective::ObjectHandle child = dispatch_resource(child_type, res_obj, *builder, logger);\n"
                            f"                                if (child.offset() != FF_NULL_OFFSET) {{\n"
                            f"                                    data.{cpp_name}.push_back(ResourceReference(child.offset(), child.recovery()));\n"
                            f"                                }}\n"
                            f"                            }}\n"
                            f"                        }} else {{\n"
                            f"                            {err_log}\n"
                            f"                        }}\n"
                            f"                    }}\n"
                        )
                        
                    elif f["fhir_type"] in SCALAR_PRIMITIVE_TYPES:
                        if f["fhir_type"] == "boolean":
                            cpp += (
                                f"                    bool val;\n"
                                f"                    if (item.get_bool().get(val) == simdjson::SUCCESS) {{\n"
                                f"                        data.{cpp_name}.push_back(val ? 1 : 0);\n"
                                f"                    }} else {{\n"
                                f"                        {err_log}\n"
                                f"                    }}\n"
                            )
                        elif f["fhir_type"] == "decimal":
                            cpp += (
                                f"                    double val;\n"
                                f"                    if (item.get_double().get(val) == simdjson::SUCCESS) {{\n"
                                f"                        data.{cpp_name}.push_back(val);\n"
                                f"                    }} else {{\n"
                                f"                        {err_log}\n"
                                f"                    }}\n"
                            )
                        else:
                            cpp += (
                                f"                    uint64_t val;\n"
                                f"                    if (item.get_uint64().get(val) == simdjson::SUCCESS) {{\n"
                                f"                        data.{cpp_name}.push_back(static_cast<uint32_t>(val));\n"
                                f"                    }} else {{\n"
                                f"                        {err_log}\n"
                                f"                    }}\n"
                            )
                    else:
                        child_fn = f.get("resolved_path", f"{path}.{json_key}").replace(".", "_") + "_from_json" if f["fhir_type"] in ("BackboneElement", "Element") else f"{f['fhir_type']}_from_json"
                        cpp += (
                            f"                    simdjson::ondemand::object obj_val;\n"
                            f"                    if (item.get_object().get(obj_val) == simdjson::SUCCESS) {{\n"
                            f"                        data.{cpp_name}.push_back({child_fn}(obj_val, logger, concurrent_queue, builder));\n"
                        )
                        cpp += (
                            f"                    }} else {{\n"
                            f"                        {err_log}\n"
                            f"                    }}\n"
                        )
                        
                    cpp += f"                }}\n"
                    
                    if path == "Bundle" and f["orig_name"] == "entry": 
                        cpp += f"                }}\n"
                        
                    cpp += (
                        f"            }} else {{\n"
                        f"                {err_log}\n"
                        f"            }}\n"
                    )
                    
                elif f["fhir_type"] == "Resource":
                    cpp += (
                        f"            if (builder) {{\n"
                        f"                simdjson::ondemand::object res_obj;\n"
                        f"                if (field.value().get_object().get(res_obj) == simdjson::SUCCESS) {{\n"
                        f"                    std::string_view child_type;\n"
                        f"                    if (res_obj[\"resourceType\"].get_string().get(child_type) == simdjson::SUCCESS) {{\n"
                        f"                        FastFHIR::Reflective::ObjectHandle child = dispatch_resource(child_type, res_obj, *builder, logger);\n"
                        f"                        if (child.offset() != FF_NULL_OFFSET) {{\n"
                        f"                            data.{cpp_name} = ResourceReference(child.offset(), child.recovery());\n"
                        f"                        }}\n"
                        f"                    }}\n"
                        f"                }} else {{\n"
                        f"                    {err_log}\n"
                        f"                }}\n"
                        f"            }}\n"
                    )
                    
                elif f["fhir_type"] == "string":
                    cpp += (
                        f"            std::string_view val;\n"
                        f"            if (field.value().get_string().get(val) == simdjson::SUCCESS) {{\n"
                        f"                data.{cpp_name} = val;\n"
                        f"            }} else {{\n"
                        f"                {err_log}\n"
                        f"            }}\n"
                    )
                    
                elif f["cpp_type"] == "Offset":
                    child_data_type = _resolve_data_type_name(f["fhir_type"], f["orig_name"], path, f.get("resolved_path"))
                    child_fn = f.get("resolved_path", f"{path}.{json_key}").replace(".", "_") + "_from_json" if f["fhir_type"] in ("BackboneElement", "Element") else f"{f['fhir_type']}_from_json"
                    cpp += (
                        f"            simdjson::ondemand::object obj_val;\n"
                        f"            if (field.value().get_object().get(obj_val) == simdjson::SUCCESS) {{\n"
                        f"                data.{cpp_name} = std::make_unique<{child_data_type}>({child_fn}(obj_val, logger, concurrent_queue, builder));\n"
                        f"            }} else {{\n"
                        f"                {err_log}\n"
                        f"            }}\n"
                    )
                    
                elif f["fhir_type"] == "code":
                    code_enum = f.get("code_enum")
                    cpp += (
                        f"            std::string_view val;\n"
                        f"            if (field.value().get_string().get(val) == simdjson::SUCCESS) {{\n"
                    )
                    
                    if code_enum:
                        cpp += f"                data.{cpp_name} = {code_enum['parse']}(std::string(val));\n"
                    else:         
                        cpp += f"                data.{cpp_name} = val;\n"
                        
                    cpp += (
                        f"            }} else {{\n"
                        f"                {err_log}\n"
                        f"            }}\n"
                    )
                    
                elif f["fhir_type"] == "boolean":
                    cpp += (
                        f"            bool val;\n"
                        f"            if (field.value().get_bool().get(val) == simdjson::SUCCESS) {{\n"
                        f"                data.{cpp_name} = val ? 1 : 0;\n"
                        f"            }} else {{\n"
                        f"                {err_log}\n"
                        f"            }}\n"
                    )
                    
                elif f["data_type"] == "double":
                    cpp += (
                        f"            double val;\n"
                        f"            if (field.value().get_double().get(val) == simdjson::SUCCESS) {{\n"
                        f"                data.{cpp_name} = val;\n"
                        f"            }} else {{\n"
                        f"                {err_log}\n"
                        f"            }}\n"
                    )
                    
                else:
                    cpp += (
                        f"            uint64_t val;\n"
                        f"            if (field.value().get_uint64().get(val) == simdjson::SUCCESS) {{\n"
                        f"                data.{cpp_name} = static_cast<{f['cpp_type']}>(val);\n"
                        f"            }} else {{\n"
                        f"                {err_log}\n"
                        f"            }}\n"
                    )
            cpp += f"        }}\n"
            
        cpp += (
            f"    }}\n"
            f"    return data;\n"
            f"}}\n\n"
        )

    # --- 2. CONCURRENT PATCHING (Independent Worker Allocation) ---
    for path, blk in master_blocks.items():
        patch_fn_name = f"patch_{path.replace('.', '_')}_from_json"
        owner_ns = _block_key_namespace(path)

        hpp += f"void {patch_fn_name}(simdjson::ondemand::object& obj, FastFHIR::Reflective::MutableEntry& wrapper, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger = nullptr);\n"
        
        cpp += (
            f"void {patch_fn_name}(simdjson::ondemand::object& obj, FastFHIR::Reflective::MutableEntry& wrapper, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger) {{\n"
            f"    for (auto field : obj) {{\n"
            f"        std::string_view key = field.unescaped_key().value_unsafe();\n"
        )

        is_first_patch = True
        for f in blk["layout"]:
            json_key, cpp_name = f["orig_name"], f["cpp_name"]
            field_key_enum = f"FastFHIR::Fields::{owner_ns}::{_field_key_short_name(json_key)}"
            err_log = f"if (logger) logger->log(\"[Warning] FastFHIR Patching: Malformed data at {path}.{json_key}\");"

            if is_first_patch:
                cpp += f"        if (key == \"{json_key}\") {{\n"
            else:
                cpp += f"        else if (key == \"{json_key}\") {{\n"
            is_first_patch = False

            if f["fhir_type"] == "string":
                cpp += (
                    f"            std::string_view val;\n"
                    f"            if (field.value().get_string().get(val) == simdjson::SUCCESS) {{\n"
                    f"                wrapper[{field_key_enum}] = builder.append_obj(val);\n"
                    f"            }} else {{\n"
                    f"                {err_log}\n"
                    f"            }}\n"
                )
            
            elif (f["cpp_type"] == "Offset" or f["fhir_type"] == "Resource") and not f["is_array"]:
                if f["fhir_type"] == "Resource":
                    cpp += (
                        f"            simdjson::ondemand::object res_obj;\n"
                        f"            if (field.value().get_object().get(res_obj) == simdjson::SUCCESS) {{\n"
                        f"                std::string_view child_type;\n"
                        f"                if (res_obj[\"resourceType\"].get_string().get(child_type) == simdjson::SUCCESS) {{\n"
                        f"                    FastFHIR::Reflective::ObjectHandle child = dispatch_resource(child_type, res_obj, builder, logger);\n"
                        f"                    if (child.offset() != FF_NULL_OFFSET) {{\n"
                        f"                        wrapper[{field_key_enum}] = child;\n"
                        f"                    }}\n"
                        f"                }}\n"
                        f"            }} else {{\n"
                        f"                {err_log}\n"
                        f"            }}\n"
                    )
                else:
                    child_fn = f.get("resolved_path", f"{path}.{json_key}").replace(".", "_") + "_from_json" if f["fhir_type"] in ("BackboneElement", "Element") else f"{f['fhir_type']}_from_json"
                    cpp += (
                        f"            simdjson::ondemand::object obj_val;\n"
                        f"            if (field.value().get_object().get(obj_val) == simdjson::SUCCESS) {{\n"
                        f"                auto child_data = {child_fn}(obj_val, logger, nullptr, &builder);\n"
                        f"                wrapper[{field_key_enum}] = builder.append_obj(child_data);\n"
                    )
                    cpp += (
                        f"            }} else {{\n"
                        f"                {err_log}\n"
                        f"            }}\n"
                    )
            
            elif f["is_array"]:
                cpp += (
                    f"            simdjson::ondemand::array arr;\n"
                    f"            if (field.value().get_array().get(arr) == simdjson::SUCCESS) {{\n"
                )
                
                if f["fhir_type"] in ("string", "code"):
                    cpp += (
                        f"                std::vector<Offset> offsets;\n"
                        f"                for (auto item : arr) {{\n"
                        f"                    std::string_view val;\n"
                        f"                    if (item.get_string().get(val) == simdjson::SUCCESS) {{\n"
                        f"                        offsets.push_back(builder.append_obj(val).offset());\n"
                        f"                    }}\n"
                        f"                }}\n"
                        f"                wrapper[{field_key_enum}] = offsets;\n"
                    )
                
                elif f["fhir_type"] == "Resource":
                    cpp += (
                        f"                std::vector<ResourceReference> refs;\n"
                        f"                for (auto item : arr) {{\n"
                        f"                    simdjson::ondemand::object res_obj;\n"
                        f"                    if (item.get_object().get(res_obj) == simdjson::SUCCESS) {{\n"
                        f"                        std::string_view child_type;\n"
                        f"                        if (res_obj[\"resourceType\"].get_string().get(child_type) == simdjson::SUCCESS) {{\n"
                        f"                            FastFHIR::Reflective::ObjectHandle child = dispatch_resource(child_type, res_obj, builder, logger);\n"
                        f"                            if (child.offset() != FF_NULL_OFFSET) {{\n"
                        f"                                refs.push_back(ResourceReference(child.offset(), child.recovery()));\n"
                        f"                            }}\n"
                        f"                        }}\n"
                        f"                    }}\n"
                        f"                }}\n"
                        f"                wrapper[{field_key_enum}] = builder.append_obj(refs);\n"
                    )
                
                elif f["fhir_type"] in SCALAR_PRIMITIVE_TYPES:
                    if f["fhir_type"] == "boolean":
                        cpp += (
                            f"                std::vector<uint8_t> vals;\n"
                            f"                for (auto item : arr) {{\n"
                            f"                    bool val;\n"
                            f"                    if (item.get_bool().get(val) == simdjson::SUCCESS) {{\n"
                            f"                        vals.push_back(val ? 1 : 0);\n"
                            f"                    }}\n"
                            f"                }}\n"
                            f"                wrapper[{field_key_enum}] = builder.append_obj(vals);\n"
                        )
                    elif f["fhir_type"] == "decimal":
                        cpp += (
                            f"                std::vector<double> vals;\n"
                            f"                for (auto item : arr) {{\n"
                            f"                    double val;\n"
                            f"                    if (item.get_double().get(val) == simdjson::SUCCESS) {{\n"
                            f"                        vals.push_back(val);\n"
                            f"                    }}\n"
                            f"                }}\n"
                            f"                wrapper[{field_key_enum}] = builder.append_obj(vals);\n"
                        )
                    else:
                        cpp += (
                            f"                std::vector<uint32_t> vals;\n"
                            f"                for (auto item : arr) {{\n"
                            f"                    uint64_t val;\n"
                            f"                    if (item.get_uint64().get(val) == simdjson::SUCCESS) {{\n"
                            f"                        vals.push_back(static_cast<uint32_t>(val));\n"
                            f"                    }}\n"
                            f"                }}\n"
                            f"                wrapper[{field_key_enum}] = builder.append_obj(vals);\n"
                        )

                else:
                    child_fn = f.get("resolved_path", f"{path}.{json_key}").replace(".", "_") + "_from_json" if f["fhir_type"] in ("BackboneElement", "Element") else f"{f['fhir_type']}_from_json"
                    cpp += (
                        f"                std::vector<Offset> offsets;\n"
                        f"                for (auto item : arr) {{\n"
                        f"                    simdjson::ondemand::object obj_val;\n"
                        f"                    if (item.get_object().get(obj_val) == simdjson::SUCCESS) {{\n"
                        f"                        auto child_data = {child_fn}(obj_val, logger, nullptr, &builder);\n"
                        f"                        offsets.push_back(builder.append_obj(child_data).offset());\n"
                        f"                    }}\n"
                        f"                }}\n"
                        f"                wrapper[{field_key_enum}] = offsets;\n"
                    )
                    
                cpp += (
                    f"            }} else {{\n"
                    f"                {err_log}\n"
                    f"            }}\n"
                )
            
            else:
                cpp += f"            // [Arrays and inline primitives inside wrappers currently bypass concurrent patch generation]\n"
                
            cpp += f"        }}\n"
            
        cpp += (
            f"    }}\n"
            f"}}\n\n"
        )

    # --- 3. AUTO-GENERATED DISPATCHERS ---
    hpp += "\n    FastFHIR::Reflective::ObjectHandle dispatch_resource(std::string_view resource_type, simdjson::ondemand::object obj, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger = nullptr);\n"
    
    cpp += (
        f"\nFastFHIR::Reflective::ObjectHandle dispatch_resource(std::string_view resource_type, simdjson::ondemand::object obj, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger) {{\n"
    )
    
    is_first_dispatch = True
    for res in resources:
        if is_first_dispatch:
            cpp += f"    if (resource_type == \"{res}\") return builder.append_obj({res}_from_json(obj, logger, nullptr, &builder));\n"
        else:                 
            cpp += f"    else if (resource_type == \"{res}\") return builder.append_obj({res}_from_json(obj, logger, nullptr, &builder));\n"
        is_first_dispatch = False
        
    cpp += (
        f"    if (logger) logger->log(\"[Warning] FastFHIR Ingestion: Unknown root resource type encountered.\");\n"
        f"    return FastFHIR::Reflective::ObjectHandle(&builder, FF_NULL_OFFSET);\n"
        f"}}\n\n"
    )

    hpp += "    FastFHIR::Reflective::ObjectHandle dispatch_block(RECOVERY_TAG expected_tag, simdjson::ondemand::value& json_val, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger = nullptr);\n"
    
    cpp += (
        f"\nFastFHIR::Reflective::ObjectHandle dispatch_block(RECOVERY_TAG expected_tag, simdjson::ondemand::value& json_val, FastFHIR::Builder& builder, FastFHIR::ConcurrentLogger* logger) {{\n"
        f"    simdjson::ondemand::object obj;\n"
        f"    if (json_val.get_object().get(obj) != simdjson::SUCCESS) return FastFHIR::Reflective::ObjectHandle(&builder, FF_NULL_OFFSET);\n\n"
        f"    switch (GetTypeFromTag(expected_tag)) {{\n"
    )
    
    for path, blk in master_blocks.items():
        tag_name = "RECOVER_FF_" + path.replace(".", "_").upper()
        fn_name = path.replace(".", "_") + "_from_json"
        cpp += f"        case {tag_name}: return builder.append_obj({fn_name}(obj, logger, nullptr, &builder));\n"
        
    cpp += (
        f"        default: return FastFHIR::Reflective::ObjectHandle(&builder, FF_NULL_OFFSET);\n"
        f"    }}\n"
        f"}}\n\n"
    )

    hpp += "\n} // namespace FastFHIR::Ingest\n"
    cpp += "} // namespace FastFHIR::Ingest\n"

    out_hpp = os.path.join(output_dir, "FF_IngestMappings.hpp")
    out_cpp = os.path.join(output_dir, "FF_IngestMappings.cpp")
    _write_if_changed(out_hpp, hpp)
    _write_if_changed(out_cpp, cpp)
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
    
    # Note: __init__.py is created by emit_python_ast() which generates the actual imports.
    # This function only generates the individual field module files.
    
    # 2. Emit a separate file for every single FHIR resource/block
    for class_name, fields in python_resource_map.items():
        file_name = f"{class_name.lower()}.py"
        file_path = os.path.join(target_dir, file_name)
        
        with open(file_path, "w") as f:
            f.write("# Auto-generated by FastFHIR pipeline. Do not edit.\n")
            f.write("from .. import _core\n\n")
            
            f.write(f"class {class_name}:\n")
            if not fields:
                f.write("    pass\n\n")
                continue
            
            for field_name, metadata in fields.items():
                # UPDATE: Unpack 4 variables to explicitly capture the fhir_type
                idx, orig_name, owner, fhir_type = metadata
                safe_name = field_name.upper()
                
                # Python keyword safety
                if safe_name in ["CLASS", "IMPORT", "GLOBAL", "FOR", "WHILE", "IN", "IS", "AS"]:
                    safe_name += "_"
                
                # UPDATE: Document the polymorphic bridge in the generated code
                if fhir_type in ["Resource", "DomainResource"]:
                    f.write(f"    # Polymorphic Wrapper Bridge (Auto-Hops to Payload)\n")
                
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
        f.write("        # If the user passes an instance (e.g., ff.Patient), use its class\n")
        f.write("        if isinstance(target_class, ASTNode):\n")
        f.write("            return type(target_class)(self.path)\n")
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
        
        # APPEND to the existing file (created by emit_python_fields) instead of overwriting
        with open(os.path.join(target_dir, file_name), "a") as f:
            # Add the ASTNode import if not already present (it's added by emit_python_fields for resources)
            # We need it for the PATH classes
            f.write("from .base import ASTNode\n")
            f.write(f"\nclass {class_name}_PATH(ASTNode):\n")
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
                if f_def['fhir_type'] in master_blocks:
                    target_path = f_def['fhir_type']
                else:
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

        def _alias_name(path):
            # Convert block paths like "Bundle.entry" -> "BundleEntry"
            # and "Encounter.statusHistory" -> "EncounterStatusHistory".
            parts = [p for p in path.split('.') if p]
            return ''.join(part[:1].upper() + part[1:] for part in parts)

        seen_aliases = set()
        exported = ["ASTNode", "ASTArrayNode"]
        for block_name in sorted(master_blocks.keys()):
            module_name = block_name.replace('.', '_').lower()
            class_name = f"{block_name.replace('.', '_').upper()}_PATH"
            alias = _alias_name(block_name)

            if alias in seen_aliases:
                continue

            # Instantiate path roots for clean syntax (e.g. Bundle.ENTRY or BundleEntry.RESOURCE).
            f.write(f"from .{module_name} import {class_name}\n")
            f.write(f"{alias} = {class_name}()\n")
            seen_aliases.add(alias)
            exported.append(alias)

        f.write("\n__all__ = [\n")
        for name in exported:
            f.write(f"    \"{name}\",\n")
        f.write("]\n")

# =====================================================================
# 7. PYTHON STUB FILE GENERATION (PEP 561 Type Hints)
# =====================================================================

def emit_python_fields_stubs(python_resource_map, output_dir="generated_src"):
    """
    Generates .pyi stub files for field modules to enable IDE autocomplete
    and type checking. One stub file per resource/block class.
    """
    import os
    target_dir = os.path.join(output_dir, "python", "fields")
    
    for class_name, fields in python_resource_map.items():
        stub_path = os.path.join(target_dir, f"{class_name.lower()}.pyi")
        
        with open(stub_path, "w") as f:
            f.write("# Auto-generated stub file for type checking.\n")
            f.write("# See corresponding .py file for implementation.\n\n")
            f.write("from typing import ClassVar\n")
            f.write("from .. import _core\n\n")
            
            f.write(f"class {class_name}:\n")
            if not fields:
                f.write("    ...\n\n")
                continue
            
            for field_name, metadata in fields.items():
                idx, orig_name, owner, fhir_type = metadata
                safe_name = field_name.upper()
                
                if safe_name in ["CLASS", "IMPORT", "GLOBAL", "FOR", "WHILE", "IN", "IS", "AS"]:
                    safe_name += "_"
                
                f.write(f"    {safe_name}: ClassVar[_core.Field]\n")
            
            f.write("\n")
    
    print(f"-- Emitted {len(python_resource_map)} Python field stub files (.pyi) into: {target_dir}/")

def emit_python_ast_stubs(master_blocks, block_key_defs, output_dir="generated_src"):
    """
    Generates .pyi stub files for AST path builder classes to enable IDE
    autocomplete and type checking. Declares property return types.
    """
    import os
    target_dir = os.path.join(output_dir, "python", "fields")
    
    # Stub for base.py
    with open(os.path.join(target_dir, "base.pyi"), "w") as f:
        f.write("# Auto-generated stub file for type checking.\n")
        f.write("from typing import Tuple, Type, TypeVar, Generic, overload\n\n")
        
        f.write("T = TypeVar('T', bound='ASTNode')\n\n")
        
        f.write("class ASTNode:\n")
        f.write("    path: Tuple\n")
        f.write("    def __init__(self, current_path: Tuple | None = ...) -> None: ...\n")
        f.write("    def cast(self, target_class: Type[T] | ASTNode) -> T: ...\n\n")
        
        f.write("class ASTArrayNode(ASTNode):\n")
        f.write("    item_class: Type[ASTNode]\n")
        f.write("    def __init__(self, current_path: Tuple, item_class: Type[ASTNode]) -> None: ...\n")
        f.write("    def __getitem__(self, index: int) -> ASTNode: ...\n\n")
    
    # Stubs for each path builder class
    for path, layout in block_key_defs:
        class_name = path.replace('.', '_').upper()
        stub_path = os.path.join(target_dir, f"{class_name.lower()}.pyi")
        
        with open(stub_path, "w") as f:
            f.write("# Auto-generated stub file for type checking.\n")
            f.write("# See corresponding .py file for implementation.\n\n")
            f.write("from typing import overload\n")
            f.write("from . import _core\n")
            f.write("from .base import ASTNode, ASTArrayNode\n\n")
            
            f.write(f"class {class_name}_PATH(ASTNode):\n")
            if not layout:
                f.write("    ...\n")
            
            for f_def in layout:
                orig_name = f_def['orig_name']
                safe_name = orig_name.upper()
                if safe_name in ["CLASS", "IMPORT", "GLOBAL", "FOR", "WHILE", "IN", "IS", "AS"]:
                    safe_name += "_"
                
                is_array = f_def.get('is_array', False)
                
                # Determine return type
                if f_def['fhir_type'] in master_blocks:
                    target_path = f_def['fhir_type']
                    base_name = target_path.replace('.', '_').upper()
                    return_type = f"{base_name}_PATH"
                    if is_array:
                        return_type = f"ASTArrayNode"
                else:
                    return_type = "ASTNode"
                    if is_array:
                        return_type = "ASTArrayNode"
                
                f.write(f"    @property\n")
                f.write(f"    def {safe_name}(self) -> {return_type}: ...\n")
            
            f.write("\n")
    
    print(f"-- Emitted {len(block_key_defs)} Python AST path stub files (.pyi) into: {target_dir}/")

def emit_py_typed_marker(output_dir="generated_src"):
    """
    Creates a py.typed marker file (PEP 561) to indicate the package
    provides inline type information for type checkers like mypy.
    """
    import os
    py_typed_path = os.path.join(output_dir, "python", "py.typed")
    os.makedirs(os.path.dirname(py_typed_path), exist_ok=True)
    
    # py.typed is an empty marker file
    with open(py_typed_path, "w") as f:
        pass
    
    print(f"-- Emitted py.typed marker file: {py_typed_path}")

# =====================================================================
# 8. MASTER BUILD ORCHESTRATOR
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
    
    fwd_decls = set([t + "Data" for t in PRODUCTION_TYPES])
    hpp_head = f"{auto_header}// MARK: - Universal Data Types\n#pragma once\n#include \"../include/FF_Primitives.hpp\"\n#include \"../include/FF_Utilities.hpp\"\n#include \"../include/FF_Builder.hpp\"\n#include \"FF_CodeSystems.hpp\"\n#include <vector>\n#include <string_view>\n#include <memory>\n\n"
    hpp_head += "namespace FastFHIR { template<typename T> struct TypeTraits; \n\n"
    hpp_head += "template<> struct TypeTraits<std::string_view> {\n"
    hpp_head += "    static constexpr auto recovery = RECOVER_FF_STRING;\n"
    hpp_head += "    static Size size(std::string_view d, uint32_t = FHIR_VERSION_R5) { return SIZE_FF_STRING(d); }\n"
    hpp_head += "    static void store(BYTE* const base, Offset off, std::string_view d, uint32_t = FHIR_VERSION_R5) { STORE_FF_STRING(base, off, d); }\n"
    hpp_head += "};\n\n"
    hpp_head += "template<> struct TypeTraits<std::vector<Offset>> {\n"
    hpp_head += "};\n\n"
    hpp_head += "template<> struct TypeTraits<std::vector<ResourceReference>> {\n"
    hpp_head += "    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_RESOURCE | RECOVER_ARRAY_BIT);\n"
    hpp_head += "    static Size size(const std::vector<ResourceReference>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_RESOURCE); }\n"
    hpp_head += "    static void store(BYTE* const base, Offset off, const std::vector<ResourceReference>& d, uint32_t = FHIR_VERSION_R5) {\n"
    hpp_head += "        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_RESOURCE, static_cast<uint32_t>(d.size()), recovery);\n"
    hpp_head += "        for (const auto& ref : d) {\n"
    hpp_head += "            STORE_U64(base + off, ref.offset); STORE_U16(base + off + DATA_BLOCK::RECOVERY, ref.recovery); off += TYPE_SIZE_RESOURCE;\n"
    hpp_head += "        }\n"
    hpp_head += "    }\n"
    hpp_head += "};\n"
    hpp_head += "template<> struct TypeTraits<std::vector<uint8_t>> {\n"
    hpp_head += "    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_BOOL | RECOVER_ARRAY_BIT);\n"
    hpp_head += "    static Size size(const std::vector<uint8_t>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_UINT8); }\n"
    hpp_head += "    static void store(BYTE* const base, Offset off, const std::vector<uint8_t>& d, uint32_t = FHIR_VERSION_R5) {\n"
    hpp_head += "        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_UINT8, static_cast<uint32_t>(d.size()), recovery);\n"
    hpp_head += "        for (const auto& v : d) { STORE_U8(base + off, v); off += TYPE_SIZE_UINT8; }\n"
    hpp_head += "    }\n"
    hpp_head += "};\n"
    hpp_head += "template<> struct TypeTraits<std::vector<uint32_t>> {\n"
    hpp_head += "    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_UINT32 | RECOVER_ARRAY_BIT);\n"
    hpp_head += "    static Size size(const std::vector<uint32_t>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_UINT32); }\n"
    hpp_head += "    static void store(BYTE* const base, Offset off, const std::vector<uint32_t>& d, uint32_t = FHIR_VERSION_R5) {\n"
    hpp_head += "        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_UINT32, static_cast<uint32_t>(d.size()), recovery);\n"
    hpp_head += "        for (const auto& v : d) { STORE_U32(base + off, v); off += TYPE_SIZE_UINT32; }\n"
    hpp_head += "    }\n"
    hpp_head += "};\n"
    hpp_head += "template<> struct TypeTraits<std::vector<double>> {\n"
    hpp_head += "    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_FLOAT64 | RECOVER_ARRAY_BIT);\n"
    hpp_head += "    static Size size(const std::vector<double>& d, uint32_t = FHIR_VERSION_R5) { return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_FLOAT64); }\n"
    hpp_head += "    static void store(BYTE* const base, Offset off, const std::vector<double>& d, uint32_t = FHIR_VERSION_R5) {\n"
    hpp_head += "        STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_FLOAT64, static_cast<uint32_t>(d.size()), recovery);\n"
    hpp_head += "        for (const auto& v : d) { STORE_F64(base + off, v); off += TYPE_SIZE_FLOAT64; }\n"
    hpp_head += "    }\n"
    hpp_head += "};\n"
    hpp_head += "} // namespace FastFHIR\n\n"
    for decl in sorted(fwd_decls): hpp_head += f"struct {decl};\n"
    hpp_head += "\n"

    # Internal header preamble — includes the public FF_DataTypes.hpp
    int_head = f"{auto_header}// MARK: - Universal Data Types (Internal — vtable structs & zero-copy Views)\n#pragma once\n#include \"FF_DataTypes.hpp\"\n\n"

    # .cpp includes the internal header so bridge fns can access HEADER_SIZE
    cpp_head = f"{auto_header}\n#include \"../include/FF_Utilities.hpp\"\n#include \"FF_DataTypes_internal.hpp\"\n#include \"FF_Dictionary.hpp\"\n\n"
    
    all_blocks = {}
    for t in PRODUCTION_TYPES:
        sch = []
        for v, bun in type_bundles:
            try: sch.append((v, extract_structure_definition(bun, t)))
            except Exception as e: print(f"  [Warning] Type '{t}' not found in version '{v}': {e}")
        if not sch:
            raise RuntimeError(f"PRODUCTION_TYPE '{t}' was not found in any version's profiles-types.json. "
                               "Ensure fhir_specs are fully downloaded and the type name is correct.")
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
        pub_h, int_h, c = generate_cxx_for_blocks(all_blocks, versions)
        hpp_head += pub_h
        int_head += int_h
        cpp_head += c
    
    _write_if_changed(os.path.join(output_dir, "FF_DataTypes.hpp"), hpp_head)
    _write_if_changed(os.path.join(output_dir, "FF_DataTypes_internal.hpp"), int_head)
    _write_if_changed(os.path.join(output_dir, "FF_DataTypes.cpp"), cpp_head)

    # Pre-load all resource bundles once (once per version, not once per resource)
    resource_bundles = []
    for v in versions:
        p = os.path.join(input_dir, v, "profiles-resources.json")
        if os.path.exists(p):
            try:
                with open(p, 'r', encoding='utf-8') as f:
                    resource_bundles.append((v, json.load(f)))
                print(f"  [Info] Loaded {p}")
            except Exception as e:
                print(f"  [Warning] Could not load {p}: {e}")

    generated_resources = []
    for res in resources:
        print(f"Generating FF_{res}...")
        sch = []
        for v, bun in resource_bundles:
            try: sch.append((v, extract_structure_definition(bun, res)))
            except Exception as e: print(f"  [Warning] {res} not found in {v}: {e}")
        if not sch:
            raise RuntimeError(f"Resource '{res}' was not found in any version's profiles-resources.json. "
                               "Ensure fhir_specs are fully downloaded and the resource name is correct.")
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
            public_hpp, internal_hpp, cpp_body = generate_cxx_for_blocks(blocks, versions)
            _write_if_changed(os.path.join(output_dir, f"FF_{res}.hpp"),
                f"{auto_header}#pragma once\n#include \"FF_DataTypes.hpp\"\n\n{public_hpp}")
            _write_if_changed(os.path.join(output_dir, f"FF_{res}_internal.hpp"),
                f"{auto_header}#pragma once\n#include \"FF_DataTypes_internal.hpp\"\n#include \"FF_{res}.hpp\"\n\n{internal_hpp}")
            _write_if_changed(os.path.join(output_dir, f"FF_{res}.cpp"),
                f"{auto_header}\n#include \"FF_{res}_internal.hpp\"\n#include \"../include/FF_Utilities.hpp\"\n#include \"FF_Dictionary.hpp\"\n\n{cpp_body}")

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
            owner_rec = f"ToArrayTag(RECOVER_{block_struct_name})" if f['is_array'] else f"RECOVER_{block_struct_name}"
            field_keys_hpp += (
                f"    inline constexpr FF_FieldKey {short_name}"
                f"{{{owner_rec}, {_field_kind_expr(f)}, {f['offset']}, "
                f"{child_rec}, {arr_offsets}, \"{f['cpp_name']}\"}};\n"
            )
            
            # Track for Registry and Python
            registry_entries.append(f"        &FastFHIR::Fields::{ns_name}::{short_name}")
            
            if path not in token_registry: 
                token_registry[path] = {}
            token_registry[path][f['orig_name']] = (token_id, ns_name)

            python_resource_map[class_name][f['orig_name']] = (token_id, f['orig_name'], ns_name, f['fhir_type'])

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
    _write_if_changed(os.path.join(output_dir, "FF_FieldKeys.hpp"), field_keys_hpp)
    _write_if_changed(os.path.join(output_dir, "FF_FieldKeys.cpp"), field_keys_cpp)

    # Emit Python field modules first (creates ADDRESS, BUNDLE, etc.)
    emit_python_fields(python_resource_map, output_dir)
    
    # Emit the Deferred Path Builder second (appends ADDRESS_PATH, BUNDLE_PATH to same files)
    emit_python_ast(all_blocks, block_key_defs, token_registry, output_dir)
    
    # Emit stubs for IDE support (PEP 561)
    emit_python_fields_stubs(python_resource_map, output_dir)
    emit_python_ast_stubs(all_blocks, block_key_defs, output_dir)
    emit_py_typed_marker(output_dir)

    # Generate reflection dispatch files
    reflection_hpp, reflection_cpp = generate_reflection_dispatch(sorted(reflected_block_names), resources)
    _write_if_changed(os.path.join(output_dir, "FF_Reflection.hpp"), reflection_hpp)
    _write_if_changed(os.path.join(output_dir, "FF_Reflection.cpp"), reflection_cpp)
    
    # FF_AllTypes.hpp is the internal aggregator — includes all _internal variants
    all_types_hpp = f"{auto_header}#pragma once\n#include \"FF_DataTypes_internal.hpp\"\n#include \"FF_FieldKeys.hpp\"\n#include \"FF_Reflection.hpp\"\n"
    for res in generated_resources:
        all_types_hpp += f"#include \"FF_{res}_internal.hpp\"\n"
    all_types_hpp += "\n"
    _write_if_changed(os.path.join(output_dir, "FF_AllTypes.hpp"), all_types_hpp)

    # Generate the RECOVERY enum from all discovered block paths
    generate_recovery_header(PRODUCTION_TYPES, resources, all_block_paths, output_dir)

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


def _discover_resource_names(specs_dir="fhir_specs", versions=None, include_abstract=False):
    """Discover resource StructureDefinition names from profiles-resources.json."""
    versions = versions or _discover_versions(specs_dir)
    discovered = set()
    for v in versions:
        p = os.path.join(specs_dir, v, "profiles-resources.json")
        if not os.path.exists(p):
            continue
        with open(p, "r", encoding="utf-8") as f:
            bundle = json.load(f)
        for entry in bundle.get("entry", []):
            resource = entry.get("resource", {})
            if resource.get("resourceType") != "StructureDefinition":
                continue
            if resource.get("kind") != "resource":
                continue
            if not include_abstract and resource.get("abstract", False):
                continue
            name = resource.get("name")
            if name:
                discovered.add(name)
    return sorted(discovered)


def resolve_production_resources(specs_dir="fhir_specs", versions=None, profile=None):
    """
    Resolve production resource scope.
    profile values:
      - us (default): use curated US Core resources
      - uk: use curated UK Core resources
      - all: discover all concrete FHIR resources
    Defaults to env FASTFHIR_PRODUCTION_PROFILE, then "us".
    """
    selected_profile = (profile or os.getenv(PRODUCTION_PROFILE_ENV, "us")).strip().lower()
    if selected_profile == "us":
        resources = list(US_CORE_RESOURCES)
    elif selected_profile == "uk":
        resources = list(UK_CORE_RESOURCES)
    elif selected_profile == "all":
        resources = _discover_resource_names(specs_dir=specs_dir, versions=versions, include_abstract=False)
        if not resources:
            raise RuntimeError(
                "FASTFHIR_PRODUCTION_PROFILE=all requested, but no resources were discovered. "
                "Ensure fhir_specs/<version>/profiles-resources.json exists."
            )
    else:
        raise ValueError(
            f"Unsupported {PRODUCTION_PROFILE_ENV}='{selected_profile}'. "
            "Use one of: us|uk|all"
        )

    print(f"Production profile: {selected_profile} ({len(resources)} resources)")
    if resources:
        print("Resolved resources:")
        for name in resources:
            print(f"  - {name}")
    return resources

def main (spec_dir="fhir_specs", output_dir="generated_src"):
    versions = _discover_versions(spec_dir)
    print(f"Discovered FHIR Versions: {', '.join(versions)}")
    resources = resolve_production_resources(specs_dir=spec_dir, versions=versions)
    enum_map = {}
    if versions:
        from generator.ffcs import generate_code_systems
        enum_map = generate_code_systems(PRODUCTION_TYPES, resources, versions)
    compile_fhir_library(resources, versions, code_enum_map=enum_map, output_dir=output_dir)
    print("\n[Success] Build Complete: generated_src/ contains all FastFHIR artifacts."
)


# =====================================================================
# 9. WASM CODEC GENERATOR (--wasm mode)
# =====================================================================
# For each extension StructureDefinition, emits a self-contained C source
# file that can be compiled to a .wasm codec module mirroring the three
# exports:  ext_size / ext_encode / ext_decode.
#
# The emitted file uses:
#   - Only fixed-size stack structs (no std::vector, no std::string).
#   - Direct LOAD_U*/STORE_U* macros against the staging buffer pointer.
#   - Three exported functions with the ABI documented in FF_Extensions.hpp.
#
# Compile with:
#   wasi-sdk clang --target=wasm32-wasi -O3 -fno-exceptions -fno-rtti
#       -nostdlib++ -Wl,--no-entry -o <ext_hash>.wasm <ext_name>_codec.c
# =====================================================================

# Maximum number of fields supported in the stack-only staging layout.
_WASM_MAX_FIELDS = 64

# WASM-safe C type mapping (no heap allocation).
_WASM_CTYPE = {
    'boolean':     ('uint8_t',  1, 'uint8'),
    'integer':     ('uint32_t', 4, 'uint32'),
    'unsignedInt': ('uint32_t', 4, 'uint32'),
    'positiveInt': ('uint32_t', 4, 'uint32'),
    'integer64':   ('uint64_t', 8, 'uint64'),
    'decimal':     ('double',   8, 'f64'),
}

# String fields are represented as a fixed-length char[] in the staging layout.
_WASM_MAX_STRING_BYTES = 512


def _wasm_field_ctype(fhir_type):
    """Return (c_type_str, byte_size) for a WASM-staging-compatible field type."""
    if fhir_type in _WASM_CTYPE:
        ct, sz, _ = _WASM_CTYPE[fhir_type]
        return ct, sz
    # Strings, codes, all reference types → fixed char array
    return 'char', _WASM_MAX_STRING_BYTES


def emit_wasm_codec(ext_url, master_blocks, ext_path, output_dir):
    """Emit a WASM codec .c file for the Extension at @p ext_path.

    Parameters
    ----------
    ext_url   : canonical URL of the extension (used in file-level comment).
    master_blocks : dict of path → block info from merge_fhir_versions.
    ext_path  : the root StructureDefinition path (e.g. 'Extension').
    output_dir : where to write the .c file.
    """
    if ext_path not in master_blocks:
        return

    blk = master_blocks[ext_path]
    layout = blk['layout']
    # Sanitise extension URL to a C identifier for the file name.
    safe_name = re.sub(r'[^A-Za-z0-9]', '_', ext_url.rstrip('/').split('/')[-1])
    if not safe_name:
        safe_name = 'extension'

    # ── Staging payload struct ────────────────────────────────────────────
    struct_lines = ['typedef struct {']
    for f in layout[:_WASM_MAX_FIELDS]:
        ctype, sz = _wasm_field_ctype(f['fhir_type'])
        if sz == _WASM_MAX_STRING_BYTES:
            struct_lines.append(f'    {ctype} {f["cpp_name"]}[{sz}];')
        else:
            struct_lines.append(f'    {ctype} {f["cpp_name"]};')
    struct_lines.append(f'}} {safe_name}_payload_t;')

    payload_struct = '\n'.join(struct_lines)

    # Fixed vtable layout (mirrors the generated C++ HEADER_SIZE).
    vtable_total = blk.get('sizes', {})
    # Use the last version's size if available, else compute from layout.
    if vtable_total:
        vtable_size = max(vtable_total.values())
    else:
        vtable_size = (layout[-1]['offset'] + layout[-1]['size']) if layout else 10

    c_src = (
        "// =================================================================\n"
        f"// FastFHIR WASM Extension Codec — {ext_url}\n"
        "// AUTO-GENERATED by ffc.py --wasm. DO NOT EDIT.\n"
        "// Compile:\n"
        "//   wasi-sdk clang --target=wasm32-wasi -O3 -fno-exceptions\n"
        "//     -fno-rtti -nostdlib++ -Wl,--no-entry \\\n"
        f"//     -o {safe_name}.wasm {safe_name}_codec.c\n"
        "// =================================================================\n"
        "#include <stdint.h>\n"
        "#include <string.h>\n\n"
        "// ── Load/store helpers (little-endian, WASM native) ──────────────\n"
        "static inline uint8_t  ld_u8 (const uint8_t* p) { return *p; }\n"
        "static inline uint16_t ld_u16(const uint8_t* p) { uint16_t v; memcpy(&v,p,2); return v; }\n"
        "static inline uint32_t ld_u32(const uint8_t* p) { uint32_t v; memcpy(&v,p,4); return v; }\n"
        "static inline uint64_t ld_u64(const uint8_t* p) { uint64_t v; memcpy(&v,p,8); return v; }\n"
        "static inline void st_u8 (uint8_t* p, uint8_t  v) { *p = v; }\n"
        "static inline void st_u16(uint8_t* p, uint16_t v) { memcpy(p,&v,2); }\n"
        "static inline void st_u32(uint8_t* p, uint32_t v) { memcpy(p,&v,4); }\n"
        "static inline void st_u64(uint8_t* p, uint64_t v) { memcpy(p,&v,8); }\n\n"
        f"#define VTABLE_SIZE {vtable_size}u\n\n"
        f"{payload_struct}\n\n"
        "// ── ext_size ─────────────────────────────────────────────────────\n"
        "__attribute__((export_name(\"ext_size\")))\n"
        "uint32_t ext_size(uint32_t staging_ptr, uint32_t version) {\n"
        "    (void)version;\n"
        "    // The encoded vtable is always a fixed VTABLE_SIZE bytes for this type.\n"
        "    (void)staging_ptr;\n"
        "    return VTABLE_SIZE;\n"
        "}\n\n"
        "// ── ext_encode ───────────────────────────────────────────────────\n"
        "__attribute__((export_name(\"ext_encode\")))\n"
        "uint32_t ext_encode(uint32_t staging_ptr, uint32_t version) {\n"
        "    (void)version;\n"
        f"    {safe_name}_payload_t src;\n"
        "    memcpy(&src, (const void*)staging_ptr, sizeof(src));\n"
        "    uint8_t* dst = (uint8_t*)staging_ptr;\n"
        "    memset(dst, 0, VTABLE_SIZE);\n"
    )

    # Emit per-field encode logic.
    for f in layout[:_WASM_MAX_FIELDS]:
        ctype, sz = _wasm_field_ctype(f['fhir_type'])
        off = f['offset']
        name = f['cpp_name']
        if sz == _WASM_MAX_STRING_BYTES:
            # String: store first 4 bytes of length + up to sz chars inline.
            # Simple fixed-slot: encode as NUL-padded char array in vtable.
            c_src += f"    memcpy(dst + {off}, src.{name}, {min(sz, f['size'])});\n"
        elif sz == 1:
            c_src += f"    st_u8(dst + {off}, src.{name});\n"
        elif sz == 2:
            c_src += f"    st_u16(dst + {off}, (uint16_t)src.{name});\n"
        elif sz == 4:
            c_src += f"    st_u32(dst + {off}, (uint32_t)src.{name});\n"
        elif sz == 8:
            c_src += f"    st_u64(dst + {off}, (uint64_t)src.{name});\n"

    c_src += (
        "    return VTABLE_SIZE;\n"
        "}\n\n"
        "// ── ext_decode ───────────────────────────────────────────────────\n"
        "__attribute__((export_name(\"ext_decode\")))\n"
        "uint32_t ext_decode(uint32_t staging_ptr, uint32_t version) {\n"
        "    (void)version;\n"
        "    const uint8_t* src = (const uint8_t*)staging_ptr;\n"
        f"    {safe_name}_payload_t dst;\n"
        "    memset(&dst, 0, sizeof(dst));\n"
    )

    for f in layout[:_WASM_MAX_FIELDS]:
        ctype, sz = _wasm_field_ctype(f['fhir_type'])
        off = f['offset']
        name = f['cpp_name']
        if sz == _WASM_MAX_STRING_BYTES:
            c_src += f"    memcpy(dst.{name}, src + {off}, {min(sz, f['size'])});\n"
        elif sz == 1:
            c_src += f"    dst.{name} = ld_u8(src + {off});\n"
        elif sz == 2:
            c_src += f"    dst.{name} = ({ctype})ld_u16(src + {off});\n"
        elif sz == 4:
            c_src += f"    dst.{name} = ({ctype})ld_u32(src + {off});\n"
        elif sz == 8:
            c_src += f"    dst.{name} = ({ctype})ld_u64(src + {off});\n"

    c_src += (
        "    memcpy((void*)staging_ptr, &dst, sizeof(dst));\n"
        "    return (uint32_t)sizeof(dst);\n"
        "}\n"
    )

    os.makedirs(output_dir, exist_ok=True)
    out_path = os.path.join(output_dir, f"{safe_name}_codec.c")
    _write_if_changed(out_path, c_src)
    print(f"-- [wasm] Emitted {out_path}")


def compile_fhir_library_wasm(
    resources,
    versions,
    extension_urls,          # list of (url, path) tuples
    input_dir="fhir_specs",
    output_dir="generated_src/wasm",
):
    """Emit WASM codec sources for each entry in @p extension_urls.

    @p extension_urls  list of (canonical_url, fhir_type_path) e.g.
                       [("http://hl7.org/fhir/StructureDefinition/geolocation", "Extension")]
    """
    # Build the type bundles and master_blocks for the Extension type so we
    # can access the layout of each extension's StructureDefinition.
    type_bundles = []
    for v in versions:
        p = os.path.join(input_dir, v, "profiles-types.json")
        if os.path.exists(p):
            with open(p, "r", encoding="utf-8") as f:
                type_bundles.append((v, json.load(f)))

    for url, fhir_path in extension_urls:
        # Attempt to locate the StructureDefinition for this extension URL.
        # For well-known hl7.org extensions, look in profiles-types first.
        master_blocks = None
        for v_name, bundle in type_bundles:
            try:
                elements = extract_structure_definition(bundle, fhir_path.split('.')[-1])
                schemas  = [(v_name, elements)]
                master_blocks = merge_fhir_versions(schemas, fhir_path.split('.')[0])
                break
            except ValueError:
                continue

        if master_blocks is None:
            # Fall back to a minimal single-field layout.
            master_blocks = {
                fhir_path: {
                    'layout': [],
                    'seen':   set(),
                    'sizes':  {},
                }
            }

        emit_wasm_codec(url, master_blocks, fhir_path, output_dir)

    print(f"[wasm] Emitted {len(extension_urls)} codec source(s) → {output_dir}/")


if __name__ == "__main__":
    main()