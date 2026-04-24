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
ResourceType = _core.ResourceType

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


def stream_readinto_to_memory(source, memory: Memory) -> int:
    """
    Stream bytes from a file-like source into FastFHIR memory using readinto().

    This helper owns the StreamHead lifecycle so callers cannot accidentally keep
    the stream lock alive beyond the transfer scope.

    Args:
        source: Any object implementing `readinto(buffer) -> int`.
        memory: Target FastFHIR memory arena.

    Returns:
        Total bytes streamed into memory.

    Raises:
        RuntimeError: If capacity is exceeded or source does not support readinto.
    """
    if not hasattr(source, "readinto"):
        raise RuntimeError("source must implement readinto(buffer)")

    total = 0
    with memory.try_acquire_stream() as head:
        while True:
            dst = memoryview(head).cast("B")
            if len(dst) == 0:
                raise RuntimeError("Downloaded payload exceeds arena capacity.")

            n = source.readinto(dst)
            if not n:
                break

            head.commit(n)
            total += n

    return total