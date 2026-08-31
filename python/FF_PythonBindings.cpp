/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <pybind11/eval.h>
#include <pybind11/stl/filesystem.h>

#include "FF_Primitives.hpp"
#include "FF_Memory.hpp"
#include "FF_Builder.hpp"
#include "FF_Parser.hpp"
#include "FF_Ops.hpp"
#include "FF_Compactor.hpp"
#include "FF_Ingestor.hpp"
#include "FastFHIR.hpp"
#include "FF_Utilities.hpp" 

// The auto-generated Registry
#include "FF_AllTypes.hpp"
#include "FF_FieldKeys.hpp" 
#include "FF_Reflection.hpp"

namespace py = pybind11;
using namespace FastFHIR;

// Temporary struct to hold the data coming from Python's fields.py
struct PythonFieldProxy {
    uint32_t registry_index;
    std::string name;
    std::string parent;
};

// =====================================================================
// Lifetime Wrappers (Python-only)
// =====================================================================
struct PyMemory {
    FF_Memory m_core;
    bool m_closed = false;

    PyMemory(size_t capacity, const std::string& shm_name) {
        FF_MemoryCreateInfo info;
        info.capacity = capacity;
        if (!shm_name.empty()) info.shm_name = shm_name.c_str();
        FF_Result res = FF_CreateMemory(info, m_core);
        if (!res) throw std::runtime_error(res.message);
    }

    PyMemory(const std::string& filepath, size_t capacity) {
        FF_MemoryCreateInfo info;
        info.capacity = capacity;
        info.filepath = filepath.c_str();
        FF_Result res = FF_CreateMemory(info, m_core);
        if (!res) throw std::runtime_error(res.message);
    }

    ~PyMemory() { close(); }

    void close() {
        m_closed = true;
        m_core.reset();
    }

    Memory& get() {
        if (m_closed || !m_core) throw py::value_error("I/O operation on closed FastFHIR Memory object.");
        return *m_core;
    }
    
    const Memory& get() const {
        if (m_closed || !m_core) throw py::value_error("I/O operation on closed FastFHIR Memory object.");
        return *m_core;
    }
};

struct PyStream {
    FF_Stream m_builder;
    bool m_closed = false;

    PyStream(PyMemory& mem, FHIR_VERSION fhir_version) {
        FF_StreamCreateInfo info;
        info.version = fhir_version;
        info.arena = mem.m_core;
        FF_Result res = FF_CreateStream(info, m_builder);
        if (!res) throw std::runtime_error(res.message);
    }

    void close() {
        m_closed = true;
        m_builder.reset();
    }

    Builder& get() {
        if (m_closed || !m_builder) throw py::value_error("I/O operation on closed FastFHIR Stream.");
        return *m_builder;
    }
    
    const Builder& get() const {
        if (m_closed || !m_builder) throw py::value_error("I/O operation on closed FastFHIR Stream.");
        return *m_builder;
    }
};

// =====================================================================
// Explicit Shared Ownership Proxies
// =====================================================================
// These carry the shared_ptr seamlessly through Python to guarantee memory safety
// without altering the lightweight C++ AST architecture.
struct PyStreamNode {
    std::shared_ptr<Builder> builder;
    Reflective::ObjectHandle handle;

    PyStreamNode(std::shared_ptr<Builder> b, Reflective::ObjectHandle h) : builder(std::move(b)), handle(h) {}
};

struct PyMutableEntry {
    std::shared_ptr<Builder> builder;
    Reflective::MutableEntry entry;

    PyMutableEntry(std::shared_ptr<Builder> b, Reflective::MutableEntry e) : builder(std::move(b)), entry(e) {}

    FF_FieldKind kind() const { return entry.m_kind; }
    RECOVERY_TAG recovery() const { return entry.m_recovery; }
    RECOVERY_TAG effective_recovery() const {
        switch (entry.m_kind) {
            case FF_FIELD_BLOCK:
            case FF_FIELD_RESOURCE:
            case FF_FIELD_ARRAY: {
                Reflective::ObjectHandle handle = entry.as_handle();
                return handle ? handle.recovery() : entry.m_recovery;
            }
            default:
                return entry.m_recovery;
        }
    }
};

// =====================================================================
// Shared Rendering Helpers
// =====================================================================
static std::string render_node_json(const Reflective::Node& node) {
    std::ostringstream oss;
    node.print_json(oss);
    return oss.str();
}

static std::string render_handle_json(const Reflective::ObjectHandle& handle) {
    return render_node_json(handle.as_node());
}

static py::object materialize_handle_value(const std::shared_ptr<Builder>& builder,
                                           const Reflective::ObjectHandle& handle);

