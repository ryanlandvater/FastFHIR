import hashlib, zlib
from typing import Optional, Callable

# 1. Import the compiled C++ extension
from . import _core

# 2. Expose the auto-generated Field schemas to the top level
from .fields import *

# 3. Hoist core classes to the main namespace
Memory      = _core.Memory
MemoryView  = _core.MemoryView
StreamHead  = _core.StreamHead
StreamNode  = _core.StreamNode
Ingestor    = _core.Ingestor

# Enums
Checksum    = _core.Checksum
SourceType  = _core.SourceType
FhirVersion = _core.FhirVersion

# ---------------------------------------------------------
# Default Hasher Injections (Zero-Copy)
# ---------------------------------------------------------
def _default_sha256_hasher(view: memoryview) -> bytes:
    return hashlib.sha256(view).digest()

def _default_md5_hasher(view: memoryview) -> bytes:
    return hashlib.md5(view).digest()

def _default_crc32_hasher(view: memoryview) -> bytes:
    # zlib returns an unsigned 32-bit int. Pack to 4 little-endian bytes.
    return zlib.crc32(view).to_bytes(4, byteorder='little')

# ---------------------------------------------------------
# Stream Wrapper (Enhances C++ Builder)
# ---------------------------------------------------------
class Stream(_core.Stream):
    """
    Pythonic context manager for FastFHIR stream generation.
    Handles automatic injection of Python hashing algorithms.
    """
    def __init__(self, memory: Memory, version: FhirVersion = FhirVersion.R5):
        super().__init__(memory, version)

    def finalize(self, algo: Checksum = Checksum.NONE, hasher: Optional[Callable[[memoryview], bytes]] = None) -> memoryview:
        """
        Seals the stream and writes the footer.
        
        Args:
            algo: The cryptographic algorithm to use.
            hasher: A custom callback. If None and algo is SHA256, 
                    the standard hashlib.sha256 is used automatically.
        """
        if hasher is None:
            if algo == Checksum.SHA256:
                hasher = _default_sha256_hasher
            elif algo == Checksum.MD5:
                hasher = _default_md5_hasher
            elif algo == Checksum.CRC32:
                hasher = _default_crc32_hasher
            
        # Call the underlying C++ method
        return super().finalize(algo, hasher)