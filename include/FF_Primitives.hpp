// MARK: - FastFHIR Core Primitives
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <limits>

// Ensure FF_EXPORT is defined (adjust to your actual export macro)
#ifndef FF_EXPORT
#define FF_EXPORT
#endif

// =====================================================================
// CORE TYPES & CONSTANTS
// =====================================================================
typedef uint8_t  BYTE;
typedef uint64_t Offset;
typedef uint64_t Size;

constexpr Offset FF_NULL_OFFSET = 0xFFFFFFFFFFFFFFFF;
constexpr uint32_t FF_CUSTOM_STRING_FLAG = 0x80000000;

// FastFHIR magic bytes: "FFHR" in little-endian
constexpr uint32_t FF_MAGIC_BYTES = 0x52484646;

#define FHIR_VERSION_R4 0x0400
#define FHIR_VERSION_R5 0x0500

// =====================================================================
// RESULT TYPE
// =====================================================================
enum FF_Result_Code : uint32_t {
    FF_SUCCESS = 0,
    FF_FAILURE = 1,
    FF_VALIDATION_FAILURE = 2,
    FF_WARNING = 4
};

struct FF_Result {
    FF_Result_Code code;
    std::string message;
    inline operator bool() const { return code == FF_SUCCESS; }
    inline bool operator==(FF_Result_Code c) const { return code == c; }
    inline bool operator!=(FF_Result_Code c) const { return code != c; }
};

// =====================================================================
// RECOVERY TAG REGISTRY
// =====================================================================
enum RECOVERY : uint16_t {
    RECOVER_FF_FILE_HEADER              = 0x0001,
    RECOVER_FF_STRING                   = 0x0002,
    RECOVER_FF_ARRAY                    = 0x0003,

    // Data Types (0x0100 range)
    RECOVER_FF_CODING                   = 0x0100,
    RECOVER_FF_CODEABLECONCEPT          = 0x0101,
    RECOVER_FF_QUANTITY                  = 0x0102,
    RECOVER_FF_IDENTIFIER               = 0x0103,
    RECOVER_FF_RANGE                    = 0x0104,
    RECOVER_FF_PERIOD                   = 0x0105,
    RECOVER_FF_EXTENSION                = 0x0106,
    RECOVER_FF_REFERENCE                = 0x0107,
    RECOVER_FF_META                     = 0x0108,
    RECOVER_FF_NARRATIVE                = 0x0109,
    RECOVER_FF_ANNOTATION               = 0x010A,
    RECOVER_FF_HUMANNAME                = 0x010B,
    RECOVER_FF_ADDRESS                  = 0x010C,
    RECOVER_FF_CONTACTPOINT             = 0x010D,
    RECOVER_FF_ATTACHMENT               = 0x010E,
    RECOVER_FF_RATIO                    = 0x010F,
    RECOVER_FF_SAMPLEDDATA              = 0x0110,
    RECOVER_FF_DURATION                 = 0x0111,
    RECOVER_FF_TIMING                   = 0x0112,
    RECOVER_FF_DOSAGE                   = 0x0113,

    // Resources (0x0200 range)
    RECOVER_FF_OBSERVATION              = 0x0200,
    RECOVER_FF_PATIENT                  = 0x0201,
    RECOVER_FF_ENCOUNTER                = 0x0202,
    RECOVER_FF_DIAGNOSTICREPORT         = 0x0203,
    RECOVER_FF_CONDITION                = 0x0204,
    RECOVER_FF_PROCEDURE                = 0x0205,
    RECOVER_FF_MEDICATIONREQUEST        = 0x0206,
    RECOVER_FF_IMMUNIZATION             = 0x0207,
    RECOVER_FF_ALLERGYINTOLERANCE       = 0x0208,

    // Sub-elements / BackboneElements (0x0300 range)
    RECOVER_FF_OBSERVATION_REFERENCERANGE   = 0x0300,
    RECOVER_FF_OBSERVATION_COMPONENT        = 0x0301,
    RECOVER_FF_OBSERVATION_TRIGGEREDBY      = 0x0302,
    RECOVER_FF_PATIENT_CONTACT              = 0x0303,
    RECOVER_FF_PATIENT_COMMUNICATION        = 0x0304,
    RECOVER_FF_PATIENT_LINK                 = 0x0305,
};

