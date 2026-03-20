import os
import shutil
import re

if __package__:
	from . import fetch_specs
	from . import ffd
	from . import ffcs
	from . import ffc
else:
	import pathlib
	import sys
	sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
	from generator import fetch_specs
	from generator import ffd
	from generator import ffcs
	from generator import ffc

def main():
	# 1. Get the specs
	fetch_specs.fetch_fhir_specs()
	versions = ffc._discover_versions("fhir_specs")
	if not versions:
		raise RuntimeError("No FHIR versions found in fhir_specs after fetch.")
	version_configs = [(v, os.path.join("fhir_specs", v)) for v in versions]

	# 2. Build the dictionaries
	ffd.generate_master_dictionary(version_configs)

	# 3. Build the code system enums (FF_CodeSystems.hpp)
	code_enum_map = ffcs.generate_code_systems(ffc.TARGET_TYPES, ffc.TARGET_RESOURCES, versions=versions)

	# 4. Build the domains/resources
	ffc.compile_fhir_library(ffc.TARGET_RESOURCES, versions, code_enum_map=code_enum_map)

	# 4. Cleanup downloaded specs after successful generation
	specs_dir = "fhir_specs"
	if os.path.isdir(specs_dir):
		shutil.rmtree(specs_dir)
		print("[Cleanup] Removed fhir_specs/")

	print("\n[Success] FastFHIR Library generation complete.")


if __name__ == "__main__":
	main()