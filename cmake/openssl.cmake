# Try to find OpenSSL on the system first.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL: system version ${OpenSSL_VERSION}")
    set(FF_OPENSSL_LIBRARY OpenSSL::Crypto)
    set(FF_OPENSSL_INCLUDE_DIR "")
    return()
endif()

# ── Not found — fetch and build via ExternalProject ────────────
message(STATUS "OpenSSL: not found — fetching via ExternalProject")
set(FF_OPENSSL_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/openssl")
set(FF_OPENSSL_LIBRARY "${FF_OPENSSL_INSTALL_DIR}/lib/libcrypto.a")
set(FF_OPENSSL_INCLUDE_DIR "${FF_OPENSSL_INSTALL_DIR}/include")

# Create dirs ahead of time — ExternalProject produces them at build time,
# but CMake validates IMPORTED target paths at configure time.
file(MAKE_DIRECTORY "${FF_OPENSSL_INCLUDE_DIR}" "${FF_OPENSSL_INSTALL_DIR}/lib")

include(ExternalProject)
ExternalProject_Add(openssl_external
    GIT_REPOSITORY  https://github.com/openssl/openssl.git
    GIT_TAG         openssl-3.2.1
    GIT_SHALLOW     ON
    UPDATE_DISCONNECTED ON
    PREFIX          "${CMAKE_BINARY_DIR}/_deps/openssl-src"
    CONFIGURE_COMMAND <SOURCE_DIR>/Configure
        --prefix=${FF_OPENSSL_INSTALL_DIR}
        --libdir=lib
        no-shared
        no-tests
    BUILD_COMMAND     ${CMAKE_MAKE_PROGRAM}
    INSTALL_COMMAND   ${CMAKE_MAKE_PROGRAM} install_sw
    BUILD_BYPRODUCTS  ${FF_OPENSSL_LIBRARY}
)

add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION           "${FF_OPENSSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${FF_OPENSSL_INCLUDE_DIR}"
)
add_dependencies(OpenSSL::Crypto openssl_external)
