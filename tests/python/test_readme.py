"""
test_readme.py  —  Validates all five code examples from python/README.md.

Run from the test/ directory with the fastfhir venv active:
    python test_readme.py

Notes:
    This script uses direct in-place stream access for mutations and verification.
"""

import gc
import http.client
import http.server
import json
import os
import sys
import tempfile
import threading
import traceback
import glob

HERE = os.path.dirname(os.path.abspath(__file__))

try:
    import fastfhir as ff
except ModuleNotFoundError as exc:
    raise RuntimeError(
        "fastfhir is not importable in this environment. Ensure FastFHIR is installed first."
    ) from exc

# In the installed build ff.Patient / ff.Bundle are ResourceType enum values
# used for "resource == ff.Patient" comparisons; the proper working pattern for
# comparing resource types is:  resource.value().recovery_tag == RT.Patient
# where RT = ff._core.ResourceType.
RT = ff._core.ResourceType   # ResourceType enum (Patient, Bundle, Observation, ...)

# The field-path singletons that carry .ID / .NAME / etc. live in fastfhir.fields.
from fastfhir.fields import Patient, HumanName, ContactPoint, Bundle, BundleEntry, Observation, ObservationComponent

# ── fixtures ─────────────────────────────────────────────────────────────────
PATIENT_JSON_CANDIDATES = [
    os.path.join(HERE, "patient.json"),
    os.path.normpath(os.path.join(HERE, "..", "..", "..", "FastFHIR_Python", "test", "patient.json")),
]
PATIENT_JSON = next((p for p in PATIENT_JSON_CANDIDATES if os.path.exists(p)), None)
PATIENT_JSON_GENERATED = None
if PATIENT_JSON is None:
    PATIENT_JSON_GENERATED = os.path.join(tempfile.gettempdir(), "fastfhir_patient.generated.json")
    with open(PATIENT_JSON_GENERATED, "w", encoding="utf-8") as f:
        f.write(json.dumps({
            "resourceType": "Patient",
            "id": "patient-1",
            "active": True,
            "gender": "male",
            "name": [{"use": "usual", "family": "Landvater", "given": ["Ryan", "Eric"]}],
            "telecom": [],
            "address": [{
                "use": "home",
                "line": ["123 Main St"],
                "city": "Springfield",
                "state": "IL",
                "postalCode": "62701",
                "country": "US"
            }],
            "identifier": [{"system": "http://example.org/fhir/ids", "value": "12345"}],
        }))
    PATIENT_JSON = PATIENT_JSON_GENERATED
_FF_ARTIFACTS_DIR = os.path.join(tempfile.gettempdir(), "fastfhir_test_artifacts")
os.makedirs(_FF_ARTIFACTS_DIR, exist_ok=True)
PATIENT_FFHR              = os.path.join(_FF_ARTIFACTS_DIR, "patient.ffhr")
BUNDLE_FFHR               = os.path.join(_FF_ARTIFACTS_DIR, "bundle.ffhr")
PATIENT_COMPACT_FFHR      = os.path.join(_FF_ARTIFACTS_DIR, "patient.compact.ffhr")
BUNDLE_COMPLEX_FFHR       = os.path.join(_FF_ARTIFACTS_DIR, "bundle.complex.ffhr")
BUNDLE_COMPLEX_COMPACT_FFHR = os.path.join(_FF_ARTIFACTS_DIR, "bundle.complex.compact.ffhr")
ARTIFACT_GLOBS = [
    os.path.join(_FF_ARTIFACTS_DIR, "*.ffhr"),
]

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

results = {}

def cleanup_artifacts():
    for pattern in ARTIFACT_GLOBS:
        for path in glob.glob(pattern):
            if os.path.isfile(path):
                try:
                    os.remove(path)
                except OSError:
                    pass