static py::object materialize_mutable_entry_value(const PyMutableEntry& entry_wrapper,
                                                  bool recursive) {
    const FF_FieldKind kind = entry_wrapper.kind();
    const Reflective::Entry entry = entry_wrapper.entry.as_entry();
    if (!entry) {
        return py::none();
    }

    const auto* builder = entry_wrapper.entry.get_builder();
    const Size arena_size = builder ? builder->memory().size() : 0;
    const uint32_t version = builder ? static_cast<uint32_t>(builder->FhirVersion()) : 0;

    switch (kind) {
        case FF_FIELD_BOOL:
            return py::bool_(entry.as_scalar<bool>(RECOVER_FF_BOOL));
        case FF_FIELD_INT32:
            return py::int_(entry.as_scalar<int32_t>(RECOVER_FF_INT32));
        case FF_FIELD_UINT32:
            return py::int_(entry.as_scalar<uint32_t>(RECOVER_FF_UINT32));
        case FF_FIELD_INT64:
            return py::int_(entry.as_scalar<int64_t>(RECOVER_FF_INT64));
        case FF_FIELD_UINT64:
            return py::int_(entry.as_scalar<uint64_t>(RECOVER_FF_UINT64));
        case FF_FIELD_FLOAT64:
            return py::float_(entry.as_scalar<double>(RECOVER_FF_FLOAT64));
        case FF_FIELD_CODE: {
            std::ostringstream oss;
            entry.print_scalar_json(oss, version);
            std::string s = oss.str();
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                s = s.substr(1, s.size() - 2);
            }
            if (s == "null") {
                return py::none();
            }
            return py::str(s);
        }
        case FF_FIELD_STRING: {
            Reflective::Node node = entry.as_node(arena_size, version, entry_wrapper.recovery(), kind);
            if (node.is_empty()) {
                return py::none();
            }
            return py::str(node.as<std::string_view>());
        }
        case FF_FIELD_URL: {
            const Offset abs_off = entry.absolute_offset();
            const uint32_t ref = LOAD_U32(entry.base + abs_off);
            if (ref == FF_NULL_UINT32) return py::none();
            // Reconstruct the URL text from the stream's FF_URL_DIRECTORY.
            const Offset dir_off = FF_HEADER(arena_size).get_url_dir_offset(entry.base);
            if (dir_off == FF_NULL_OFFSET || dir_off >= arena_size) return py::none();
            std::string url = FF_URL_DIRECTORY(dir_off, arena_size, version).get_url(entry.base, ref);
            return py::str(url);
        }
        case FF_FIELD_DATETIME: {
            // DT-2: packed values format canonically; a flagged fallback
            // returns the ORIGINAL text byte-exact.
            const Offset abs_off = entry.absolute_offset();
            const uint64_t raw = LOAD_U64(entry.base + abs_off);
            if (raw == FF_DATETIME_NULL) {
                return py::none();
            }
            std::string dt_text;
            if (FF_DATETIME_IS_FALLBACK(raw)) {
                FF_STRING s(FF_ResolveDateTimeOffset(raw, entry.parent_offset), 0, version);
                dt_text = std::string(s.read_view(entry.base));
            } else {
                dt_text = FF_FORMAT_DATETIME(FF_UNPACK_DATETIME(raw), entry.target_recovery);
            }
            return py::str(dt_text);
        }
        case FF_FIELD_RESOURCE: {
            // A resource outside the compiled profile is retained as an
            // opaque-JSON block, which has string layout and NO V-Table. Handing
            // it to ObjectHandle below would offer Python a navigable object
            // whose every field lookup misses. Return the raw JSON text instead
            // and let the caller decide whether to parse it.
            const Offset abs_off = entry.absolute_offset();
            const auto slot_tag = FF_GET_RECOVERY_TAG(entry.base, abs_off);
            if (slot_tag == RECOVER_FF_OPAQUE_JSON) {
                const Offset child_off = LOAD_U64(entry.base + abs_off);
                if (child_off == FF_NULL_OFFSET) return py::none();
                return py::str(FF_STRING(child_off, arena_size, version,
                                         entry.m_engine_version).read_view(entry.base));
            }
            [[fallthrough]];
        }
        case FF_FIELD_BLOCK:
        case FF_FIELD_ARRAY: {
            Reflective::ObjectHandle elevated = entry_wrapper.entry.as_handle();
            if (elevated.offset() == FF_NULL_OFFSET) {
                return py::none();
            }
            if (!recursive) {
                return py::cast(PyStreamNode(entry_wrapper.builder, elevated));
            }
            return materialize_handle_value(entry_wrapper.builder, elevated);
        }
        case FF_FIELD_CHOICE: {
            // Resolve the polymorphic choice [x] field by reading its actual tag.
            // The choice slot is 10 bytes: [uint64_t data][uint16_t recovery_tag].
            const Offset abs_off = entry.absolute_offset();
            RECOVERY_TAG actual_tag = FF_GET_RECOVERY_TAG(entry.base, abs_off);
            if (actual_tag == FF_RECOVER_UNDEFINED) {
                return py::none(); // No choice selected
            }

            // ── Inline scalar choices (bool, int, float, etc.) ──
            // The value is stored directly in the 8-byte data slot at abs_off.
            if (FF_IsScalarBlockTag(actual_tag)) {
                switch (actual_tag) {
                    case RECOVER_FF_BOOL:
                        return py::bool_(static_cast<bool>(
                            LOAD_U8(entry.base + abs_off)));
                    case RECOVER_FF_INT32:
                        return py::int_(static_cast<int32_t>(
                            LOAD_U32(entry.base + abs_off)));
                    case RECOVER_FF_UINT32:
                        return py::int_(static_cast<uint32_t>(
                            LOAD_U32(entry.base + abs_off)));
                    case RECOVER_FF_INT64:
                        return py::int_(static_cast<int64_t>(
                            LOAD_U64(entry.base + abs_off)));
                    case RECOVER_FF_UINT64:
                        return py::int_(static_cast<uint64_t>(
                            LOAD_U64(entry.base + abs_off)));
                    case RECOVER_FF_FLOAT64: {
                        uint64_t raw = LOAD_U64(entry.base + abs_off);
                        double val;
                        std::memcpy(&val, &raw, sizeof(double));
                        return py::float_(val);
                    }
                    default:
                        return py::none();
                }
            }

            // ── Block/String/Array choices ──
            // The slot contains an offset to the child data block.
            // For strings, the data slot holds the string block offset.
            Offset child_off = LOAD_U64(entry.base + abs_off);
            if (child_off == FF_NULL_OFFSET) {
                return py::none();
            }
            if (actual_tag == RECOVER_FF_STRING) {
                auto str = FF_STRING(child_off, arena_size, version,
                    entry.m_engine_version).read_view(entry.base);
                return py::str(std::string_view(str));
            }
            // Block/resource: read tag from child and wrap as handle
            RECOVERY_TAG child_tag = FF_GET_RECOVERY_TAG(entry.base, child_off);
            Reflective::ObjectHandle elevated(
                const_cast<Builder*>(builder),
                child_off, child_tag);
            if (elevated.offset() == FF_NULL_OFFSET) {
                return py::none();
            }
            if (!recursive) {
                return py::cast(PyStreamNode(entry_wrapper.builder, elevated));
            }
            return materialize_handle_value(entry_wrapper.builder, elevated);
        }
        default:
            return py::none();
    }
}

