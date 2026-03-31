#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "FF_Parser.hpp"

namespace py = pybind11;

namespace {
    using namespace FastFHIR;

    // A lightweight wrapper for a registry entry
    struct PyField {
        uint32_t registry_index;
        std::string name;
        std::string owner_name;
    };

    /**
     * @brief Python wrapper for FastFHIR::Node.
     * * Manages a reference to the source Python buffer to prevent the underlying 
     * memory from being garbage collected while this Node (or its children) exists.
     */
    class PyNode {
        Node       m_node;
        py::object m_buffer_ref; // Increments ref-count of the memory provider

    public:
        PyNode(Node node, py::object buffer_ref) 
            : m_node(node), m_buffer_ref(buffer_ref) {}

        // --- Core Status Checks ---
        bool is_empty()  const { return m_node.is_empty(); }
        bool is_array()  const { return m_node.is_array(); }
        bool is_object() const { return m_node.is_object(); }
        bool is_string() const { return m_node.is_string(); }
        bool is_scalar() const { return m_node.is_scalar(); }
        size_t size()    const { return m_node.size(); }

        // --- Metadata ---
        std::vector<std::string_view> keys() const { return m_node.keys(); }

        // --- Navigation: The O(1) Validated Access Path ---
        PyNode get_item_strict(const PyField& field) const {
            if (!m_node.is_object()) throw py::type_error("Node is not an object.");
            
            // 1. Resolve the C++ FieldKey from the registry
            const FF_FieldKey& key = *FieldKeys::Registry[field.registry_index];
            
            // 2. Strict Runtime Validation
            // Compare the Node's actual resource type to the Key's required resource type
            if (m_node.recovery_tag() != key.owner_recovery) {
                throw py::type_error(
                    "FastFHIR Type Mismatch: Cannot access field '" + field.owner_name + "." + field.name + 
                    "' on a Node of type '" + std::string(GetResourceName(m_node.recovery_tag())) + "'"
                );
            }

            // 3. Perform the O(1) jump
            Node child = m_node[key];
            if (child.is_empty()) throw py::key_error(field.name);
            
            return PyNode(child, m_buffer_ref);
        }

        // --- Navigation: The O(1) Registry Fast Path ---
        PyNode get_item_registry(uint32_t registry_index) const {
            if (!m_node.is_object()) throw py::type_error("Node is not an object.");

            // Bounds check against the pre-compiled C++ registry
            if (registry_index >= FieldKeys::RegistrySize) {
                throw py::index_error("Invalid FastFHIR field registry index.");
            }

            // Retrieve the pre-baked FieldKey pointer
            const FF_FieldKey& key = *FieldKeys::Registry[registry_index];

            try {
                // Delegate to validated C++ logic (checks recovery tags & offsets)
                Node child = m_node[key];
                if (child.is_empty()) throw py::key_error(key.name.data());
                return PyNode(child, m_buffer_ref);
            } 
            catch (const std::invalid_argument& e) {
                // Raise schema mismatches as Python TypeErrors
                throw py::type_error(e.what());
            }
        }

        // --- Navigation: Fallback String Lookup ---
        PyNode get_item_str(std::string_view key_name) const {
            if (!m_node.is_object()) throw py::type_error("Node is not an object.");
            
            // Reconstruct a key from string (triggers hashing/lookup)
            Node child = m_node[FF_FieldKey::from_cstr(key_name)];
            if (child.is_empty()) throw py::key_error(std::string(key_name));
            return PyNode(child, m_buffer_ref);
        }

        // --- Navigation: Array Indexing ---
        PyNode get_item_idx(size_t index) const {
            if (!m_node.is_array()) throw py::type_error("Node is not an array.");
            if (index >= m_node.size()) throw py::index_error("Index out of bounds.");
            return PyNode(m_node[index], m_buffer_ref);
        }

        // --- Primitive Accessors ---
        std::string_view as_string() const { return m_node.as<std::string_view>(); }
        bool             as_bool()   const { return m_node.as<bool>(); }
        uint32_t         as_int()    const { return m_node.as<uint32_t>(); }
        double           as_float()  const { return m_node.as<double>(); }

        /**
         * @brief Duck-typed value accessor.
         * Converts scalars to Python primitives or returns the PyNode itself for collections.
         */
        py::object value() const {
            if (m_node.is_empty()) return py::none();
            auto k = m_node.kind();
            switch (k) {
                case FF_FIELD_STRING:
                case FF_FIELD_CODE:    return py::str(as_string());
                case FF_FIELD_BOOL:    return py::bool_(as_bool());
                case FF_FIELD_UINT32:  return py::int_(as_int());
                case FF_FIELD_FLOAT64: return py::float_(as_float());
                default:               return py::cast(*this); 
            }
        }
    };
} // end anonymous namespace

static inline void DEFINE_PARSER_SUBMODULE(py::module_& base) {
    auto m = base.def_submodule("parser", "Zero-copy parsing engine");

    // 1. Bind the Field token type first
py::class_<PyField>(m, "Field")
    .def_readonly("index", &PyField::registry_index)
    .def_readonly("name",  &PyField::name)
    .def_readonly("owner", &PyField::owner_name)
    .def("__repr__", [](const PyField &f) {
        return "<FastFHIR Field: " + f.owner_name + "." + f.name + ">";
    });

// 2. Bind the Node with strict type-checked access
py::class_<PyNode>(m, "Node")
    .def("is_empty",  &PyNode::is_empty)
    .def("is_array",  &PyNode::is_array)
    .def("is_object", &PyNode::is_object)
    .def("is_string", &PyNode::is_string)
    .def("is_scalar", &PyNode::is_scalar)
    .def("__len__",   &PyNode::size)
    .def("keys",      &PyNode::keys)
    // Primary O(1) Path: Strict Resource-to-Field validation
    .def("__getitem__", &PyNode::get_item_strict, 
         "O(1) access using a typed Field token. Validates that the field belongs to this resource type.")
    // Secondary Paths: Fallbacks for dynamic keys or array indexing
    .def("__getitem__", &PyNode::get_item_str, 
         "Dynamic lookup via string key. Slower than Field tokens.")
    .def("__getitem__", &PyNode::get_item_idx, 
         "Standard array indexing for list-type nodes.")
    // Accessors
    .def("as_string", &PyNode::as_string)
    .def("as_bool",   &PyNode::as_bool)
    .def("as_int",    &PyNode::as_int)
    .def("as_float",  &PyNode::as_float)
    .def_property_readonly("value", &PyNode::value)
    // Hidden fast-path for performance-critical loops (bypasses owner check)
    .def("_get_raw", &PyNode::get_item_registry, py::arg("index"));

    py::class_<PyParser>(m, "Parser")
        .def(py::init<py::buffer>())
        .def("version", &PyParser::version)
        .def("root", &PyParser::root);
}