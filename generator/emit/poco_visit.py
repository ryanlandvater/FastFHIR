"""Emit `visit_fields()` -- reflection over a POCO, independent of any wire.

Why this exists
---------------
`node.as<PatientData>()` hands a caller a struct, and there has been NO way to
enumerate what it contains. The wire side has had reflection for a long time
(`reflected_fields_view`, `FF_FieldInfo`), but that walks BYTES: it needs an
arena and an offset. Anything holding only the struct -- a consumer that
deserialized once and moved on, a differ, a validator, a benchmark comparing two
encodings -- had to fall back on hand-written per-type code.

That gap has a cost beyond inconvenience. In the benchmark repo the only POCO
walkers in existence were the four ARMS' OWN ENCODERS, so "the same document"
could only ever be expressed as one encoder's output, and every comparison was
measured against a competitor rather than against the data. A neutral instrument
is impossible without reflection that belongs to neither side.

What is emitted
---------------
For every generated Data struct, next to the struct itself:

    template <typename F> void visit_fields(const PatientData &d, F &&f);

`f` is called once per field as `f(name, value)`, where `name` is the FHIR
element name -- `multipleBirth`, not the C++-sanitised `multiplebirth` -- so a
caller can rebuild real element paths. Type dispatch stays with the CALLER: a
generic lambda sees the true static type of each member (std::string_view, a
std::vector<T>, a std::unique_ptr<T>, a ChoiceEntry, an enum) and decides what a
leaf is. The generator's job is enumeration, not policy, exactly as
`reflected_fields_view` enumerates and leaves interpretation to the reader.
"""

from __future__ import annotations


def _one(layout: list[dict], data_name: str, *, const: bool) -> str:
    qualifier = "const " if const else ""
    cpp = (
        f"template <typename F>\n"
        f"inline void visit_fields({qualifier}{data_name} &d, F &&f) {{\n"
    )
    if not layout:
        # An empty struct still needs the overload: a caller that recurses
        # generically must not fail to compile on the one type with no fields.
        cpp += "    (void)d;\n    (void)f;\n"
    for field in layout:
        # orig_name, never cpp_name: the C++ member is lower-cased and may carry
        # a keyword-avoiding suffix, and a path built from it is not a FHIR path.
        cpp += f'    f("{field["orig_name"]}", d.{field["cpp_name"]});\n'
    cpp += "}\n\n"
    return cpp


def generate_poco_visitor(layout: list[dict], data_name: str) -> str:
    """Return BOTH visit_fields overloads for one Data struct.

    The MUTABLE overload is what makes an inverse converter possible without
    hand-writing one per format. A decoder that can address a field by its FHIR
    name can populate a POCO generically, so each arm inverts its own encoder
    instead of reconstructing FHIR JSON and borrowing somebody else's parser.

    Borrowing one would put a single reader in the path of every arm's score,
    and anything that reader dropped would vanish from all four alike --
    masking exactly the differences a format comparison exists to surface.
    """
    return _one(layout, data_name, const=True) + _one(layout, data_name, const=False)
