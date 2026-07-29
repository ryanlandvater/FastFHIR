# ============================================================
# FastFHIR Generator — FHIR specification fetcher (NPM packages).
#
# Downloads hl7.fhir.r4.core and hl7.fhir.r5.core .tgz packages from
# https://packages.fhir.org, extracts into fhir_packages/R4/package/
# and fhir_packages/R5/package/.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/
# ============================================================

import os
import urllib.request
import tarfile
import shutil
from pathlib import Path

FHIR_PACKAGES = {
    "R4": "https://packages.fhir.org/hl7.fhir.r4.core/-/hl7.fhir.r4.core-4.0.1.tgz",
    "R5": "https://packages.fhir.org/hl7.fhir.r5.core/-/hl7.fhir.r5.core-5.0.0.tgz",
}

BASE_DIR = Path("fhir_packages")


def fetch_fhir_specs(force_download=False):
    """Download and extract FHIR R4/R5 NPM packages.

    Ensures 'fhir_packages/R4/package/' and 'fhir_packages/R5/package/'
    are populated with CodeSystem-*.json and ValueSet-*.json files.
    """
    BASE_DIR.mkdir(exist_ok=True)

    for version, url in FHIR_PACKAGES.items():
        v_dir = BASE_DIR / version
        package_dir = v_dir / "package"

        if package_dir.exists() and not force_download:
            code_systems = list(package_dir.glob("CodeSystem-*.json"))
            print(
                f"[Info] FHIR {version} package already exists ({len(code_systems)} CodeSystems). Skipping download."
            )
            continue

        # Clean and recreate
        if v_dir.exists():
            shutil.rmtree(v_dir)
        v_dir.mkdir()

        tgz_path = v_dir / "package.tgz"
        print(f"Downloading FHIR {version} NPM package...")
        try:
            urllib.request.urlretrieve(url, tgz_path)
        except Exception as e:
            print(f"[Error] Download failed for {version}: {e}")
            continue

        print(f"Extracting {version}...")
        try:
            with tarfile.open(tgz_path, "r:gz") as tar:
                tar.extractall(path=v_dir)
        except tarfile.TarError as e:
            print(f"[Error] Extraction failed for {version}: {e}")
            continue

        tgz_path.unlink()  # remove the .tgz after extraction
        code_systems = list(package_dir.glob("CodeSystem-*.json"))
        print(f"[Success] FHIR {version} ready ({len(code_systems)} CodeSystems).")


if __name__ == "__main__":
    fetch_fhir_specs()
