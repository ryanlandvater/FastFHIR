"""No POCO string field may be a bare view that can be bound to a temporary.

HISTORY, because it is why this gate exists. The generated POD structs used to
hold `std::string_view` for every string-like field, pointing into the JSON
buffer. Parse and store are two separate passes, so anything a view pointed at
had to outlive the gap between them. `generator/emit/ingest_mappings.py` emitted
this for every `code` field:

    data.code = std::string(c);      // field is std::string_view

The temporary died at the semicolon. The store pass then read freed memory, and
`"8867-4"` was written to the arena as `'xIG'`. On real Synthea records it
produced non-UTF-8 bytes in exported JSON. Sibling fields were unaffected
because `string`/`uri` assigned the view directly (`data.system = s;`) -- only
the `code` branch wrapped it. Nothing in the generator suite caught it: it was
found by a byte-level trace of a corrupted round-trip.

WHAT CHANGED. The member type is now `FastFHIR::String`, which borrows or owns
depending on how the assignment is spelled: a `std::string` is COPIED, so
`data.code = std::string(c)` is no longer a dangling view -- it is merely an
allocation. That removes the whole class of bug by construction, so this gate
no longer looks for the old spelling. It asserts the property that makes the bug
impossible instead: no POCO string field is a bare view or a bare std::string.

Checked by TYPE rather than by spelling, so any future field that regresses to
a raw view is caught, not just the one that did.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_GENERATED = _REPO_ROOT / "generated_src"

# A POCO member declaration: leading indent, a type, a name, then `;` or `= ...;`.
_RAW_VIEW_MEMBER = re.compile(
    r"^\s+(?:std::string_view|std::string|"
    r"std::vector<std::string_view>|std::vector<std::string>)\s+(\w+)\s*[;=]",
    re.M,
)
_STRING_MEMBER = re.compile(
    r"^\s+(?:FastFHIR::String|std::vector<FastFHIR::String>|"
    r"FF_Optional<FastFHIR::String>)\s+(\w+)\s*[;=]",
    re.M,
)


def _poco_bodies() -> dict[str, str]:
    """{StructName: struct body} for every generated *Data struct."""
    out: dict[str, str] = {}
    for hpp in sorted(_GENERATED.glob("FF_*.hpp")):
        for struct, body in re.findall(
            r"struct (\w+Data) \{(.*?)\n\};", hpp.read_text(encoding="utf-8"), re.S
        ):
            out[struct] = body
    return out


def test_every_poco_string_field_is_ff_string():
    """No generated string member may be a raw std::string_view or std::string.

    A raw view is what let a temporary dangle; FastFHIR::String copies when it
    is handed anything whose lifetime it cannot vouch for.
    """
    if not _GENERATED.is_dir():
        pytest.skip("generated_src/ not present -- configure with the generator enabled")

    bodies = _poco_bodies()
    assert bodies, "no *Data structs found in generated_src/"

    # P0-2: assert a non-zero floor before asserting the absence of anything.
    total_strings = sum(len(_STRING_MEMBER.findall(b)) for b in bodies.values())
    assert total_strings > 100, (
        f"only {total_strings} FastFHIR::String members found across {len(bodies)} "
        f"structs -- the scan is not seeing the POCOs, so its absence check is vacuous"
    )

    offenders: list[str] = []
    for struct, body in sorted(bodies.items()):
        for field in _RAW_VIEW_MEMBER.findall(body):
            offenders.append(f"{struct}::{field}")

    assert not offenders, (
        f"{len(offenders)} POCO string field(s) are a raw std::string_view or "
        f"std::string rather than FastFHIR::String. A raw view can be bound to a "
        f"temporary that dies before the store pass reads it. First few: "
        f"{offenders[:5]}"
    )


def test_code_fields_assign_the_view_directly():
    """Positive check: the `code` branch emits `= c`, like string/uri fields do.

    Guards the specific emitter site that regressed
    (generator/emit/ingest_mappings.py, the non-enum `code` branch). Still worth
    pinning under FastFHIR::String: assigning the simdjson view borrows it,
    which is correct and free, while `std::string(c)` would silently allocate on
    every ingested code.
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
        "materialises a std::string, every ingested code costs an allocation."
    )