def cleanup_generated_patient_fixture():
    if PATIENT_JSON_GENERATED and os.path.isfile(PATIENT_JSON_GENERATED):
        try:
            os.remove(PATIENT_JSON_GENERATED)
        except OSError:
            pass

def run(name, fn):
    print(f"\n{'='*60}")
    print(f"  {name}")
    print(f"{'='*60}")
    try:
        fn()
        results[name] = True
        print(f"\n  → {PASS}")
    except Exception:
        results[name] = False
        traceback.print_exc()
        print(f"\n  → {FAIL}")
    finally:
        gc.collect()

# Inline fixture used by the Getting Started test and the bundle tests.
GETTING_STARTED_JSON = json.dumps({
    "resourceType": "Patient",
    "id": "patient-1",
    "active": False,
    "gender": "male",
    "name": [{"family": "Smith", "given": ["John"]}]
})

BUNDLE_JSON = json.dumps({
    "resourceType": "Bundle",
    "id": "test-bundle",
    "type": "collection",
    "entry": [
        {"fullUrl": "Patient/patient-1", "resource": {
            "resourceType": "Patient", "id": "patient-1", "active": True,
            "name": [{"use": "usual", "family": "Landvater", "given": ["Ryan", "Eric"]}],
            "gender": "male"}},
        {"fullUrl": "Patient/patient-2", "resource": {
            "resourceType": "Patient", "id": "patient-2", "active": False,
            "name": [{"use": "usual", "family": "Smith", "given": ["John"]}],
            "gender": "female"}}
    ]
})

BUNDLE_COMPLEX_JSON = json.dumps({
    "resourceType": "Bundle",
    "id": "complex-bundle",
    "type": "collection",
    "entry": [
        {"fullUrl": "Patient/patient-1", "resource": {
            "resourceType": "Patient", "id": "patient-1", "active": True,
            "name": [{"use": "usual", "family": "Landvater", "given": ["Ryan", "Eric"]}],
            "gender": "male"}},
        {"fullUrl": "Observation/obs-1", "resource": {
            "resourceType": "Observation", "id": "obs-1", "status": "final",
            "code": {"coding": [{"system": "http://loinc.org", "code": "8867-4", "display": "Heart rate"}]},
            "subject": {"reference": "Patient/patient-1"},
            "valueString": "final-value",
            "component": [{
                "code": {"coding": [{"system": "http://loinc.org", "code": "8480-6",
                                     "display": "Systolic blood pressure"}]},
                "valueString": "nested-choice"
            }]}}
    ]
})

# ═════════════════════════════════════════════════════════════════════════════
# Getting Started — Step 2 → Step 3 → Step 1
# ═════════════════════════════════════════════════════════════════════════════
def test_getting_started():
    # Step 2: Create file-backed Memory.
    mem = ff.Memory.create_from_file(PATIENT_FFHR, capacity=64 * 1024 * 1024)

    # Step 3: Ingest inline JSON, enrich with typed fields, and seal.
    ingestor = ff.Ingestor()
    with ff.Stream(mem, ff.FhirVersion.R5) as stream:
        patient_node, count = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, GETTING_STARTED_JSON)
        assert count > 0, "getting-started ingest parsed 0 resources"
        assert patient_node, "getting-started patient handle is null"

        patient_node[Patient.ACTIVE]    = True
        patient_node[Patient.BIRTHDATE] = "1990-03-21"

        stream.root = patient_node
        view = stream.finalize(ff.Checksum.SHA256)
        assert view is not None and view.size > 0, "getting-started finalize returned empty view"
    mem.close()

    # Step 1: Re-open and verify the sealed archive.
    mem2 = ff.Memory.create_from_file(PATIENT_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem2, ff.FhirVersion.R5) as stream2:
        node = stream2.root
        assert node, "getting-started root is null after seal"

        pid    = node[Patient.ID].value()
        active = node[Patient.ACTIVE].value()
        gender = node[Patient.GENDER].value()
        dob    = node[Patient.BIRTHDATE].value()
        print(f"  id={pid!r}  gender={gender!r}  active={active!r}  dob={dob!r}")

        assert pid == "patient-1",    f"expected 'patient-1', got {pid!r}"
        assert active is True,         f"expected True, got {active!r}"
        assert gender == "male",       f"expected 'male', got {gender!r}"
        assert dob == "1990-03-21",    f"expected '1990-03-21', got {dob!r}"

        for name_entry in node[Patient.NAME]:
            n      = name_entry.value()
            family = n[HumanName.FAMILY].value()
            given  = [g.value() for g in n[HumanName.GIVEN]]
            print(f"  name: {given} {family}")
            assert family == "Smith",  f"expected 'Smith', got {family!r}"
            assert "John" in given,    f"'John' not in {given!r}"
    mem2.close()

