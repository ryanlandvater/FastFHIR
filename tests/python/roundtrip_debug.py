"""Read `ff_roundtrip --debug` output and say WHY a round-trip diff happened.

`print_json` output answers "did the value survive?".  It cannot answer "was it
decoded as the right thing?", and every defect in this format's history has been
the second question: a `dateTime` tagged RECOVER_FF_STRING exports as
`effectiveString`, a scalar array element labelled a block exports as `[]`.  The
text either matches or the field is simply gone, and in both cases a DOM diff
has nothing to point at.

`Node::to_debug_json` wraps every value in its wire metadata.  This module strips
that envelope back to a plain DOM -- so the existing `diff_doms` works unchanged
-- while keeping a path -> metadata map, so each diff can be reported with the
tag, offset and kind the reader actually used.

What the round-trip asserts is that every field in the source FHIR survives.
The debug dump carries more than `print_json` does, and those additions must not
count against it -- but only the additions the ENVELOPE creates.  Dropping
EXTRA_KEY wholesale would also silence fabricated fields, making the debug run a
weaker gate than the plain one; `drop_debug_artifacts` therefore identifies
artifacts from the metadata instead of assuming them.  Verified equal: 86 diffs
in both modes on the first fixture.

Two things this can see that a plain diff cannot:

  * **`_empty`** -- a field present on the wire and dropped on export.  A plain
    diff reports that as an ordinary missing key, indistinguishable from "never
    ingested", and the difference is the whole diagnosis: it points at the reader
    rather than the writer.  That distinction took two sessions to make by hand
    for the scalar-array defect.
  * **A renamed choice** -- a missing `valueInteger` beside an emitted
    `valueUnsignedInt` is one tag naming the wrong FHIR type, not a lost value.
    A plain diff shows a missing key and an extra key and looks like two
    unrelated defects; `_find_siblings` pairs them back up and names the tag.
"""

from __future__ import annotations

from typing import Any

from roundtrip_diff import DiffEntry, DiffKind, _path_join

# Keys to_debug_json adds. `_v` is the value; the rest is metadata.
_META_PREFIX = "_"
_VALUE_KEY = "_v"


def is_debug_node(node: Any) -> bool:
    """True for an object emitted by to_debug_json (vs. plain FHIR JSON)."""
    return isinstance(node, dict) and "_tag" in node and "_off" in node


def strip_debug(node: Any, path: str = "") -> tuple[Any, dict[str, dict[str, Any]]]:
    """Debug DOM -> (plain DOM, {path: metadata}).

    The plain DOM is shaped exactly like `print_json` output, so it can be handed
    straight to `diff_doms`. Metadata is keyed by the same paths `diff_doms`
    reports, which is what lets a diff be annotated without a second traversal.
    """
    meta: dict[str, dict[str, Any]] = {}

    if isinstance(node, list):
        out_list = []
        for i, item in enumerate(node):
            value, sub = strip_debug(item, _path_join(path, i))
            out_list.append(value)
            meta.update(sub)
        return out_list, meta

    if not isinstance(node, dict):
        return node, meta

    if is_debug_node(node):
        meta[path] = {
            k: v for k, v in node.items() if k.startswith(_META_PREFIX) and k != _VALUE_KEY
        }
        if _VALUE_KEY in node:
            value, sub = strip_debug(node[_VALUE_KEY], path)
            meta.update(sub)
            return value, meta
        # A block: metadata keys stripped, real fields kept.
        out_block: dict[str, Any] = {}
        for key, child in node.items():
            if key.startswith(_META_PREFIX):
                continue
            value, sub = strip_debug(child, _path_join(path, key))
            out_block[key] = value
            meta.update(sub)
        return out_block, meta

    # Plain object (only `resourceType`-style leaves reach here).
    out_plain: dict[str, Any] = {}
    for key, child in node.items():
        value, sub = strip_debug(child, _path_join(path, key))
        out_plain[key] = value
        meta.update(sub)
    return out_plain, meta


def drop_debug_artifacts(
    diffs: list[DiffEntry], meta: dict[str, dict[str, Any]]
) -> list[DiffEntry]:
    """Remove the diffs the debug envelope itself creates -- and only those.

    What matters is that every field in the source FHIR survives, so a key the
    debug dump carries and `print_json` does not is no failure. The temptation is
    to drop EXTRA_KEY wholesale, and that would be wrong: it would also silence
    fabricated fields, which are a real defect this gate is supposed to catch,
    and it would make the debug run a WEAKER check than the plain one.

    Only one class is genuinely an artifact: a key present because the dump emits
    `_empty` nodes that `print_json` withholds. Those are identified from the
    metadata rather than assumed, so anything else still fails.
    """
    return [
        d
        for d in diffs
        if not (d.kind is DiffKind.EXTRA_KEY and (meta.get(d.meta_path()) or {}).get("_empty"))
    ]


