#ifndef FASTFHIR_FIFO_QUEUE_HPP
#define FASTFHIR_FIFO_QUEUE_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <stdexcept>

namespace FastFHIR {
namespace FIFO {

template <class T, uint32_t CAPACITY>
class Queue {
    static_assert((CAPACITY != 0) && ((CAPACITY & (CAPACITY - 1)) == 0), 
                  "Queue capacity must be a power of 2.");

private:
    static constexpr uint32_t NODE_ENTRIES = 2000;
    static constexpr uint32_t NULL_INDEX = 0xFFFFFFFF;

    enum EntryFlag : uint8_t { ENTRY_FREE = 0, ENTRY_WRITING, ENTRY_PENDING, ENTRY_READING, ENTRY_COMPLETE };

    struct Entry {
        T handle;
        std::atomic<EntryFlag> flag{ENTRY_FREE};
    };

    struct Node {
        Entry entries[NODE_ENTRIES];
        std::atomic<uint32_t> front_idx{0};
        std::atomic<uint32_t> next_index{NULL_INDEX};
    };

    class NodeRegistry {
        static constexpr uint32_t INDEX_MASK = CAPACITY - 1;
        static constexpr uint64_t USE_COUNT_MASK = 0x3FFFFFFF;
        static constexpr uint64_t RETIRING_BIT   = 0x40000000;
        static constexpr uint64_t WRITING_BIT    = 0x80000000;
        static constexpr int      GEN_SHIFT      = 32;

        struct alignas(16) Slot {
            std::atomic<uint64_t> state;
            std::atomic<Node*> node{nullptr};
        };

        Slot _slots[CAPACITY];
        std::mutex _mut;
        std::condition_variable _cv;
        std::atomic<uint64_t>* _queue_weak_head{nullptr};

        inline uint32_t wrap(uint32_t raw_index) const { return raw_index & INDEX_MASK; }
        inline Slot& get_slot(uint32_t index) { return _slots[wrap(index)]; }

    public:
        NodeRegistry() {
            for (uint32_t i = 0; i < CAPACITY; ++i) {
                _slots[i].state.store(make_state(1, false, false, 0), std::memory_order_relaxed);
            }
        }

        void set_weak_head_ptr(std::atomic<uint64_t>* ptr) { _queue_weak_head = ptr; }

        static inline uint64_t make_state(uint32_t gen, bool writing, bool retiring, uint32_t uses) {
            uint64_t s = (static_cast<uint64_t>(gen) << GEN_SHIFT) | (uses & USE_COUNT_MASK);
            if (writing)  s |= WRITING_BIT;
            if (retiring) s |= RETIRING_BIT;
            return s;
        }
        
        static inline uint32_t get_uses(uint64_t s) { return s & USE_COUNT_MASK; }
        static inline uint32_t get_gen(uint64_t s)  { return s >> GEN_SHIFT; }
        static inline bool is_retiring(uint64_t s)  { return (s & RETIRING_BIT) != 0; }
        static inline bool is_writing(uint64_t s)   { return (s & WRITING_BIT) != 0; }

        class NodeRef {
            NodeRegistry* _reg;
        public:
            uint32_t index{NULL_INDEX};
            Node* node{nullptr};

            NodeRef() : _reg(nullptr) {}
            NodeRef(NodeRegistry& reg, uint32_t idx, Node* n) : _reg(&reg), index(idx), node(n) {}
            
            ~NodeRef() {
                if (_reg && index != NULL_INDEX) _reg->decrement_node(index);
            }

            NodeRef(const NodeRef&) = delete;
            NodeRef& operator=(const NodeRef&) = delete;

            NodeRef(NodeRef&& other) noexcept : _reg(other._reg), index(other.index), node(other.node) {
                other.index = NULL_INDEX;
                other.node = nullptr;
            }

            void advance(uint32_t next_idx, Node* next_node) {
                if (next_idx != NULL_INDEX) _reg->increment_node(next_idx);
                if (index != NULL_INDEX) _reg->decrement_node(index);
                index = next_idx;
                node = next_node;
            }

            Node* operator->() { return node; }
            explicit operator bool() const { return node != nullptr; }
        };

        Node* acquire_node(uint32_t index, uint32_t expected_gen) {
            auto& slot = get_slot(index);
            uint64_t s = slot.state.load(std::memory_order_acquire);
            
            while (true) {
                if (get_gen(s) != expected_gen || get_uses(s) == 0 || is_retiring(s) || is_writing(s)) {
                    return nullptr; 
                }
                uint64_t next_s = make_state(get_gen(s), false, false, get_uses(s) + 1);
                if (slot.state.compare_exchange_weak(s, next_s, std::memory_order_acq_rel)) {
                    return slot.node.load(std::memory_order_acquire);
                }
            }
        }