if __name__ == "__main__":
    cleanup_artifacts()
    run("Getting Started — Step 2 → Step 3 → Step 1", test_getting_started)

# ═════════════════════════════════════════════════════════════════════════════
# Example 1 — Ingest patient.json and save as patient.ffhr
# ═════════════════════════════════════════════════════════════════════════════
def test_1():
    ingestor = ff.Ingestor()
    with open(PATIENT_JSON) as f:
        json_string = f.read()

    mem = ff.Memory.create_from_file(PATIENT_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem, ff.FhirVersion.R5) as stream:
        patient_node, count = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, json_string)
        print(f"  ingest count : {count}")

        pid    = patient_node[Patient.ID].value()
        gender = patient_node[Patient.GENDER].value()
        active = patient_node[Patient.ACTIVE].value()
        print(f"  id     : {pid!r}")
        print(f"  gender : {gender!r}")
        print(f"  active : {active!r}")
        assert pid == "patient-1", f"expected 'patient-1', got {pid!r}"
        assert gender == "male",   f"expected 'male', got {gender!r}"
        assert active is True,     f"expected True, got {active!r}"

        family = None
        last_given = []
        for name_entry in patient_node[Patient.NAME]:
            name   = name_entry.value()
            family = name[HumanName.FAMILY].value()
            last_given = [g.value() for g in name[HumanName.GIVEN]]
            print(f"  name   : {last_given} {family}")
        assert family == "Landvater",   f"expected 'Landvater', got {family!r}"
        assert "Ryan" in last_given,    f"'Ryan' not in {last_given!r}"

        stream.root = patient_node
        stream.finalize(ff.Checksum.SHA256)

    mem.close()
    assert os.path.exists(PATIENT_FFHR), "patient.ffhr not created"
    sz = os.path.getsize(PATIENT_FFHR)
    print(f"  file size : {sz:,} bytes")
    assert sz > 0, "patient.ffhr is empty"

if __name__ == "__main__": run("Example 1 — Ingest patient.json → save patient.ffhr", test_1)

# ═════════════════════════════════════════════════════════════════════════════
# Example 2 — Open and read patient.ffhr
#
#   Traverse directly from stream.root.
#   No JSON round-trip and no re-ingest.
# ═════════════════════════════════════════════════════════════════════════════
def test_2():
    mem = ff.Memory.create_from_file(PATIENT_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem, ff.FhirVersion.R5) as stream:
        patient_node = stream.root
        if not patient_node:
            raise RuntimeError('stream.root is null; archive root must be set before read.')

        pid    = patient_node[Patient.ID].value()
        gender = patient_node[Patient.GENDER].value()
        active = patient_node[Patient.ACTIVE].value()
        dob    = patient_node[Patient.BIRTHDATE].value()   # None — not in source
        print(f"  id     : {pid!r}")
        print(f"  gender : {gender!r}")
        print(f"  active : {active!r}")
        print(f"  dob    : {dob!r}")
        assert pid == "patient-1", f"expected 'patient-1', got {pid!r}"
        assert gender == "male",   f"expected 'male', got {gender!r}"
        assert active is True,     f"expected True, got {active!r}"

        for name_entry in patient_node[Patient.NAME]:
            name   = name_entry.value()
            family = name[HumanName.FAMILY].value()
            given  = [g.value() for g in name[HumanName.GIVEN]]
            print(f"  name   : {given} {family}")

        # Materialize entire record as plain Python dict
        # (installed items() returns (key, MutableEntry) pairs without recursive;
        #  use to_json() + json.loads for a fully materialized plain-Python dict)
        patient_dict = json.loads(patient_node.to_json())
        print(f"  dict keys : {sorted(patient_dict.keys())}")
        for human_name in patient_dict.get("name", []):
            print(f"  dict name : {human_name.get('family')} {human_name.get('given', [])}")
        assert patient_dict.get("gender") == "male", "dict gender mismatch"

    mem.close()

