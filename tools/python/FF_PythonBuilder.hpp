#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "FF_Builder.hpp"

namespace py = pybind11;

static inline void DEFINE_BUILDER_SUBMODULE(py::module_& base) {
    using namespace FastFHIR;

    auto m = base.def_submodule("builder", "Lock-free generation engine");

    py::class_<Builder>(m, "Builder")
        .def(py::init<>())
        // Finalize seals the stream and returns a safely-copied Python bytes object
        .def("finalize", [](Builder& b) {
            // Seals the builder and returns the contiguous memory view
            auto payload = b.finalize(FF_CHECKSUM_NONE, nullptr); 
            
            // Cast the C++ string_view into a standalone Python bytes object
            // This guarantees memory safety by copying the arena before the Builder is destroyed
            return py::bytes(reinterpret_cast<const char*>(payload.data()), payload.size());
        }, "Seals the lock-free arena and returns the binary stream as a Python bytes object.");
}