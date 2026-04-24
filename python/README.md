# FastFHIR Python API

FastFHIR is a zero-copy clinical data engine. Ingest FHIR JSON or HL7 directly into a memory arena and traverse it with O(1) field access using generated field constants.

---

## 1 — Ingest a FHIR JSON file and save as `.ffhr`

The arena is memory-mapped directly to the destination file. Every byte the ingestor
writes goes straight to the OS page cache — there is no intermediate buffer, no
`write()` call, and no copy at `finalize()`. When `finalize()` returns, `patient.ffhr`
is already a complete, sealed FastFHIR archive on disk.

```py
import fastfhir as ff
from fastfhir.fields import Patient, HumanName

ingestor = ff.Ingestor()

with open("patient.json") as f:
    json_string = f.read()

# Map the arena straight to a file — every write goes directly to disk
mem = ff.Memory.create_from_file("patient.ffhr", capacity=64 * 1024 * 1024)

with ff.Stream(mem, ff.FhirVersion.R5) as stream:
    patient_node, _ = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, json_string)

    # Inspect while still in the stream
    print(patient_node[Patient.ID].value())       # "patient-1"
    print(patient_node[Patient.GENDER].value())   # "male"
    print(patient_node[Patient.ACTIVE].value())   # True

    for name_entry in patient_node[Patient.NAME]:
        name   = name_entry.value()                                   # StreamNode
        family = name[HumanName.FAMILY].value()                    # str
        given  = [g.value() for g in name[HumanName.GIVEN]]       # list[str]
        print(given, family)   # ['Ryan', 'Eric'] Landvater

    # Seal the file — writes the header + SHA-256 footer into the mapped pages
    stream.finalize(algo=ff.Checksum.SHA256)

mem.close()   # patient.ffhr is now a valid, portable FastFHIR archive
```

---

## 2 — Open and read a `.ffhr` file

Mount an existing archive and traverse directly via `stream.root`.
Do not recover context with `to_json()` followed by re-ingest.

```py
import fastfhir as ff
import json
from fastfhir.fields import Patient, HumanName

mem = ff.Memory.create_from_file("patient.ffhr", capacity=64 * 1024 * 1024)

with ff.Stream(mem, ff.FhirVersion.R5) as stream:
    patient_node = stream.root
    if not patient_node:
        raise RuntimeError("stream.root is null; archive root must be set before read.")

    # Scalars coerce directly to Python types
    pid     = patient_node[Patient.ID].value()        # str
    gender  = patient_node[Patient.GENDER].value()    # str
    active  = patient_node[Patient.ACTIVE].value()    # bool
    dob     = patient_node[Patient.BIRTHDATE].value() # str | None
    print(pid, gender, active, dob)

    # Walk structured arrays
    for name_entry in patient_node[Patient.NAME]:
        name   = name_entry.value()
        family = name[HumanName.FAMILY].value()
        given  = [g.value() for g in name[HumanName.GIVEN]]
        print(given, family)

    # Materialize as plain Python dict/list
    patient_dict = json.loads(patient_node.to_json())
    for human_name in patient_dict.get("name", []):
        print(human_name["family"], human_name.get("given", []))

mem.close()
```

---

## 3 — Re-open a `.ffhr` file and enrich it in place

FastFHIR's arena is append-only and memory-mapped. Writing new fields appends the
new bytes to the tail of the arena and amends only the field pointers in the
header — the original record bytes are never touched. The OS page cache flushes
only the dirty pages (the new tail + the updated pointers). The file grows solely
by the delta; no copy of the existing data is ever made.

```py
import fastfhir as ff
from fastfhir.fields import Patient

# Mount the existing archive — it stays mapped to the same file
mem = ff.Memory.create_from_file("patient.ffhr", capacity=64 * 1024 * 1024)

with ff.Stream(mem, ff.FhirVersion.R4) as stream:
    # Intentionally pass R4 to demonstrate existing-archive fallback:
    # builder degrades to the stream header version for in-place enrichment.
    patient_node = stream.root

    # Add or overwrite scalar fields (appends new bytes, amends pointer)
    patient_node[Patient.BIRTHDATE] = "1990-03-21"
    patient_node[Patient.ACTIVE]    = True

    # Append a structured sub-object via the ingestor
    patient_node[Patient.TELECOM] = {
        "system": "phone",
        "value":  "555-0199",
        "use":    "mobile"
    }

    # Re-seal with updated checksum — old data untouched, new tail written
    stream.finalize(algo=ff.Checksum.SHA256)

mem.close()   # patient.ffhr now contains the enriched record
```

---