if __name__ == "__main__": run("Example 2 — Open and read patient.ffhr", test_2)

# ═════════════════════════════════════════════════════════════════════════════
# Example 3 — Re-open patient.ffhr and enrich in place
# ═════════════════════════════════════════════════════════════════════════════
def test_3():
    ingestor = ff.Ingestor()
    mem = ff.Memory.create_from_file(PATIENT_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem, ff.FhirVersion.R4) as stream:
        patient_node = stream.root
        if not patient_node:
            raise RuntimeError('stream.root is null; cannot mutate without explicit root.')

        # Add new scalar fields (not present in the source JSON)
        patient_node[Patient.BIRTHDATE] = "1990-03-21"
        patient_node[Patient.ACTIVE]    = True

        # Append a new structured sub-object
        patient_node[Patient.TELECOM] = {
            "system": "phone",
            "value":  "802-555-0199",
            "use":    "mobile"
        }

        stream.root = patient_node
        stream.finalize(ff.Checksum.SHA256)
    mem.close()

    # Verify the enrichment persisted
    mem2 = ff.Memory.create_from_file(PATIENT_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem2, ff.FhirVersion.R4) as stream2:
        patient_node2 = stream2.root
        dob    = patient_node2[Patient.BIRTHDATE].value()
        active = patient_node2[Patient.ACTIVE].value()
        print(f"  birthDate after enrich : {dob!r}")
        print(f"  active after enrich    : {active!r}")
        assert dob == "1990-03-21", f"expected '1990-03-21', got {dob!r}"
        assert active is True, "expected True after enrich"
    mem2.close()

if __name__ == "__main__": run("Example 3 — Re-open patient.ffhr and enrich in place", test_3)

