"""FastFHIR Generator — resource-traits emitter.

Relocated from ffc.py lines 555-597.  Emits FF_ResourceTypes.hpp with the
RESOURCETYPE enum, per-resource TypeTraits, and the recovery-to-type lookup.
"""

from generator.emit.header import auto_header
from generator.utilities import enclose_namespace


def generate_resource_traits_header(resources):
    # Build the namespace body first, then wrap once with enclose_namespace.
    # No manual open/close tracking.
    body = ""
    body += "enum class RESOURCETYPE : uint16_t {\n"
    body += "    UNKNOWN = 0,\n"
    for res in resources:
        body += f"    {res.upper()},\n"
    body += (
        "};\n"
        "using ResourceType = RESOURCETYPE;\n\n"
        "template <RESOURCETYPE T> struct ResourceTypeTraits;\n"
        "template <> struct ResourceTypeTraits<RESOURCETYPE::UNKNOWN> {\n"
        "    static constexpr RECOVERY_TAG recovery = FF_RECOVER_UNDEFINED;\n"
        '    static constexpr std::string_view name = "";\n'
        "};\n"
    )
    for res in resources:
        body += (
            f"template <> struct ResourceTypeTraits<RESOURCETYPE::{res.upper()}> {{\n"
            f"    static constexpr RECOVERY_TAG recovery = RECOVER_FF_{res.upper()};\n"
            f'    static constexpr std::string_view name = "{res}";\n'
            "};\n"
        )
    body += (
        "\ninline constexpr RESOURCETYPE resource_type_from_recovery(RECOVERY_TAG recovery) {\n"
        "    switch (recovery) {\n"
    )
    for res in resources:
        body += f"        case RECOVER_FF_{res.upper()}: return RESOURCETYPE::{res.upper()};\n"
    body += "        default: return RESOURCETYPE::UNKNOWN;\n" "    }\n" "}\n"

    hpp = (
        f"{auto_header}"
        "#pragma once\n"
        '#include "FF_Primitives.hpp"\n'
        "#include <string_view>\n\n" + enclose_namespace("FastFHIR", body)
    )
    return hpp
