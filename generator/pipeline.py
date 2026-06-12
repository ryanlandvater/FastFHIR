# ============================================================
# FastFHIR Generator — pipeline orchestrator.
#
# Relocated from tools/generator/make_lib.py::main(). ORCHESTRATION ONLY:
# it sequences the generation stages and threads `output_dir` through them.
# No code generation lives here.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: FastFHIR Shared Source License (FF-SSL)
# ============================================================

from __future__ import annotations

import os
import shutil

from generator import specs as fetch_specs
from generator.emit.dictionary import generate_master_dictionary
from generator.emit.codesystems import generate_code_systems
from generator.emit.extensions_known import generate_known_extensions
from generator.library import compile_fhir_library
from generator.model.structure import discover_versions, resolve_production_resources
from generator.model.type_map import PRODUCTION_TYPES

_SPECS_DIR = "fhir_specs"


def run(output_dir: str = "generated_src", *, keep_specs: bool = False) -> None:
    """Run the full FastFHIR generation pipeline.

    Args:
        output_dir: destination for generated C++ (default matches the build).
            Parametrising this is what lets the wire-format test harness
            regenerate into a temp dir for diffing.
        keep_specs: if True, do not delete `fhir_specs/` after generation
            (useful when iterating locally to avoid re-downloading).
    """
    # 1. Get the specs (network fetch into fhir_specs/).
    fetch_specs.fetch_fhir_specs()
    versions = discover_versions(_SPECS_DIR)
    if not versions:
        raise RuntimeError("No FHIR versions found in fhir_specs after fetch.")
    version_configs = [(v, os.path.join(_SPECS_DIR, v)) for v in versions]
    resources = resolve_production_resources(specs_dir=_SPECS_DIR, versions=versions)

    # 2. Build the dictionaries (FF_*_Dictionary.*).
    code_system_urls = generate_master_dictionary(version_configs, output_dir=output_dir)

    # 3. Build the code-system enums (FF_CodeSystems.hpp).
    code_enum_map, external_system_map = generate_code_systems(
        PRODUCTION_TYPES, resources, versions=versions, output_dir=output_dir
    )

    # 4. Build the domains/resources (the bulk of generated_src/).
    compile_fhir_library(
        resources, versions, code_enum_map=code_enum_map,
        external_system_map=external_system_map, output_dir=output_dir
    )

    # 5. Build the known-extension filter table (includes code system URLs).
    generate_known_extensions(versions, specs_dir=_SPECS_DIR, output_dir=output_dir, code_system_urls=code_system_urls)

    # 6. Cleanup downloaded specs after successful generation.
    if not keep_specs and os.path.isdir(_SPECS_DIR):
        shutil.rmtree(_SPECS_DIR)
        print(f"[Cleanup] Removed {_SPECS_DIR}/")

    print("\n[Success] FastFHIR Library generation complete.")
