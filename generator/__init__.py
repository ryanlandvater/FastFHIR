# ============================================================
# FastFHIR Generator — public package façade.
#
# External callers (build scripts, tests, other tools) import from
# `generator` ONLY — never from internal submodules. That keeps the public
# surface to five stable symbols while S2-S6 reorganise the internals.
#
# During migration (S1) these symbols still resolve to the legacy
# tools/generator modules via the path bridge in pipeline.py. As S2-S5 move
# logic into model/emit/bindings, the imports below are repointed there with
# no change to the public names.
# ============================================================

from __future__ import annotations

from generator import pipeline as pipeline  # noqa: F401
from generator.emit.header import auto_header as auto_header
from generator.library import compile_fhir_library as compile_fhir_library
from generator.model import structure as _structure
from generator.model import type_map as _type_map

# --- stable public API -------------------------------------------------------
resolve_production_resources = _structure.resolve_production_resources
discover_versions = _structure.discover_versions
PRODUCTION_TYPES = _type_map.PRODUCTION_TYPES

run = pipeline.run

__all__ = [
    "compile_fhir_library",
    "resolve_production_resources",
    "discover_versions",
    "PRODUCTION_TYPES",
    "auto_header",
    "run",
]
