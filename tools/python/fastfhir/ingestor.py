from . import _core
from .builder import Builder

class Ingestor:
    def __init__(self):
        # The simdjson parser tape is allocated here
        self._internal = _core.Ingestor()

    def process(self, builder: Builder, json_payload: str) -> int:
        """
        Parses JSON and writes directly to the Builder's arena.
        Returns the offset of the new resource in the arena.
        """
        if not isinstance(builder, Builder):
            raise TypeError("Ingestor.process requires a fastfhir.Builder instance.")
            
        return self._internal.process(builder._internal, json_payload)

    def process_file(self, builder: Builder, file_path: str) -> int:
        """
        Streams a JSON file from disk into the Builder's arena.
        """
        if not isinstance(builder, Builder):
            raise TypeError("Ingestor.process_file requires a fastfhir.Builder instance.")
            
        return self._internal.process_file(builder._internal, file_path)