// =====================================================================
// TYPE SIZE CONSTANTS (Iris-style)
// =====================================================================
enum TYPE_SIZE : uint8_t {
    TYPE_SIZE_UINT8     = 1,
    TYPE_SIZE_UINT16    = 2,
    TYPE_SIZE_UINT24    = 3,
    TYPE_SIZE_UINT32    = 4,
    TYPE_SIZE_INT32     = 4,
    TYPE_SIZE_UINT64    = 8,
    TYPE_SIZE_FLOAT32   = 4,
    TYPE_SIZE_FLOAT64   = 8,
    TYPE_SIZE_OFFSET    = 8,
};

// =====================================================================
// MEMORY ALIGNMENT & UNALIGNED ACCESS
// =====================================================================
inline Offset align_up(Offset offset, uint32_t alignment) {
    return (offset + (alignment - 1)) & ~(alignment - 1);
}

// Safe unaligned memory access (compiles to efficient instructions on x86, prevents traps on ARM/Wasm)
template<typename T>
inline T load_unaligned(const void* ptr) {
    static_assert(std::is_trivially_copyable_v<T>, "load_unaligned requires trivially copyable type");
    T val; std::memcpy(&val, ptr, sizeof(T)); return val;
}
template<typename T>
inline void store_unaligned(void* const ptr, T val) {
    static_assert(std::is_trivially_copyable_v<T>, "store_unaligned requires trivially copyable type");
    std::memcpy(ptr, &val, sizeof(T));
}

// =====================================================================
// ENDIANNESS DISPATCH (Iris-pattern)
// =====================================================================
// Wire format is little-endian. On BE systems, byte-swap on load/store.
constexpr bool ff_little_endian = std::endian::native == std::endian::little;
constexpr bool ff_is_ieee754    = std::numeric_limits<float>::is_iec559;

// --- Little-endian load/store (native on LE systems) ---
inline uint8_t  __FF_LE_LOAD_U8 (const void* p) { return *static_cast<const uint8_t*>(p); }
inline uint16_t __FF_LE_LOAD_U16(const void* p) { return load_unaligned<uint16_t>(p); }
inline uint32_t __FF_LE_LOAD_U32(const void* p) { return load_unaligned<uint32_t>(p); }
inline uint64_t __FF_LE_LOAD_U64(const void* p) { return load_unaligned<uint64_t>(p); }
inline float    __FF_LE_LOAD_F32(const void* p) { return std::bit_cast<float>(__FF_LE_LOAD_U32(p)); }
inline double   __FF_LE_LOAD_F64(const void* p) { return std::bit_cast<double>(__FF_LE_LOAD_U64(p)); }

inline void __FF_LE_STORE_U8 (void* p, uint8_t  v) { *static_cast<uint8_t*>(p) = v; }
inline void __FF_LE_STORE_U16(void* p, uint16_t v) { store_unaligned<uint16_t>(p, v); }
inline void __FF_LE_STORE_U32(void* p, uint32_t v) { store_unaligned<uint32_t>(p, v); }
inline void __FF_LE_STORE_U64(void* p, uint64_t v) { store_unaligned<uint64_t>(p, v); }
inline void __FF_LE_STORE_F32(void* p, float    v) { __FF_LE_STORE_U32(p, std::bit_cast<uint32_t>(v)); }
inline void __FF_LE_STORE_F64(void* p, double   v) { __FF_LE_STORE_U64(p, std::bit_cast<uint64_t>(v)); }

// --- Big-endian byte-swap wrappers ---
#ifdef _MSC_VER
#define __FF_BSWAP16(X) _byteswap_ushort(X)
#define __FF_BSWAP32(X) _byteswap_ulong(X)
#define __FF_BSWAP64(X) _byteswap_uint64(X)
#else
#define __FF_BSWAP16(X) __builtin_bswap16(X)
#define __FF_BSWAP32(X) __builtin_bswap32(X)
#define __FF_BSWAP64(X) __builtin_bswap64(X)
#endif

inline uint16_t __FF_BE_LOAD_U16(const void* p) { return __FF_BSWAP16(load_unaligned<uint16_t>(p)); }
inline uint32_t __FF_BE_LOAD_U32(const void* p) { return __FF_BSWAP32(load_unaligned<uint32_t>(p)); }
inline uint64_t __FF_BE_LOAD_U64(const void* p) { return __FF_BSWAP64(load_unaligned<uint64_t>(p)); }
inline float    __FF_BE_LOAD_F32(const void* p) { return std::bit_cast<float>(__FF_BE_LOAD_U32(p)); }
inline double   __FF_BE_LOAD_F64(const void* p) { return std::bit_cast<double>(__FF_BE_LOAD_U64(p)); }

