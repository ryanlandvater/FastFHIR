#include "../include/FF_Compactor.hpp"
#include "../include/FF_Queue.hpp"
#include "../include/FF_Utilities.hpp"

#include "../generated_src/FF_Reflection.hpp"
#include <cstring>
#include <vector>

namespace FastFHIR {

enum class PendingWriteKind {
    StringPointer,
    ArrayPointer,
    NodePointer,
    ResourcePointer,
    Code32,
    ChoiceNode,
};

struct PendingWrite {
    PendingWriteKind kind = PendingWriteKind::NodePointer;
    Reflective::Node node;
    Reflective::Entry entry;
    Offset slot_offset = FF_NULL_OFFSET;
    Offset parent_anchor = FF_NULL_OFFSET;
};

using PendingQueue = FIFO::Queue<PendingWrite, 1024>;

struct ArchiveContext {
    Memory& destination;
    PendingQueue queue;
    PendingQueue::Injector injector;
    PendingQueue::Consumer consumer;

    explicit ArchiveContext(Memory& dst)
        : destination(dst), queue(), injector(queue.get_injector()), consumer(queue.get_consumer()) {}
};

static inline uint32_t compact_presence_bytes(size_t field_count) {
    return static_cast<uint32_t>(field_count / 8 + 1);
}

static inline uint16_t compact_slot_size(FF_FieldKind kind) {
    switch (kind) {
        case FF_FIELD_BOOL:    return TYPE_SIZE_UINT8;
        case FF_FIELD_INT32:   return TYPE_SIZE_INT32;
        case FF_FIELD_UINT32:  return TYPE_SIZE_UINT32;
        case FF_FIELD_INT64:   return TYPE_SIZE_UINT64;
        case FF_FIELD_UINT64:  return TYPE_SIZE_UINT64;
        case FF_FIELD_FLOAT64: return TYPE_SIZE_FLOAT64;
        case FF_FIELD_CODE:    return TYPE_SIZE_UINT32;
        case FF_FIELD_RESOURCE:return TYPE_SIZE_RESOURCE;
        case FF_FIELD_CHOICE:  return TYPE_SIZE_CHOICE;
        default:               return TYPE_SIZE_OFFSET;
    }
}

static inline bool is_inline_scalar_kind(FF_FieldKind kind) {
    switch (kind) {
        case FF_FIELD_BOOL:
        case FF_FIELD_INT32:
        case FF_FIELD_UINT32:
        case FF_FIELD_INT64:
        case FF_FIELD_UINT64:
        case FF_FIELD_FLOAT64:
        case FF_FIELD_CODE:
            return true;
        default:
            return false;
    }
}

static Offset archive_node(const Reflective::Node& node, ArchiveContext& context);
static Offset archive_array(const Reflective::Node& node, ArchiveContext& context);
static Offset archive_object(const Reflective::Node& node, ArchiveContext& context);

static inline void enqueue_pending_write(ArchiveContext& context, const PendingWrite& pending) {
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
    if (raw_code == FF_CODE_NULL || (raw_code & FF_CUSTOM_STRING_FLAG) == 0) {
        STORE_U32(base + dense_off, raw_code);
        return;
    }

    const std::string_view code_str = static_cast<std::string_view>(entry);
    const Offset string_off = archive_string(code_str, destination);
    const Offset relative_off = string_off - compact_parent_off;
    if (relative_off > 0x7FFFFFFF) {
        throw std::runtime_error("FastFHIR Compactor Error: custom code relative offset exceeds 2GB.");
    }
    STORE_U32(base + dense_off, static_cast<uint32_t>(relative_off) | FF_CUSTOM_STRING_FLAG);
}

static void write_choice_slot(const Reflective::Entry& entry, ArchiveContext& context,
                              Offset compact_parent_off, Offset dense_off) {
    BYTE* base = context.destination.base();
    const Offset src_slot = entry.absolute_offset();
    const RECOVERY_TAG tag = static_cast<RECOVERY_TAG>(LOAD_U16(entry.base + src_slot + DATA_BLOCK::RECOVERY));
    STORE_U16(base + dense_off + DATA_BLOCK::RECOVERY, tag);

    if (FF_IsScalarBlockTag(tag)) {
        if (tag == RECOVER_FF_CODE) {
            Reflective::Entry code_entry(entry.base, entry.parent_offset,
                                         static_cast<uint32_t>(src_slot - entry.parent_offset),
                                         RECOVER_FF_CODE, FF_FIELD_CODE,
                                         entry.m_size, entry.m_version, entry.m_ops);
            uint64_t raw_bits = 0;
            const uint32_t raw_code = LOAD_U32(entry.base + src_slot);
            if (raw_code == FF_CODE_NULL || (raw_code & FF_CUSTOM_STRING_FLAG) == 0) {
                raw_bits = raw_code;
            } else {
                const std::string_view code_str = static_cast<std::string_view>(code_entry);
                const Offset string_off = archive_string(code_str, context.destination);
                const Offset relative_off = string_off - compact_parent_off;
                if (relative_off > 0x7FFFFFFF) {
                    throw std::runtime_error("FastFHIR Compactor Error: custom choice-code relative offset exceeds 2GB.");
                }
                raw_bits = static_cast<uint32_t>(relative_off) | FF_CUSTOM_STRING_FLAG;
            }
            STORE_U64(base + dense_off, raw_bits);
            return;
        }

        std::memcpy(base + dense_off, entry.base + src_slot, TYPE_SIZE_UINT64);
        return;
    }

    STORE_U64(base + dense_off, FF_NULL_OFFSET);
    enqueue_pending_write(context, PendingWrite{
        PendingWriteKind::ChoiceNode,
        {},
        entry,
        dense_off,
        compact_parent_off,
    });
}

static Offset archive_array(const Reflective::Node& node, ArchiveContext& context) {
    const auto elements = node.entries();
    const Size array_size = FF_ARRAY::HEADER_SIZE + static_cast<Size>(elements.size()) * TYPE_SIZE_OFFSET;
    Offset array_off = context.destination.claim_space(array_size);
    Offset write_head = array_off;
    STORE_FF_ARRAY_HEADER(context.destination.base(), write_head, FF_ARRAY::OFFSET, TYPE_SIZE_OFFSET,
                          static_cast<uint32_t>(elements.size()), node.recovery());

    for (const auto& element : elements) {
        STORE_U64(context.destination.base() + write_head, FF_NULL_OFFSET);
        if (element) {
            enqueue_pending_write(context, PendingWrite{
                PendingWriteKind::NodePointer,
                element,
                {},
                write_head,
                FF_NULL_OFFSET,
            });
        }
        write_head += TYPE_SIZE_OFFSET;
    }

    return array_off;
}

static Offset archive_object(const Reflective::Node& node, ArchiveContext& context) {
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
        dense_bytes += compact_slot_size(field.kind);
    }

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
                    raw_code == FF_CODE_NULL || (raw_code & FF_CUSTOM_STRING_FLAG) == 0) {
                    STORE_U32(base + dense_off, raw_code);
                } else {
                    STORE_U32(base + dense_off, FF_CODE_NULL);
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
                STORE_U64(base + dense_off, FF_NULL_OFFSET);
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
                STORE_U64(base + dense_off, FF_NULL_OFFSET);
                enqueue_pending_write(context, PendingWrite{
                    PendingWriteKind::ArrayPointer,
                    entry.as_node(),
                    {},
                    dense_off,
                    object_off,
                });
                break;
            }
            case FF_FIELD_RESOURCE: {
                STORE_U64(base + dense_off, FF_NULL_OFFSET);
                STORE_U16(base + dense_off + DATA_BLOCK::RECOVERY, FF_RECOVER_UNDEFINED);
                enqueue_pending_write(context, PendingWrite{
                    PendingWriteKind::ResourcePointer,
                    entry.as_node(),
                    {},
                    dense_off,
                    object_off,
                });
                break;
            }
            case FF_FIELD_CHOICE:
                write_choice_slot(entry, context, object_off, dense_off);
                break;
            default: {
                STORE_U64(base + dense_off, FF_NULL_OFFSET);
                enqueue_pending_write(context, PendingWrite{
                    PendingWriteKind::NodePointer,
                    entry.as_node(),
                    {},
                    dense_off,
                    object_off,
                });
                break;
            }
        }

        dense_off += compact_slot_size(field.kind);
    }

    return object_off;
}

