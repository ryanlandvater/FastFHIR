"""Unit tests for the typed layout model (generator/model/types.py).

These test the layout MODEL in isolation — no C++ emission, no generated tree.
They are the first tests that catch wire-relevant structure bugs (field order,
shape flags) at the source rather than after a full regeneration.

The headline guarantee: the Field/Block adapters round-trip the exact dict
shape the existing emitters consume, so introducing types changes no behaviour.
"""

from __future__ import annotations

from generator.model.types import Block, Field

# A representative ffc.py layout dict for a string field with an extra,
# not-yet-modelled key to prove `extra` preservation.
_STRING_FIELD = {
    "name": "given",
    "orig_name": "given",
    "fhir_type": "string",
    "cpp_type": "Offset",
    "data_type": "std::string_view",
    "is_array": True,
    "is_choice": False,
    "resolved_path": "HumanName.given",
    "some_future_key": 42,  # unmodelled -> must survive in `extra`
}

_CODE_FIELD = {
    "name": "status",
    "orig_name": "status",
    "fhir_type": "code",
    "cpp_type": "uint32_t",
    "data_type": "std::string",
    "code_enum": {"enum": "NarrativeStatus", "parse": "p", "serialize": "s"},
}


def test_field_roundtrip_is_lossless():
    """from_dict -> to_dict restores the original dict exactly."""
    f = Field.from_dict(_STRING_FIELD)
    assert f.to_dict() == _STRING_FIELD


def test_field_preserves_unmodelled_keys():
    """Unknown layout keys land in `extra` and come back on to_dict()."""
    f = Field.from_dict(_STRING_FIELD)
    assert f.extra == {"some_future_key": 42}
    assert f.to_dict()["some_future_key"] == 42


def test_field_optional_keys_omitted_when_absent():
    """code_enum/resolved_path are emitted only when present (matches `.get`)."""
    f = Field.from_dict(_CODE_FIELD)
    d = f.to_dict()
    assert "resolved_path" not in d        # was absent in source
    assert d["code_enum"]["enum"] == "NarrativeStatus"


def test_field_shape_flags_typed():
    f = Field.from_dict(_STRING_FIELD)
    assert f.is_array is True and f.is_choice is False


def test_block_roundtrip_preserves_layout_order():
    """Field order is wire-significant (sets V-Table offsets) — must be stable."""
    raw = {"layout": [_STRING_FIELD, _CODE_FIELD], "header_size": 46}
    blk = Block.from_dict("HumanName", "FF_HUMANNAME", raw)
    out = blk.to_dict()
    assert [f["name"] for f in out["layout"]] == ["given", "status"]
    assert out["header_size"] == 46        # non-layout block keys preserved