# ═════════════════════════════════════════════════════════════════════════════
# Example 4 — Static HTTP file server GET/PUT with patient.ffhr
# ═════════════════════════════════════════════════════════════════════════════
def test_4():
    with tempfile.TemporaryDirectory(prefix="ff_http_") as srv_dir:
        clean_ffhr = os.path.join(srv_dir, "patient.ffhr")
        uploaded_path = os.path.join(srv_dir, "patient.uploaded.ffhr")

        # Seed a clean local patient.ffhr for static GET/PUT using the same flow as notebook Segment E.1.
        with open(PATIENT_JSON) as f:
            json_src = f.read()

        mem_srv = ff.Memory.create_from_file(clean_ffhr, capacity=8 * 1024 * 1024)
        with ff.Stream(mem_srv, ff.FhirVersion.R5) as stream_srv:
            srv_node, _ = ff.Ingestor().ingest(stream_srv, ff.SourceType.FHIR_JSON, json_src)
            stream_srv.root = srv_node
            stream_srv.finalize(ff.Checksum.SHA256)
        mem_srv.close()

        seed_size = os.path.getsize(clean_ffhr)
        assert seed_size > 0, "seed patient.ffhr is empty"

        class StaticBundleHandler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=srv_dir, **kwargs)

            def do_PUT(self):
                if self.path != "/patient.uploaded.ffhr":
                    self.send_response(404)
                    self.end_headers()
                    return
                length = int(self.headers.get("Content-Length", "0"))
                with open(uploaded_path, "wb") as f:
                    remaining = length
                    while remaining > 0:
                        chunk = self.rfile.read(min(65536, remaining))
                        if not chunk:
                            break
                        f.write(chunk)
                        remaining -= len(chunk)
                self.send_response(200)
                self.end_headers()

            def log_message(self, format, *args):
                return

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), StaticBundleHandler)
        port = server.server_address[1]
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()

        mem_http = None

        try:
            # Step 1: GET and stream directly into FastFHIR memory (Segment E.2 behavior).
            conn_get = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            conn_get.request("GET", "/patient.ffhr")
            res_get = conn_get.getresponse()
            assert res_get.status == 200, f"GET /patient.ffhr failed: {res_get.status}"

            mem_http = ff.Memory.create(capacity=16 * 1024 * 1024)
            total = 0
            with mem_http.try_acquire_stream() as head:
                while True:
                    dst = memoryview(head).cast("B")
                    if len(dst) == 0:
                        raise RuntimeError("Downloaded payload exceeds arena capacity.")

                    n = res_get.readinto(dst)
                    if not n:
                        break

                    head.commit(n)
                    total += n

            conn_get.close()
            print(f"  GET streamed bytes : {total:,} bytes")

            # Step 2: assert the stream lock was released after context exit.
            probe = mem_http.try_acquire_stream()
            assert probe is not None, "stream lock still held after GET streaming context"
            if hasattr(probe, "__exit__"):
                probe.__exit__(None, None, None)

            # Step 3: enrich in-memory archive and finalize (Segment E.3 behavior).
            with ff.Stream(mem_http, ff.FhirVersion.R5) as stream:
                patient_node = stream.root
                if not patient_node:
                    raise RuntimeError('stream.root is null; cannot mutate without explicit root.')

                patient_node[Patient.TELECOM] = {
                    "system": "phone",
                    "value": "802-555-4242",
                    "use": "mobile"
                }

                stream.root = patient_node
                outbound_ffhr = bytes(stream.finalize(ff.Checksum.SHA256))

            # Step 4: PUT enriched payload back to server.
            conn_put = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            conn_put.request(
                "PUT",
                "/patient.uploaded.ffhr",
                body=outbound_ffhr,
                headers={"Content-Type": "application/octet-stream"}
            )
            res_put = conn_put.getresponse()
            assert res_put.status == 200, f"PUT /patient.uploaded.ffhr failed: {res_put.status}"
            res_put.read()
            conn_put.close()
            put_size = os.path.getsize(uploaded_path)
            print(f"  PUT bytes : {put_size:,} bytes")

            # Step 5: crash-safe verify via byte scan (matches notebook Segment E.4).
            assert os.path.exists(uploaded_path), "server did not receive uploaded .ffhr"
            assert put_size > 0, "uploaded .ffhr is empty"

            with open(uploaded_path, "rb") as f:
                uploaded_bytes = f.read()
            found_enriched = b"802-555-4242" in uploaded_bytes
            print(f"  enriched found (byte-scan) : {found_enriched}")
            assert found_enriched, "enrichment not found after PUT byte scan"
        finally:
            if mem_http is not None:
                mem_http.close()
            server.shutdown()
            server.server_close()

if __name__ == "__main__": run("Example 4 — Static HTTP GET/PUT with patient.ffhr", test_4)

