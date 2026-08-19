/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FF_Compactor.hpp"
#include "FF_Queue.hpp"
#include "FF_Utilities.hpp"
#include "FF_SIMD.hpp"

#include "FF_Reflection.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace FastFHIR {

enum class PendingWriteKind {
    StringPointer,
    ArrayPointer,
    NodePointer,
    ResourcePointer,
    Code32,
    ChoiceNode,
    ResolvedOffset,
};

struct PendingWrite {
    PendingWriteKind kind = PendingWriteKind::NodePointer;
    Reflective::Node node;
    Reflective::Entry entry;
    Offset slot_offset = FF_NULL_OFFSET;
    Offset parent_anchor = FF_NULL_OFFSET;
    // Depth of `node` in the stored object graph (root = 0). Threaded through
    // the pending queue because the traversal is queue-based: archive_node's
    // frame ends when the queue pop that spawned it returns, so a shared
    // ancestry vector is empty again by the time a child is processed -- only
    // an explicit depth can survive the hop and feed the depth bound.
    std::size_t depth = 0;
    // Full ancestry of `node` (all ancestor block offsets, root first),
    // snapshotted at enqueue time. Cycle detection needs it: the queue-based
    // traversal pops an ancestor's frame before its children process, so the
    // live path is empty at process time -- the ancestry must ride the write
    // and be restored, or a node that reaches an ancestor reports the ancestor
    // as merely "already archived" and the cycle is accepted (XP-1.3).
    std::vector<Offset> ancestry;
    // For PendingWriteKind::ResolvedOffset: the recorded archive offset of an
    // already-completed child, stored straight into the slot at process time.
    // Defaults to the invalid sentinel, not FF_NULL_OFFSET: an unset value
    // must read as "not a resolved offset", never as a valid-looking absent
    // one -- the same reason slot standins use FF_PENDING_OFFSET.
    Offset resolved_offset = FF_PENDING_OFFSET;
};

using PendingQueue = FIFO::Queue<PendingWrite, 1024>;

struct ArchiveContext {
    Memory& destination;
    PendingQueue queue;
    PendingQueue::Injector injector;
    PendingQueue::Consumer consumer;

    // Node identity access. Node::offset() is not public API; ArchiveContext is
    // the friendship grant that lets the traversal key `path`/`done` on it.
    Offset node_offset(const Reflective::Node& node) const { return node.offset(); }

    // XP-1.1: bound the stored-graph traversal and detect cycles. `path` is
    // the ancestry of the node being archived (source-arena offsets); `done`
    // maps every fully archived source offset to the offset it was archived
    // at, so a shared subtree is visited exactly once and any later reference
    // to it returns the recorded offset. Recorded on completion, never on
    // entry: marking on entry would let a structure that reaches itself find
    // its own entry and report success instead of the cycle the path exists
    // to catch (the ordering IFE learned in validate_nested_attributes).
    // `path` is live only within one archive_node frame; it is restored from
    // each pending write's ancestry snapshot before the node processes, so
    // the cycle check sees the true ancestors of a queue-popped node.
    //
    // MAX_NODE_DEPTH is a security cap, not a conformance one. The deepest
    // acyclic chain in the generated R4/R5 model is 8 object blocks
    // (FF_ALLERGYINTOLERANCE, measured from the FIELDS tables), ~16 nodes
    // with the array block each container level interleaves. The FHIR data
    // model's only unbounded nesting axes are the recursive types
    // (Extension.extension, QuestionnaireResponse.item.item,
    // PlanDefinition.action.action), which the spec leaves uncapped; 64 nodes
    // = 32 item/action recursion levels, an order of magnitude past any
    // legitimate document while still bounding a crafted file's traversal.
    static constexpr std::size_t MAX_NODE_DEPTH = 64;
    std::vector<Offset> path;
    std::unordered_map<Offset, Offset> done;

    // Deferred-write balance: every enqueue_pending_write increments, every
    // process_pending_write decrements. A nonzero balance when the drain loop
    // ends means a pending write was dropped and the sealed stream would
    // silently lose fields -- Compactor::archive turns that into a throw.
    std::size_t pending_balance = 0;

