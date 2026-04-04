#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <pybind11/eval.h>
#include <pybind11/stl/filesystem.h>

#include "FF_Memory.hpp"
#include "FF_Builder.hpp"
#include "FF_Parser.hpp"
#include "FF_Ingestor.hpp"
#include "FF_Primitives.hpp"

// The auto-generated Registry
#include "FF_AllTypes.hpp"
#include "FF_FieldKeys.hpp" 

namespace py = pybind11;
using namespace FastFHIR;

// Temporary struct to hold the data coming from Python's fields.py
struct PythonFieldProxy {
    uint32_t registry_index;
    std::string name;
    std::string parent;
};

// =====================================================================
// Type-Safe Python to C++ Assignment Dispatcher
// =====================================================================
void assign_py_obj(MutableEntry& entry, py::handle obj, ObjectHandle& parent_handle, const FF_FieldKey& key) {
    if (py::isinstance<ObjectHandle>(obj)) {
        entry = obj.cast<ObjectHandle>();
    } 
    else if (py::isinstance<py::str>(obj)) {
        entry = obj.cast<std::string_view>();
    } 
    else if (py::isinstance<py::bool_>(obj)) {
        entry.assign_inline<uint8_t>(obj.cast<bool>() ? 1 : 0);
    } 
    else if (py::isinstance<py::int_>(obj)) {
        entry.assign_inline<uint32_t>(obj.cast<uint32_t>());
    } 
    else if (py::isinstance<py::float_>(obj)) {
        entry.assign_inline<double>(obj.cast<double>());
    } 
    else if (py::isinstance<py::list>(obj) || py::isinstance<py::dict>(obj)) {
        auto list = py::isinstance<py::list>(obj) ? obj.cast<py::list>() : py::list();
        
        // Fast-path: Native array of ObjectHandles
        if (!list.empty() && py::isinstance<ObjectHandle>(list[0])) {
            entry = list.cast<std::vector<ObjectHandle>>();
            return;
        }
        
        // Fallback: Serialize raw Python dicts/lists and pipe to the Ingestor
        py::module_ json = py::module_::import("json");
        std::string json_string = py::cast<std::string>(json.attr("dumps")(obj));
        
        Ingest::Ingestor ingestor; 
        FF_Result res = ingestor.insert_at_field(parent_handle, key, json_string);
        if (res.code != FF_SUCCESS) throw std::runtime_error(res.message);
    } 
    else {
        throw py::type_error("FastFHIR: Unsupported Python type for stream assignment.");
    }
}

// =====================================================================
// Deep AST Traversal Helper (Executes entirely in C++)
// =====================================================================
MutableEntry resolve_ast_path(ObjectHandle root, py::tuple path) {
    if (path.empty()) throw py::value_error("FastFHIR: Cannot traverse an empty AST path.");
    
    // Initialize the bridge from the root handle
    MutableEntry current_bridge = [&]() {
        py::handle first = path[0];
        if (py::isinstance<py::int_>(first)) {
            return root[first.cast<size_t>()];
        } else if (py::isinstance<PythonFieldProxy>(first)) {
            auto field = first.cast<PythonFieldProxy>();
            if (field.registry_index >= FastFHIR::FieldKeys::RegistrySize) throw py::index_error();
            return root[*FastFHIR::FieldKeys::Registry[field.registry_index]];
        }
        throw py::type_error("FastFHIR: AST path elements must be Field objects or integers.");
    }();

    // Loop through the remaining path elements natively in C++
    for (size_t i = 1; i < path.size(); ++i) {
        py::handle item = path[i];
        if (py::isinstance<py::int_>(item)) {
            current_bridge = current_bridge[item.cast<size_t>()];
        } else if (py::isinstance<PythonFieldProxy>(item)) {
            auto field = item.cast<PythonFieldProxy>();
            if (field.registry_index >= FastFHIR::FieldKeys::RegistrySize) throw py::index_error();
            current_bridge = current_bridge[*FastFHIR::FieldKeys::Registry[field.registry_index]];
        } else {
            throw py::type_error("FastFHIR: AST path elements must be Field objects or integers.");
        }
    }
    
    return current_bridge;
}

