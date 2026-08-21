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

# ---------------------------------------------------------------------------
# Resource groupings
# ---------------------------------------------------------------------------
# FASTFHIR_PRODUCTION_PROFILE takes a COMMA-SEPARATED LIST of the names in
# RESOURCE_GROUPINGS below, and the generator compiles their union -- real
# deployments compose (a payer needs US Core *and* claims), so these are not
# mutually exclusive. See resolve_production_resources() in model/structure.py.
#
# "The US profile" is not one document. HL7 publishes several US-realm IGs for
# different actors, which is why ExplanationOfBenefit is absent from US Core and
# its absence is not an oversight:
#
#   US Core          -- HL7, US Realm Steering Committee. Provider/EHR clinical
#                       data; realizes USCDI (defined by ONC/ASTP, not HL7).
#                       https://hl7.org/fhir/us/core/
#   CARIN Blue Button-- HL7 + CARIN Alliance. Payer claims; EOB is the
#                       centerpiece. https://hl7.org/fhir/us/carin-bb/
#   UK Core          -- HL7 UK. https://simplifier.net/hl7fhirukcorer4
#
# TODO(A27): these lists are hand-maintained and carry no IG *version*, so drift
# against a republished IG is undetectable. HL7 ships them machine-readably as
# NPM packages on packages.fhir.org -- the same registry the generator already
# pulls hl7.fhir.r4.core / hl7.fhir.r5.core from -- so these should eventually be
# derived from `hl7.fhir.us.core` etc. rather than transcribed.
#
# COST NOTE: every resource added here needs its own RECOVERY_TAG plus one per
# nested BackboneElement. Those are permanent wire constants, but they are no
# longer a per-grouping cost: dictionaries/master_tags.json covers the whole
# R4 union R5 spec (978 tags), so the tags for every grouping already exist and
# generated_src/FF_Recovery.hpp is byte-identical whichever profile is compiled.
# What a grouping still costs is emitted C++ volume and compile time.
# Historical: before the ledger, BILLING_RESOURCES needed 59 tags appended by
# hand and profile "all" needed 884 -- see TASKS.md A27.5.

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

# Payer / claims resources. Coverage is deliberately absent -- it is already in
# US Core, and the groupings are unioned, so listing it here would only obscure
# which grouping introduced it. Scoped to the CARIN Blue Button and Da Vinci PAS
# core rather than every financial resource: Account, Invoice, ChargeItem and
# Contract would add a further 29 tags for resources no claims flow requires.
BILLING_RESOURCES: list[str] = [
    "Claim",
    "ClaimResponse",
    "ExplanationOfBenefit",
    "PaymentNotice",
    "PaymentReconciliation",
]

# The accepted grouping names. Add a grouping by adding one entry here; nothing
# else in the generator needs to change. "all" is handled separately in
# resolve_production_resources() because it is discovered from the packages
# rather than listed.
RESOURCE_GROUPINGS: dict[str, list[str]] = {
    "us-core": US_CORE_RESOURCES,
    "uk-core": UK_CORE_RESOURCES,
    "billing": BILLING_RESOURCES,
}