    // Every slot that received a pending sentinel, so the seal step can verify
    // each was resolved. Split by width because the sentinels differ: 8-byte
    // pointer slots hold FF_PENDING_OFFSET, 4-byte code slots FF_PENDING_CODE.
    std::vector<Offset> deferred_slots;
    std::vector<Offset> deferred_code_slots;

    explicit ArchiveContext(Memory& dst)
        : destination(dst), queue(), injector(queue.get_injector()), consumer(queue.get_consumer()) {}
};

// Slot widths come from ff_slot_width() in FF_Primitives.hpp -- the same
// function the generated COMPACT_SLOT_SIZES tables are emitted from, so the
// writer here and the compact reader cannot disagree.

// Deferred slots are pre-filled with FF_PENDING_OFFSET / FF_PENDING_CODE,
// the reserved in-flight sentinels declared in FF_Primitives.hpp. Neither
// FF_NULL_OFFSET nor FF_CODE_NULL may be used here: they are the reader's
// canonical "field absent" values, so a standin that survived would read as a
// silently dropped field instead of an incomplete write.
//
// Two checks, and neither subsumes the other. The pending-balance counter
// proves every enqueued write was processed. The residual scan proves every
// pre-filled sentinel was overwritten -- which the counter cannot see, because
// a pre-fill that never enqueued was never counted. That divergence is exactly
// what the paired helpers below exist to prevent, so always use them rather
// than writing a sentinel and tracking it by hand.
//
// Absent values are written directly (a null array entry stores
// FF_NULL_OFFSET, never a sentinel) and are deliberately not tracked.

// Write the pending offset sentinel into a deferred 8-byte slot and remember
// it so the seal step can verify it was resolved.
static inline void write_pending_slot(ArchiveContext& context, BYTE* base, Offset slot) {
    STORE_U64(base + slot, FF_PENDING_OFFSET);
    context.deferred_slots.push_back(slot);
}

// The 4-byte code-slot counterpart. FF_PENDING_CODE (dictionary ID 0) is
// never a legal resolved value here: this slot is only deferred when the
// source code carries FF_CODEABLE_CONCEPT_FLAG, and write_compact_code_slot
// always resolves it to another bit-31-set value, so 0 is unambiguous.
static inline void write_pending_code_slot(ArchiveContext& context, BYTE* base, Offset slot) {
    STORE_U32(base + slot, FF_PENDING_CODE);
    context.deferred_code_slots.push_back(slot);
}

static Offset archive_node(const Reflective::Node& node, ArchiveContext& context, std::size_t depth);
static Offset archive_array(const Reflective::Node& node, ArchiveContext& context, std::size_t depth);
static Offset archive_object(const Reflective::Node& node, ArchiveContext& context, std::size_t depth);

static inline void enqueue_pending_write(ArchiveContext& context, const PendingWrite& pending) {
    // Every deferred write passes through here; the drain loop asserts the
    // enqueue/process balance returns to zero before the stream is sealed.
    ++context.pending_balance;
    // Cycle guard and archived-once memo, both at enqueue time. The enqueuing
    // node's full ancestry is live in context.path right now, so a child that
    // is its own ancestor closes a loop; a child already fully archived
    // resolves to its recorded offset instead of being archived again. Neither
    // check can live in archive_node: a node already in `done` would memo-
    // return at entry before its own children were enqueued, silently
    // absorbing a cycle that runs through it (XP-1.3).
    if (pending.node) {
        const Offset child_off = context.node_offset(pending.node);
        if (std::find(context.path.begin(), context.path.end(), child_off) != context.path.end()) {
            throw std::runtime_error(
                "FastFHIR Compactor Error: cycle in the stored graph at node offset " +
                std::to_string(child_off) + " (an ancestor references it)");
        }
        if (const auto done = context.done.find(child_off); done != context.done.end()) {
            context.injector.push(PendingWrite{
                PendingWriteKind::ResolvedOffset, {}, {}, pending.slot_offset,
                FF_NULL_OFFSET, 0, {}, done->second,
            });
            return;
        }
    }
    context.injector.push(pending);
}

