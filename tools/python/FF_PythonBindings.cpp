#include <pybind11/pybind11.h>

#include "FF_PythonParser.hpp"
#include "FF_PythonBuilder.hpp"
#include "FF_PythonIngestor.hpp"

PYBIND11_MODULE(fastfhir, m) {
    m.doc() = "FastFHIR Zero-Copy Python Bindings";

    // Initialize Submodules
    DEFINE_PARSER_SUBMODULE(m);
    DEFINE_BUILDER_SUBMODULE(m);
    DEFINE_INGESTOR_SUBMODULE(m);
}