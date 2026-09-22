/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FF_StreamCheck.hpp"

#include "FF_BundleIndex.hpp"
#include "FF_ConformanceEngine.hpp"  // report(): the shared sink-and-count step
#include "FF_FieldKeys.hpp"
#include "FF_Logger.hpp"
#include "FF_Parser.hpp"
#include "FF_Primitives.hpp"
#include "FF_Reflection.hpp"
#include "FF_Utilities.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace FastFHIR
{
namespace Conformance
{
namespace
{

using Reflective::Entry;
using Reflective::Node;

/// Room for one diagnostic. A reference's text is bounded only by what is in the
/// arena, and a check that reads a stream it did not write must not build an
/// unbounded string out of it — so messages are composed into a fixed buffer and
/// truncated rather than allocated.
constexpr std::size_t kMessageMax = 512;

/// Two buffers, not one. `g_message` holds the issue being reported right now
/// and is reused as the walk advances; `g_hard` holds the FIRST hard failure's
/// text, which a later issue must not overwrite, because the Status returned
/// from this check outlives the call and the Builder formats its exception from
/// it. thread_local for the same reason: the value is read on the thread that
/// ran the walk, immediately after it returns.
thread_local char g_message[kMessageMax];
thread_local char g_hard[kMessageMax];

/// Composes into a fixed buffer, truncating rather than allocating or
/// overflowing. Always leaves the buffer NUL-terminated so it can back a Status.
struct Appender
{
    char* const       buf;
    const std::size_t cap;
    std::size_t       len = 0;

    Appender(char* b, std::size_t c) noexcept : buf(b), cap(c) {}

    void put(std::string_view s) noexcept
    {
        const std::size_t room = (len + 1 < cap) ? cap - len - 1 : 0;
        const std::size_t n    = s.size() < room ? s.size() : room;
        std::memcpy(buf + len, s.data(), n);
        len += n;
    }

    [[nodiscard]] std::string_view view() noexcept
    {
        buf[len] = '\0';
        return {buf, len};
    }
};

/// The walk's state: what to resolve against, where the sink is, the dotted path
/// of the field under inspection, and the first hard failure found so far.
struct Scan
{
    const ValidationHooks* self  = nullptr;
    const BundleIndex*     index = nullptr;

    char        path[kMessageMax]{};
    std::size_t path_len = 0;

    Status first_hard{};
    bool   has_hard = false;

    /// Retype the path as @p segment and return the length to restore.
    [[nodiscard]] std::size_t begin(std::string_view segment) noexcept
    {
        const std::size_t saved = path_len;
        path_len = 0;
        append(segment);
        return saved;
    }
    /// Append `.<name>` and return the length to restore.
    [[nodiscard]] std::size_t extend(std::string_view name) noexcept
    {
        const std::size_t saved = path_len;
        append(".");
        append(name);
        return saved;
    }
    void restore(std::size_t len) noexcept { path_len = len; }

    [[nodiscard]] std::string_view path_view() const noexcept { return {path, path_len}; }

private:
    void append(std::string_view s) noexcept
    {
        const std::size_t room = (path_len + 1 < kMessageMax) ? kMessageMax - path_len - 1 : 0;
        const std::size_t n    = s.size() < room ? s.size() : room;
        std::memcpy(path + path_len, s.data(), n);
        path_len += n;
    }
};

/// A URL with an authority is external by construction — nothing in this stream
/// is expected to name it. `urn:` also carries a scheme, so it is handled before
/// this test rather than by it.
[[nodiscard]] bool is_external(std::string_view reference) noexcept
{
    return reference.find("://") != std::string_view::npos;
}

/// A `urn:` identifies a resource WITHIN the Bundle that defines it, with no
/// meaning outside. An unresolved one is therefore a broken document, not a
/// reference that might resolve elsewhere.
[[nodiscard]] bool is_bundle_local(std::string_view reference) noexcept
{
    return reference.rfind("urn:", 0) == 0;
}

/// Records one unresolved reference: reports it through the layer's own sink and
/// counter, and keeps the first hard failure for the Builder to act on.
void record(Scan& scan, std::string_view reference_text, bool hard) noexcept
{
    Appender message(g_message, kMessageMax);
    message.put(scan.path_view());
    message.put(" references \"");
    message.put(reference_text);
    message.put("\", which is not an entry in this Bundle");

    Status status{};
    status.code  = Check::UNRESOLVED_REFERENCE;
    status.block = RECOVER_FF_BUNDLE;
    // The human text names the path itself, so `path` stays empty rather than
    // repeating it; the Builder's formatter skips an empty path.
    status.human = message.view().data();
    report(scan.self, status);

    if (hard && !scan.has_hard)
    {
        Appender durable(g_hard, kMessageMax);
        durable.put(message.view());
        scan.first_hard = status;
        scan.first_hard.human = durable.view().data();
        scan.has_hard = true;
    }
}

/// One `Reference` block: read its `reference` text and classify it.
void check_reference(const Node& reference_block, Scan& scan) noexcept
{
    const Entry slot = reference_block[Fields::REFERENCE::REFERENCE];
    if (!slot) return;  // identifier-only, or absent: it never named a target here

    const std::string_view text = static_cast<std::string_view>(slot);
    if (text.empty()) return;
    if (scan.index->resolve(text)) return;  // it names an entry in this Bundle
    if (is_external(text)) return;          // absolute URL: nothing resolves here

    record(scan, text, /*hard=*/is_bundle_local(text));
}

/// A reference-bearing field. Singular and repeating differ only in how many
/// `Reference` blocks are behind the slot.
void check_reference_field(const Node& field, Scan& scan) noexcept
{
    if (field.kind() == FF_FIELD_ARRAY)
    {
        const std::size_t count = field.size();
        for (std::size_t i = 0; i < count; ++i)
            check_reference(field[i], scan);
        return;
    }
    check_reference(field, scan);
}

/// The reflective walk. Descends the whole document the way `print_json` does —
/// the same field table, the same slot lookup — so a reference is found wherever
/// the schema puts it, at any depth, singular or repeating.
void walk(const Node& node, Scan& scan) noexcept
{
    if (!node) return;

    const FF_FieldKind node_kind = node.kind();
    if (node_kind == FF_FIELD_ARRAY)
    {
        const std::size_t count = node.size();
        for (std::size_t i = 0; i < count; ++i)
            walk(node[i], scan);
        return;
    }
    if (node_kind != FF_FIELD_BLOCK && node_kind != FF_FIELD_RESOURCE) return;

    // A resource restarts the path, so a report reads `Observation.subject` and
    // not the chain of Bundle backbones that led to it.
    const std::size_t saved = FF_IsResourceTag(node.recovery())
                                  ? scan.begin(reflected_resource_type(node.recovery()))
                                  : scan.path_len;

    for (const FF_FieldInfo& field : node.fields())
    {
        const FF_FieldKey key = FF_FieldKey::from_cstr(
            node.recovery(), field.kind, field.field_offset,
            field.child_recovery, field.array_entries_are_offsets, field.name);
        const Entry slot = node[key];
        if (!slot) continue;
        if (ff_kind_is_inline_scalar(field.kind)) continue;

        const Node child = slot.as_node();
        if (!child) continue;

        const std::size_t mark = scan.extend(field.name);
        // A choice's `child_recovery` names only its FIRST variant, so the live
        // type comes from the slot; every other field already carries the tag
        // the schema declares.
        const RECOVERY_TAG tag = (field.kind == FF_FIELD_CHOICE) ? slot.target_recovery
                                                                 : field.child_recovery;
        if (tag == RECOVER_FF_REFERENCE) check_reference_field(child, scan);
        else                             walk(child, scan);
        scan.restore(mark);
    }

    scan.restore(saved);
}

}  // namespace

Status check_stream_references(const StreamCheckInfo& info)
{
    // References resolve WITHIN a Bundle, against its entry set. A stream rooted
    // at anything else has no such set, so a "unresolved" verdict here would be
    // a false positive on every reference it holds. Nothing to say: return OK.
    if (info.root_recovery != RECOVER_FF_BUNDLE) return {};
    if (info.arena == nullptr || info.root_offset == FF_NULL_OFFSET) return {};

    const Node root(static_cast<const BYTE*>(info.arena), static_cast<Size>(info.arena_size),
                    info.fhir_version, static_cast<Offset>(info.root_offset),
                    static_cast<RECOVERY_TAG>(info.root_recovery), FF_FIELD_BLOCK);
    if (!root) return {};

    // The check's one allocation, and the reason it may allocate at all:
    // "does any entry name this reference" is a question about the whole
    // document. Built once and reused by every reference the walk finds.
    const BundleIndex index(root);

    Scan scan;
    scan.self  = info.self;
    scan.index = &index;
    walk(root, scan);
    return scan.first_hard;
}

}  // namespace Conformance
}  // namespace FastFHIR