static Offset archive_string(std::string_view value, Memory& destination) {
    if (value.empty()) return FF_NULL_OFFSET;

    const Offset string_off = destination.claim_space(SIZE_FF_STRING(value));
    STORE_FF_STRING(destination.base(), string_off, value);
    return string_off;
}

static void write_compact_code_slot(const Reflective::Entry& entry, Memory& destination,
                                    Offset compact_parent_off, Offset dense_off) {
    BYTE* base = destination.base();
    const uint32_t raw_code = LOAD_U32(entry.base + entry.absolute_offset());
    if (raw_code == FF_CODE_NULL || (raw_code & FF_CODEABLE_CONCEPT_FLAG) == 0) {
        STORE_U32(base + dense_off, raw_code);
        return;
    }

    // CodeableConcept block — copy the source block verbatim to preserve
    // SYSTEM byte, LENGTH byte, and payload.  Creating an FF_STRING would
    // lose the discriminator and break FF_DECODE_CODEABLE_CONCEPT on read.
    int32_t rel_off = static_cast<int32_t>(raw_code << 1) >> 1;
    Offset src_cc_off = entry.parent_offset + static_cast<Offset>(static_cast<int64_t>(rel_off));
    const BYTE* src = entry.base;

    uint8_t cc_len = src[src_cc_off + FF_CODEABLE_CONCEPT::LENGTH];
    Size total_bytes = FF_CODEABLE_CONCEPT::HEADER_SIZE + cc_len;

    Offset dst_cc_off = destination.claim_space(total_bytes);
    std::memcpy(base + dst_cc_off, src + src_cc_off, total_bytes);

    Offset relative_off = dst_cc_off - compact_parent_off;
    if (relative_off > 0x7FFFFFFF) {
        throw std::runtime_error("FastFHIR Compactor Error: CodeableConcept relative offset exceeds 31-bit signed range.");
    }
    STORE_U32(base + dense_off, static_cast<uint32_t>(relative_off) | FF_CODEABLE_CONCEPT_FLAG);
}

static void write_choice_slot(const Reflective::Entry& entry, ArchiveContext& context,
                              Offset compact_parent_off, Offset dense_off, std::size_t depth) {
    BYTE* base = context.destination.base();
    const Offset src_slot = entry.absolute_offset();
    const RECOVERY_TAG tag = static_cast<RECOVERY_TAG>(LOAD_U16(entry.base + src_slot + DATA_BLOCK::RECOVERY));
    STORE_U16(base + dense_off + DATA_BLOCK::RECOVERY, tag);

    if (FF_IsScalarBlockTag(tag)) {
        if (tag == RECOVER_FF_CODE) {
            uint64_t raw_bits = 0;
            const uint32_t raw_code = LOAD_U32(entry.base + src_slot);
            if (raw_code == FF_CODE_NULL || (raw_code & FF_CODEABLE_CONCEPT_FLAG) == 0) {
                raw_bits = raw_code;
            } else {
                // Resolve and copy the source CodeableConcept block verbatim
                int32_t rel_off = static_cast<int32_t>(raw_code << 1) >> 1;
                Offset src_cc_off = entry.parent_offset + static_cast<Offset>(static_cast<int64_t>(rel_off));
                uint8_t cc_len = entry.base[src_cc_off + FF_CODEABLE_CONCEPT::LENGTH];
                Size block_total = FF_CODEABLE_CONCEPT::HEADER_SIZE + cc_len;
                Offset dst_cc_off = context.destination.claim_space(block_total);
                std::memcpy(context.destination.base() + dst_cc_off, entry.base + src_cc_off, block_total);
                Offset relative_off = dst_cc_off - compact_parent_off;
                if (relative_off > 0x7FFFFFFF) {
                    throw std::runtime_error("FastFHIR Compactor Error: CodeableConcept relative offset exceeds 31-bit signed range.");
                }
                raw_bits = static_cast<uint32_t>(relative_off) | FF_CODEABLE_CONCEPT_FLAG;
            }
            STORE_U64(base + dense_off, raw_bits);
            return;
        }

        std::memcpy(base + dense_off, entry.base + src_slot, TYPE_SIZE_UINT64);
        return;
    }

    // Through the paired helper, not a hand-rolled store + push_back: keeping
    // the pre-fill and the tracking inseparable is the whole point of it. The
    // RECOVERY tag was already written at +8 above; this covers bytes 0..7.
    write_pending_slot(context, base, dense_off);
    // Resolve the choice child here so the enqueue-time cycle guard sees it
    // (the pending write carries the resolved node; the process side reuses it).
    const Reflective::Node choice_child = entry.as_node();
    enqueue_pending_write(context, PendingWrite{
        PendingWriteKind::ChoiceNode,
        choice_child,
        entry,
        dense_off,
        compact_parent_off,
        depth + 1,
        context.path,
    });
}

