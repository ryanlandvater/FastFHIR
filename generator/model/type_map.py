# ============================================================
# FastFHIR Generator — FHIR type map (model layer).
#
# Relocated body from tools/generator/ffc.py lines 33–171 + 2264.
# TYPE_MAP, PRODUCTION_TYPES, profile catalogs, scalar/string type sets,
# name normalisation helpers, version sorting.
#
# All consuming modules (model/structure, emit/*, library) import from here
# instead of reading ffc.py globals. This is the deepest dependency so it
# imports nothing from the rest of the generator.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/
# ============================================================

from __future__ import annotations

import os
import re

# ---------------------------------------------------------------------------
# Production profile
# ---------------------------------------------------------------------------
PRODUCTION_TYPES: list[str] = [
    "Extension",
    "Coding",
    "CodeableConcept",
    "Quantity",
    "Identifier",
    "Age",
    "Count",
    "Distance",
    "Range",
    "Period",
    "Reference",
    "Meta",
    "Narrative",
    "Annotation",
    "HumanName",
    "Address",
    "ContactPoint",
    "Attachment",
    "Ratio",
    "SampledData",
    "Duration",
    "Availability",
    "ExtendedContactDetail",
    "Timing",
    "Dosage",
    "Signature",
    "CodeableReference",
    "VirtualServiceDetail",
]

US_CORE_RESOURCES: list[str] = [
    "AllergyIntolerance",
    "Bundle",
    "CarePlan",
    "CareTeam",
    "Condition",
    "Coverage",
    "Device",
    "DiagnosticReport",
    "DocumentReference",
    "Encounter",
    "Goal",
    "Immunization",
    "Location",
    "Medication",
    "MedicationDispense",
    "MedicationRequest",
    "MedicationStatement",
    "Observation",
    "Organization",
    "Patient",
    "Practitioner",
    "PractitionerRole",
    "Procedure",
    "Provenance",
    "QuestionnaireResponse",
    "RelatedPerson",
    "ServiceRequest",
    "Specimen",
]

UK_CORE_RESOURCES: list[str] = [
    "AllergyIntolerance",
    "Appointment",
    "Bundle",
    "CarePlan",
    "CareTeam",
    "Condition",
    "DiagnosticReport",
    "Encounter",
    "Immunization",
    "Location",
    "Medication",
    "MedicationDispense",
    "MedicationRequest",
    "MedicationStatement",
    "Observation",
    "Organization",
    "Patient",
    "Practitioner",
    "Procedure",
    "QuestionnaireResponse",
    "RelatedPerson",
    "ServiceRequest",
    "Specimen",
]

PRODUCTION_PROFILE_ENV: str = "FASTFHIR_PRODUCTION_PROFILE"

# ---------------------------------------------------------------------------
# FHIR → C++ type map
# ---------------------------------------------------------------------------
TYPE_MAP: dict[str, dict] = {
    "boolean": {
        "cpp": "uint8_t",
        "data_type": "uint8_t",
        "null": "FF_NULL_UINT8",
        "size": 1,
        "size_const": "TYPE_SIZE_UINT8",
        "macro": "LOAD_U8",
    },
    "integer": {
        "cpp": "uint32_t",
        "data_type": "uint32_t",
        "null": "FF_NULL_UINT32",
        "size": 4,
        "size_const": "TYPE_SIZE_UINT32",
        "macro": "LOAD_U32",
    },
    "unsignedInt": {
        "cpp": "uint32_t",
        "data_type": "uint32_t",
        "null": "FF_NULL_UINT32",
        "size": 4,
        "size_const": "TYPE_SIZE_UINT32",
        "macro": "LOAD_U32",
    },
    "positiveInt": {
        "cpp": "uint32_t",
        "data_type": "uint32_t",
        "null": "FF_NULL_UINT32",
        "size": 4,
        "size_const": "TYPE_SIZE_UINT32",
        "macro": "LOAD_U32",
    },
    "integer64": {
        "cpp": "uint64_t",
        "data_type": "uint64_t",
        "null": "FF_NULL_UINT64",
        "size": 8,
        "size_const": "TYPE_SIZE_UINT64",
        "macro": "LOAD_U64",
    },
    "decimal": {
        "cpp": "double",
        "data_type": "double",
        "null": "FF_NULL_F64",
        "size": 8,
        "size_const": "TYPE_SIZE_FLOAT64",
        "macro": "LOAD_F64",
    },
    "code": {
        "cpp": "uint32_t",
        "data_type": "std::string_view",
        "null": '""',
        "size": 4,
        "size_const": "TYPE_SIZE_UINT32",
        "macro": "LOAD_U32",
    },
    "Resource": {
        "cpp": "ResourceReference",
        "data_type": "ResourceReference",
        "null": "{}",
        "size": 10,
        "size_const": "TYPE_SIZE_RESOURCE",
        "macro": "LOAD_RESOURCE",
    },
    "CHOICE": {
        "cpp": "ChoiceEntry",
        "data_type": "ChoiceEntry",
        "null": "{}",
        "size": 10,
        "size_const": "TYPE_SIZE_CHOICE",
        "macro": "LOAD_VARIANT",
    },
    "DEFAULT": {
        "cpp": "Offset",
        "data_type": "Offset",
        "null": "FF_NULL_OFFSET",
        "size": 8,
        "size_const": "TYPE_SIZE_OFFSET",
        "macro": "LOAD_U64",
    },
}

# Concrete primitive FHIR types that get an inline vtable entry.
SCALAR_PRIMITIVE_TYPES: set[str] = {
    "boolean",
    "integer",
    "unsignedInt",
    "positiveInt",
    "integer64",
    "decimal",
}

# FHIR types that collapse to 'string' in the layout model.
STRING_TYPES: set[str] = {
    "string",
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
    "xhtml",
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _scalar_recovery_tag(fhir_type: str) -> str:
    """Map a scalar FHIR type to its RECOVER_FF_* tag name."""
    tag_map = {
        "boolean": "RECOVER_FF_BOOL",
        "integer": "RECOVER_FF_UINT32",
        "unsignedInt": "RECOVER_FF_UINT32",
        "positiveInt": "RECOVER_FF_UINT32",
        "integer64": "RECOVER_FF_UINT64",
        "decimal": "RECOVER_FF_FLOAT64",
    }
    return tag_map.get(fhir_type, "FF_RECOVER_UNDEFINED")


def sanitize_fhir_type(raw_type: str) -> str:
    """Normalise a raw FHIR type token (strips URLs, System. prefix, etc.)."""
    if "/" in raw_type:
        raw_type = raw_type.split("/")[-1]
    if raw_type.startswith("System."):
        raw_type = raw_type[7:]
    if raw_type.lower() in STRING_TYPES:
        return "string"
    return raw_type


def get_store_macro(load_macro: str) -> str:
    """Map a LOAD_* macro name to its STORE_* counterpart."""
    return load_macro.replace("LOAD_", "STORE_")


def _version_sort_key(label: str) -> tuple:
    """Sort labels like R4, R4B, R5, R65 deterministically."""
    m = re.match(r"^R(\d+)([A-Za-z]*)$", label)
    if not m:
        return (10**9, label)
    major = int(m.group(1))
    minor = m.group(2).upper()
    return (major, minor)
