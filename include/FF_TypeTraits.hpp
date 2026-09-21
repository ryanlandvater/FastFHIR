/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file FF_TypeTraits.hpp
 * @brief TypeTraits for the primitives: string_view and the inline-scalar arrays.
 *
 * The generator emits one TypeTraits specialization per generated struct, and
 * these six are the ones it does NOT emit, because nothing about them varies
 * with a FHIR revision or a build profile. They used to be a fixed C++ string
 * constant inside generator/library.py, which meant fixed C++ was maintained as
 * Python data: no syntax highlighting, no compiler until the next generate, and
 * a diff that read as a generator change.
 *
 * store() is declared here and defined in FF_TypeTraits.cpp. It is the only
 * part that writes arena bytes, so it is the only part that needs the internal
 * FF_Ops.hpp -- keeping it out of line is what lets the generated POCO headers
 * be public without dragging the byte arithmetic along (CLAUDE.md invariant 9).
 * size() is arithmetic, so it stays inline where the append path can see it.
 */
#pragma once

#include "FF_Primitives.hpp"

#include <string_view>
#include <vector>

namespace FastFHIR
{

template <typename T>
struct TypeTraits;

template <>
struct TypeTraits<std::string_view>
{
    static constexpr auto recovery = RECOVER_FF_STRING;
    static Size size(std::string_view d, uint32_t = FHIR_VERSION_R5) { return SIZE_FF_STRING(d); }
    static Offset store(BYTE *const base, Offset off, std::string_view d, uint32_t = FHIR_VERSION_R5);
};

// String is the POCO member type; on the wire it is an FF_STRING like any
// other, so this forwards rather than duplicating. It is needed because
// MutableEntry::operator= DEDUCES its argument type, and deduction runs before
// conversions -- `handle[KEY] = some_string` would otherwise instantiate an
// undefined TypeTraits<String> instead of converting to string_view.
template <>
struct TypeTraits<String>
{
    static constexpr auto recovery = TypeTraits<std::string_view>::recovery;
    static Size size(std::string_view d, uint32_t v = FHIR_VERSION_R5)
    {
        return TypeTraits<std::string_view>::size(d, v);
    }
    static Offset store(BYTE *const base, Offset off, std::string_view d,
                        uint32_t v = FHIR_VERSION_R5)
    {
        return TypeTraits<std::string_view>::store(base, off, d, v);
    }
};

// Offsets are written by the array overload of Builder_t::append, which builds
// the header itself; there is nothing for a trait to do.
template <>
struct TypeTraits<std::vector<Offset>>
{
};

template <>
struct TypeTraits<std::vector<ResourceReference>>
{
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_RESOURCE | RECOVER_ARRAY_BIT);
    static Size size(const std::vector<ResourceReference> &d, uint32_t = FHIR_VERSION_R5)
    {
        return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_RESOURCE);
    }
    static Offset store(BYTE *const base, Offset off, const std::vector<ResourceReference> &d,
                        uint32_t = FHIR_VERSION_R5);
};

template <>
struct TypeTraits<std::vector<uint8_t>>
{
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_BOOL | RECOVER_ARRAY_BIT);
    static Size size(const std::vector<uint8_t> &d, uint32_t = FHIR_VERSION_R5)
    {
        return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_UINT8);
    }
    static Offset store(BYTE *const base, Offset off, const std::vector<uint8_t> &d,
                        uint32_t = FHIR_VERSION_R5);
};

template <>
struct TypeTraits<std::vector<uint32_t>>
{
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_UINT32 | RECOVER_ARRAY_BIT);
    static Size size(const std::vector<uint32_t> &d, uint32_t = FHIR_VERSION_R5)
    {
        return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_UINT32);
    }
    static Offset store(BYTE *const base, Offset off, const std::vector<uint32_t> &d,
                        uint32_t = FHIR_VERSION_R5);
};

template <>
struct TypeTraits<std::vector<double>>
{
    static constexpr auto recovery = static_cast<RECOVERY_TAG>(RECOVER_FF_FLOAT64 | RECOVER_ARRAY_BIT);
    // A decimal entry is TYPE_SIZE_DECIMAL wide, not TYPE_SIZE_FLOAT64: the 9th
    // byte is the source digit count. See the store() definition.
    static Size size(const std::vector<double> &d, uint32_t = FHIR_VERSION_R5)
    {
        return FF_ARRAY::HEADER_SIZE + (static_cast<uint32_t>(d.size()) * TYPE_SIZE_DECIMAL);
    }
    static Offset store(BYTE *const base, Offset off, const std::vector<double> &d,
                        uint32_t = FHIR_VERSION_R5);
};

} // namespace FastFHIR