static Offset archive_array(const Reflective::Node& node, ArchiveContext& context, std::size_t depth) {
    const auto elements = node.entries();
    const Size array_size = FF_ARRAY::HEADER_SIZE + static_cast<Size>(elements.size()) * TYPE_SIZE_OFFSET;
    Offset array_off = context.destination.claim_space(array_size);
    Offset write_head = array_off;
    STORE_FF_ARRAY_HEADER(context.destination.base(), write_head, FF_ARRAY::OFFSET, TYPE_SIZE_OFFSET,
                          static_cast<uint32_t>(elements.size()), node.recovery());

    for (const auto& element : elements) {
        if (element) {
            write_pending_slot(context, context.destination.base(), write_head);
            enqueue_pending_write(context, PendingWrite{
                PendingWriteKind::NodePointer,
                element,
                {},
                write_head,
                FF_NULL_OFFSET,
                depth + 1,
                context.path,
            });
        } else {
            // Null entry: the reader's "absent" value, never the pending
            // sentinel -- a sentinel here would read as a present entry with
            // an out-of-bounds offset.
            STORE_U64(context.destination.base() + write_head, FF_NULL_OFFSET);
        }
        write_head += TYPE_SIZE_OFFSET;
    }

    return array_off;
}

static Offset archive_object(const Reflective::Node& node, ArchiveContext& context, std::size_t depth) {
    struct PresentField {
        size_t index;
        FF_FieldInfo info;
        Reflective::Entry entry;
    };

    const auto fields = node.fields();
    std::vector<PresentField> present_fields;
    present_fields.reserve(fields.size());

    Size dense_bytes = 0;
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto& field = fields[i];
        FF_FieldKey key = FF_FieldKey::from_cstr(
            node.recovery(), field.kind, field.field_offset,
            field.child_recovery, field.array_entries_are_offsets, field.name
        );
        auto entry = node[key];
        if (!entry) continue;

        present_fields.push_back(PresentField{i, field, entry});
        dense_bytes += ff_slot_width(field.kind);
    }

    // Layout: [DATA_BLOCK header][presence bitmap (pbytes)][dense region].
    // compact_presence_bytes() is shared with the reader via FF_SIMD.hpp so the
    // two sides cannot disagree on where the dense region starts.
    const uint32_t pbytes = compact_presence_bytes(fields.size());
    const Size object_size = DATA_BLOCK::HEADER_SIZE + pbytes + dense_bytes;
    const Offset object_off = context.destination.claim_space(object_size);
    BYTE* base = context.destination.base();

    STORE_U64(base + object_off + DATA_BLOCK::VALIDATION, object_off);
    STORE_U16(base + object_off + DATA_BLOCK::RECOVERY, node.recovery());

    BYTE* presence = base + object_off + DATA_BLOCK::HEADER_SIZE;
    std::memset(presence, 0, pbytes);

    Offset dense_off = object_off + DATA_BLOCK::HEADER_SIZE + pbytes;
    for (const auto& present : present_fields) {
        const auto& field = present.info;
        const auto& entry = present.entry;
        presence[present.index / 8] |= static_cast<uint8_t>(1u << (present.index % 8));

        switch (field.kind) {
            case FF_FIELD_BOOL:
                STORE_U8(base + dense_off, static_cast<uint8_t>(entry.as<bool>()));
                break;
            case FF_FIELD_INT32:
                STORE_U32(base + dense_off, static_cast<uint32_t>(entry.as<int32_t>()));
                break;
            case FF_FIELD_UINT32:
                STORE_U32(base + dense_off, entry.as<uint32_t>());
                break;
            case FF_FIELD_INT64:
                STORE_U64(base + dense_off, static_cast<uint64_t>(entry.as<int64_t>()));
                break;
            case FF_FIELD_UINT64:
                STORE_U64(base + dense_off, entry.as<uint64_t>());
                break;
            case FF_FIELD_FLOAT64:
                std::memcpy(base + dense_off, entry.base + entry.absolute_offset(), TYPE_SIZE_FLOAT64);
                break;
            case FF_FIELD_CODE:
                if (const uint32_t raw_code = LOAD_U32(entry.base + entry.absolute_offset());
                    raw_code == FF_CODE_NULL || (raw_code & FF_CODEABLE_CONCEPT_FLAG) == 0) {
                    STORE_U32(base + dense_off, raw_code);
                } else {
                    // FF_PENDING_CODE, never FF_CODE_NULL: the latter is the
                    // reader's "no code present", so an unresolved slot would
                    // read as a dropped clinical code rather than a failure.
                    write_pending_code_slot(context, base, dense_off);
                    enqueue_pending_write(context, PendingWrite{
                        PendingWriteKind::Code32,
                        {},
                        entry,
                        dense_off,
                        object_off,
                    });
                }
                break;
            case FF_FIELD_STRING: {
                write_pending_slot(context, base, dense_off);
                enqueue_pending_write(context, PendingWrite{
                    PendingWriteKind::StringPointer,
                    {},
                    entry,
                    dense_off,
                    object_off,
                });
                break;
            }
            case FF_FIELD_ARRAY: {
                write_pending_slot(context, base, dense_off);
                enqueue_pending_write(context, PendingWrite{
                    PendingWriteKind::ArrayPointer,
                    entry.as_node(),
                    {},
                    dense_off,
                    object_off,
                    depth + 1,
                    context.path,
                });
                break;
            }
            case FF_FIELD_RESOURCE: {
                write_pending_slot(context, base, dense_off);
                STORE_U16(base + dense_off + DATA_BLOCK::RECOVERY, FF_RECOVER_UNDEFINED);
                enqueue_pending_write(context, PendingWrite{
                    PendingWriteKind::ResourcePointer,
                    entry.as_node(),
                    {},
                    dense_off,
                    object_off,
                    depth + 1,
                    context.path,
                });
                break;
            }
            case FF_FIELD_CHOICE:
                write_choice_slot(entry, context, object_off, dense_off, depth);
                break;
            default: {
                write_pending_slot(context, base, dense_off);
                enqueue_pending_write(context, PendingWrite{
                    PendingWriteKind::NodePointer,
                    entry.as_node(),
                    {},
                    dense_off,
                    object_off,
                    depth + 1,
                    context.path,
                });
                break;
            }
        }

        dense_off += ff_slot_width(field.kind);
    }

    return object_off;
}