# ═════════════════════════════════════════════════════════════════════════════
# Example 5 — Re-open the same patient.ffhr and reseal another surgical edit
# ═════════════════════════════════════════════════════════════════════════════
def test_5():
    # Step 1: re-open the same archive from Example 1.
    mem = ff.Memory.create_from_file(PATIENT_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem, ff.FhirVersion.R5) as stream:
        patient_node = stream.root
        if not patient_node:
            raise RuntimeError('stream.root is null; cannot mutate without explicit root.')

        # Apply another surgical mutation without JSON round-trip and without
        # reassigning pointer fields that were already populated in prior tests.
        patient_node[Patient.DECEASED] = False

        stream.root = patient_node
        stream.finalize(ff.Checksum.SHA256)
    mem.close()

    # Step 2: verify the second mutation persisted.
    mem2 = ff.Memory.create_from_file(PATIENT_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem2, ff.FhirVersion.R5) as stream2:
        verify_node = stream2.root
        if not verify_node:
            raise RuntimeError('stream.root is null; archive root must be set before verify.')

        deceased = verify_node[Patient.DECEASED].value()
    mem2.close()

    assert deceased is False, "deceased not updated in Example 5"
    print(f"  patient.ffhr final size : {os.path.getsize(PATIENT_FFHR):,} bytes")

if __name__ == "__main__": run("Example 5 — Reuse patient.ffhr for another surgical edit", test_5)

# ═════════════════════════════════════════════════════════════════════════════
# Example 6 — Surgically edit one patient in a bundle and reseal
# ═════════════════════════════════════════════════════════════════════════════
def test_6():
    # Step A: ingest bundle with two patients.
    ingestor = ff.Ingestor()
    mem = ff.Memory.create_from_file(BUNDLE_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem, ff.FhirVersion.R5) as stream:
        bundle_node, count = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, BUNDLE_JSON)
        assert count >= 2, f"expected at least 2 resources, got {count}"
        print(f"  bundle ingested : {count} resources")
        stream.root = bundle_node
        stream.finalize(ff.Checksum.SHA256)
    mem.close()

    # Step B: re-open and find patient-1.
    ingestor2 = ff.Ingestor()
    mem2 = ff.Memory.create_from_file(BUNDLE_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem2, ff.FhirVersion.R5) as stream2:
        bundle_node2 = stream2.root
        assert bundle_node2, "bundle root is null after seal"

        target_patient = None
        for entry in bundle_node2[Bundle.ENTRY]:
            resource = entry[BundleEntry.RESOURCE]
            if not resource:
                continue
            node_val = resource.value()
            if not node_val or node_val.recovery_tag != RT.Patient:
                continue
            if node_val[Patient.ID].value() == "patient-1":
                target_patient = node_val
                break

        assert target_patient is not None, "patient-1 not found in bundle"
        print("  found patient-1")

        # Step C: surgically enrich patient-1 only.
        target_patient[Patient.TELECOM] = {"system": "phone", "value": "555-0199", "use": "mobile"}

        # Step D: reseal.
        stream2.root = bundle_node2
        stream2.finalize(ff.Checksum.SHA256)
    mem2.close()

    # Step E: verify patient-1 enriched, patient-2 untouched.
    mem3 = ff.Memory.create_from_file(BUNDLE_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem3, ff.FhirVersion.R5) as stream3:
        bundle_node3 = stream3.root
        assert bundle_node3

        found_enriched = False
        for entry in bundle_node3[Bundle.ENTRY]:
            resource = entry[BundleEntry.RESOURCE]
            if not resource:
                continue
            node_val = resource.value()
            if not node_val or node_val.recovery_tag != RT.Patient:
                continue
            pid = node_val[Patient.ID].value()
            if pid == "patient-1":
                telecom_json = node_val[Patient.TELECOM].to_json()
                assert "555-0199" in telecom_json, f"telecom value not in JSON: {telecom_json}"
                print("  patient-1 telecom : 555-0199 (verified via to_json)")
                found_enriched = True
            elif pid == "patient-2":
                # Verify patient-2 was not modified
                telecom_items = list(node_val[Patient.TELECOM].items())
                assert len(telecom_items) == 0, f"patient-2 telecom should be empty but has {telecom_items}"
                print("  patient-2 untouched (telecom empty as expected)")

        assert found_enriched, "patient-1 not found in re-sealed bundle"
    mem3.close()

