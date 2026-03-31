# tools/python/fastfhir/parser.py
from . import _core

class Node:
    """Wrapper for the C++ PyNode to provide cleaner property access."""
    def __init__(self, raw_node):
        self._raw = raw_node

    def __getitem__(self, key):
        # This will trigger the get_item_strict or get_item_str logic we built
        return Node(self._raw[key])

    @property
    def value(self):
        return self._raw.value

    def __repr__(self):
        if self._raw.is_object():
            return f"<FastFHIR Node: Object ({len(self._raw)} keys)>"
        return f"<FastFHIR Node: {self.value}>"

class Parser:
    def __init__(self, data):
        if isinstance(data, str):
            data = data.encode('utf-8')
        # Initialize the C++ Parser with a buffer
        self._internal = _core.Parser(data)

    def root(self) -> Node:
        return Node(self._internal.root())