// Read-only: does any direct child of `node` sit on the restored ancestry
// path? Only consulted for re-processings of already-completed nodes, where an
// unconditional memo-return would hide a cycle that a re-walk would newly
// detect (the graph is static, so the only thing a re-walk can newly do is
// enqueue the same children against a *different* ancestry -- and if one of
// those children is on the path, the enqueue-time guard will throw).
static bool child_touches_path(const Reflective::Node& node, ArchiveContext& context) {
    const auto on_path = [&context](const Reflective::Node& child) {
        return child && std::find(context.path.begin(), context.path.end(),
                                  context.node_offset(child)) != context.path.end();
    };
    switch (node.kind()) {
        case FF_FIELD_BLOCK:
        case FF_FIELD_RESOURCE: {
            for (const auto& field : node.fields()) {
                if (field.kind != FF_FIELD_ARRAY && field.kind != FF_FIELD_BLOCK &&
                    field.kind != FF_FIELD_RESOURCE && field.kind != FF_FIELD_CHOICE)
                    continue;
                const Reflective::Entry entry = node[FF_FieldKey::from_cstr(
                    node.recovery(), field.kind, field.field_offset,
                    field.child_recovery, field.array_entries_are_offsets, field.name)];
                if (!entry) continue;
                if (on_path(entry.as_node())) return true;
            }
            return false;
        }
        case FF_FIELD_ARRAY: {
            for (const auto& child : node.entries())
                if (on_path(child)) return true;
            return false;
        }
        default:
            return false;
    }
}

