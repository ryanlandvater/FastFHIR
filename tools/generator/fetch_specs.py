import os
import zipfile
import urllib.request
from pathlib import Path

# --- Configuration ---
FHIR_CONFIG = {
    "R4": "http://hl7.org/fhir/R4/definitions.json.zip",
    "R5": "http://hl7.org/fhir/R5/definitions.json.zip"
}

# Where the raw HL7 data lives
BASE_DIR = Path("fhir_specs")

# The specific files our ffc.py and ffd.py need
REQUIRED_FILES = [
    "profiles-resources.json",
    "profiles-types.json",
    "valuesets.json"
]

def fetch_fhir_specs(force_download=False):
    """
    Downloads and extracts the FHIR R4 and R5 specifications.
    Ensures 'fhir_specs/' is populated without polluting the root.
    """
    BASE_DIR.mkdir(exist_ok=True)

    for version, url in FHIR_CONFIG.items():
        v_dir = BASE_DIR / version
        v_dir.mkdir(exist_ok=True)
        
        zip_path = v_dir / "definitions.zip"

        # 1. Download (only if missing or forced)
        if not zip_path.exists() or force_download:
            print(f"Downloading FHIR {version} from HL7...")
            try:
                # Basic progress reporter
                def progress(count, block_size, total_size):
                    percent = int(count * block_size * 100 / total_size)
                    print(f"\r   Progress: {percent}%", end="")

                urllib.request.urlretrieve(url, zip_path, reporthook=progress)
                print(f"\n   [Success] Downloaded {version}.")
            except Exception as e:
                print(f"\n   [Error] Error downloading {version}: {e}")
                continue
        else:
            print(f"[Info] FHIR {version} zip already exists in {v_dir}. Skipping download.")

        # 2. Extract
        print(f"[Info] Extracting required bundles to {v_dir}...")
        try:
            with zipfile.ZipFile(zip_path, 'r') as zip_ref:
                # Only extract what we actually use
                for file_name in REQUIRED_FILES:
                    if file_name in zip_ref.namelist():
                        zip_ref.extract(file_name, v_dir)
                        print(f"   [Info] Extracted {file_name}")
                    else:
                        print(f"   [Warning] {file_name} not found in zip.")
            print(f"[Success] FHIR {version} ready.")
        except zipfile.BadZipFile:
            print(f"   [Error] {zip_path} is not a valid zip file. Try running with force_download=True.")

if __name__ == "__main__":
    fetch_fhir_specs()