# Back-compat spellings for the pre-array profile values. "us"/"uk" were the
# only accepted values when the setting was a single string.
GROUPING_ALIASES: dict[str, str] = {
    "us": "us-core",
    "uk": "uk-core",
}

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
    # DT-2 — packed date/time. `cpp` is the 8-byte slot, `data_type` is the
    # author-facing TEXT: the slot stores a packed civil value, and the text is
    # SYNTHESISED on read by FF_FORMAT_DATETIME, so the data struct owns a
    # std::string rather than viewing arena bytes like a real string field does.
    # That is the one place a date/time field differs from every other scalar:
    # its `cpp` and `data_type` are not the same shape, so it needs a codec
    # rather than a LOAD/STORE macro pair.
    "date": {
        "cpp": "uint64_t",
        "data_type": "std::string",
        "null": "FF_DATETIME_NULL",
        "size": 8,
        "size_const": "TYPE_SIZE_UINT64",
        "macro": "LOAD_U64",
        "encode": "ENCODE_FF_DATETIME",
        "decode": "FF_FORMAT_DATETIME",
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
#
# date/dateTime/instant/time were removed from this set by DT-2: they are no
# longer an offset to an FF_STRING but an 8-byte inline packed value (see
# DATETIME_TYPES below and architecture.md 6.3). The slot WIDTH is unchanged --
# 8 bytes either way -- so no V-Table offset moves; only the interpretation of
# those 8 bytes does.
STRING_TYPES: set[str] = {
    "string",
    "id",
    "markdown",
    "uri",
    "url",
    "canonical",
    "oid",
    "base64Binary",
    "xhtml",
}

# The four FHIR date/time types and the permanent RECOVERY_TAG each one carries.
#
# One packed layout, four tags: the tag names which FHIR type a slot holds and
# is the ONLY thing that can, because a choice ([x]) slot has nothing else to
# distinguish valueDate from valueDateTime. The single FF_FIELD_DATETIME kind
# says only "inline 8 bytes"; it deliberately cannot be mapped back to a tag.
#
# Membership must be tested with `fhir_type in DATETIME_TYPES`, never against
# individual names -- the same rule CLAUDE.md states for STRING_TYPES, and for
# the same reason: an ad-hoc `== "dateTime"` silently drops `date`.
DATETIME_TYPES: dict[str, str] = {
    "date": "RECOVER_FF_DATE",
    "dateTime": "RECOVER_FF_DATETIME",
    "instant": "RECOVER_FF_INSTANT",
    "time": "RECOVER_FF_TIME",
}

# The remaining three share `date`'s descriptor exactly -- same width, same
# codec, same null -- and differ only in the RECOVERY_TAG, which lives in
# DATETIME_TYPES. Projected rather than copy-pasted so a change to the packed
# representation cannot be applied to three of the four by accident.
for _dt_type in DATETIME_TYPES:
    if _dt_type not in TYPE_MAP:
        TYPE_MAP[_dt_type] = dict(TYPE_MAP["date"])


# ---------------------------------------------------------------------------
# Code-enum unset sentinel
# ---------------------------------------------------------------------------

# Every generated code enum carries this enumerator, and every code-typed POD
# member defaults to it. Without it, enum value 0 is a real FHIR code and an
# absent field is indistinguishable from an asserted one -- so a Patient with no
# gender was written to the wire as "female", an AllergyIntolerance with no
# criticality as "high", and a Quantity with no comparator as "<" (TASKS.md A24).
#
# It lives here, in the model layer, because both emit/codesystems.py (which
# declares the enums) and model/merge.py (which defaults the POD members) need
# the same spelling, and merge.py must never import from emit/.
#
# The value is pinned rather than appended so that adding a code to a ValueSet
# does not move it. serialize_*() has no case for it and falls through to
# `default: return ""`, which ENCODE_FF_CODE already maps to FF_CODE_NULL and
# SIZE_FF_CODE already sizes as 0 -- so the sentinel needs no special handling
# on the store path, and SIZE/STORE stay in agreement (A23.6).
UNSET_ENUMERATOR: str = "FF_UNSET"


def enum_underlying_type(code_count: int) -> tuple[str, int]:
    """Return the (C++ underlying type, sentinel value) for a code enum.

    The enum ordinal is **not** a wire value -- the wire carries the dictionary
    code produced by ENCODE_FF_CODE -- so widening the underlying type is a
    source-level change and costs only POD bytes.

    Widening is not optional at profile=all: FF_SPDXLicense carries 346 codes,
    and in a uint8_t enum values 256..345 wrap onto 0..89, silently aliasing
    distinct licences onto each other. That landmine predates the UNSET sentinel;
    the sentinel merely made it fail loudly instead of at runtime (TASKS.md A26).
    """
    if code_count < 255:
        return "uint8_t", 255
    if code_count < 65535:
        return "uint16_t", 65535
    raise ValueError(
        f"a code enum with {code_count} codes exceeds uint16_t; widen "
        "enum_underlying_type() before adding a ValueSet this large."
    )


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
        # DT-2: one packed layout, four tags. The tag is what the exporter reads
        # to choose valueDate vs valueDateTime, so it must survive per type --
        # FF_FIELD_DATETIME alone cannot express which of the four this is.
        **DATETIME_TYPES,
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
