/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file FF_TypeTraits.cpp
 * @brief store() for the primitive TypeTraits — the half that writes bytes.
 *
 * Out of line so FF_Ops.hpp (arena byte arithmetic, internal) stays out of the
 * public headers. Each store() must agree byte for byte with what
 * generate_store_fields emits for the same array; a disagreement of one byte
 * per element overlaps the next claim and walks the reader off the entries.
 */

#include "FF_TypeTraits.hpp"

#include "FF_Ops.hpp"

namespace FastFHIR
{

Offset TypeTraits<std::string_view>::store(BYTE *const base, Offset off, std::string_view d, uint32_t)
{
    return off + STORE_FF_STRING(base, off, d);
}

Offset TypeTraits<std::vector<ResourceReference>>::store(BYTE *const base, Offset off,
                                                         const std::vector<ResourceReference> &d, uint32_t)
{
    STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_RESOURCE,
                          static_cast<uint32_t>(d.size()), recovery);
    for (const auto &ref : d)
    {
        STORE_U64(base + off, ref.offset);
        STORE_U16(base + off + DATA_BLOCK::RECOVERY, ref.recovery);
        off += TYPE_SIZE_RESOURCE;
    }
    return off;
}

Offset TypeTraits<std::vector<uint8_t>>::store(BYTE *const base, Offset off,
                                               const std::vector<uint8_t> &d, uint32_t)
{
    STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_UINT8,
                          static_cast<uint32_t>(d.size()), recovery);
    for (const auto &v : d)
    {
        STORE_U8(base + off, v);
        off += TYPE_SIZE_UINT8;
    }
    return off;
}

Offset TypeTraits<std::vector<uint32_t>>::store(BYTE *const base, Offset off,
                                                const std::vector<uint32_t> &d, uint32_t)
{
    STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_UINT32,
                          static_cast<uint32_t>(d.size()), recovery);
    for (const auto &v : d)
    {
        STORE_U32(base + off, v);
        off += TYPE_SIZE_UINT32;
    }
    return off;
}

// std::vector<double> has nowhere to keep a per-element digit count, so every
// entry writes the sentinel and exports shortest-round-trip. The STRIDE is
// still TYPE_SIZE_DECIMAL, matching the emitted writer for the same array.
Offset TypeTraits<std::vector<double>>::store(BYTE *const base, Offset off,
                                              const std::vector<double> &d, uint32_t)
{
    STORE_FF_ARRAY_HEADER(base, off, FF_ARRAY::INLINE_BLOCK, TYPE_SIZE_DECIMAL,
                          static_cast<uint32_t>(d.size()), recovery);
    for (const auto &v : d)
    {
        STORE_F64(base + off, v);
        STORE_U8(base + off + TYPE_SIZE_UINT64, FF_DECIMAL_SIGFIGS_UNSPECIFIED);
        off += TYPE_SIZE_DECIMAL;
    }
    return off;
}

// =====================================================================
// Descriptors for the hand-written traits
// =====================================================================
// The generated types get their descriptor() from the generator, beside their
// SIZE_/STORE_. These six are hand-written, so their descriptors are too. Each
// thunk casts the erased pointer back to ITS OWN type before calling through —
// that is the whole contract of TypeDescriptor::size/store, and getting the
// cast type wrong here would read the wrong bytes rather than fail to compile.

const TypeDescriptor& TypeTraits<std::string_view>::descriptor()
{
    static const TypeDescriptor d{
        TypeTraits<std::string_view>::recovery,
        [](const void *__data, uint32_t __v) -> Size {
            return TypeTraits<std::string_view>::size(*static_cast<const std::string_view *>(__data), __v);
        },
        [](BYTE *__base, Offset __off, const void *__data, uint32_t __v) -> Offset {
            return TypeTraits<std::string_view>::store(__base, __off, *static_cast<const std::string_view *>(__data), __v);
        },
    };
    return d;
}

// String is a distinct wire-caller type from string_view even though it FORWARDS
// to it, so it needs its own descriptor: the thunks must cast the erased pointer
// back to String, not to string_view.
const TypeDescriptor& TypeTraits<String>::descriptor()
{
    static const TypeDescriptor d{
        TypeTraits<String>::recovery,
        [](const void *__data, uint32_t __v) -> Size {
            return TypeTraits<String>::size(*static_cast<const String *>(__data), __v);
        },
        [](BYTE *__base, Offset __off, const void *__data, uint32_t __v) -> Offset {
            return TypeTraits<String>::store(__base, __off, *static_cast<const String *>(__data), __v);
        },
    };
    return d;
}

const TypeDescriptor& TypeTraits<std::vector<ResourceReference>>::descriptor()
{
    static const TypeDescriptor d{
        TypeTraits<std::vector<ResourceReference>>::recovery,
        [](const void *__data, uint32_t __v) -> Size {
            return TypeTraits<std::vector<ResourceReference>>::size(*static_cast<const std::vector<ResourceReference> *>(__data), __v);
        },
        [](BYTE *__base, Offset __off, const void *__data, uint32_t __v) -> Offset {
            return TypeTraits<std::vector<ResourceReference>>::store(__base, __off, *static_cast<const std::vector<ResourceReference> *>(__data), __v);
        },
    };
    return d;
}

const TypeDescriptor& TypeTraits<std::vector<uint8_t>>::descriptor()
{
    static const TypeDescriptor d{
        TypeTraits<std::vector<uint8_t>>::recovery,
        [](const void *__data, uint32_t __v) -> Size {
            return TypeTraits<std::vector<uint8_t>>::size(*static_cast<const std::vector<uint8_t> *>(__data), __v);
        },
        [](BYTE *__base, Offset __off, const void *__data, uint32_t __v) -> Offset {
            return TypeTraits<std::vector<uint8_t>>::store(__base, __off, *static_cast<const std::vector<uint8_t> *>(__data), __v);
        },
    };
    return d;
}

const TypeDescriptor& TypeTraits<std::vector<uint32_t>>::descriptor()
{
    static const TypeDescriptor d{
        TypeTraits<std::vector<uint32_t>>::recovery,
        [](const void *__data, uint32_t __v) -> Size {
            return TypeTraits<std::vector<uint32_t>>::size(*static_cast<const std::vector<uint32_t> *>(__data), __v);
        },
        [](BYTE *__base, Offset __off, const void *__data, uint32_t __v) -> Offset {
            return TypeTraits<std::vector<uint32_t>>::store(__base, __off, *static_cast<const std::vector<uint32_t> *>(__data), __v);
        },
    };
    return d;
}

const TypeDescriptor& TypeTraits<std::vector<double>>::descriptor()
{
    static const TypeDescriptor d{
        TypeTraits<std::vector<double>>::recovery,
        [](const void *__data, uint32_t __v) -> Size {
            return TypeTraits<std::vector<double>>::size(*static_cast<const std::vector<double> *>(__data), __v);
        },
        [](BYTE *__base, Offset __off, const void *__data, uint32_t __v) -> Offset {
            return TypeTraits<std::vector<double>>::store(__base, __off, *static_cast<const std::vector<double> *>(__data), __v);
        },
    };
    return d;
}

} // namespace FastFHIR
