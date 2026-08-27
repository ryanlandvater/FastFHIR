# ============================================================
# FastFHIR Generator — pipeline orchestrator.
#
# Sequences generation stages. No code generation lives here.
#
# Two kinds of output, and the difference matters:
#
#   dictionaries/   PERMANENT numbering. Stage 2 may only APPEND code IDs;
#                   an existing ID is a wire constant and never moves.
#                   Committed to git. See dictionaries/README.md.
#
#   generated_src/  Freely regenerable C++. Gitignored, rebuilt from the
#                   HL7 packages on every configure.
# ============================================================

from __future__ import annotations

import os

from generator import specs as fetch_specs
from generator.emit.code_names import generate as generate_code_names
from generator.emit.codesystems import generate_code_systems
from generator.emit.code_ids import generate_dictionary_tables, generate_master_codes
from generator.emit.extensions_known import generate_known_extensions
from generator.emit.recovery_tags import generate_recovery_tags
from generator.library import compile_fhir_library
from generator.model.structure import resolve_production_resources
from generator.model.type_map import PRODUCTION_TYPES

# Root of the extracted HL7 NPM packages. Individual versions live at
# <root>/<version>/package; the callees below append that themselves, so they
# each take the ROOT, not a version directory.
_PKG_ROOT = "fhir_packages"


def _pkg(version: str) -> str:
    """Path to one version's package directory."""
    return os.path.join(_PKG_ROOT, version, "package")


def run(output_dir: str = "generated_src", *, keep_specs: bool = False) -> None:
    """Run the full FastFHIR generation pipeline."""
    versions = ["R4", "R5"]
    os.makedirs(output_dir, exist_ok=True)

    # 1. Fetch the FHIR specs as NPM packages.
    fetch_specs.fetch_fhir_specs()

    # 1b. Reconcile the permanent RECOVERY_TAG ledger and emit FF_RecoveryTags.hpp.
    #     Runs before anything that references a tag. Append-only: a new FHIR
    #     release takes the next free value in its band and nothing already
    #     assigned moves. Covers the whole spec, so the header does NOT vary
    #     with FASTFHIR_PRODUCTION_PROFILE.
    n_tags, n_new = generate_recovery_tags(
        specs_dir=_PKG_ROOT,
        header_path=os.path.join(output_dir, "FF_RecoveryTags.hpp"),
    )
    print(f"  Recovery tags: {n_tags} in ledger, {n_new} appended")

    # 2. Reconcile the HL7 code sets against the permanent ID ledger.
    #    APPEND-ONLY: existing IDs are never reassigned. Stage 3 then projects
    #    the ledger into dictionaries/.
    _systems, ledger, code_system_urls = generate_master_codes(
        {
            "R4": _pkg("R4"),
            "R5": _pkg("R5"),
        }
    )

    # 3. Project the ledger into the output dir (FF_Codes.hpp + runtime tables).
    generate_code_names(output_dir=output_dir)
    generate_dictionary_tables(ledger, output_dir=output_dir)

    # 4. Resolve resource types and build code-system enums.
    resources = resolve_production_resources(specs_dir=_PKG_ROOT)
    code_enum_map, external_system_map = generate_code_systems(
        PRODUCTION_TYPES,
        resources,
        versions=versions,
        input_dir=_PKG_ROOT,
        output_dir=output_dir,
    )

    # 5. Compile the FHIR library (data types + resources).
    compile_fhir_library(
        resources,
        versions,
        code_enum_map=code_enum_map,
        external_system_map=external_system_map,
        input_dir=_PKG_ROOT,
        output_dir=output_dir,
    )

    # 6. Build the known-extension filter table.
    generate_known_extensions(
        versions,
        specs_dir=_PKG_ROOT,
        output_dir=output_dir,
        code_system_urls=code_system_urls,
    )

    if not keep_specs:
        pass  # specs are cached in fhir_packages/ and reused; nothing to clean.

    print("\n[Success] FastFHIR Library generation complete.")
