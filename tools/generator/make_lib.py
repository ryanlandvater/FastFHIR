import os
import json
import shutil
import re

def _write_if_changed(path: str, content: str, encoding: str = "utf-8") -> None:
    """Write only when content has changed, preserving mtime on no-op regeneration."""
    if os.path.exists(path):
        try:
            with open(path, "r", encoding=encoding) as fh:
                if fh.read() == content:
                    return
        except OSError:
            pass
    with open(path, "w", encoding=encoding) as fh:
        fh.write(content)

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


# =====================================================================
# KNOWN-EXTENSION FILTER GENERATOR
# Emits generated_src/FF_KnownExtensions.hpp — a compile-time sorted
# URL table used by FF_PredigestExtensionURLs to suppress known/native
# extensions without WASM dispatch.
# =====================================================================

# Category 1: Profile-native extensions will be fetched from spec bundles at generation time.
# These include base FHIR extensions (hl7.org) + major profiles (us-core, uk-core).
# They are suppressed during predigestion to avoid wasting URL directory space on
# extensions that are either compiled into profiles or are so standard they're
# always expected to be present.

# Category 2: HL7-known-informational-only extensions.
# ⚠️  STRICT FILTER: Only extensions that are GUARANTEED to carry zero semantic
# weight and are NOT compiled into any profile.  These are pure metadata hints
# (rendering, narrative, ISO qualifiers) with no clinical impact.
# Patient/event/workflow extensions are NOT included here because they are
# often compiled into profiles or carry semantic weight. When in doubt, let
# WASM dispatch handle the extension — never assume it's safe to drop.
_HL7_KNOWN_SAFE_URLS = [
	"http://hl7.org/fhir/StructureDefinition/data-absent-reason",
	"http://hl7.org/fhir/StructureDefinition/display",
	"http://hl7.org/fhir/StructureDefinition/geolocation",
	"http://hl7.org/fhir/StructureDefinition/iso21090-AD-use",
	"http://hl7.org/fhir/StructureDefinition/iso21090-EN-qualifier",
	"http://hl7.org/fhir/StructureDefinition/iso21090-EN-use",
	"http://hl7.org/fhir/StructureDefinition/iso21090-nullFlavor",
	"http://hl7.org/fhir/StructureDefinition/narrativeLink",
	"http://hl7.org/fhir/StructureDefinition/originalText",
	"http://hl7.org/fhir/StructureDefinition/rendered-value",
	"http://hl7.org/fhir/StructureDefinition/translation",
]