static py::object materialize_handle_value(const std::shared_ptr<Builder>& builder,
                                           const Reflective::ObjectHandle& handle) {
    if (!handle || handle.offset() == FF_NULL_OFFSET) {
        return py::none();
    }

    if (handle.is_array()) {
        py::list values;
        for (size_t i = 0; i < handle.size(); ++i) {
            values.append(materialize_mutable_entry_value(PyMutableEntry(builder, handle[i]), true));
        }
        return std::move(values);
    }

    py::dict values;
    Reflective::Node node = handle.as_node();
    if (!node || !node.is_object()) {
        return values;
    }

    for (const FF_FieldInfo& field : FastFHIR::reflected_fields_view(handle.recovery())) {
        FF_FieldKey key(handle.recovery(), field.kind, field.field_offset, field.child_recovery,
                        field.array_entries_are_offsets, field.name,
                        field.name ? std::char_traits<char>::length(field.name) : 0);
        Reflective::Entry parsed_entry = node[key];
        if (parsed_entry) {
            values[py::str(field.name ? field.name : "")] =
                materialize_mutable_entry_value(PyMutableEntry(builder, handle[key]), true);
        }
    }

    return std::move(values);
}

static bool entry_has_value(const PyMutableEntry& entry_wrapper) {
    return static_cast<bool>(entry_wrapper.entry.as_entry());
}

static std::string render_mutable_entry_json(const PyMutableEntry& entry_wrapper) {
    const Reflective::Entry entry = entry_wrapper.entry.as_entry();
    if (!entry) {
        return "null";
    }

    // For arrays, iterate through elements and materialize each one
    // (this handles mutations correctly unlike reading from the original offset)
    if (entry_wrapper.entry.is_array()) {
        py::list items;
        size_t sz = entry_wrapper.entry.size();
        for (size_t i = 0; i < sz; ++i) {
            auto elem = PyMutableEntry(entry_wrapper.builder, entry_wrapper.entry[i]);
            py::object mat = materialize_mutable_entry_value(elem, true);
            items.append(mat);
        }
        
        // Serialize array to JSON
        try {
            py::module_ json = py::module_::import("json");
            return py::cast<std::string>(json.attr("dumps")(items));
        } catch (const std::exception&) {
            return "[]";
        }
    }
    
    // For non-array fields, materialize and serialize
    py::object materialized = materialize_mutable_entry_value(entry_wrapper, true);
    try {
        py::module_ json = py::module_::import("json");
        return py::cast<std::string>(json.attr("dumps")(materialized));
    } catch (const std::exception&) {
        return "null";
    }
}

static std::string render_parser_json(const Parser& parser) {
    std::ostringstream oss;
    parser.print_json(oss);
    return oss.str();
}

/// Snapshot the live stream via the FF_* surface; query() is private on Builder.
static Parser stream_query(const PyStream& self) {
    Parser parser;
    FF_Result res = FF_StreamQuery(FF_StreamQueryInfo{
        .stream = self.m_builder,
    }, parser);
    if (!res) throw std::runtime_error(res.message);
    return parser;
}