inline void __FF_BE_STORE_U16(void* p, uint16_t v) { store_unaligned<uint16_t>(p, __FF_BSWAP16(v)); }
inline void __FF_BE_STORE_U32(void* p, uint32_t v) { store_unaligned<uint32_t>(p, __FF_BSWAP32(v)); }
inline void __FF_BE_STORE_U64(void* p, uint64_t v) { store_unaligned<uint64_t>(p, __FF_BSWAP64(v)); }
inline void __FF_BE_STORE_F32(void* p, float    v) { __FF_BE_STORE_U32(p, std::bit_cast<uint32_t>(v)); }
inline void __FF_BE_STORE_F64(void* p, double   v) { __FF_BE_STORE_U64(p, std::bit_cast<uint64_t>(v)); }

// --- Dispatch: compile-time selected inline functions ---
inline uint8_t  LOAD_U8 (const void* p) { return __FF_LE_LOAD_U8(p); }
inline uint16_t LOAD_U16(const void* p) { return ff_little_endian ? __FF_LE_LOAD_U16(p) : __FF_BE_LOAD_U16(p); }
inline uint32_t LOAD_U32(const void* p) { return ff_little_endian ? __FF_LE_LOAD_U32(p) : __FF_BE_LOAD_U32(p); }
inline uint64_t LOAD_U64(const void* p) { return ff_little_endian ? __FF_LE_LOAD_U64(p) : __FF_BE_LOAD_U64(p); }
inline float    LOAD_F32(const void* p) { return ff_little_endian ? __FF_LE_LOAD_F32(p) : __FF_BE_LOAD_F32(p); }
inline double   LOAD_F64(const void* p) { return ff_little_endian ? __FF_LE_LOAD_F64(p) : __FF_BE_LOAD_F64(p); }

inline void STORE_U8 (void* p, uint8_t  v) { __FF_LE_STORE_U8(p, v); }
inline void STORE_U16(void* p, uint16_t v) { ff_little_endian ? __FF_LE_STORE_U16(p, v) : __FF_BE_STORE_U16(p, v); }
inline void STORE_U32(void* p, uint32_t v) { ff_little_endian ? __FF_LE_STORE_U32(p, v) : __FF_BE_STORE_U32(p, v); }
inline void STORE_U64(void* p, uint64_t v) { ff_little_endian ? __FF_LE_STORE_U64(p, v) : __FF_BE_STORE_U64(p, v); }
inline void STORE_F32(void* p, float    v) { ff_little_endian ? __FF_LE_STORE_F32(p, v) : __FF_BE_STORE_F32(p, v); }
inline void STORE_F64(void* p, double   v) { ff_little_endian ? __FF_LE_STORE_F64(p, v) : __FF_BE_STORE_F64(p, v); }

// =====================================================================
// BASE DATA BLOCK
// =====================================================================
struct FF_EXPORT DATA_BLOCK {
    enum vtable_sizes {
        VALIDATION_S    = TYPE_SIZE_UINT64,
        RECOVERY_S      = TYPE_SIZE_UINT16,
    };
    enum vtable_offsets {
        VALIDATION      = 0,
        RECOVERY        = VALIDATION + VALIDATION_S,
        HEADER_SIZE     = RECOVERY + RECOVERY_S,
    };

#ifdef __EMSCRIPTEN__
    Offset      __remote   = FF_NULL_OFFSET;
    void*       __response = nullptr;
#endif

    Offset      __offset   = FF_NULL_OFFSET;
    Size        __size     = 0;
    uint32_t    __version  = 0;

    explicit DATA_BLOCK() = default;
    explicit DATA_BLOCK(Offset off, Size total_size, uint32_t ver) 
        : __offset(off), __size(total_size), __version(ver) {}

    operator bool() const { return __offset != FF_NULL_OFFSET; }

    FF_Result validate_offset(const BYTE* const __base, const char* type_name, uint16_t recovery_tag) const noexcept;
    
#ifdef __EMSCRIPTEN__
    void check_and_fetch_remote(const BYTE* const& __base);
#endif
};