static Offset archive_node(const Reflective::Node& node, ArchiveContext& context) {
    if (!node) return FF_NULL_OFFSET;

    switch (node.kind()) {
        case FF_FIELD_BLOCK:  return archive_object(node, context);
        case FF_FIELD_ARRAY:  return archive_array(node, context);
        case FF_FIELD_STRING: return archive_string(node.as<std::string_view>(), context.destination);
        default:
            throw std::runtime_error(
                std::string("FastFHIR Compactor Error: unsupported node kind in archive_node(): ") +
                std::to_string(static_cast<int>(node.kind())));
    }
}

static void process_pending_write(ArchiveContext& context, const PendingWrite& pending) {
    BYTE* base = context.destination.base();

    switch (pending.kind) {
        case PendingWriteKind::StringPointer: {
            const Offset string_off = archive_string(static_cast<std::string_view>(pending.entry), context.destination);
            STORE_U64(base + pending.slot_offset, string_off);
            break;
        }
        case PendingWriteKind::ArrayPointer: {
            const Offset child_off = archive_array(pending.node, context);
            STORE_U64(base + pending.slot_offset, child_off);
            break;
        }
        case PendingWriteKind::NodePointer: {
            const Offset child_off = archive_node(pending.node, context);
            STORE_U64(base + pending.slot_offset, child_off);
            break;
        }
        case PendingWriteKind::ResourcePointer: {
            const Offset child_off = archive_node(pending.node, context);
            STORE_U64(base + pending.slot_offset, child_off);
            STORE_U16(base + pending.slot_offset + DATA_BLOCK::RECOVERY, pending.node.recovery());
            break;
        }
        case PendingWriteKind::Code32:
            write_compact_code_slot(pending.entry, context.destination, pending.parent_anchor, pending.slot_offset);
            break;
        case PendingWriteKind::ChoiceNode: {
            const Reflective::Node child = pending.entry.as_node();
            const Offset child_off = archive_node(child, context);
            STORE_U64(base + pending.slot_offset, child_off);
            break;
        }
    }
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

    const Offset compact_root_off = archive_object(root, context);

    PendingWrite pending;
    while (true) {
        if (context.consumer.pop(pending)) {
            process_pending_write(context, pending);
            continue;
        }
        if (context.consumer.at_end()) break;
    }

    BYTE* base = dst.base();
    const Offset checksum_off = dst.claim_space(FF_CHECKSUM::HEADER_SIZE);
    STORE_FF_HEADER(
        base,
        static_cast<uint16_t>(source.version()),
        dst.size(),
        compact_root_off,
        static_cast<RECOVERY_TAG>(source.root_type()),
        checksum_off,
        FF_NULL_OFFSET, // url_dir_offset — not preserved across compaction
        FF_NULL_OFFSET, // module_reg_offset — not preserved across compaction
        FF_STREAM_LAYOUT_COMPACT
    );
    BYTE* hash_dst = STORE_FF_CHECKSUM_METADATA(base, checksum_off, algo);

    if (hasher != nullptr && algo != FF_CHECKSUM_NONE) {
        Size bytes_to_hash = checksum_off + FF_CHECKSUM::HASH_DATA;
        std::vector<BYTE> hash_value = hasher(base, bytes_to_hash);
        size_t copy_len = std::min(hash_value.size(), static_cast<size_t>(FF_MAX_HASH_BYTES));
        std::memcpy(hash_dst, hash_value.data(), copy_len);
    }

    return dst.view();
}

} // namespace FastFHIR
