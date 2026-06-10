# FastFHIR Generator — emit subpackage.
# C++/C emitters. Each module is one wire-format concern and exposes pure
# functions of the shape `(model_object, *opts) -> str`. No file I/O lives
# here except the shared `write_if_changed` in `header.py`.