def generate_known_extensions(versions, specs_dir="fhir_specs", output_dir="generated_src"):
	"""Emit generated_src/FF_KnownExtensions.hpp.

	Collects Extension StructureDefinition URLs from the FHIR spec bundles
	(all versions), automatically identifies profile-native extensions (base FHIR
	+ major profiles like US Core, UK Core), merges with HL7-known-safe list,
	and emits sorted compile-time arrays with O(log n) lookup helpers.
	"""
	# Collect all extension URLs from the downloaded FHIR spec bundles.
	all_spec_ext_urls: set[str] = set()
	for v in versions:
		for bundle_file in ("profiles-types.json", "profiles-resources.json"):
			path = os.path.join(specs_dir, v, bundle_file)
			if not os.path.exists(path):
				continue
			with open(path, "r", encoding="utf-8") as fh:
				bundle = json.load(fh)
			for entry in bundle.get("entry", []):
				res = entry.get("resource", {})
				if res.get("resourceType") != "StructureDefinition":
					continue
				if res.get("type") == "Extension":
					url = res.get("url", "").strip()
					if url:
						all_spec_ext_urls.add(url)

	# Identify profile-native extensions: base FHIR + major profiles (US Core, UK Core).
	# These are suppressed because they're either compiled into profiles or so standard
	# they don't need WASM dispatch or URL directory entries.
	profile_native_urls = {
		url for url in all_spec_ext_urls
		if (url.startswith("http://hl7.org/fhir/StructureDefinition/") or
		    url.startswith("http://hl7.org/fhir/us/core/") or
		    url.startswith("https://fhir.hl7.org.uk/"))
	}

	native_sorted    = sorted(profile_native_urls)
	all_known_sorted = sorted(profile_native_urls | set(_HL7_KNOWN_SAFE_URLS) | all_spec_ext_urls)

	def _url_array(urls: list[str], arr_name: str, count_name: str) -> str:
		lines = [f"static constexpr size_t {count_name} = {len(urls)};"]
		if urls:
			lines.append(f"static constexpr const char* const {arr_name}[] = {{")
			for url in urls:
				escaped = url.replace("\\", "\\\\").replace('"', '\\"')
				lines.append(f'    "{escaped}",')
			lines.append("};")
		else:
			lines.append(f"static constexpr const char* const* {arr_name} = nullptr;")
		return "\n".join(lines)

	hpp  = ffc.auto_header
	hpp += "#pragma once\n"
	hpp += "#include <string_view>\n"
	hpp += "#include <algorithm>\n"
	hpp += "#include <cstddef>\n\n"
	hpp += "namespace FastFHIR {\n\n"

	hpp += "// --- Category 1: profile-native extensions ---\n"
	hpp += "// Already stored as native vtable fields; always suppressed.\n"
	hpp += _url_array(native_sorted, "FF_NATIVE_EXTENSION_URLS", "FF_NATIVE_EXTENSION_URL_COUNT")
	hpp += "\n\n"

	hpp += "// --- Category 1+2+spec: all known/safe extensions ---\n"
	hpp += "// Profile-native + HL7-informational-only + all FHIR-spec-defined Extension types.\n"
	hpp += "// Category 2 is a STRICT set of display/metadata hints only.\n"
	hpp += "// Extensions that may be compiled into profiles are excluded to prevent silent data loss.\n"
	hpp += _url_array(all_known_sorted, "FF_ALL_KNOWN_EXTENSION_URLS", "FF_ALL_KNOWN_EXTENSION_URL_COUNT")
	hpp += "\n\n"

	hpp += (
		"/// Returns true when @p url is a profile-native extension that is\n"
		"/// already stored as a native vtable field (should always be suppressed).\n"
		"inline bool FF_IsNativeExtension(std::string_view url) noexcept {\n"
		"    if (url.empty() || FF_NATIVE_EXTENSION_URL_COUNT == 0) return false;\n"
		"    const char* const* begin = FF_NATIVE_EXTENSION_URLS;\n"
		"    const char* const* end   = FF_NATIVE_EXTENSION_URLS + FF_NATIVE_EXTENSION_URL_COUNT;\n"
		"    auto it = std::lower_bound(begin, end, url,\n"
		"        [](const char* a, std::string_view b) noexcept { return std::string_view(a) < b; });\n"
		"    return it != end && std::string_view(*it) == url;\n"
		"}\n\n"
	)

	hpp += (
		"/// Returns true when @p url is in the all-known set (category 1+2+spec).\n"
		"/// Under FILTER_ALL_KNOWN mode this URL will not produce a WASM dispatch.\n"
		"inline bool FF_IsKnownExtension(std::string_view url) noexcept {\n"
		"    if (url.empty() || FF_ALL_KNOWN_EXTENSION_URL_COUNT == 0) return false;\n"
		"    const char* const* begin = FF_ALL_KNOWN_EXTENSION_URLS;\n"
		"    const char* const* end   = FF_ALL_KNOWN_EXTENSION_URLS + FF_ALL_KNOWN_EXTENSION_URL_COUNT;\n"
		"    auto it = std::lower_bound(begin, end, url,\n"
		"        [](const char* a, std::string_view b) noexcept { return std::string_view(a) < b; });\n"
		"    return it != end && std::string_view(*it) == url;\n"
		"}\n\n"
	)

	hpp += "} // namespace FastFHIR\n"

	os.makedirs(output_dir, exist_ok=True)
	out_path = os.path.join(output_dir, "FF_KnownExtensions.hpp")
	_write_if_changed(out_path, hpp, encoding="utf-8")
	print(f"-- Emitted {out_path} "
	      f"({len(all_known_sorted)} known, {len(native_sorted)} native)")


def main():
	# 1. Get the specs
	fetch_specs.fetch_fhir_specs()
	versions = ffc._discover_versions("fhir_specs")
	if not versions:
		raise RuntimeError("No FHIR versions found in fhir_specs after fetch.")
	version_configs = [(v, os.path.join("fhir_specs", v)) for v in versions]
	resources = ffc.resolve_production_resources(specs_dir="fhir_specs", versions=versions)

	# 2. Build the dictionaries
	ffd.generate_master_dictionary(version_configs)

	# 3. Build the code system enums (FF_CodeSystems.hpp)
	code_enum_map = ffcs.generate_code_systems(ffc.PRODUCTION_TYPES, resources, versions=versions)

	# 4. Build the domains/resources
	ffc.compile_fhir_library(resources, versions, code_enum_map=code_enum_map)

	# 5. Build the known-extension filter table (must run while fhir_specs/ still exists)
	generate_known_extensions(versions, specs_dir="fhir_specs")

	# 6. Cleanup downloaded specs after successful generation
	specs_dir = "fhir_specs"
	if os.path.isdir(specs_dir):
		shutil.rmtree(specs_dir)
		print("[Cleanup] Removed fhir_specs/")

	print("\n[Success] FastFHIR Library generation complete.")


if __name__ == "__main__":
	main()