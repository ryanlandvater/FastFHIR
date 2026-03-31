# tools/python/fastfhir/__init__.py

__version__ = "0.1.0"

# 1. Attempt to import the compiled C++ extension
try:
    from . import _core
except ImportError as e:
    raise ImportError(
        "FastFHIR C++ extension (_core) not found. "
        "Ensure you have built the project using CMake."
    ) from e

# 2. Expose the generated fields registry
# Note: fields.py imports _core internally, so it must stay below the _core import.
from . import fields
from .parser import Parser, Node
from .builder import Builder

__all__ = ["Parser", "Node", "Builder", "fields", "_core"]

# Optional: Load Ingestor if simdjson was enabled in the build
try:
    from .ingestor import Ingestor
    __all__.append("Ingestor")
except ImportError:
    pass