static bool try_extract_recovery_tag(py::handle obj, RECOVERY_TAG& out_tag) {
    if (py::isinstance<py::int_>(obj)) {
        out_tag = obj.cast<RECOVERY_TAG>();
        return true;
    }

    if (!py::hasattr(obj, "path")) {
        return false;
    }

    py::tuple path = obj.attr("path").cast<py::tuple>();
    if (!path.empty()) {
        return false;
    }

    std::string class_name = py::cast<std::string>(py::type::of(obj).attr("__name__"));
    constexpr std::string_view suffix = "_PATH";
    if (class_name.size() <= suffix.size() ||
        class_name.compare(class_name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }

    const std::string type_name = class_name.substr(0, class_name.size() - suffix.size());
    const RECOVERY_TAG resource_tags[] = {
        RECOVER_FF_OBSERVATION,
        RECOVER_FF_PATIENT,
        RECOVER_FF_ENCOUNTER,
        RECOVER_FF_DIAGNOSTICREPORT,
        RECOVER_FF_BUNDLE,
    };

    for (RECOVERY_TAG tag : resource_tags) {
        std::string reflected_name;
        for (char ch : FastFHIR::reflected_resource_type(tag)) {
            reflected_name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
        if (reflected_name == type_name) {
            out_tag = tag;
            return true;
        }
    }

    return false;
}

static py::list collect_filled_object_values(const std::shared_ptr<Builder>& builder,
                                            const Reflective::ObjectHandle& handle) {
    py::list filled_fields;
    if (!handle || handle.offset() == FF_NULL_OFFSET) {
        return filled_fields;
    }

    Reflective::Node node = handle.as_node();
    if (!node || !node.is_object()) {
        return filled_fields;
    }

    for (const FF_FieldInfo& field : FastFHIR::reflected_fields_view(handle.recovery())) {
        FF_FieldKey key(handle.recovery(), field.kind, field.field_offset, field.child_recovery,
                        field.array_entries_are_offsets, field.name,
                        field.name ? std::char_traits<char>::length(field.name) : 0);
        Reflective::Entry parsed_entry = node[key];
        if (parsed_entry) {
            filled_fields.append(PyMutableEntry(builder, handle[key]));
        }
    }

    return filled_fields;
}

static py::list collect_filled_object_items(const std::shared_ptr<Builder>& builder,
                                           const Reflective::ObjectHandle& handle) {
    py::list items;
    if (!handle || handle.offset() == FF_NULL_OFFSET) {
        return items;
    }

    Reflective::Node node = handle.as_node();
    if (!node || !node.is_object()) {
        return items;
    }

    for (const FF_FieldInfo& field : FastFHIR::reflected_fields_view(handle.recovery())) {
        FF_FieldKey key(handle.recovery(), field.kind, field.field_offset, field.child_recovery,
                        field.array_entries_are_offsets, field.name,
                        field.name ? std::char_traits<char>::length(field.name) : 0);
        Reflective::Entry parsed_entry = node[key];
        if (parsed_entry) {
            items.append(py::make_tuple(py::str(field.name ? field.name : ""),
                                        PyMutableEntry(builder, handle[key])));
        }
    }

    return items;
}

// =====================================================================
// Type-Safe Python to C++ Assignment Dispatcher
// =====================================================================
void assign_py_obj(Reflective::MutableEntry& entry, py::handle obj, Reflective::ObjectHandle& parent_handle, const FF_FieldKey& key) {
    if (py::isinstance<PyStreamNode>(obj)) {
        entry = obj.cast<PyStreamNode>().handle;
        return;
    } 
    if (py::isinstance<py::str>(obj)) {
        entry = obj.cast<std::string_view>();
        return;
    } 
    if (py::isinstance<py::bool_>(obj)) {
        entry = obj.cast<bool>();
        return;
    } 
    if (py::isinstance<py::int_>(obj)) {
        entry = obj.cast<int64_t>();
        return;
    } 
    if (py::isinstance<py::float_>(obj)) {
        entry = obj.cast<double>();
        return;
    } 

    if (py::isinstance<py::list>(obj) || py::isinstance<py::dict>(obj)) {
        if (py::isinstance<py::list>(obj)) {
            auto list = obj.cast<py::list>();
            if (!list.empty() && py::isinstance<PyStreamNode>(list[0])) {
                auto wrappers = list.cast<std::vector<PyStreamNode>>();
                if (FF_IsResourceTag(wrappers[0].handle.recovery())) {
                    std::vector<ResourceReference> refs;
                    for (const auto& w : wrappers) refs.push_back({w.handle.offset(), w.handle.recovery()});
                    entry = refs; 
                } else {
                    std::vector<Offset> offsets;
                    for (const auto& w : wrappers) offsets.push_back(w.handle.offset());
                    entry = offsets; 
                }
                return;
            }
        }
        
        py::module_ json = py::module_::import("json");
        std::string json_string = py::cast<std::string>(json.attr("dumps")(obj));

        FF_IngestorCreateInfo ingest_info;
        FF_Ingestor ingestor;
        FF_Result create_res = FF_CreateIngestor(ingest_info, ingestor);
        if (!create_res) throw std::runtime_error(create_res.message);

        FF_Result res = FF_IngestInsertAtField(FF_IngestInsertInfo{
            .ingestor = ingestor,
            .parent = parent_handle,
            .key = key,
            .payload = json_string,
        });

        if (res.failed()) {
            throw std::runtime_error(res.message);
        }
        return;
    } 
    throw py::type_error("FastFHIR: Unsupported Python type for stream assignment.");
}

// =====================================================================
// Deep AST Traversal Helper (Executes entirely in C++)
// =====================================================================
PyMutableEntry resolve_ast_path(const PyStreamNode& root, py::tuple path) {
    if (path.empty()) throw py::value_error("FastFHIR: Cannot traverse an empty AST path.");

    auto get_next_leaf = [](Reflective::ObjectHandle parent, py::handle item) -> Reflective::MutableEntry {
        if (py::isinstance<py::int_>(item)) {
            return parent[item.cast<size_t>()];
        } else if (py::isinstance<PythonFieldProxy>(item)) {
            auto field = item.cast<PythonFieldProxy>();
            if (field.registry_index >= FastFHIR::FieldKeys::RegistrySize) {
                throw py::index_error("FastFHIR: Field registry index out of bounds.");
            }
            return parent[*FastFHIR::FieldKeys::Registry[field.registry_index]];
        }
        throw py::type_error("FastFHIR: AST path elements must be Field objects or integers.");
    };

    Reflective::ObjectHandle current_parent = root.handle;
    for (size_t i = 0; i < path.size() - 1; ++i) {
        current_parent = get_next_leaf(current_parent, path[i]).as_handle();
    }

    return PyMutableEntry(root.builder, get_next_leaf(current_parent, path[path.size() - 1]));
}

PYBIND11_MODULE(_core, m) {
    m.doc() = "FastFHIR core C++ engine.";
    m.attr("__version__") = FASTFHIR_VERSION_STRING;

    // =====================================================================
    // 1. Enums
    // =====================================================================
    py::enum_<FF_Checksum_Algorithm>(m, "Checksum")
        .value("NONE", FF_CHECKSUM_NONE)
        .value("CRC32", FF_CHECKSUM_CRC32)
        .value("MD5", FF_CHECKSUM_MD5)
        .value("SHA256", FF_CHECKSUM_SHA256)
        .export_values();

    py::enum_<FF_SourceType>(m, "SourceType")
        .value("FHIR_JSON", FF_SOURCE_FHIR_JSON)
        .value("HL7_V2", FF_SOURCE_HL7_V2)
        .value("HL7_V3", FF_SOURCE_HL7_V3)
        .export_values();

    py::enum_<FHIR_VERSION>(m, "FhirVersion")
        .value("R4", FHIR_VERSION_R4)
        .value("R5", FHIR_VERSION_R5)
        .export_values();

    py::class_<PythonFieldProxy>(m, "Field")
        .def(py::init<uint32_t, std::string, std::string>())
        .def_readonly("registry_index", &PythonFieldProxy::registry_index)
        .def_readonly("name", &PythonFieldProxy::name)
        .def_readonly("parent", &PythonFieldProxy::parent);

    py::enum_<RECOVERY_TAG>(m, "ResourceType")
        .value("Patient", RECOVER_FF_PATIENT)
        .value("Bundle", RECOVER_FF_BUNDLE)
        .value("Encounter", RECOVER_FF_ENCOUNTER)
        .value("Observation", RECOVER_FF_OBSERVATION)
        .value("DiagnosticReport", RECOVER_FF_DIAGNOSTICREPORT)
        .export_values();
    
    // =====================================================================
    // 2. Memory & Zero-Copy Buffers
    // =====================================================================
    py::class_<Memory::View>(m, "MemoryView", py::buffer_protocol())
        .def_buffer([](Memory::View &v) -> py::buffer_info {
            return py::buffer_info(
                const_cast<char*>(v.data()), sizeof(char),
                py::format_descriptor<char>::format(), 1, { v.size() }, { sizeof(char) }, true);
        })
        .def_property_readonly("size", &Memory::View::size)
        .def_property_readonly("empty", &Memory::View::empty);

    py::class_<Memory::StreamHead>(m, "StreamHead", py::buffer_protocol())
        .def_buffer([](Memory::StreamHead &s) -> py::buffer_info {
            return py::buffer_info(
                s.write_ptr(), sizeof(uint8_t),
                py::format_descriptor<uint8_t>::format(), 1, { s.available_space() }, { sizeof(uint8_t) }, false);
        })
        .def("commit", &Memory::StreamHead::commit, py::arg("bytes_written"))
        .def("close", &Memory::StreamHead::close)
        .def_property_readonly("available_space", &Memory::StreamHead::available_space)
        .def("__enter__", [](Memory::StreamHead& self) -> Memory::StreamHead& { return self; })
        .def("__exit__", [](Memory::StreamHead& self, py::object, py::object, py::object) { self.close(); });

    py::class_<PyMemory, std::shared_ptr<PyMemory>>(m, "Memory")
        .def_static("create", [](size_t capacity, const std::string& shm) {
            return std::make_shared<PyMemory>(capacity, shm); },
            py::arg("capacity") = 4ULL * 1024 * 1024 * 1024, py::arg("shared_memory_name") = "")
        .def_static("create_from_file", [](const std::string& path, size_t cap) {
            return std::make_shared<PyMemory>(path, cap); }, 
            py::arg("filepath"), py::arg("capacity") = 4ULL * 1024 * 1024 * 1024)
        .def("try_acquire_stream", [](PyMemory& mem) {
            auto sh = mem.get().try_acquire_stream();
            if (!sh) throw std::runtime_error("Stream lock currently held by another socket/thread.");
            return std::move(*sh);
        }, py::keep_alive<0, 1>())
        .def("reset", [](PyMemory& mem, size_t committed_size) { mem.get().reset(committed_size); },
            py::arg("committed_size") = 0)
        .def("close", &PyMemory::close)
        .def("__enter__", [](PyMemory& self) -> PyMemory& { return self; })
        .def("__exit__", [](PyMemory& self, py::object, py::object, py::object) { self.close(); })
        .def_property_readonly("capacity", [](const PyMemory& self) { return self.get().capacity(); })
        .def_property_readonly("name", [](const PyMemory& self) { return self.get().name(); })
        .def_property_readonly("size", [](const PyMemory& self) { return self.get().size(); })
        .def("view", [](const PyMemory& self) { return self.get().view(); });

    // =====================================================================
    // 3. Object Proxies
    // =====================================================================
    py::class_<PyStreamNode>(m, "StreamNode")
        .def_property_readonly("offset", [](const PyStreamNode& s) { return s.handle.offset(); })
        .def_property_readonly("recovery_tag", [](const PyStreamNode& s) { return s.handle.recovery(); })
        .def("is_array", [](const PyStreamNode& s) { return s.handle.is_array(); })
        .def("to_json", [](const PyStreamNode& s) { return render_handle_json(s.handle); })
        .def("__str__", [](const PyStreamNode& s) { return render_handle_json(s.handle); })
        .def("__repr__", [](const PyStreamNode& s) { return render_handle_json(s.handle); })
        .def("__bool__", [](const PyStreamNode& s) { return s.handle.offset() != FF_NULL_OFFSET; })
        .def("__eq__", [](const PyStreamNode& self, py::object other) -> py::object {
            RECOVERY_TAG other_tag = FF_RECOVER_UNDEFINED;
            if (try_extract_recovery_tag(other, other_tag)) {
                return py::bool_(self.handle.recovery() == other_tag);
            }
            return py::reinterpret_borrow<py::object>(Py_NotImplemented);
        })
        .def("__len__", [](const PyStreamNode& s) -> size_t {
            if (!s.handle.is_array()) return 0;
            return s.handle.size();
        })
        .def("__iter__", [](const PyStreamNode& self) {
            if (self.handle.is_array()) {
                // For arrays, yield each entry as a PyMutableEntry
                py::list items;
                for (size_t i = 0; i < self.handle.size(); ++i) {
                    items.append(PyMutableEntry(self.builder, self.handle[i]));
                }
                return py::iter(items);
            } else {
                return py::iter(collect_filled_object_values(self.builder, self.handle));
            }
        })
        .def("items", [](const PyStreamNode& self, bool recursive) {
            // Dict-like items() for nodes; for arrays, iterate as (index, item) pairs
            py::list items;
            if (self.handle.is_array()) {
                for (size_t i = 0; i < self.handle.size(); ++i) {
                    auto entry = PyMutableEntry(self.builder, self.handle[i]);
                    items.append(py::make_tuple(py::int_(i), materialize_mutable_entry_value(entry, recursive)));
                }
            } else {
                if (recursive) {
                    py::object values = materialize_handle_value(self.builder, self.handle);
                    if (py::isinstance<py::dict>(values)) {
                        for (auto item : values.cast<py::dict>()) {
                            items.append(py::make_tuple(item.first, item.second));
                        }
                    }
                } else {
                    items = collect_filled_object_items(self.builder, self.handle);
                }
            }
            return items;
        }, py::arg("recursive") = false)
        .def("__getitem__", [](const PyStreamNode& self, const std::string& key) -> py::object {
            throw py::key_error("FastFHIR Python API requires generated Field objects.");
        })
        .def("__getitem__", [](const PyStreamNode& self, const PythonFieldProxy& field) {
            if (field.registry_index >= FastFHIR::FieldKeys::RegistrySize) throw py::index_error();
            const auto& key = *FastFHIR::FieldKeys::Registry[field.registry_index];
            return PyMutableEntry(self.builder, self.handle[key]);
        })
        .def("__getitem__", [](const PyStreamNode& self, py::object ast_node) {
            // Support both explicit ASTNode objects and field path instances
            if (py::hasattr(ast_node, "path")) {
                return resolve_ast_path(self, ast_node.attr("path").cast<py::tuple>());
            }
            // Fallback: try to treat it as a field path by looking for a __call__ or recovery_tag
            throw py::type_error("FastFHIR: Expected ASTNode with .path attribute or field path accessor.");
        })
        .def("__setitem__", [](PyStreamNode& self, const PythonFieldProxy& field, py::object value) {
            // Direct field assignment (the documented API:
            // patient_node[Patient.ACTIVE] = True). Must be registered BEFORE
            // the generic py::object overload below or pybind11 dispatches
            // every Field object to the ASTNode path and throws.
            if (field.registry_index >= FastFHIR::FieldKeys::RegistrySize) throw py::index_error();
            const auto& key = *FastFHIR::FieldKeys::Registry[field.registry_index];
            PyMutableEntry leaf(self.builder, self.handle[key]);
            assign_py_obj(leaf.entry, value, self.handle, key);
        })
        .def("__setitem__", [](PyStreamNode& self, py::object ast_node, py::object value) {
            if (!py::hasattr(ast_node, "path")) throw py::type_error("Requires ASTNode.");
            py::tuple path = ast_node.attr("path").cast<py::tuple>();
            
            PyMutableEntry leaf = resolve_ast_path(self, path);
            py::handle last_step = path[path.size() - 1];
            
            if (py::isinstance<PythonFieldProxy>(last_step)) {
                auto field = last_step.cast<PythonFieldProxy>();
                const auto& key = *FastFHIR::FieldKeys::Registry[field.registry_index];
                py::tuple p_path = path[py::slice(0, path.size() - 1, 1)];
                Reflective::ObjectHandle p_handle = p_path.empty() ? self.handle : resolve_ast_path(self, p_path).entry.as_handle();
                assign_py_obj(leaf.entry, value, p_handle, key);
            } else {
                Reflective::ObjectHandle dummy = self.handle; 
                FF_FieldKey dummy_k;
                assign_py_obj(leaf.entry, value, dummy, dummy_k);
            }
        });

    py::class_<PyMutableEntry>(m, "MutableEntry")
        .def("offset", [](const PyMutableEntry& s) { return s.entry.offset(); })
        .def("to_json", [](const PyMutableEntry& s) { return render_mutable_entry_json(s); })
        .def("__str__", [](const PyMutableEntry& s) { return render_mutable_entry_json(s); })
        .def("__repr__", [](const PyMutableEntry& s) { return render_mutable_entry_json(s); })
        .def("__bool__", [](const PyMutableEntry& s) { return entry_has_value(s); })
        .def("__eq__", [](const PyMutableEntry& self, py::object other) -> py::object {
            RECOVERY_TAG other_tag = FF_RECOVER_UNDEFINED;
            if (try_extract_recovery_tag(other, other_tag)) {
                return py::bool_(self.effective_recovery() == other_tag);
            }
            return py::reinterpret_borrow<py::object>(Py_NotImplemented);
        })
        .def_property_readonly("is_array", [](const PyMutableEntry& s) { return s.entry.is_array(); })
        .def("__len__", [](const PyMutableEntry& s) -> size_t {
            if (!s.entry.is_array()) return 0;
            return s.entry.size();
        })
        .def("__iter__", [](const PyMutableEntry& self) {
            if (self.entry.is_array()) {
                // For arrays, yield each entry as a PyMutableEntry
                py::list items;
                for (size_t i = 0; i < self.entry.size(); ++i) {
                    items.append(PyMutableEntry(self.builder, self.entry[i]));
                }
                return py::iter(items);
            } else {
                Reflective::ObjectHandle node_handle = self.entry.as_handle();
                return py::iter(collect_filled_object_values(self.builder, node_handle));
            }
        })
        .def("items", [](const PyMutableEntry& self, bool recursive = true) {
            // Dict-like items() for nodes; for arrays, iterate as (index, item) pairs
            py::list items;
            if (self.entry.is_array()) {
                for (size_t i = 0; i < self.entry.size(); ++i) {
                    auto entry = PyMutableEntry(self.builder, self.entry[i]);
                    items.append(py::make_tuple(py::int_(i), materialize_mutable_entry_value(entry, recursive)));
                }
            } else {
                Reflective::ObjectHandle node_handle = self.entry.as_handle();
                if (recursive) {
                    py::object values = materialize_handle_value(self.builder, node_handle);
                    if (py::isinstance<py::dict>(values)) {
                        for (auto item : values.cast<py::dict>()) {
                            items.append(py::make_tuple(item.first, item.second));
                        }
                    }
                } else {
                    items = collect_filled_object_items(self.builder, node_handle);
                }
            }
            return items;
        }, py::arg("recursive") = false)
        .def("__getitem__", [](const PyMutableEntry& self, size_t index) {
            if (index >= self.entry.size()) throw py::index_error();
            return PyMutableEntry(self.builder, self.entry[index]);
        })
        .def("__getitem__", [](const PyMutableEntry& self, const PythonFieldProxy& field) {
            if (field.registry_index >= FastFHIR::FieldKeys::RegistrySize) throw py::index_error();
            const auto& key = *FastFHIR::FieldKeys::Registry[field.registry_index];
            return PyMutableEntry(self.builder, self.entry[key]);
        })
        .def("__getitem__", [](const PyMutableEntry& self, py::object ast) {
            if (!py::hasattr(ast, "path")) throw py::type_error("Requires ASTNode.");
            return resolve_ast_path(PyStreamNode(self.builder, self.entry.as_handle()), ast.attr("path").cast<py::tuple>());
        })
        .def("__setitem__", [](const PyMutableEntry& self, py::object ast, py::object value) {
            if (!py::hasattr(ast, "path")) throw py::type_error("Requires ASTNode.");
            py::tuple path = ast.attr("path").cast<py::tuple>();
            
            PyStreamNode elevated(self.builder, self.entry.as_handle());
            PyMutableEntry leaf = resolve_ast_path(elevated, path);
            py::handle last = path[path.size() - 1];
            
            if (py::isinstance<PythonFieldProxy>(last)) {
                auto f = last.cast<PythonFieldProxy>();
                const auto& key = *FieldKeys::Registry[f.registry_index];
                py::tuple p_path = path[py::slice(0, path.size() - 1, 1)];
                Reflective::ObjectHandle p_handle = p_path.empty() ? elevated.handle : resolve_ast_path(elevated, p_path).entry.as_handle();
                assign_py_obj(leaf.entry, value, p_handle, key);
            } else {
                Reflective::ObjectHandle dummy = elevated.handle; 
                FF_FieldKey dummy_k;
                assign_py_obj(leaf.entry, value, dummy, dummy_k);
            }
        })
        .def("value", [](const PyMutableEntry& self) -> py::object {
            return materialize_mutable_entry_value(self, false);
        });

    // =====================================================================
    // 4. Stream (Builder Wrapper) & Context Manager
    // =====================================================================
    py::class_<PyStream, std::shared_ptr<PyStream>>(m, "Stream")
        .def(py::init<PyMemory&, FHIR_VERSION>(), py::arg("memory"), py::arg("fhir_version") = FHIR_VERSION_R5)
        .def("__enter__", [](PyStream& self) -> PyStream& { return self; })
        .def("__exit__", [](PyStream& self, py::object, py::object, py::object) { self.close(); })
        .def_property("root", 
            [](PyStream& self) { return PyStreamNode(self.m_builder, self.get().root_handle()); }, 
            [](PyStream& self, const PyStreamNode& handle) {
                FF_Result res = FF_StreamSetRoot(FF_StreamSetRootInfo{
                    .stream = self.m_builder,
                    .root = handle.handle,
                });
                if (!res) throw std::runtime_error(res.message);
            }
        )
        .def("query", [](const PyStream& self) { return stream_query(self); })
        .def_property_readonly("version", [](const PyStream& self) { return stream_query(self).version(); })
        .def_property_readonly("root_type", [](const PyStream& self) { return stream_query(self).root_type(); })
        .def_property_readonly("checksum", [](const PyStream& self) { return stream_query(self).checksum(); })
        .def("to_json", [](const PyStream& self) { return render_parser_json(stream_query(self)); })
        .def("__str__", [](const PyStream& self) { return render_parser_json(stream_query(self)); })
        .def("__repr__", [](const PyStream& self) { return render_parser_json(stream_query(self)); })
        .def("finalize", [](PyStream& self, FF_Checksum_Algorithm algo, py::object py_hasher) {
            FF_HashCallback cpp_hasher = nullptr;
            if (!py_hasher.is_none()) {
                cpp_hasher = [py_hasher](const unsigned char* data, Size size) -> std::vector<BYTE> {
                    py::memoryview view = py::memoryview::from_memory(data, size);
                    std::string_view str_view = py_hasher(view).cast<py::bytes>();
                    return std::vector<BYTE>(str_view.begin(), str_view.end());
                };
            }
            Memory::View out;
            FF_Result res = FF_StreamFinalize(FF_StreamFinalizeInfo{
                .stream = self.m_builder,
                .algorithm = algo,
                .hasher = cpp_hasher,
            }, out);
            if (!res) throw std::runtime_error(res.message);
            return out;
        }, py::arg("algo") = FF_CHECKSUM_NONE, py::arg("hasher") = py::none())
        .def("compact", [](PyStream& self, FF_Checksum_Algorithm algo, py::object py_hasher) {
            FF_HashCallback cpp_hasher = nullptr;
            if (!py_hasher.is_none()) {
                cpp_hasher = [py_hasher](const unsigned char* data, Size size) -> std::vector<BYTE> {
                    py::memoryview view = py::memoryview::from_memory(data, size);
                    std::string_view str_view = py_hasher(view).cast<py::bytes>();
                    return std::vector<BYTE>(str_view.begin(), str_view.end());
                };
            }
            Memory::View out;
            FF_Result res = FF_Compact(FF_CompactInfo{
                .source = stream_query(self),
                .algorithm = algo,
                .hasher = cpp_hasher,
            }, out);
            if (!res) throw std::runtime_error(res.message);
            return out;
        }, py::arg("algo") = FF_CHECKSUM_NONE, py::arg("hasher") = py::none())
        .def_property_readonly("has_url_directory", [](const PyStream& self) {
            return stream_query(self).has_url_directory();
        })
        .def_property_readonly("url_directory", [](const PyStream& self) -> py::object {
            Parser q = stream_query(self);
            if (!q.has_url_directory()) return py::none();
            const BYTE* base = q.data();
            FF_URL_DIRECTORY dir = q.url_directory();
            uint32_t n = dir.entry_count(base);
            py::dict d;
            d["entry_count"] = n;
            py::list entries;
            for (uint32_t i = 0; i < n; ++i) {
                py::dict e;
                uint32_t prior = dir.prior_idx(base, i);
                e["prior"] = (prior == FF_URL_DIRECTORY::NO_PRIOR) ? py::object(py::none()) : py::cast(prior);
                e["seg"]   = std::string(dir.seg_string(base, i));
                e["url"]   = dir.get_url(base, i);
                entries.append(e);
            }
            d["entries"] = entries;
            return d;
        })
        .def_property_readonly("has_module_registry", [](const PyStream& self) {
            return stream_query(self).has_module_registry();
        })
        .def_property_readonly("module_registry", [](const PyStream& self) -> py::object {
            Parser q = stream_query(self);
            if (!q.has_module_registry()) return py::none();
            const BYTE* base = q.data();
            FF_MODULE_REGISTRY reg(q.module_registry_offset(), q.size_bytes(), q.version());
            uint32_t n = reg.entry_count(base);
            py::dict d;
            d["entry_count"] = n;
            py::list entries;
            constexpr char hex_digits[] = "0123456789abcdef";
            for (uint32_t i = 0; i < n; ++i) {
                py::dict e;
                e["url_idx"]          = reg.url_idx(base, i);
                e["wasm_blob_offset"] = reg.wasm_blob_offset(base, i);
                e["wasm_blob_size"]   = reg.wasm_blob_size(base, i);
                std::string_view hash = reg.module_hash(base, i);
                std::string hex;
                hex.reserve(hash.size() * 2);
                for (unsigned char c : hash) {
                    hex.push_back(hex_digits[c >> 4]);
                    hex.push_back(hex_digits[c & 0xF]);
                }
                e["module_hash"] = hex;
                entries.append(e);
            }
            d["entries"] = entries;
            return d;
        });

    // =====================================================================
    // 5. Ingestor
    // =====================================================================
    py::class_<FF_Ingestor_t, std::shared_ptr<FF_Ingestor_t>>(m, "Ingestor")
        .def(py::init([](size_t logger_capacity, unsigned int concurrency) {
            FF_IngestorCreateInfo info;
            info.logger_capacity = logger_capacity;
            info.concurrency = static_cast<uint32_t>(concurrency);
            FF_Ingestor ingestor;
            FF_Result res = FF_CreateIngestor(info, ingestor);
            if (!res) throw std::runtime_error(res.message);
            return ingestor;
        }), py::arg("logger_capacity") = 64 * 1024 * 1024, py::arg("concurrency") = 0)
        .def("ingest", [](const FF_Ingestor& self, PyStream& stream, FF_SourceType type, std::string_view payload) {
            FF_IngestInfo info{
                .ingestor = self,
                .stream = stream.m_builder,
                .source_type = type,
                .payload = payload,
            };
            Reflective::ObjectHandle root;
            Size count = 0;
            FF_Result res = FF_Ingest(info, root, count);
            if (res.failed()) throw std::runtime_error(res.message);
            return py::make_tuple(PyStreamNode(stream.m_builder, root), static_cast<size_t>(count));
        }, py::arg("stream"), py::arg("source_type"), py::arg("payload"))
        .def("reset", [](FF_Ingestor_t& self) { return self.impl.reset(); })
        .def_property_readonly("is_faulted", [](FF_Ingestor_t& self) { return self.impl.is_faulted(); });
}
