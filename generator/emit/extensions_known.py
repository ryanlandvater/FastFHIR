"""FastFHIR Generator — known-extension filter-table emitter.

Relocated from tools/generator/make_lib.py::generate_known_extensions.
This was SV-4 in the plan: an emitter living inside the orchestrator.
It now lives in emit/ with the other emitters.
"""

import json
import os

from generator.emit.header import auto_header, write_if_changed
from generator.utilities import enclose_namespace

# Category 2: HL7-known-informational-only extensions.
# STRICT FILTER: Only extensions GUARANTEED to carry zero semantic weight and
# are NOT compiled into any profile.  Pure metadata hints (rendering, narrative,
# ISO qualifiers) with no clinical impact.  Patient/event/workflow extensions
# are excluded because they're often compiled into profiles or carry semantic
# weight.  When in doubt, let WASM dispatch handle it.
_HL7_KNOWN_SAFE_URLS: list[str] = [
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


def generate_known_extensions(
    versions: list[str],
    specs_dir: str = "fhir_specs",
    output_dir: str = "generated_src",
    code_system_urls: set[str] | None = None,
) -> None:
    """Emit generated_src/FF_KnownExtensions.hpp.

    Collects Extension StructureDefinition URLs from the FHIR spec bundles
    (all versions), identifies profile-native extensions (base FHIR + major
    profiles like US Core, UK Core), merges with HL7-known-safe list, and
    emits sorted compile-time arrays with O(log n) lookup helpers.
    """
    # Collect all extension URLs from NPM package StructureDefinition files.
    all_spec_ext_urls: set[str] = set()
    for v in versions:
        pkg = os.path.join(specs_dir, v, "package")
        if not os.path.isdir(pkg):
            continue
        for fname in os.listdir(pkg):
            if not fname.startswith("StructureDefinition-") or not fname.endswith(".json"):
                continue
            with open(os.path.join(pkg, fname), "r", encoding="utf-8") as fh:
                sd = json.load(fh)
            if sd.get("type") == "Extension":
                url = sd.get("url", "").strip()
                if url:
                    all_spec_ext_urls.add(url)

    # Profile-native: base FHIR + major profiles (US Core, UK Core).
    profile_native_urls: set[str] = {
        url
        for url in all_spec_ext_urls
        if (
            url.startswith("http://hl7.org/fhir/StructureDefinition/")
            or url.startswith("http://hl7.org/fhir/us/core/")
            or url.startswith("https://fhir.hl7.org.uk/")
        )
    }

    native_sorted = sorted(profile_native_urls)
    extra_known = set(code_system_urls) if code_system_urls else set()
    all_known_sorted = sorted(
        profile_native_urls | set(_HL7_KNOWN_SAFE_URLS) | all_spec_ext_urls | extra_known
    )

    def _url_array(urls, arr_name, count_name):
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

    hpp = auto_header
    hpp += "#pragma once\n"
    hpp += "#include <string_view>\n"
    hpp += "#include <algorithm>\n"
    hpp += "#include <cstddef>\n\n"

    # Build namespace body first, then wrap with enclose_namespace.
    ns_body = ""
    ns_body += "// --- Category 1: profile-native extensions ---\n"
    ns_body += "// Already stored as native vtable fields; always suppressed.\n"
    ns_body += _url_array(native_sorted, "FF_NATIVE_EXTENSION_URLS", "FF_NATIVE_EXTENSION_URL_COUNT")
    ns_body += "\n\n"

    ns_body += "// --- Category 1+2+spec: all known/safe extensions ---\n"
    ns_body += _url_array(all_known_sorted, "FF_ALL_KNOWN_EXTENSION_URLS", "FF_ALL_KNOWN_EXTENSION_URL_COUNT")
    ns_body += "\n\n"

    ns_body += (
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

    ns_body += (
        "/// Returns true when @p url is in the all-known set (category 1+2+spec).\n"
        "inline bool FF_IsKnownExtension(std::string_view url) noexcept {\n"
        "    if (url.empty() || FF_ALL_KNOWN_EXTENSION_URL_COUNT == 0) return false;\n"
        "    const char* const* begin = FF_ALL_KNOWN_EXTENSION_URLS;\n"
        "    const char* const* end   = FF_ALL_KNOWN_EXTENSION_URLS + FF_ALL_KNOWN_EXTENSION_URL_COUNT;\n"
        "    auto it = std::lower_bound(begin, end, url,\n"
        "        [](const char* a, std::string_view b) noexcept { return std::string_view(a) < b; });\n"
        "    return it != end && std::string_view(*it) == url;\n"
        "}\n\n"
    )

    hpp += enclose_namespace("FastFHIR", ns_body)

    os.makedirs(output_dir, exist_ok=True)
    out_path = os.path.join(output_dir, "FF_KnownExtensions.hpp")
    write_if_changed(out_path, hpp)
    print(
        f"-- Emitted {out_path} "
        f"({len(all_known_sorted)} known, {len(native_sorted)} native)"
    )