PYBIND11_MODULE(_core, m) {
    m.doc() = "FastFHIR core C++ engine.";


    // =====================================================================
    // 1. Enums
    // =====================================================================
    py::enum_<FF_Checksum_Algorithm>(m, "Checksum")
        .value("NONE", FF_CHECKSUM_NONE)
        .value("CRC32", FF_CHECKSUM_CRC32)
        .value("MD5", FF_CHECKSUM_MD5)
        .value("SHA256", FF_CHECKSUM_SHA256)
        .export_values();

    py::enum_<Ingest::SourceType>(m, "SourceType")
        .value("FHIR_JSON", Ingest::SourceType::FHIR_JSON)
        .value("HL7_V2", Ingest::SourceType::HL7_V2)
        .value("HL7_V3", Ingest::SourceType::HL7_V3)
        .export_values();

    enum class FHIRVersion : uint16_t {
        R4 = FHIR_VERSION_R4,
        R5 = FHIR_VERSION_R5
    };
    py::enum_<FHIRVersion>(m, "FhirVersion")
        .value("R4", FHIRVersion::R4)
        .value("R5", FHIRVersion::R5)
        .export_values();

    // ---------------------------------------------------------
    // The Field class used by fields.py
    // ---------------------------------------------------------
    py::class_<PythonFieldProxy>(m, "Field")
        .def(py::init<uint32_t, std::string, std::string>())
        .def_readonly("registry_index", &PythonFieldProxy::registry_index)
        .def_readonly("name", &PythonFieldProxy::name)
        .def_readonly("parent", &PythonFieldProxy::parent);
    
    // =====================================================================
    // 2. Memory & Zero-Copy Buffers
    // =====================================================================
    
    // Expose Memory::View as a read-only Python memoryview
    py::class_<Memory::View>(m, "MemoryView", py::buffer_protocol())
        .def_buffer([](Memory::View &v) -> py::buffer_info {
            return py::buffer_info(
                const_cast<char*>(v.data()),    // Pointer to buffer
                sizeof(char),                   // Size of one scalar
                py::format_descriptor<char>::format(), // Python struct-style format
                1,                              // Number of dimensions
                { v.size() },                   // Buffer dimensions
                { sizeof(char) },               // Strides
                true                            // Read-only
            );
        })
        .def_property_readonly("size", &Memory::View::size)
        .def_property_readonly("empty", &Memory::View::empty);

    // Expose Memory::StreamHead as a writable Python memoryview
    py::class_<Memory::StreamHead>(m, "StreamHead", py::buffer_protocol())
        .def_buffer([](Memory::StreamHead &s) -> py::buffer_info {
            return py::buffer_info(
                s.write_ptr(),                  // Pointer to active write head
                sizeof(uint8_t),                // Size of one scalar
                py::format_descriptor<uint8_t>::format(), 
                1,                              // Number of dimensions
                { s.available_space() },        // Available contiguous space
                { sizeof(uint8_t) },            // Strides
                false                           // Writable
            );
        })
        .def("commit", &Memory::StreamHead::commit, py::arg("bytes_written"), 
             "Publishes written data and advances the atomic write head.")
        .def_property_readonly("available_space", &Memory::StreamHead::available_space)
        .def("__enter__", [](Memory::StreamHead& self) -> Memory::StreamHead& { return self; })
        .def("__exit__", [](Memory::StreamHead& self, py::object, py::object, py::object) {
            // Automatically handled by C++ RAII destructor
        });

    // Memory API
    py::class_<Memory>(m, "Memory")
        .def(py::init<>())
        .def_static("create", &Memory::create, 
                    py::arg("shm_name") = "", 
                    py::arg("capacity") = 4ULL * 1024 * 1024 * 1024)
        .def_static("create_from_file", &Memory::createFromFile, 
                    py::arg("filepath"), 
                    py::arg("capacity") = 4ULL * 1024 * 1024 * 1024)
        .def("try_acquire_stream", [](Memory& mem) {
            auto sh = mem.try_acquire_stream();
            if (!sh) throw std::runtime_error("Stream lock currently held by another socket/thread.");
            return std::move(*sh);
        }, "Acquires the exclusive network ingestion lock.")
        .def_property_readonly("capacity", &Memory::capacity)
        .def_property_readonly("name", &Memory::name)
        .def_property_readonly("size", &Memory::size)
        .def("view", &Memory::view);

    // =====================================================================
    // 3. Object Proxies (Node / ObjectHandle / MutableEntry)
    // =====================================================================
    
    // Unified StreamNode wrapping ObjectHandle
    py::class_<ObjectHandle>(m, "StreamNode")
        .def_property_readonly("offset", &ObjectHandle::offset)
        .def_property_readonly("recovery_tag", &ObjectHandle::recovery)
        .def("is_array", &ObjectHandle::is_array)
        .def("__bool__", [](const ObjectHandle& self) { 
            return self.offset() != FF_NULL_OFFSET; 
        })
        .def("__len__", [](const ObjectHandle& self) -> size_t {
            if (!self.is_array()) return 0;
            return self.as_node().size();
        })
        
        // Enforce the use of Field Enums rather than strings for O(1) safety
        .def("__getitem__", [](const ObjectHandle& self, const std::string& key) -> py::object {
            throw py::key_error("FastFHIR Python API requires generated Field objects (e.g., PATIENT.ACTIVE) for dictionary access, not strings.");
        })
        
        // -----------------------------------------------------------------
        // READ PATH: Deep AST Traversal
        // -----------------------------------------------------------------
        .def("__getitem__", [](const ObjectHandle& self, py::object ast_node) -> py::object {
            if (!py::hasattr(ast_node, "path")) {
                throw py::type_error("FastFHIR: Bracket notation strictly requires an ASTNode (e.g., bundle[Bundle.ENTRY[0]]).");
            }
            py::tuple path = ast_node.attr("path").cast<py::tuple>();
            return py::cast(resolve_ast_path(self, path));
        })
        
        // -----------------------------------------------------------------
        // WRITE PATH: Deep AST Assignment
        // -----------------------------------------------------------------
        .def("__setitem__", [](ObjectHandle& self, py::object ast_node, py::object value) {
            if (!py::hasattr(ast_node, "path")) {
                throw py::type_error("FastFHIR: Bracket notation strictly requires an ASTNode.");
            }
            py::tuple path = ast_node.attr("path").cast<py::tuple>();
            
            MutableEntry leaf_entry = resolve_ast_path(self, path);
            py::handle last_step = path[path.size() - 1];
            
            if (py::isinstance<PythonFieldProxy>(last_step)) {
                auto field = last_step.cast<PythonFieldProxy>();
                const auto& key = *FastFHIR::FieldKeys::Registry[field.registry_index];
                
                py::tuple parent_path = path[py::slice(0, path.size() - 1, 1)];
                ObjectHandle parent_handle = parent_path.empty() ? self : resolve_ast_path(self, parent_path).as_handle();
                
                assign_py_obj(leaf_entry, value, parent_handle, key);
            } 
            else {
                // If appending to an array index, there is no JSON dictionary fallback
                ObjectHandle dummy_parent = self; 
                FF_FieldKey dummy_key;
                assign_py_obj(leaf_entry, value, dummy_parent, dummy_key);
            }
        });

    // Mirror Class for MutableEntry to allow deep chaining
    py::class_<MutableEntry>(m, "MutableEntry")
        .def("offset", &MutableEntry::offset)
        .def("__bool__", [](const MutableEntry& self) { 
            return !self.as_node().is_empty(); 
        })
        .def_property_readonly("is_array", [](const MutableEntry& self) {
            // Evaluates the raw node kind without elevating
            return self.as_node().kind() == FF_FIELD_ARRAY; 
        })
        .def("__len__", [](const MutableEntry& self) -> size_t {
            if (self.as_node().kind() != FF_FIELD_ARRAY) return 0;
            return self.as_node().size(); // NOTE: Adjust to your exact C++ array length method
        })
        
        // Read Chaining
        .def("__getitem__", [](const MutableEntry& self, const PythonFieldProxy& field) -> py::object {
            if (field.registry_index >= FastFHIR::FieldKeys::RegistrySize) throw py::index_error();
            const auto& key = *FastFHIR::FieldKeys::Registry[field.registry_index];
            return py::cast(self[key]);
        })
        // Read Chaining via AST
        .def("__getitem__", [](const MutableEntry& self, py::object ast_node) -> py::object {
            if (!py::hasattr(ast_node, "path")) throw py::type_error("FastFHIR: Bracket notation strictly requires an ASTNode.");
            py::tuple path = ast_node.attr("path").cast<py::tuple>();
            return py::cast(resolve_ast_path(self.as_handle(), path));
        })
        .def("__getitem__", [](const MutableEntry& self, size_t index) -> py::object {
            ObjectHandle elevated = self.as_handle();
            
            // Bounds check is REQUIRED for Python iterators to terminate
            if (index >= elevated.as_node().size()) {
                throw py::index_error("FastFHIR: Array index out of bounds.");
            }
            return py::cast(self[index]);
        })
        
        // Write Chaining via AST
        .def("__setitem__", [](const MutableEntry& self, py::object ast_node, py::object value) {
            if (!py::hasattr(ast_node, "path")) throw py::type_error("FastFHIR: Bracket notation strictly requires an ASTNode.");
            py::tuple path = ast_node.attr("path").cast<py::tuple>();
            
            ObjectHandle self_elevated = self.as_handle();
            MutableEntry leaf_entry = resolve_ast_path(self_elevated, path);
            py::handle last_step = path[path.size() - 1];
            
            if (py::isinstance<PythonFieldProxy>(last_step)) {
                auto field = last_step.cast<PythonFieldProxy>();
                const auto& key = *FieldKeys::Registry[field.registry_index];
                
                py::tuple parent_path = path[py::slice(0, path.size() - 1, 1)];
                ObjectHandle parent_handle = parent_path.empty() ? self_elevated : resolve_ast_path(self_elevated, parent_path).as_handle();
                
                assign_py_obj(leaf_entry, value, parent_handle, key);
            } else {
                ObjectHandle dummy_parent = self_elevated; 
                FF_FieldKey dummy_key;
                assign_py_obj(leaf_entry, value, dummy_parent, dummy_key);
            }
        })
        
        // Final evaluation to extract native types from the bridge
        .def("value", [](const MutableEntry& self) -> py::object {
            Node child = self.as_node();
            if (child.is_empty()) return py::none();

            switch (child.kind()) {
                case FF_FIELD_BOOL:     return py::bool_(child.as<bool>());
                case FF_FIELD_UINT32:   return py::int_(child.as<uint32_t>());
                case FF_FIELD_FLOAT64:  return py::float_(child.as<double>());
                case FF_FIELD_STRING:   return py::str(child.as<std::string_view>());
                case FF_FIELD_BLOCK:   
                case FF_FIELD_ARRAY:   
                case FF_FIELD_CODE: {
                    ObjectHandle elevated = self.as_handle();
                    if (elevated.offset() == FF_NULL_OFFSET) return py::none();
                    return py::cast(elevated);
                }
                default: return py::none();
            }
        });

    // =====================================================================
    // 4. Stream (Builder Wrapper) & Context Manager
    // =====================================================================
    py::class_<Builder>(m, "Stream")
        .def(py::init([](const Memory& memory, FHIRVersion fhir_version) {
            return std::make_unique<Builder>(memory, static_cast<FHIR_VERSION>(fhir_version));
        }), 
        py::arg("memory"), 
        py::arg("fhir_version") = FHIRVersion::R5)
        .def("__enter__", [](Builder& b) -> Builder& { return b; })
        .def("__exit__", [](Builder& b, py::object exc_type, py::object exc_val, py::object exc_tb) {})
        .def("set_root", &Builder::set_root)
        
        // Finalize now accepts a Python callable
        .def("finalize", [](Builder& b, FF_Checksum_Algorithm algo, py::object py_hasher) {
            Builder::HashCallback cpp_hasher = nullptr;
            
            if (!py_hasher.is_none()) {
                // Wrap the Python hash function in a C++ lambda
                cpp_hasher = [py_hasher](const unsigned char* data, Size size) -> std::vector<BYTE> {
                    // 1. Create a zero-copy memoryview of the VMA for Python
                    py::memoryview view = py::memoryview::from_memory(data, size);
                    
                    // 2. Execute the Python callback (e.g., hashlib.sha256)
                    py::bytes result = py_hasher(view);
                    
                    // 3. Convert the resulting bytes back to C++
                    std::string_view str_view = result;
                    return std::vector<BYTE>(str_view.begin(), str_view.end());
                };
            }
            return b.finalize(algo, cpp_hasher); 
        });
    
    // =====================================================================
    // 5. Ingestor
    // =====================================================================
    py::class_<Ingest::Ingestor>(m, "Ingestor")
        .def(py::init<size_t, unsigned int>(), 
             py::arg("logger_byte_capacity") = 64 * 1024 * 1024, 
             py::arg("concurrency") = 0)
             
        .def("ingest", [](Ingest::Ingestor& self, Builder& builder, Ingest::SourceType type, std::string_view payload) {
            ObjectHandle root_handle(&builder, FF_NULL_OFFSET);
            size_t parsed_count = 0;
            
            Ingest::IngestRequest req{builder, type, payload};
            FF_Result res = self.ingest(req, root_handle, parsed_count);
            
            if (res.code != FF_SUCCESS) {
                throw std::runtime_error(res.message);
            }
            
            // Returns a tuple of (StreamNode, parsed_count)
            return py::make_tuple(root_handle, parsed_count);
        }, py::arg("stream"), py::arg("source_type"), py::arg("payload"))
        
        .def("reset", &Ingest::Ingestor::reset, "Clears faults and returns execution logs.")
        .def_property_readonly("is_faulted", &Ingest::Ingestor::is_faulted);
}