        uint32_t allocate_and_link(uint32_t current_tail_idx, Node* tail_node) {
            uint32_t new_idx = (current_tail_idx + 1) & INDEX_MASK;
            uint32_t attempts = 0;

            while (true) {
                if (attempts >= CAPACITY) {
                    std::unique_lock<std::mutex> lock(_mut);
                    _cv.wait(lock);
                    attempts = 0;
                }

                auto& slot = get_slot(new_idx);
                uint64_t s = slot.state.load(std::memory_order_acquire);
                
                if (get_uses(s) == 0 && !is_retiring(s) && !is_writing(s)) {
                    uint64_t claim_st = make_state(get_gen(s), true, false, 2);
                    if (slot.state.compare_exchange_strong(s, claim_st, std::memory_order_acq_rel)) {
                        
                        slot.node.store(new Node(), std::memory_order_release);
                        slot.state.store(make_state(get_gen(s), false, false, 2), std::memory_order_release);

                        uint32_t expected_next = NULL_INDEX;
                        if (tail_node->next_index.compare_exchange_strong(expected_next, new_idx, std::memory_order_release)) {
                            return new_idx; 
                        } else {
                            decrement_node(new_idx); 
                            decrement_node(new_idx); 
                            return expected_next;
                        }
                    }
                }
                new_idx = (new_idx + 1) & INDEX_MASK;
                attempts++;
            }
        }

        void increment_node(uint32_t index) {
            auto& state = get_slot(index).state;
            uint64_t s = state.load(std::memory_order_acquire);
            while (!state.compare_exchange_weak(s, 
                make_state(get_gen(s), is_writing(s), is_retiring(s), get_uses(s) + 1),
                std::memory_order_acq_rel));
        }

        void decrement_node(uint32_t index) {
            auto& slot = get_slot(index);
            uint64_t s = slot.state.load(std::memory_order_acquire);
            
            while (true) {
                uint32_t uses = get_uses(s);
                #if DEBUG
                if (uses == 0) throw std::logic_error("Double-free detected.");
                #else
                if (uses == 0) return;
                #endif

                else if (uses == 1) {
                    uint64_t retiring = make_state(get_gen(s), false, true, 0);
                    if (slot.state.compare_exchange_weak(s, retiring, std::memory_order_acq_rel)) {
                        Node* n = slot.node.exchange(nullptr, std::memory_order_release);
                        
                        if (_queue_weak_head) {
                            uint32_t next_idx = n->next_index.load(std::memory_order_relaxed);
                            if (next_idx != NULL_INDEX) {
                                uint32_t next_gen = get_gen(get_slot(next_idx).state.load(std::memory_order_relaxed));
                                uint64_t old_head = (static_cast<uint64_t>(get_gen(s)) << 32) | index;
                                uint64_t new_head = (static_cast<uint64_t>(next_gen) << 32) | next_idx;
                                
                                _queue_weak_head->compare_exchange_strong(old_head, new_head, std::memory_order_release);
                            }
                        }

                        delete n;
                        slot.state.store(make_state(get_gen(s) + 1, false, false, 0), std::memory_order_release);
                        
                        std::unique_lock<std::mutex> lock(_mut);
                        _cv.notify_one();
                        return;
                    }
                } 
                
                else {
                    uint64_t dec = make_state(get_gen(s), is_writing(s), is_retiring(s), uses - 1);
                    if (slot.state.compare_exchange_weak(s, dec, std::memory_order_acq_rel)) {
                        return;
                    }
                }
            }
        }
    };

    NodeRegistry registry;
    std::atomic<uint32_t> _tail_index{0};
    std::atomic<uint64_t> _weak_head{0}; 

    void advance_tail(uint32_t old_idx, uint32_t new_idx) {
        if (_tail_index.compare_exchange_strong(old_idx, new_idx, std::memory_order_release)) {
            registry.decrement_node(old_idx);
        }
    }

public:
    // ---------------------------------------------------------
    // Injector
    // ---------------------------------------------------------
    class Injector {
        friend class Queue;

        Queue* _queue;
        typename NodeRegistry::NodeRef _ref;

        Injector(Queue* q, typename NodeRegistry::NodeRef ref) 
            : _queue(q), _ref(std::move(ref)) {}

    public:
        Injector(Injector&&) = default;
        Injector& operator=(Injector&&) = default;
        Injector(const Injector&) = delete;
        Injector& operator=(const Injector&) = delete;