## 4 — Receive over a socket, enrich, and send back

The OS writes network data directly into the arena — zero copies on ingest.
After enrichment, `finalize()` returns a buffer-protocol view that `sendall()`
reads straight from the same arena pages — zero copies on egress.

```py
import fastfhir as ff
import socket
from fastfhir.fields import Patient

HOST, PORT = "0.0.0.0", 9000

ingestor = ff.Ingestor()
mem      = ff.Memory.create(capacity=256 * 1024 * 1024)   # 256 MB anonymous arena

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.bind((HOST, PORT))
server.listen(1)
conn, _ = server.accept()

with conn:
    # ── Step 1: receive FHIR JSON directly into the arena (zero-copy ingest) ──
    with mem.try_acquire_stream() as head:
        bytes_received = conn.recv_into(head)   # OS DMA → VMA, no Python buffer
        inbound_payload = bytes(memoryview(head)[:bytes_received])
        head.commit(bytes_received)

    # ── Step 2: parse payload into a mutable stream node ──
    # recv_into/commit already wrote into the arena in-place.
    # Keep a copy of your transport payload for ingestion framing.
    raw = inbound_payload.decode()

    with ff.Stream(mem, ff.FhirVersion.R5) as stream:
        patient_node, _ = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, raw)

        # ── Step 3: enrich in place ──
        patient_node[Patient.ACTIVE] = True
        patient_node[Patient.TELECOM] = {
            "system": "phone",
            "value":  "555-0199",
            "use":    "mobile"
        }

        # ── Step 4: seal and send back — buffer reads straight from the arena ──
        stream.root = patient_node
        final_view = stream.finalize(algo=ff.Checksum.CRC32)

    conn.sendall(final_view)   # zero-copy egress

mem.close()
```

---

## 5 — Surgically edit one patient in a 5 GB bundle and reseal

The bundle is memory-mapped — the OS pages only the entries you actually touch.
Finding one patient, appending a lab result, and resealing never loads the other
5 GB into RAM. Only the dirty pages (the new Observation tail + updated pointers)
are ever written back to disk.

```py
import fastfhir as ff
import json
from fastfhir.fields import Bundle, BundleEntry, Patient

RT = ff._core.ResourceType

ingestor = ff.Ingestor()

# Map the entire 5 GB archive — address space is reserved, pages are not loaded
mem = ff.Memory.create_from_file("bundle.ffhr", capacity=8 * 1024 ** 3)  # 8 GB cap

with ff.Stream(mem, ff.FhirVersion.R5) as stream:
    json_str = stream.to_json()
    bundle_node, _ = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, json_str)

    # Walk bundle.entry; the OS faults in only the pages we read
    target_patient = None
    for entry in bundle_node[Bundle.ENTRY]:
        resource = entry[BundleEntry.RESOURCE]
        if not resource:
            continue
        node_val = resource.value()
        if not node_val or node_val.recovery_tag != RT.Patient:
            continue
        if node_val[Patient.ID].value() == "patient-42":
            target_patient = node_val
            break

    if target_patient is None:
        raise LookupError("patient-42 not found")

    # Build the new Observation inline and append it to this patient only.
    # Every other entry in the bundle is untouched — its pages are never faulted in.
    new_obs = {
        "resourceType": "Observation",
        "status": "final",
        "code": {
            "coding": [{"system": "http://loinc.org", "code": "2345-7", "display": "Glucose"}]
        },
        "subject": {"reference": "Patient/patient-42"},
        "valueQuantity": {"value": 94.0, "unit": "mg/dL", "system": "http://unitsofmeasure.org"}
    }
    ingestor.ingest(stream, ff.SourceType.FHIR_JSON, json.dumps(new_obs))

    # Amend the patient record to reference the new observation
    target_patient[Patient.TELECOM] = {"system": "phone", "value": "555-0199"}

    # Reseal — rewrites only the header + checksum pages, nothing else
    stream.root = bundle_node
    stream.finalize(algo=ff.Checksum.SHA256)

mem.close()   # bundle.ffhr updated; 5 GB of untouched entries were never copied
```

---

---
## Reference

### Enums

```py
ff.FhirVersion.R4,  ff.FhirVersion.R5
ff.SourceType.FHIR_JSON,  ff.SourceType.HL7_V2,  ff.SourceType.HL7_V3
ff.Checksum.NONE,  ff.Checksum.CRC32,  ff.Checksum.MD5,  ff.Checksum.SHA256
ff.ResourceType.Patient,  ff.ResourceType.Bundle,  ff.ResourceType.Encounter
ff.ResourceType.Observation,  ff.ResourceType.DiagnosticReport
```

