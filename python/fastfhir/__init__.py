import hashlib, zlib
from typing import Optional, Callable

# 1. Import the compiled C++ extension
from . import _core

# 2. Expose the auto-generated Field schemas to the top level
from .fields import *

# 3. Hoist core classes to the main namespace
Memory          = _core.Memory
MemoryView      = _core.MemoryView
StreamHead      = _core.StreamHead
BuilderNode     = _core.BuilderNode
Ingestor        = _core.Ingestor

# Enums
Checksum        = _core.Checksum
SourceType      = _core.SourceType
FhirVersion     = _core.FhirVersion
ResourceType    = _core.ResourceType

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

# The one place a Checksum maps to its hashlib/zlib implementation. finalize()
# and compact() both seal through it, so adding an algorithm is one edit here,
# not one per caller -- the same single-source rule as the rest of the tree.
_HASHERS: dict = {
    Checksum.SHA256: _default_sha256_hasher,
    Checksum.MD5:    _default_md5_hasher,
    Checksum.CRC32:  _default_crc32_hasher,
}

def _resolve_hasher(algo: Checksum,
                    hasher: Optional[Callable[[memoryview], bytes]]) -> Optional[Callable[[memoryview], bytes]]:
    """The caller's hasher if given, else the standard one for @p algo (None for NONE)."""
    if hasher is not None:
        return hasher
    return _HASHERS.get(algo)

# ---------------------------------------------------------
# Builder Wrapper (Enhances C++ Builder)
# ---------------------------------------------------------
class Builder(_core.Builder):
    """
    Pythonic context manager for FastFHIR stream generation.
    Handles automatic injection of Python hashing algorithms.
    """
    def __init__(self, memory: Memory, version: FhirVersion = FhirVersion.R5):
        super().__init__(memory, version)

    def finalize(self, algo: Checksum = Checksum.NONE, hasher: Optional[Callable[[memoryview], bytes]] = None) -> MemoryView:
        """
        Seals the stream and writes the footer.

        Returns a FastFHIR MemoryView object that exports the Python buffer protocol.
        Use `.size` for byte length, or wrap with `memoryview(...)` if you need native
        Python buffer helpers like `len()`, slicing metadata, or casting.

        Args:
            algo: The cryptographic algorithm to use.
            hasher: A custom callback. If None and algo is SHA256,
                    the standard hashlib.sha256 is used automatically.
        """
        # Call the underlying C++ method
        return super().finalize(algo, _resolve_hasher(algo, hasher))

    def compact(self, algo: Checksum = Checksum.NONE, hasher: Optional[Callable[[memoryview], bytes]] = None) -> MemoryView:
        """
        Compact a finalized stream into a dense-layout archive.

        The compact archive is written into a fresh arena sized from the source;
        the source stream and its backing Memory are not modified.

        Returns a FastFHIR MemoryView object that exports the Python buffer protocol.
        Use `.size` for byte length, or wrap with `memoryview(...)` if you need native
        Python buffer helpers like `len()`, slicing metadata, or casting.

        Args:
            algo: The cryptographic algorithm to use for the checksum seal.
            hasher: A custom callback. If None and algo is SHA256/MD5/CRC32,
                    the standard library implementation is used automatically.
        Returns:
            A zero-copy sealed compact archive view.
        """
        return super().compact(algo, _resolve_hasher(algo, hasher))


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