# FastFHIR Trademark & Conformance Policy

The FastFHIR source code is available under the Mozilla Public License, v. 2.0.
That license covers the *code*. It does **not** grant rights to the FastFHIR
name (MPL-2.0 §2.3). This policy governs the name.

## Why this policy exists

FastFHIR is a binary *interchange format* for HL7 FHIR. Its entire value is
that a `.ffhr` stream written by one implementation is readable by every
other. A divergent implementation that ships under the same name — reading or
writing streams that differ from the specification — destroys that guarantee
for everyone. The license permits forks; this policy prevents divergent forks
from being *called FastFHIR*.

## The mark

"FastFHIR", the FastFHIR logo, and confusingly similar names are claimed as
trademarks of Dr. Ryan Erik Landvater / the FastFHIR project.

## What you may do without permission

- Use the name nominatively: "built on FastFHIR", "reads FastFHIR (.ffhr)
  files", "a Rust port of the FastFHIR format" — provided the statement is
  accurate and does not imply endorsement.
- Redistribute unmodified builds of this library under the FastFHIR name.
- Fork the code under the MPL for any purpose — under a **different name**.

## What requires conformance

Any implementation (fork, port, rewrite, or embedded engine) that:

- calls itself "FastFHIR", or
- claims to be "FastFHIR-compatible", "FastFHIR-conformant", or to
  read/write FastFHIR streams as a compatibility promise,

must pass the **official FastFHIR conformance suite** for the format version
it claims. The conformance suite is maintained in this repository and
consists of:

1. the wire-format witness gate (`tests/generator/` — recovery tags,
   dictionary code IDs, vtable layout pinned against the committed golden
   baseline), and
2. the round-trip corpus (ingest → binary → export DOM-parity tests in
   `tests/python/` / `tests/cpp/`).

(The suite is under active development — see `TASKS.md` A4 and B5. Until it
is published as a tagged, versioned artifact, compatibility claims require
written permission from the maintainer.)

Implementations that do not pass the suite (or that extend the wire format
beyond the specification) must not use the name or claim compatibility, and
must not describe their divergent format as FastFHIR.

## Enforcement & contact

Questions, permission requests, and conformance submissions:
ryanlandvater [at] gmail [dot] com.
