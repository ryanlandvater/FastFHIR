#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "FF_Builder.hpp"
#include "FF_Ingestor.hpp"

namespace py = pybind11;

static inline void DEFINE_INGESTOR_SUBMODULE(py::module_& base) {
    using namespace FastFHIR;

    auto m = base.def_submodule("ingestor", "High-speed JSON ingestion engine (Powered by simdjson)");

    py::class_<Ingestor>(m, "Ingestor")
        // Initialize the ingestor once in Python to keep the simdjson parser buffers alive
        .def(py::init<>(), "Initializes the Ingestor and pre-allocates the SIMD parsing tape.")
        
        // Process a raw JSON string from memory
        .def("process", [](Ingestor& ingestor, Builder& builder, std::string_view json_payload) {
            // Append the JSON to the builder's memory arena
            auto handle = ingestor.process(builder, json_payload);
            
            // Return the raw byte offset so Python can link it to a Bundle later if needed
            return handle.offset();
        }, py::arg("builder"), py::arg("json_payload"), 
           "Parses a raw JSON string using SIMD instructions and writes it directly to the Builder's zero-copy arena.")
        
        // Process a JSON file directly from disk
        .def("process_file", [](Ingestor& ingestor, Builder& builder, const std::string& file_path) {
            auto handle = ingestor.process_file(builder, file_path);
            return handle.offset();
        }, py::arg("builder"), py::arg("file_path"), 
           "Reads a JSON file from disk and streams it directly into the Builder's zero-copy arena.");
}