// =====================================================================
// FILE HEADER
// =====================================================================
struct FF_EXPORT FF_FILE_HEADER : DATA_BLOCK {
    static constexpr char type [] = "FF_FILE_HEADER";
    static constexpr enum RECOVERY recovery = RECOVER_FF_FILE_HEADER;
    enum vtable_sizes {
        VALIDATION_S    = TYPE_SIZE_UINT64,
        RECOVERY_S      = TYPE_SIZE_UINT16,
        MAGIC_S         = TYPE_SIZE_UINT32,
        VERSION_S       = TYPE_SIZE_UINT32,
        ROOT_OFFSET_S   = TYPE_SIZE_UINT64,
        ROOT_RECOVERY_S = TYPE_SIZE_UINT16,
        PAYLOAD_SIZE_S  = TYPE_SIZE_UINT64,
    };
    enum vtable_offsets {
        VALIDATION      = 0,
        RECOVERY        = VALIDATION   + VALIDATION_S,
        MAGIC           = RECOVERY     + RECOVERY_S,
        VERSION         = MAGIC        + MAGIC_S,
        ROOT_OFFSET     = VERSION      + VERSION_S,
        ROOT_RECOVERY   = ROOT_OFFSET  + ROOT_OFFSET_S,
        PAYLOAD_SIZE    = ROOT_RECOVERY + ROOT_RECOVERY_S,
        HEADER_SIZE     = PAYLOAD_SIZE + PAYLOAD_SIZE_S,
    };

    explicit FF_FILE_HEADER(Size file_size) noexcept
        : DATA_BLOCK(0, file_size, UINT32_MAX) {}

    FF_Result validate_full(const BYTE* const __base) const noexcept;
    uint32_t  get_version  (const BYTE* const __base) const;
    Offset    get_root     (const BYTE* const __base) const;
    uint16_t  get_root_type(const BYTE* const __base) const;
};

void FF_EXPORT STORE_FF_FILE_HEADER(BYTE* const __base, uint32_t version,
                                     Offset root_offset, uint16_t root_recovery,
                                     Size payload_size);

// =====================================================================
// ARRAY BLOCK
// =====================================================================
struct FF_EXPORT FF_ARRAY : DATA_BLOCK {
    static constexpr char type [] = "FF_ARRAY";
    static constexpr enum RECOVERY recovery = RECOVER_FF_ARRAY;
    enum vtable_sizes {
        VALIDATION_S    = TYPE_SIZE_UINT64,
        RECOVERY_S      = TYPE_SIZE_UINT16,
        ENTRY_STEP_S    = TYPE_SIZE_UINT16,
        ENTRY_COUNT_S   = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION      = 0,
        RECOVERY        = VALIDATION  + VALIDATION_S,
        ENTRY_STEP      = RECOVERY    + RECOVERY_S,
        ENTRY_COUNT     = ENTRY_STEP  + ENTRY_STEP_S,
        HEADER_SIZE     = ENTRY_COUNT + ENTRY_COUNT_S,
    };

    explicit FF_ARRAY(Offset off, Size size, uint32_t ver) : DATA_BLOCK(off, size, ver) {}

    FF_Result validate_full(const BYTE* const __base) const noexcept;
    uint16_t  entry_step (const BYTE* const __base) const;
    uint32_t  entry_count(const BYTE* const __base) const;
    const BYTE* entries  (const BYTE* const __base) const;
};

void FF_EXPORT STORE_FF_ARRAY_HEADER(BYTE* const __base, Offset& write_head,
                                      uint16_t entry_step, uint32_t entry_count);

// =====================================================================
// STRING HANDLING
// =====================================================================
struct FF_EXPORT FF_STRING : DATA_BLOCK {
    static constexpr char type [] = "FF_STRING";
    static constexpr enum RECOVERY recovery = RECOVER_FF_STRING;
    enum vtable_sizes {
        VALIDATION_S    = TYPE_SIZE_UINT64,
        RECOVERY_S      = TYPE_SIZE_UINT16,
        LENGTH_S        = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION      = 0,
        RECOVERY        = VALIDATION + VALIDATION_S,
        LENGTH          = RECOVERY   + RECOVERY_S,
        STRING_DATA     = LENGTH     + LENGTH_S,
        HEADER_SIZE     = STRING_DATA,
    };

    explicit FF_STRING(Offset off, Size size, uint32_t ver) : DATA_BLOCK(off, size, ver) {}
    
    FF_Result   validate_full(const BYTE* const __base) const noexcept;
    std::string read         (const BYTE* const __base) const;
};

// Emitter Signatures
Offset   STORE_FF_STRING (BYTE* const __base, Offset& write_head, const std::string& str);
uint32_t ENCODE_FF_CODE  (BYTE* const __base, Offset block_offset, Offset& write_head,
                           const std::string& code_str, uint32_t version = FHIR_VERSION_R5);