---

### Field registry — `ff.<Type>.<FIELD>`

All FHIR fields are generated path objects. Type names are PascalCase; field names are SCREAMING_CASE. Array fields support integer indexing to build deep paths.

```py
ff.Patient.ID               # patient.id         (scalar)
ff.Patient.ACTIVE           # patient.active      (bool)
ff.Patient.GENDER           # patient.gender      (code → str)
ff.Patient.BIRTHDATE        # patient.birthDate   (str)
ff.Patient.NAME             # patient.name        (array of HumanName)
ff.Patient.NAME[0]          # patient.name[0]
ff.Patient.NAME[0].FAMILY   # patient.name[0].family  (deep path)
ff.Patient.NAME[0].GIVEN    # patient.name[0].given   (array of str)
ff.Patient.TELECOM          # patient.telecom     (array of ContactPoint)
ff.Patient.ADDRESS          # patient.address     (array of Address)

ff.Bundle.ENTRY                         # bundle.entry
ff.Bundle.ENTRY[0].RESOURCE             # bundle.entry[0].resource
ff.BundleEntry.RESOURCE                 # (used when iterating entry nodes)

ff.Observation.STATUS                   # "final" | "preliminary" | …
ff.Observation.CODE                     # CodeableConcept block
ff.Observation.VALUE                    # Quantity block
ff.CodeableConcept.CODING               # array of Coding
ff.Coding.SYSTEM                        # str
ff.Coding.CODE                          # str
ff.Coding.DISPLAY                       # str

ff.HumanName.USE                        # "usual" | "official" | …
ff.HumanName.FAMILY                     # str
ff.HumanName.GIVEN                      # array of str
ff.HumanName.PREFIX                     # array of str

ff.Quantity.VALUE                       # float
ff.Quantity.UNIT                        # str
ff.Quantity.SYSTEM                      # str

ff.Identifier.SYSTEM                    # str
ff.Identifier.VALUE                     # str
```

Available path types (complete list):
`ff.Patient`, `ff.Bundle`, `ff.BundleEntry`, `ff.BundleEntryRequest`, `ff.BundleEntryResponse`,
`ff.Observation`, `ff.ObservationComponent`, `ff.Encounter`, `ff.EncounterParticipant`,
`ff.DiagnosticReport`, `ff.HumanName`, `ff.CodeableConcept`, `ff.Coding`, `ff.Identifier`,
`ff.Reference`, `ff.Quantity`, `ff.ContactPoint`, `ff.Address`, `ff.Period`, `ff.Meta`,
`ff.Annotation`, `ff.Attachment`, `ff.Signature`, `ff.Extension`, `ff.Narrative`,
`ff.PatientContact`, `ff.PatientCommunication`, `ff.PatientLink`

---

### `MutableEntry` — returned by every field subscript

```py
entry = node[ff.Patient.ACTIVE]
```

| Operation | Returns | Notes |
|---|---|---|
| `entry.value()` | `bool` / `int` / `float` / `str` / `StreamNode` / `None` | Scalars coerced; blocks/arrays return a `StreamNode` |
| `bool(entry)` | `bool` | `True` if the field is present and populated |
| `len(entry)` | `int` | Element count for arrays; `0` for non-arrays |
| `for e in entry` | `MutableEntry` | Iterate array elements |
| `entry.items()` | `list[(str, MutableEntry)]` | Present fields as lazy wrappers |
| `entry.items(recursive=True)` | `list[(str, native)]` | Present fields as native Python values (dicts/lists/scalars) |
| `entry == ff.Patient` | `bool` | Resource type check |
| `entry.to_json()` | `str` | JSON text of this field |
| `entry[ff.X.FIELD]` | `MutableEntry` | Subscript into a block entry |

`.value()` return types by field kind:

| Field kind | `.value()` returns |
|---|---|
| `bool` | `bool` |
| `int32 / uint32 / int64 / uint64` | `int` |
| `float64` | `float` |
| `string / code` | `str` |
| `block / resource / array` | `StreamNode` |
| absent or null | `None` |

---

### `StreamNode` — a live proxy into the arena

```py
node = patient_entry.value()   # for block/array entries
```

| Operation | Returns | Notes |
|---|---|---|
| `node[ff.X.FIELD]` | `MutableEntry` | Single field |
| `node[ff.X.FIELD[i].SUBFIELD]` | `MutableEntry` | Deep path traversal |
| `bool(node)` | `bool` | `True` if the node is present |
| `len(node)` | `int` | Array size; `0` for non-arrays |
| `for e in node` | `MutableEntry` | Iterate array elements |
| `node.items()` | `list[(str, MutableEntry)]` | Present fields, lazy wrappers |
| `node.items(recursive=True)` | `list[(str, native)]` | Present fields, fully materialized |
| `node.recovery_tag == ff._core.ResourceType.Patient` | `bool` | Resource type check |
| `node.to_json()` | `str` | JSON text |

