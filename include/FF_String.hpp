/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file FF_String.hpp
 * @brief The string member type of every generated POCO.
 *
 * NOT A WIRE TYPE. This is the build-and-materialize side only; no byte of any
 * stream changes because of it. Same status as FF_Optional.
 */
#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace FastFHIR
{

/**
 * A POCO string field that cannot dangle by accident.
 *
 * One member type serves both directions, which is the constraint that rules
 * out std::string on its own -- the read path would copy every string out of
 * the arena -- and rules out std::string_view on its own, because a producer's
 * runtime string dies before the append:
 *
 *     { std::string temp = lot_number(); coding.code = temp; }
 *     builder.append_obj(coding);        // temp is gone; the view is not
 *
 * So the field holds either, and knows which. std::string does the owning
 * half and is NOT reimplemented here: its small-string optimization keeps text
 * of roughly 22 characters or fewer inside the object with no allocation at
 * all -- which is most FHIR codes -- and its copy and compare paths are better
 * tuned than anything written for this. std::string_view does the borrowing
 * half, which is what keeps node.as<PatientData>() free of allocations.
 *
 * Which one you get is chosen by HOW THE RIGHT-HAND SIDE IS SPELLED:
 *
 *     field = "http://loinc.org";     // literal   -> borrow (lives forever)
 *     field = some_std_string;        // string    -> own    (may die)
 *     field = std::string_view(buf);  // explicit  -> borrow (caller vouches)
 *     field = runtime_char_pointer;   // COMPILE ERROR -- say which you meant
 *
 * The consteval constructor is what makes the last line an error rather than a
 * use-after-free: a pointer that is not a compile-time constant cannot reach
 * it, which catches `p = s.c_str()` and `char buf[64]; field = buf;` alike.
 *
 * There is deliberately no data() and no c_str(). Printing a non-terminated
 * view with %s walks past the field and emits whatever follows it, and not
 * exposing a pointer makes that unwritable. When a C string is genuinely
 * needed, std::string(field).c_str() says so and is always correct.
 */
class String
{
public:
    String() noexcept : m_weak() {}

    /// consteval, so ONLY a string literal binds here. See the class note.
    consteval String(const char *literal) noexcept : m_weak(literal) {}

    /// The caller asserting the bytes outlive the append: the read path
    /// (FF_STRING::read_view) and the ingestor's JSON buffer. Both stay
    /// zero-copy, which is the constraint this type exists to keep.
    String(std::string_view view) noexcept : m_weak(view) {}

    /// A std::string may be gone before the append, so its text is kept.
    /// By value, then moved: an lvalue copies once, a temporary costs nothing.
    String(std::string text) noexcept : m_strong(std::move(text)), m_owns(true) {}

    ~String() { release(); }

    /// Deep copy, per the POCO value semantics settled in T4: a brace list can
    /// only copy its elements. A borrowed string copies as a borrow, so copying
    /// a materialized POCO still allocates nothing.
    String(const String &other) : m_owns(other.m_owns)
    {
        if (m_owns) std::construct_at(&m_strong, other.m_strong);
        else        std::construct_at(&m_weak, other.m_weak);
    }

    String(String &&other) noexcept : m_owns(other.m_owns)
    {
        if (m_owns) std::construct_at(&m_strong, std::move(other.m_strong));
        else        std::construct_at(&m_weak, other.m_weak);
    }

    /// Same-arm assignment stays in the arm, so the common case is std::string's
    /// own operator= and never re-activates the union. Crossing arms builds the
    /// replacement BEFORE releasing, so a throwing copy leaves this object
    /// intact and self-assignment cannot free what it is about to read.
    String &operator=(const String &other)
    {
        if (this == &other) return *this;
        if (m_owns && other.m_owns) { m_strong = other.m_strong; return *this; }
        String replacement(other);
        release();
        adopt(std::move(replacement));
        return *this;
    }

    String &operator=(String &&other) noexcept
    {
        if (this == &other) return *this;
        if (m_owns && other.m_owns) { m_strong = std::move(other.m_strong); return *this; }
        release();
        adopt(std::move(other));
        return *this;
    }

    /// Both implicit, so a String is accepted wherever either std type is.
    /// string_view is what every emitter already sees, so no generated code
    /// learns a new type; std::string is the owning, NUL-terminated form.
    operator std::string_view() const noexcept
    {
        return m_owns ? std::string_view(m_strong) : m_weak;
    }
    operator std::string() const
    {
        return m_owns ? m_strong : std::string(m_weak);
    }

    /// The generated store path gates every string field on empty().
    [[nodiscard]] bool empty() const noexcept { return std::string_view(*this).empty(); }
    [[nodiscard]] size_t size() const noexcept { return std::string_view(*this).size(); }

    /// True when this String holds its own copy. Diagnostic only -- no caller
    /// should branch on it, because which arm is active is a property of how
    /// the field was assigned, not of the text.
    [[nodiscard]] bool owns() const noexcept { return m_owns; }

    friend bool operator==(const String &a, std::string_view b) noexcept
    {
        return std::string_view(a) == b;
    }
    friend bool operator!=(const String &a, std::string_view b) noexcept
    {
        return std::string_view(a) != b;
    }

    /// Streams the text, not the object. Without this `os << field` is
    /// ambiguous: both conversions are viable and neither is better.
    friend std::ostream &operator<<(std::ostream &os, const String &s)
    {
        return os << std::string_view(s);
    }

private:
    /// Takes over other's arm. noexcept because both arms move without
    /// throwing, which is what lets the assignment operators release first.
    void adopt(String &&other) noexcept
    {
        m_owns = other.m_owns;
        if (m_owns) std::construct_at(&m_strong, std::move(other.m_strong));
        else        std::construct_at(&m_weak, other.m_weak);
    }

    /// The single point where the owning arm is torn down.
    void release() noexcept
    {
        if (m_owns) std::destroy_at(&m_strong);
    }

    union {
        std::string      m_strong;
        std::string_view m_weak;
    };
    bool m_owns = false;
};

}  // namespace FastFHIR

/// Global alias, matching FF_Optional's spelling for consumers outside the
/// namespace. The FF_ prefix is the namespace when there is not one.
using FF_String = FastFHIR::String;