static Offset archive_node(const Reflective::Node& node, ArchiveContext& context, std::size_t depth) {
    if (!node) return FF_NULL_OFFSET;

    // XP-1.1: every recursive entry into the stored graph funnels through this
    // guard. Order matters, exactly as IFE learned it in
    // validate_nested_attributes: depth over the bound first, then ancestry,
    // then the done-set, and `done` is recorded on completion -- never on
    // entry -- because a node that reaches itself must find its ancestor on
    // `path`, not its own done entry reporting success.
    if (depth > ArchiveContext::MAX_NODE_DEPTH) {
        throw std::runtime_error(
            "FastFHIR Compactor Error: node nesting exceeds the maximum depth of " +
            std::to_string(ArchiveContext::MAX_NODE_DEPTH) +
            " (refusing a crafted or malformed stored graph)");
    }

    const Offset node_off = context.node_offset(node);
    if (std::find(context.path.begin(), context.path.end(), node_off) != context.path.end()) {
        throw std::runtime_error(
            "FastFHIR Compactor Error: cycle in the stored graph at node offset " +
            std::to_string(node_off) + " (an ancestor references it)");
    }

    // Already archived: a shared subtree (or a duplicate enqueued while its
    // twin was still in the queue) resolves to the recorded offset instead of
    // being archived again -- UNLESS a re-walk could newly close a cycle (a
    // direct child on the restored path), in which case we must process so the
    // enqueue-time guard throws. An unconditional memo here would hide the
    // cycle: a node referenced back by one of its own descendants is already
    // in `done`, and the early return would skip the very enqueue that closes
    // the loop (XP-1.3).
    if (const auto done = context.done.find(node_off); done != context.done.end()) {
        if (!child_touches_path(node, context)) {
            return done->second;
        }
    }

    context.path.push_back(node_off);
    // Popped on every exit, not only the successful one: a stale ancestor
    // would make an unrelated sibling report a cycle it does not have.
    struct PathScope {
        std::vector<Offset>& path;
        ~PathScope() { path.pop_back(); }
    } scope{context.path};

    Offset result;
    switch (node.kind()) {
        case FF_FIELD_BLOCK:  result = archive_object(node, context, depth); break;
        case FF_FIELD_ARRAY:  result = archive_array(node, context, depth); break;
        case FF_FIELD_STRING: result = archive_string(node.as<std::string_view>(), context.destination); break;
        default:
            throw std::runtime_error(
                std::string("FastFHIR Compactor Error: unsupported node kind in archive_node(): ") +
                std::to_string(static_cast<int>(node.kind())));
    }
    // Recorded only once the node is fully archived -- the whole correctness
    // argument for the done-set (see ArchiveContext).
    context.done.emplace(node_off, result);
    return result;
}

static void process_pending_write(ArchiveContext& context, const PendingWrite& pending) {
    BYTE* base = context.destination.base();

    // Restore the ancestry the enqueuing node snapshotted (PendingWrite::
    // ancestry). The queue popped this pending long after the parent's frame
    // exited; without the restore the cycle check below would see an empty
    // path and accept a node that reaches its own ancestor (XP-1.3).
    context.path = pending.ancestry;

    switch (pending.kind) {
        case PendingWriteKind::StringPointer: {
            const Offset string_off = archive_string(static_cast<std::string_view>(pending.entry), context.destination);
            STORE_U64(base + pending.slot_offset, string_off);
            break;
        }
        case PendingWriteKind::ArrayPointer: {
            // Through archive_node, not archive_array directly: the array node
            // itself must pass the depth/cycle/done guards -- a cycle can run
            // through an array block.
            const Offset child_off = archive_node(pending.node, context, pending.depth);
            STORE_U64(base + pending.slot_offset, child_off);
            break;
        }
        case PendingWriteKind::NodePointer: {
            const Offset child_off = archive_node(pending.node, context, pending.depth);
            STORE_U64(base + pending.slot_offset, child_off);
            break;
        }
        case PendingWriteKind::ResourcePointer: {
            const Offset child_off = archive_node(pending.node, context, pending.depth);
            STORE_U64(base + pending.slot_offset, child_off);
            STORE_U16(base + pending.slot_offset + DATA_BLOCK::RECOVERY, pending.node.recovery());
            break;
        }
        case PendingWriteKind::Code32:
            write_compact_code_slot(pending.entry, context.destination, pending.parent_anchor, pending.slot_offset);
            break;
        case PendingWriteKind::ResolvedOffset: {
            STORE_U64(base + pending.slot_offset, pending.resolved_offset);
            break;
        }
        case PendingWriteKind::ChoiceNode: {
            const Reflective::Node child = pending.node ? pending.node : pending.entry.as_node();
            const Offset child_off = archive_node(child, context, pending.depth);
            STORE_U64(base + pending.slot_offset, child_off);
            break;
        }
    }
    // Mirrors the enqueue-side increment: one fewer outstanding deferred write.
    --context.pending_balance;
}