---

### `ff.Memory`

```py
mem = ff.Memory.create(capacity=4 * 1024**3)                          # anonymous RAM
mem = ff.Memory.create_from_file("data.ffhr", capacity=4 * 1024**3)   # file-backed
```

| Member | Returns | Notes |
|---|---|---|
| `.capacity` | `int` | Total bytes allocated |
| `.size` | `int` | Bytes currently written |
| `.name` | `str` | Shared memory name; empty for anonymous arenas |
| `.view()` | `MemoryView` | Zero-copy buffer-protocol slice of the arena |
| `.close()` | — | Release the mapping |

---

### `ff.Stream`

```py
with ff.Stream(mem, ff.FhirVersion.R5) as stream:
    ...
    stream.finalize(algo=ff.Checksum.SHA256)
```

| Member | Returns | Notes |
|---|---|---|
| `.root` | `StreamNode` | Root node |
| `.version` | `FhirVersion` | R4 or R5 |
| `.root_type` | `ResourceType` | Resource kind at root |
| `.to_json()` | `str` | Full stream JSON |
| `.finalize(algo, hasher=None)` | `memoryview` | Seal + write checksum footer |

---

### `ff.Ingestor`

```py
ingestor = ff.Ingestor(concurrency=4)
node, count = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, json_string)
# node  → StreamNode at root resource
# count → number of resources written
```

---

### Complete bundle traversal example (for AI use)

```py
import fastfhir as ff
import json
from fastfhir.fields import Bundle, BundleEntry, Patient, HumanName, Observation, CodeableConcept, Coding, Quantity

RT = ff._core.ResourceType

mem      = ff.Memory.create(capacity=512 * 1024 * 1024)
ingestor = ff.Ingestor()

with open("bundle.json") as f:
    bundle_json = f.read()

with ff.Stream(mem, ff.FhirVersion.R5) as stream:
    bundle_node, count = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, bundle_json)

    for bundle_entry in bundle_node[Bundle.ENTRY]:
        resource = bundle_entry[BundleEntry.RESOURCE]
        if not resource:
            continue
        node_val = resource.value()
        if not node_val:
            continue

        # ── Patient ──────────────────────────────────────────────────────
        if node_val.recovery_tag == RT.Patient:
            patient = node_val                                      # StreamNode

            pid     = patient[Patient.ID].value()                  # str
            active  = patient[Patient.ACTIVE].value()              # bool
            gender  = patient[Patient.GENDER].value()              # str
            dob     = patient[Patient.BIRTHDATE].value()           # str

            for name_entry in patient[Patient.NAME]:
                name   = name_entry.value()                        # StreamNode
                use    = name[HumanName.USE].value()               # str
                family = name[HumanName.FAMILY].value()            # str
                given  = [g.value() for g in name[HumanName.GIVEN]]  # list[str]
                print(f"[{pid}] {use}: {' '.join(given)} {family}, {gender}, DOB {dob}")

            # Alternative: get everything as plain Python
            patient_dict = json.loads(patient.to_json())
            # patient_dict["name"] → list of dicts, e.g.
            # [{"use": "usual", "family": "Fay", "given": ["Bailey", "Marie"]}]

        # ── Observation ───────────────────────────────────────────────────
        elif node_val.recovery_tag == RT.Observation:
            obs    = node_val
            status = obs[Observation.STATUS].value()               # str
            code   = obs[Observation.CODE].value()                 # StreamNode or None

            if code:
                for coding_entry in code[CodeableConcept.CODING]:
                    coding  = coding_entry.value()
                    display = coding[Coding.DISPLAY].value()       # str
                    system  = coding[Coding.SYSTEM].value()        # str
                    print(f"  obs [{status}] {display} ({system})")

            value_entry = obs[Observation.VALUE]
            if value_entry:
                qty = value_entry.value()                          # StreamNode
                print(f"  value: {qty[Quantity.VALUE].value()} {qty[Quantity.UNIT].value()}")

        # ── DiagnosticReport ──────────────────────────────────────────────
        elif node_val.recovery_tag == RT.DiagnosticReport:
            dr     = resource.value()
            status = dr[ff.DiagnosticReport.STATUS].value()        # str
            print(f"  report status: {status}")

    stream.finalize(algo=ff.Checksum.SHA256)
```