def _base_name(leaf: str) -> str:
    """`valueInteger` -> `value`. The FHIR field name without its [x] type suffix.

    A choice field's JSON key is the base name plus the ACTIVE variant's type,
    and the variant comes from the runtime tag -- so a wrong tag renames the
    field rather than corrupting its value. Splitting at the first capital
    recovers the base, which is what makes the two spellings comparable.
    """
    for i, ch in enumerate(leaf):
        if ch.isupper():
            return leaf[:i]
    return leaf


def _find_siblings(path: str, meta: dict[str, dict[str, Any]]) -> list[tuple[str, dict[str, Any]]]:
    """Nodes the reader DID emit where `path` was expected.

    A missing `valueInteger` beside an emitted `valueUnsignedInt` is not a lost
    value -- it is one tag naming the wrong FHIR type, which reads as a missing
    key and an extra key and looks like two unrelated defects.
    """
    parent, _, leaf = path.rpartition("/")
    base = _base_name(leaf)
    if not base or base == leaf:
        return []
    hits = []
    for other, m in meta.items():
        o_parent, _, o_leaf = other.rpartition("/")
        if o_parent == parent and o_leaf != leaf and _base_name(o_leaf) == base:
            hits.append((o_leaf, m))
    return hits


def annotate(diffs: list[DiffEntry], meta: dict[str, dict[str, Any]]) -> str:
    """Render diffs with the wire metadata behind each one."""
    if not diffs:
        return ""

    lines: list[str] = []
    for d in diffs:
        m = meta.get(d.meta_path())
        lines.append(f"  [{d.kind.name:<16}] {d.path}")
        if d.expected is not None:
            lines.append(f"      expected: {d.expected!r}")
        if m is None:
            # meta_path(), not path: _find_siblings searches the OUTPUT metadata,
            # which strip_debug keys by output indices. After a dropped Bundle
            # entry the input and output indices diverge, so searching with the
            # input path finds nothing and the wire cause -- the whole point of
            # this branch -- is silently lost. That is the exact case
            # DiffEntry.actual_path exists for, and this call site was missed
            # when it was introduced.
            siblings = _find_siblings(d.meta_path(), meta)
            if siblings:
                for leaf, sm in siblings:
                    lines.append(
                        f"      ** emitted as '{leaf}' instead — "
                        f"tag={sm.get('_tag')} kind={sm.get('_kind')} "
                        f"off={sm.get('_off')} **"
                    )
                    if sm.get("_suffix"):
                        lines.append(
                            f"      choice suffix chosen from that tag: "
                            f"'{sm['_suffix']}' (wanted '{_base_name(d.path.rpartition('/')[2])}"
                            f"{d.path.rpartition('/')[2][len(_base_name(d.path.rpartition('/')[2])):]}')"
                        )
                continue
            # Genuinely nothing here: the field never reached the stream.
            lines.append("      wire: <no node at this path — not in the stream>")
            continue
        lines.append(f"      wire: tag={m.get('_tag')} kind={m.get('_kind')} off={m.get('_off')}")
        if m.get("_schema_tag"):
            lines.append(f"      ** schema/runtime tag disagree: schema={m['_schema_tag']} **")
        if m.get("_empty"):
            lines.append("      ** present on the wire, dropped by is_empty() — reader bug **")
        for key in (
            "_suffix",
            "_elem",
            "_entry_kind",
            "_stride",
            "_count",
            "_cc_fallback",
            "_dt_fallback",
        ):
            if key in m:
                lines.append(f"      {key}={m[key]}")
    return "\n".join(lines)


def tag_census(meta: dict[str, dict[str, Any]]) -> dict[tuple[str, str], int]:
    """{(leaf field name, tag): count} -- the corpus audit in one pass.

    Answers "is anything date-shaped still being stored as a string?" without a
    grep over a 10 MB dump per fixture.
    """
    census: dict[tuple[str, str], int] = {}
    for path, m in meta.items():
        field = path.rsplit("/", 1)[-1]
        if not field or field.isdigit():
            continue
        key = (field, str(m.get("_tag")))
        census[key] = census.get(key, 0) + 1
    return census
