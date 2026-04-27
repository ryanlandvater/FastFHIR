# FastFHIR Python Bindings — Compaction Checkpoint
*April 27, 2026. Pick up from here in a new conversation.*

---

## Root Problem Being Solved

`to_json()` on a `MutableEntry` was not reflecting mutations — it returned the original field state (usually `null`/`[]` for empty fields). The goal is to make `to_json()` serialize the mutated/enriched value correctly, the same way `.items()` already does.

---

## Changes Made This Session

### 1. `python/FF_PythonBindings.cpp` — Fixed `render_mutable_entry_json()`

The old implementation read from the original packed-data offset, skipping builder mutations. The new implementation mirrors the working `.items()` logic:

```cpp
static std::string render_mutable_entry_json(const PyMutableEntry& entry_wrapper) {
    const Reflective::Entry entry = entry_wrapper.entry.as_entry();
    if (!entry) {
        return "null";
    }

    const FF_FieldKind kind = entry_wrapper.kind();
    
    // For arrays, iterate through elements and materialize each one
    // (this handles mutations correctly unlike reading from the original offset)
    if (entry_wrapper.entry.is_array()) {
        py::list items;
        size_t sz = entry_wrapper.entry.size();
        for (size_t i = 0; i < sz; ++i) {
            auto elem = PyMutableEntry(entry_wrapper.builder, entry_wrapper.entry[i]);
            py::object mat = materialize_mutable_entry_value(elem, true);
            items.append(mat);
        }
        
        // Serialize array to JSON
        try {
            py::module_ json = py::module_::import("json");
            return py::cast<std::string>(json.attr("dumps")(items));
        } catch (const std::exception&) {
            return "[]";
        }
    }
    
    // For non-array fields, materialize and serialize
    py::object materialized = materialize_mutable_entry_value(entry_wrapper, true);
    try {
        py::module_ json = py::module_::import("json");
        return py::cast<std::string>(json.attr("dumps")(materialized));
    } catch (const std::exception&) {
        return "null";
    }
}
```

The `to_json()` binding itself is now clean:
```cpp
.def("to_json", [](const PyMutableEntry& s) { return render_mutable_entry_json(s); })
```

**Status:** Compiled. **NOT yet verified** — the fix was correct in logic but the `.venv` was being loaded from the stale installed copy instead of the newly built one.

### 2. CMake install prefix changed to `.venv`

```bash
cd build && cmake -DCMAKE_INSTALL_PREFIX="/Users/RyanLandvater/Programming_Projects/FastFHIR/.venv" ..
```

This ensures `cmake --install .` puts `_core.cpython-311-darwin.so` and all Python files into `.venv/lib/python3.11/site-packages/fastfhir/`.

**Status:** ✅ Done. `cmake --install .` now installs to the venv.

### 3. `tools/generator/ffc.py` — `emit_python_fields` / `emit_python_ast` coordination

**Problem discovered:** `emit_python_ast()` was overwriting `__init__.py` with only the AST exports, dropping the field class exports (e.g., `Patient`, `Bundle`, `BundleEntry`, etc.). The test suite imports `from fastfhir.fields import Patient, ...` which was failing with `ImportError`.

**Fix applied:**
- `emit_python_fields()` runs **first** and generates each `<resource>.py`
- `emit_python_ast()` runs **second** and **appends** the `_PATH` class to those same files
- `emit_python_ast()` generates `__init__.py` with the AST path root aliases
- `emit_python_ast()`'s `__init__.py` was updated to also re-export the field classes (e.g., `Patient = PATIENT`, etc.)

**Status:** Code changes made but **not yet rebuilt/tested** — the conversation hit the context limit before re-running cmake.

---

## Current State of `fields/__init__.py` (generated)

Currently installed `__init__.py` at `.venv/lib/python3.11/site-packages/fastfhir/fields/__init__.py` only has 2 lines:
```python
# Auto-generated FastFHIR fields module.
# Import resources lazily to save memory.
```

It is **missing** the exports. This is why `from fastfhir.fields import Patient` fails.

The fix is in `ffc.py`'s `emit_python_ast()` which generates the `__init__.py`. After regenerating the fields (via cmake configure), the `__init__.py` should contain proper exports.

---

## TODO

> ⚠️ The `/build` directory was deleted. Must fully reconfigure from scratch.

1. **Reconfigure cmake — Python install dir MUST point to the `.venv` site-packages:**
   ```bash
   mkdir build && cd build
   cmake -DCMAKE_INSTALL_PYTHON_LIBDIR="/Users/RyanLandvater/Programming_Projects/FastFHIR/.venv/lib/python3.11/site-packages" ..
   ```
   Note: `CMAKE_INSTALL_PREFIX` is **not** the right variable — the Python package uses `CMAKE_INSTALL_PYTHON_LIBDIR` directly as an absolute path (see `CMakeLists.txt` line ~389: `FASTFHIR_INSTALL_PYTHON_LIBDIR` is derived from it). Setting the prefix alone won't move the Python install.
   
   This also triggers the FHIR code generator and regenerates `generated_src/python/fields/__init__.py` with proper exports.

2. **Rebuild and install:**
   ```bash
   cmake --build . && cmake --install .
   ```

3. **Verify `__init__.py` now has proper exports:**
   ```bash
   head -30 /Users/RyanLandvater/Programming_Projects/FastFHIR/.venv/lib/python3.11/site-packages/fastfhir/fields/__init__.py
   ```
   Should see `Patient`, `Bundle`, `BundleEntry`, etc. exported.

4. **Run the test suite:**
   ```bash
   cd /Users/RyanLandvater/Programming_Projects/FastFHIR && source .venv/bin/activate && python3 tests/python/test_readme.py
   ```
   All 11 tests should pass.

5. **Specifically verify `to_json()` fix (test_6 surgical editing):**
   ```python
   node_val[PATIENT.TELECOM] = {'system': 'phone', 'value': '555-0199'}
   after = node_val[PATIENT.TELECOM].to_json()
   assert '555-0199' in after  # Should now be True
   ```

6. **If test_6 uses `.items()` workaround, update it to use `.to_json()` directly** — now that `to_json()` works for mutated fields, the test should verify the end-to-end behavior.

---

## Key Architecture Notes

- `MutableEntry.is_array` → True even if size is 0 (it's a type property, not a count check)
- `MutableEntry.items()` correctly reads from the builder mutations (always worked)
- `MutableEntry.to_json()` was reading from original packed offset (now fixed to use `materialize_mutable_entry_value` like `.items()` does)
- The field classes are named `PATIENT`, `BUNDLE`, `BUNDLE_ENTRY` etc. (all caps) — not `Patient`, `BundleEntry`
- The `__init__.py` must alias them: `Patient = PATIENT`, `BundleEntry = BUNDLE_ENTRY` for backward compat with tests
- CMake install prefix is now `.venv` — `cmake --install .` is the correct deploy step (not `sudo cmake --install .` to `/usr/local`)
