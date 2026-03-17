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

RESOURCES = ["Observation", "Patient"]

def _version_sort_key(label):
	"""Sort labels like R4, R4B, R5, R65 deterministically."""
	m = re.match(r"^R(\d+)([A-Za-z]*)$", label)
	if not m:
		return (10**9, label)
	major = int(m.group(1))
	minor = m.group(2).upper()
	return (major, minor)

def _discover_versions(specs_dir="fhir_specs"):
	"""Discover available FHIR versions from extracted spec folders."""
	if not os.path.isdir(specs_dir):
		return []
	versions = []
	for name in os.listdir(specs_dir):
		full = os.path.join(specs_dir, name)
		if os.path.isdir(full) and re.match(r"^R\d+[A-Za-z]*$", name):
			versions.append(name)
	return sorted(set(versions), key=_version_sort_key)

def main():
	# 1. Get the specs
	fetch_specs.fetch_fhir_specs()
	versions = _discover_versions("fhir_specs")
	if not versions:
		raise RuntimeError("No FHIR versions found in fhir_specs after fetch.")
	version_configs = [(v, os.path.join("fhir_specs", v)) for v in versions]

	# 2. Build the dictionaries
	ffd.generate_master_dictionary(version_configs)

	# 3. Build the code system enums (FF_CodeSystems.hpp)
	code_enum_map = ffcs.generate_code_systems(ffc.TARGET_TYPES, RESOURCES, versions)

	# 4. Build the domains/resources
	ffc.compile_fhir_library(RESOURCES, versions, code_enum_map=code_enum_map)

	# 4. Cleanup downloaded specs after successful generation
	specs_dir = "fhir_specs"
	if os.path.isdir(specs_dir):
		shutil.rmtree(specs_dir)
		print("[Cleanup] Removed fhir_specs/")

	print("\n[Success] FastFHIR Library generation complete.")


if __name__ == "__main__":
	main()