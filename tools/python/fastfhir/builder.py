from . import _core
from contextlib import contextmanager

class Builder:
    def __init__(self):
        self._internal = _core.Builder()

    def append(self, field, value):
        """Appends a scalar or string value to the current context."""
        if not isinstance(field, _core.Field):
            raise TypeError("FastFHIR: Builder.append requires a typed Field token.")
        self._internal.append_by_index(field.index, value)

    def append_from_json(self, field, json_payload: str, ingestor: Ingestor = None):
        """
        Ingests a raw JSON string directly into the current builder context.
        
        Args:
            field: The typed Field token (e.g., PATIENT.CONTACT).
            json_payload: The raw FHIR JSON string.
            ingestor: An optional Ingestor instance to reuse SIMD buffers. 
                      If None, a temporary one is created (slower).
        """
        if not isinstance(field, _core.Field):
            raise TypeError("FastFHIR: Builder.from_json requires a typed Field token.")
        
        # If no ingestor provided, we create a transient one.
        # Note: For high-performance loops, always pass a persistent Ingestor.
        target_ingestor = ingestor if ingestor else Ingestor()
        
        # We assume the C++ Builder has a method to accept a pre-parsed 
        # result or that the Ingestor can write to a specific field.
        # Here we use the Ingestor's internal logic to route to this builder.
        target_ingestor.process_at_field(self, field, json_payload)

    @contextmanager
    def object(self, field):
        """
        Context manager for nested blocks (e.g., Patient.contact).
        Pushes a new object frame onto the C++ builder stack.
        """
        if not isinstance(field, _core.Field):
            raise TypeError("FastFHIR: Builder.object requires a typed Field token.")
        
        self._internal.push_object(field.index)
        try:
            yield self
        finally:
            self._internal.pop_object()

    @contextmanager
    def array(self, field):
        """Context manager for FHIR arrays."""
        if not isinstance(field, _core.Field):
            raise TypeError("FastFHIR: Builder.array requires a typed Field token.")
            
        self._internal.push_array(field.index)
        try:
            yield self
        finally:
            self._internal.pop_array()

    def finalize(self) -> bytes:
        return self._internal.finalize()

    def reset(self):
        self._internal.reset()