if __name__ == "__main__": run("Example 6 — Surgically edit one patient in a bundle and reseal", test_6)

# ═════════════════════════════════════════════════════════════════════════════
# Example 7 — Lock-free concurrent generation (thread-safety smoke test)
# ═════════════════════════════════════════════════════════════════════════════
def test_7():
    import threading
    N = 8
    mem = ff.Memory.create(capacity=256 * 1024 * 1024)
    node_results = [None] * N
    errors       = []
    lock         = threading.Lock()

    with ff.Stream(mem, ff.FhirVersion.R5) as stream:
        def worker(i):
            try:
                obs_json = json.dumps({
                    "resourceType": "Observation",
                    "id": f"obs-{i}",
                    "status": "preliminary",
                    "code": {"coding": [{"system": "http://loinc.org", "code": "8867-4"}]}
                })
                local_ingestor = ff.Ingestor()
                node, _ = local_ingestor.ingest(stream, ff.SourceType.FHIR_JSON, obs_json)
                with lock:
                    node_results[i] = node
            except Exception as e:
                with lock:
                    errors.append((i, str(e)))

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(N)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        assert not errors, f"thread errors: {errors}"
        assert all(r is not None for r in node_results), "not all threads produced a handle"

        stream.root = node_results[0]
        stream.finalize(ff.Checksum.SHA256)
    mem.close()

    print(f"  {N} threads completed lock-free writes")
    print(f"  all {N} StreamNode handles valid")

if __name__ == "__main__": run("Example 7 — Lock-free concurrent generation", test_7)

# ═════════════════════════════════════════════════════════════════════════════
# Example 8 — Post-finalize archival compaction
# ═════════════════════════════════════════════════════════════════════════════
def test_8():
    # Source: sealed patient.ffhr produced by test_1 / test_3.
    src_mem     = ff.Memory.create_from_file(PATIENT_FFHR,         capacity=64 * 1024 * 1024)
    compact_mem = ff.Memory.create_from_file(PATIENT_COMPACT_FFHR, capacity=64 * 1024 * 1024)

    with ff.Stream(src_mem, ff.FhirVersion.R5) as src_stream:
        compact_view = src_stream.compact(compact_mem, ff.Checksum.SHA256)

    src_size = src_mem.size
    src_mem.close()

    assert compact_view is not None and compact_view.size > 0, "compact archive view is empty"
    print(f"  original patient bytes : {src_size:,}")
    print(f"  compact archive bytes  : {compact_view.size:,}")

    compact_bytes = bytes(compact_view)
    assert b"patient-1" in compact_bytes,  "patient-1 not found in compact archive"
    assert b"Landvater" in compact_bytes,  "Landvater not found in compact archive"
    assert b"male"      in compact_bytes,  "gender not found in compact archive"
    compact_mem.close()

if __name__ == "__main__": run("Example 8 — Post-finalize archival compaction", test_8)

