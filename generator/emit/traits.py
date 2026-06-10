"""FastFHIR Generator — resource-traits emitter.

Relocated from ffc.py lines 555-597.  Emits FF_ResourceTypes.hpp with the
RESOURCETYPE enum, per-resource TypeTraits, and the recovery-to-type lookup.
"""

from generator.emit.header import auto_header


def generate_resource_traits_header(resources):
    hpp = (
        f"{auto_header}"
        "#pragma once\n"
        '#include "../include/FF_Primitives.hpp"\n'
        "#include <string_view>\n\n"
        "namespace FastFHIR {\n"
        "enum class RESOURCETYPE : uint16_t {\n"
        "    UNKNOWN = 0,\n"
    )
    for res in resources:
        hpp += f"    {res.upper()},\n"
    hpp += (
        "};\n"
        "using ResourceType = RESOURCETYPE;\n\n"
        "template <RESOURCETYPE T> struct ResourceTypeTraits;\n"
        "template <> struct ResourceTypeTraits<RESOURCETYPE::UNKNOWN> {\n"
        "    static constexpr RECOVERY_TAG recovery = FF_RECOVER_UNDEFINED;\n"
        "    static constexpr std::string_view name = \"\";\n"
        "};\n"
    )
    for res in resources:
        hpp += (
            f"template <> struct ResourceTypeTraits<RESOURCETYPE::{res.upper()}> {{\n"
            f"    static constexpr RECOVERY_TAG recovery = RECOVER_FF_{res.upper()};\n"
            f"    static constexpr std::string_view name = \"{res}\";\n"
            "};\n"
        )
    hpp += (
        "\ninline constexpr RESOURCETYPE resource_type_from_recovery(RECOVERY_TAG recovery) {\n"
        "    switch (recovery) {\n"
    )
    for res in resources:
        hpp += f"        case RECOVER_FF_{res.upper()}: return RESOURCETYPE::{res.upper()};\n"
    hpp += (
        "        default: return RESOURCETYPE::UNKNOWN;\n"
        "    }\n"
        "}\n"
        "\n} // namespace FastFHIR\n"
    )
    return hpp
