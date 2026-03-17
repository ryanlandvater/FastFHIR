import os
import shutil

from src import fetch_specs
from src import ffd
from src import ffcs
from src import ffc

VERSIONS  = ["R4", "R5"]
RESOURCES = ["Observation", "Patient"]

def main():
	# 1. Get the specs
	fetch_specs.fetch_fhir_specs()

	# 2. Build the dictionaries
	ffd.generate_master_dictionary([("R4", "fhir_specs/R4"), ("R5", "fhir_specs/R5")])

	# 3. Build the code system enums (FF_CodeSystems.hpp)
	code_enum_map = ffcs.generate_code_systems(ffc.TARGET_TYPES, RESOURCES, VERSIONS)

	# 4. Build the domains/resources
	ffc.compile_fhir_library(RESOURCES, VERSIONS, code_enum_map=code_enum_map)

	# 4. Cleanup downloaded specs after successful generation
	specs_dir = "fhir_specs"
	if os.path.isdir(specs_dir):
		shutil.rmtree(specs_dir)
		print("[Cleanup] Removed fhir_specs/")

	print("\n[Success] FastFHIR Library generation complete.")


if __name__ == "__main__":
	main()