        void push(const T& value) {
            if (!_ref) return;

            for (;;) {
                uint32_t entry_idx = _ref->front_idx.fetch_add(1, std::memory_order_relaxed);

                if (entry_idx < NODE_ENTRIES) {
                    EntryFlag expected = ENTRY_FREE;
                    if (_ref->entries[entry_idx].flag.compare_exchange_strong(expected, ENTRY_WRITING, std::memory_order_acquire)) {
                        _ref->entries[entry_idx].handle = value;
                        _ref->entries[entry_idx].flag.store(ENTRY_PENDING, std::memory_order_release);
                        return; 
                    }
                } else {
                    uint32_t next = _ref->next_index.load(std::memory_order_acquire);
                    
                    if (next == NULL_INDEX) {
                        next = _queue->registry.allocate_and_link(_ref.index, _ref.node);
                    }

                    _queue->advance_tail(_ref.index, next);
                    
                    Node* next_node = _queue->registry.get_slot(next).node.load(std::memory_order_acquire);
                    _ref.advance(next, next_node); 
                }
            }
        }
    };

    // ---------------------------------------------------------
    // Consumer
    // ---------------------------------------------------------
    class Consumer {
        friend class Queue;

        Queue* _queue;
        typename NodeRegistry::NodeRef _ref;
        uint32_t _entry_idx{0};

        Consumer(Queue* q, typename NodeRegistry::NodeRef ref) 
            : _queue(q), _ref(std::move(ref)) {}

    public:
        Consumer(Consumer&&) = default;
        Consumer& operator=(Consumer&&) = default;
        Consumer(const Consumer&) = delete;
        Consumer& operator=(const Consumer&) = delete;

        bool pop(T& reference) {
            if (!_ref) return false;

            for (;;) {
                if (_entry_idx < NODE_ENTRIES) {
                    EntryFlag expected = ENTRY_PENDING;
                    if (_ref->entries[_entry_idx].flag.compare_exchange_strong(expected, ENTRY_READING, std::memory_order_acquire)) {
                        reference = _ref->entries[_entry_idx].handle;
                        _ref->entries[_entry_idx].flag.store(ENTRY_COMPLETE, std::memory_order_release);
                        _entry_idx++;
                        return true;
                    }
                    
                    if (expected == ENTRY_FREE || expected == ENTRY_WRITING) {
                        return false; 
                    }
                    
                    _entry_idx++; 
                    continue;
                }

                uint32_t next = _ref->next_index.load(std::memory_order_acquire);
                if (next == NULL_INDEX) return false; 

                Node* next_node = _queue->registry.get_slot(next).node.load(std::memory_order_acquire);
                _ref.advance(next, next_node);
                _entry_idx = 0;
            }
        }

        bool at_end() const {
            if (!_ref) return true;

            if (_entry_idx < NODE_ENTRIES) {
                EntryFlag flag = _ref->entries[_entry_idx].flag.load(std::memory_order_acquire);
                return (flag == ENTRY_FREE || flag == ENTRY_WRITING);
            }
            
            return _ref->next_index.load(std::memory_order_acquire) == NULL_INDEX;
        }
    };

    // ---------------------------------------------------------
    // Queue Setup
    // ---------------------------------------------------------
    Queue() {
        registry.set_weak_head_ptr(&_weak_head);

        auto& slot = registry.get_slot(0);
        slot.node.store(new Node(), std::memory_order_relaxed);
        slot.state.store(NodeRegistry::make_state(1, false, false, 1), std::memory_order_relaxed);
        
        _tail_index.store(0, std::memory_order_relaxed);
        _weak_head.store((1ULL << 32) | 0, std::memory_order_relaxed);
    }

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    Injector get_injector() {
        uint32_t t_idx = _tail_index.load(std::memory_order_acquire);
        uint32_t t_gen = NodeRegistry::get_gen(registry.get_slot(t_idx).state.load());
        
        Node* n = registry.acquire_node(t_idx, t_gen);
        if (!n) throw std::runtime_error("Critical state failure: Active tail destroyed.");
        
        return Injector(this, typename NodeRegistry::NodeRef(registry, t_idx, n));
    }

    Consumer get_consumer() {
        while (true) {
            uint64_t head = _weak_head.load(std::memory_order_acquire);
            uint32_t gen = head >> 32;
            uint32_t idx = head & 0xFFFFFFFF;

            Node* n = registry.acquire_node(idx, gen);
            if (n) {
                return Consumer(this, typename NodeRegistry::NodeRef(registry, idx, n));
            }
        }
    }
};

} // namespace FIFO
} // namespace FastFHIR

#endif