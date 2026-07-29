"""No emitted field assignment may bind a std::string_view to a temporary.

The generated POD structs hold `std::string_view` for every string-like field,
pointing into the JSON buffer. Parse and store are two separate passes, so
anything a view points at must outlive the gap between them.

`generator/emit/ingest_mappings.py` emitted this for every `code` field:

    data.code = std::string(c);      // field is std::string_view

The temporary dies at the semicolon. The store pass then read freed memory, and
`"8867-4"` was written to the arena as `'xIG'`. On real Synthea records it
produced non-UTF-8 bytes in exported JSON. Sibling fields were unaffected
because `string`/`uri` assign the view directly (`data.system = s;`) -- only the
`code` branch wrapped it.

Nothing in the generator suite caught this: it was found by a byte-level trace
of a corrupted round-trip. The C++ suite catches it, but only after a full build
and only as a generic value mismatch.

This test checks the property by TYPE rather than by spelling, so it catches any
future field that acquires the same shape -- not just the one that did.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_GENERATED = _REPO_ROOT / "generated_src"


def _string_view_fields() -> dict[str, set[str]]:
    """{StructName: {field names declared std::string_view}} from FF_DataTypes.hpp."""
    hpp = _GENERATED / "FF_DataTypes.hpp"
    out: dict[str, set[str]] = {}
    for struct, body in re.findall(
        r"struct (\w+Data) \{(.*?)\n\};", hpp.read_text(encoding="utf-8"), re.S
    ):
        fields = set(re.findall(r"std::string_view\s+(\w+)\s*[;=]", body))
        fields |= set(re.findall(r"std::vector<std::string_view>\s+(\w+)\s*[;=]", body))
        if fields:
            out[struct] = fields
    return out


def test_no_view_field_is_assigned_a_temporary():
    """A std::string_view field must never be assigned std::string(...) directly.

    `data.x = parse_Enum(std::string(c))` is fine -- the temporary is consumed
    producing an enum value. `data.x = std::string(c)` is not.
    """
    if not _GENERATED.is_dir():
        pytest.skip("generated_src/ not present -- configure with the generator enabled")

    view_fields = _string_view_fields()
    assert view_fields, "no *Data structs with std::string_view fields found"
    all_view_names = {f for fields in view_fields.values() for f in fields}

    # data.<field> = std::string(...)   /   data.<field>.emplace_back(std::string(...))
    direct = re.compile(r"data\.(\w+)\s*=\s*std::string\s*\(")
    pushed = re.compile(r"data\.(\w+)\.emplace_back\s*\(\s*std::string\s*\(")

    offenders: list[str] = []
    for cpp in sorted(_GENERATED.glob("*.cpp")):
        text = cpp.read_text(encoding="utf-8")
        for pattern, how in ((direct, "assigned"), (pushed, "emplace_back")):
            for match in pattern.finditer(text):
                field = match.group(1)
                if field in all_view_names:
                    line = text.count("\n", 0, match.start()) + 1
                    offenders.append(
                        f"{cpp.name}:{line} data.{field} {how} a temporary std::string"
                    )

    assert not offenders, (
        f"{len(offenders)} std::string_view field(s) are bound to a temporary that dies at "
        f"the end of the statement. The store pass runs later and will read freed memory. "
        f"Assign the view directly (`data.x = c;`). First few: {offenders[:5]}"
    )


def test_code_fields_assign_the_view_directly():
    """Positive check: the `code` branch emits `= c`, like string/uri fields do.

    Guards the specific emitter site that regressed
    (generator/emit/ingest_mappings.py, the non-enum `code` branch).
    """
    if not _GENERATED.is_dir():
        pytest.skip("generated_src/ not present")

    mappings = (_GENERATED / "FF_IngestMappings.cpp").read_text(encoding="utf-8")
    # Anchor on the DEFINITION (ends in `{`), not the forward declaration.
    coding = re.search(r"static CodingData Coding_from_json\([^;]*?\)\s*\{.*?\n\}", mappings, re.S)
    assert coding, "Coding_from_json not found in generated ingest mappings"
    body = coding.group(0)

    assert re.search(r'key == "code"\)\s*\{.*?data\.code = c;', body, re.S), (
        "Coding.code no longer assigns the simdjson view directly. If it now "
        "materialises a std::string, CodingData::code is a dangling view."
    )
