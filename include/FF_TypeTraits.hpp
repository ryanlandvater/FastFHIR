/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file FF_TypeTraits.hpp
 * @brief TypeTraits for the primitives: string_view and the inline-scalar arrays.
 *
 * One TypeTraits specialization describes how a POCO type is measured and
 * written to the wire. The six here cover the primitives; every generated
 * struct has its own, emitted beside the struct.
 *
 * `size()` is arithmetic and stays inline. `store()` writes arena bytes and is
 * declared here but defined in FF_TypeTraits.cpp.
 */
#pragma once

#include "FF_Primitives.hpp"

#include <string_view>
#include <vector>

namespace FastFHIR
{

template <typename T>
struct TypeTraits;

/**
 * @brief How to measure and write one POCO type, without naming the type.
 *
 * Each TypeTraits specialization exposes a descriptor(), which hands back this
 * struct: the type's recovery tag plus the two functions that answer "how many
 * bytes" and "write them". `Builder::append_object` accepts any type that has
 * one, which is why a generated resource can be appended without the caller
 * naming a builder detail.
 */
struct TypeDescriptor
{
    RECOVERY_TAG recovery = FF_RECOVER_UNDEFINED;         ///< The type's recovery tag.
    /// Bytes this value needs on the wire, including any child blocks.
    Size   (*size) (const void *data, uint32_t version) = nullptr;
    /// Write the value into @p base at @p off; returns the end offset.
    Offset (*store)(BYTE *base, Offset off, const void *data, uint32_t version) = nullptr;
};

template <>
struct TypeTraits<std::string_view>
{
    static constexpr auto recovery = RECOVER_FF_STRING;
    static Size size(std::string_view d, uint32_t = FHIR_VERSION_R5) { return SIZE_FF_STRING(d); }
    static Offset store(BYTE *const base, Offset off, std::string_view d, uint32_t = FHIR_VERSION_R5);
    static const TypeDescriptor& descriptor();
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
    static const TypeDescriptor& descriptor();
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
    static const TypeDescriptor& descriptor();
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
    static const TypeDescriptor& descriptor();
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
    static const TypeDescriptor& descriptor();
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
    static const TypeDescriptor& descriptor();
};

} // namespace FastFHIR