# ═════════════════════════════════════════════════════════════════════════════
# Example 9 — Standard array-tagged field key coverage
# ═════════════════════════════════════════════════════════════════════════════
def test_9():
    # Patient — verify array-tagged NAME key and element traversal.
    src_mem = ff.Memory.create_from_file(PATIENT_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(src_mem, ff.FhirVersion.R5) as stream:
        patient_root = stream.root
        assert patient_root, "standard patient root is null"

        name_entry = patient_root[Patient.NAME]
        assert bool(name_entry), "PATIENT::NAME key failed on standard stream"

        found_family = False
        for name in patient_root[Patient.NAME]:
            n      = name.value()
            family = n[HumanName.FAMILY].value()
            given  = [g.value() for g in n[HumanName.GIVEN]]
            assert family == "Landvater", f"expected 'Landvater', got {family!r}"
            assert "Ryan" in given,       f"'Ryan' not in {given!r}"
            found_family = True
            break
        assert found_family, "patient name array should not be empty"
    src_mem.close()

    # Bundle — verify array-tagged ENTRY key and resource field.
    bundle_mem = ff.Memory.create_from_file(BUNDLE_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(bundle_mem, ff.FhirVersion.R5) as bundle_stream:
        bundle_root = bundle_stream.root
        assert bundle_root, "standard bundle root is null"

        entry_count = sum(1 for _ in bundle_root[Bundle.ENTRY])
        assert entry_count >= 2, f"expected at least 2 bundle entries, got {entry_count}"

        for entry in bundle_root[Bundle.ENTRY]:
            resource = entry[BundleEntry.RESOURCE]
            assert bool(resource), "resource field missing from first bundle entry"
            break
    bundle_mem.close()

    print("  standard array-tagged keys verified for patient.name and bundle.entry")

if __name__ == "__main__": run("Example 9 — Standard array-tagged field key coverage", test_9)

# ═════════════════════════════════════════════════════════════════════════════
# Example 10 — Compact nested choice/resource (Bundle + Observation)
# ═════════════════════════════════════════════════════════════════════════════
def test_10():
    ingestor = ff.Ingestor()
    mem = ff.Memory.create_from_file(BUNDLE_COMPLEX_FFHR, capacity=64 * 1024 * 1024)
    with ff.Stream(mem, ff.FhirVersion.R5) as stream:
        bundle_node, count = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, BUNDLE_COMPLEX_JSON)
        assert bundle_node, "complex bundle handle is null"

        # Verify source observation value[x] and nested component value[x].
        found_obs_source = False
        for entry in bundle_node[Bundle.ENTRY]:
            resource = entry[BundleEntry.RESOURCE]
            if not resource:
                continue
            node_val = resource.value()
            if not node_val or node_val.recovery_tag != RT.Observation:
                continue
            found_obs_source = True
            obs_json_str = node_val.to_json()
            assert "final-value" in obs_json_str, f"source observation value[x] mismatch"
            assert "nested-choice" in obs_json_str, f"source component value[x] mismatch"
        assert found_obs_source, "source complex bundle observation not found"

        stream.root = bundle_node
        source_view = stream.finalize(ff.Checksum.SHA256)
        assert source_view.size > 0, "complex bundle finalize returned empty view"

        # Compact the sealed source into a separate arena.
        compact_mem = ff.Memory.create_from_file(BUNDLE_COMPLEX_COMPACT_FFHR, capacity=64 * 1024 * 1024)
        compact_view = stream.compact(compact_mem, ff.Checksum.SHA256)
        assert compact_view.size > 0, "complex compact archive view is empty"

        compact_bytes = bytes(compact_view)
        assert b"final-value"    in compact_bytes, "compact observation value[x] not found"
        assert b"nested-choice"  in compact_bytes, "compact component value[x] not found"

        print(f"  complex source bytes  : {source_view.size:,}")
        print(f"  complex compact bytes : {compact_view.size:,}")
        compact_mem.close()
    mem.close()

if __name__ == "__main__":
    run("Example 10 — Compact nested choice/resource (Bundle + Observation)", test_10)

    # ═══════════════════════════════════════════════════════════════════════════
    # Summary
    # ═══════════════════════════════════════════════════════════════════════════
    print(f"\n{'='*60}")
    print("  RESULTS")
    print(f"{'='*60}")
    passed = sum(v for v in results.values())
    total  = len(results)
    for name, ok in results.items():
        status = PASS if ok else FAIL
        print(f"  [{status}] {name}")
    print(f"\n  {passed}/{total} passed")
    print(f"{'='*60}\n")

    if passed < total:
        cleanup_artifacts()
        cleanup_generated_patient_fixture()
        sys.exit(1)

    cleanup_artifacts()
    cleanup_generated_patient_fixture()