Memory::View Compactor::archive(const Parser& source, const Memory& destination,
                                FF_Checksum_Algorithm algo, const HashCallback& hasher) {
    if (!destination) {
        throw std::runtime_error("FastFHIR Compactor Error: destination memory is null.");
    }

    const BYTE* src = source.data();
    const Size src_size = source.size_bytes();
    if (src == nullptr || src_size < FF_HEADER::HEADER_SIZE) {
        throw std::runtime_error("FastFHIR Compactor Error: source parser has invalid stream bytes.");
    }

    Memory& dst = const_cast<Memory&>(destination);
    dst.reset(0);
    ArchiveContext context(dst);

    const Offset header_off = dst.claim_space(FF_HEADER::HEADER_SIZE);
    (void)header_off;

    auto root = source.root();
    if (!root || !root.is_object()) {
        throw std::runtime_error("FastFHIR Compactor Error: source root must be a valid object node.");
    }

    const Offset compact_root_off = archive_node(root, context, 0);

    PendingWrite pending;
    while (true) {
        if (context.consumer.pop(pending)) {
            process_pending_write(context, pending);
            continue;
        }
        if (context.consumer.at_end()) break;
    }

    // Every deferred slot must be resolved before the header is stamped: a
    // nonzero balance here means a pending write was dropped and the sealed
    // stream would silently lose fields.
    if (context.pending_balance != 0) {
        throw std::runtime_error(
            "FastFHIR Compactor Error: " + std::to_string(context.pending_balance) +
            " deferred write(s) never resolved; the compact stream would silently lose fields.");
    }

    // Defense-in-depth: every tracked deferred slot must hold a resolved
    // offset by now. The balance proves every pending was processed; this
    // proves every sentinel was overwritten -- a sentinel left behind (e.g. by
    // a gated pre-fill that never enqueued) would read as a present field with
    // an out-of-bounds offset. Scans only tracked slots, never raw payload:
    // string bytes can legitimately equal the sentinel value.
    BYTE* base = dst.base();
    for (const Offset slot : context.deferred_slots) {
        if (LOAD_U64(base + slot) == FF_PENDING_OFFSET) {
            throw std::runtime_error(
                "FastFHIR Compactor Error: residual pending offset at slot " +
                std::to_string(slot) + "; a deferred write was never resolved.");
        }
    }
    for (const Offset slot : context.deferred_code_slots) {
        if (LOAD_U32(base + slot) == FF_PENDING_CODE) {
            throw std::runtime_error(
                "FastFHIR Compactor Error: residual pending code at slot " +
                std::to_string(slot) + "; a deferred code write was never resolved.");
        }
    }

    // Shared sealing (header + checksum + hash) with Builder::finalize. The
    // URL/module directory offsets are not preserved across compaction.
    return seal_stream(dst, static_cast<uint16_t>(source.version()),
                       compact_root_off,
                       static_cast<RECOVERY_TAG>(source.root_type()), algo,
                       hasher, FF_STREAM_COMPACTED, FF_NULL_OFFSET, FF_NULL_OFFSET);
}

} // namespace FastFHIR
