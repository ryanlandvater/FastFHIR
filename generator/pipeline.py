# ============================================================
# FastFHIR Generator — pipeline orchestrator.
#
# Sequences generation stages. No code generation lives here.
# ============================================================

from __future__ import annotations
import os

from generator import specs as fetch_specs
from generator.emit.dictionary import generate_master_codes
from generator.emit.codesystems import generate_code_systems
from generator.emit.codes_header import generate as generate_codes_header
from generator.emit.extensions_known import generate_known_extensions
from generator.library import compile_fhir_library
from generator.model.structure import resolve_production_resources
from generator.model.type_map import PRODUCTION_TYPES

_PKG_DIR = "fhir_packages"


def _pkg(version: str) -> str:
    return os.path.join(_PKG_DIR, version, "package")


def run(output_dir: str = "generated_src", *, keep_specs: bool = False) -> None:
    """Run the full FastFHIR generation pipeline."""

    # 1. Fetch FHIR specs as NPM packages
    fetch_specs.fetch_fhir_packages()
    versions = ["R4", "R5"]

    # 2. Build the master code dictionary + per-system code enums
    systems, ids_by_version, code_system_urls = generate_master_codes({
        "R4": _pkg("R4"),
        "R5": _pkg("R5"),
    })

    # 3. Build the C++ code header (FF_Codes.hpp)
    generate_codes_header()

    # 4. Resolve resource types and build code-system enums
    resources = resolve_production_resources(specs_dir=_pkg(""))
    code_enum_map, external_system_map = generate_code_systems(
        PRODUCTION_TYPES, resources, versions=versions, input_dir=_pkg(""),
        output_dir=output_dir,
    )

    # 5. Compile the FHIR library (data types + resources)
    compile_fhir_library(
        resources, versions, code_enum_map=code_enum_map,
        external_system_map=external_system_map,
        input_dir=_pkg(""), output_dir=output_dir,
    )

    # 6. Build the known-extension filter table
    generate_known_extensions(
        versions, specs_dir=_pkg(""),
        output_dir=output_dir, code_system_urls=code_system_urls,
    )

    print("\n[Success] FastFHIR Library generation complete.")
