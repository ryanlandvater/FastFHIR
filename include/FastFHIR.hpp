/**
 * @file FastFHIR.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @version 0.1
 *
 * @brief FastFHIR — Public API
 *
 * This is the only header consumers of the FastFHIR library need to include.
 *
 * Quick start:
 * @code
 *   #include <FastFHIR.hpp>
 *
 *   auto parser = FastFHIR::Parser::create(buffer, size);
 *   auto root   = parser.root();
 *   auto status = root["status"].as_string();
 * @endcode
 */

#pragma once

// Include the generated FHIR field keys for convenient access to common FHIR field names.
#include "FF_FieldKeys.hpp"

// Include the public headers
#include "FF_Parser.hpp"

