"""
Stress-test FastFHIR array insertion via Python bindings.

What it validates:
- Ingest a bundle with one Patient.
- Patch Patient.telecom with a very long JSON array payload.
- Finalize and re-open archive.
- Verify telecom array length and boundary values.

This exercises the insert_at_field array path used by Python assignment,
which is backed by the C++ queue-based ingestion flow.

Usage:
    python tests/python/test_long_array_queue.py --count 5000
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path


try:
    import fastfhir as ff
    from fastfhir.fields import Bundle, BundleEntry, Patient
    try:
        from fastfhir.fields import Appointment
    except ImportError:
        Appointment = None  # Not generated in this build profile (e.g. US Core)
except ModuleNotFoundError as exc:
    raise RuntimeError(
        "fastfhir is not importable. Build/install bindings first."
    ) from exc


RT = ff._core.ResourceType


def build_bundle_json(scalar_count: int) -> str:
    excluding_recurrence_a = [i + 1 for i in range(scalar_count)]
    excluding_recurrence_b = [scalar_count + i + 1 for i in range(scalar_count)]

    payload = {
        "resourceType": "Bundle",
        "id": "queue-array-stress",
        "type": "collection",
        "entry": [
            {
                "fullUrl": "Patient/patient-1",
                "resource": {
                    "resourceType": "Patient",
                    "id": "patient-1",
                    "active": True,
                    "name": [{"use": "official", "family": "Stress", "given": ["Array"]}],
                    "gender": "unknown",
                },
            },
            {
                "fullUrl": "Appointment/appointment-1",
                "resource": {
                    "resourceType": "Appointment",
                    "id": "appointment-1",
                    "status": "booked",
                    "participant": [{"status": "accepted"}],
                    "recurrenceTemplate": [
                        {
                            "occurrenceCount": scalar_count,
                            "excludingRecurrenceId": excluding_recurrence_a,
                        }
                    ],
                },
            },
            {
                "fullUrl": "Appointment/appointment-2",
                "resource": {
                    "resourceType": "Appointment",
                    "id": "appointment-2",
                    "status": "booked",
                    "participant": [{"status": "accepted"}],
                    "recurrenceTemplate": [
                        {
                            "occurrenceCount": scalar_count,
                            "excludingRecurrenceId": excluding_recurrence_b,
                        }
                    ],
                },
            },
        ],
    }
    return json.dumps(payload)


def build_telecom_array(n: int) -> list[dict[str, str]]:
    out = []
    for i in range(n):
        out.append(
            {
                "system": "phone",
                "value": f"555-{100000 + i}",
                "use": "mobile" if (i % 2 == 0) else "home",
            }
        )
    return out


def _resource_type_supported(name: str) -> bool:
    return hasattr(RT, name)


def _resource_type_note() -> str:
    if _resource_type_supported("Appointment"):
        return "available"
    return "missing (falling back to ID-path matching)"


def _maybe_check_patient_type(node_val) -> bool:
    if _resource_type_supported("Patient"):
        return node_val.recovery_tag == RT.Patient
    return True


def _maybe_check_appointment_type(node_val) -> bool:
    if _resource_type_supported("Appointment"):
        return node_val.recovery_tag == RT.Appointment
    return True


def find_patient(bundle_node, patient_id: str):
    for entry in bundle_node[Bundle.ENTRY]:
        resource = entry[BundleEntry.RESOURCE]
        if not resource:
            continue
        node_val = resource.value()
        if not node_val or not _maybe_check_patient_type(node_val):
            continue
        try:
            if node_val[Patient.ID].value() == patient_id:
                return node_val
        except Exception:
            continue
    return None


def find_appointment(bundle_node, appointment_id: str):
    for entry in bundle_node[Bundle.ENTRY]:
        resource = entry[BundleEntry.RESOURCE]
        if not resource:
            continue
        node_val = resource.value()
        if not node_val or not _maybe_check_appointment_type(node_val):
            continue
        try:
            if node_val[Appointment.ID].value() == appointment_id:
                return node_val
        except Exception:
            continue
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--count", type=int, default=5000, help="Number of telecom elements to insert")
    parser.add_argument(
        "--capacity-mb",
        type=int,
        default=0,
        help="Explicit memory/file capacity in MiB (0 = auto-estimate)",
    )
    args = parser.parse_args()

    if args.count <= 0:
        print("count must be > 0", file=sys.stderr)
        return 2
    if args.capacity_mb < 0:
        print("capacity-mb must be >= 0", file=sys.stderr)
        return 2

    root = Path(__file__).resolve().parents[2]
    out_dir = root / "build" / "tests" / "python"
    out_dir.mkdir(parents=True, exist_ok=True)
    ffhr_path = out_dir / "long-array-queue.ffhr"

    if ffhr_path.exists():
        ffhr_path.unlink()

    ingestor = ff.Ingestor()

    # Rough per-item estimate (FHIR JSON + internal block overhead) for telecom entries.
    # We bias high and add a fixed headroom to avoid allocation failures on larger arrays.
    estimated_payload_bytes = 64 * 1024 + (args.count * 320)
    auto_capacity_bytes = max(64 * 1024 * 1024, estimated_payload_bytes * 2)
    if args.capacity_mb > 0:
        capacity_bytes = args.capacity_mb * 1024 * 1024
        capacity_source = f"user ({args.capacity_mb} MiB)"
    else:
        capacity_bytes = auto_capacity_bytes
        capacity_source = "auto"

    timings: dict[str, float] = {}

    # Step 1: Create baseline bundle with one patient.
    t0 = time.perf_counter()
    mem = ff.Memory.create_from_file(str(ffhr_path), capacity=capacity_bytes)
    timings["create_memory"] = time.perf_counter() - t0

    with ff.Stream(mem, ff.FhirVersion.R5) as stream:
        t0 = time.perf_counter()
        bundle_json = build_bundle_json(args.count)
        bundle_node, count = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, bundle_json)
        timings["ingest_bundle"] = time.perf_counter() - t0
        if count < 1:
            raise RuntimeError(f"ingest parsed {count} resources")

        patient = find_patient(bundle_node, "patient-1")
        if patient is None:
            raise RuntimeError("patient-1 not found after ingest")

        appointment = find_appointment(bundle_node, "appointment-1")
        if appointment is None:
            raise RuntimeError("appointment-1 not found after ingest")

        t0 = time.perf_counter()
        telecom = build_telecom_array(args.count)
        timings["build_array_python"] = time.perf_counter() - t0

        # Step 2: Queue-backed array patch via Python assignment.
        t0 = time.perf_counter()
        patient[Patient.TELECOM] = telecom
        timings["patch_array_assignment"] = time.perf_counter() - t0

        stream.root = bundle_node
        t0 = time.perf_counter()
        stream.finalize(ff.Checksum.SHA256)
        timings["finalize"] = time.perf_counter() - t0
    mem.close()

    # Step 3: Re-open and verify length + boundary values.
    t0 = time.perf_counter()
    mem2 = ff.Memory.create_from_file(str(ffhr_path), capacity=capacity_bytes)
    timings["reopen_memory"] = time.perf_counter() - t0

    with ff.Stream(mem2, ff.FhirVersion.R5) as stream2:
        t0 = time.perf_counter()
        bundle2 = stream2.root
        patient2 = find_patient(bundle2, "patient-1")
        appointment2 = find_appointment(bundle2, "appointment-1")
        appointment3 = find_appointment(bundle2, "appointment-2")
        timings["find_patient_after_reopen"] = time.perf_counter() - t0
        if patient2 is None:
            raise RuntimeError("patient-1 not found after re-open")
        if appointment2 is None:
            raise RuntimeError("appointment-1 not found after re-open")
        if appointment3 is None:
            raise RuntimeError("appointment-2 not found after re-open")

        t0 = time.perf_counter()
        telecom_json = patient2[Patient.TELECOM].to_json()
        telecom_back = json.loads(telecom_json)
        timings["decode_telecom_json"] = time.perf_counter() - t0

        t0 = time.perf_counter()
        if len(telecom_back) != args.count:
            raise RuntimeError(
                f"telecom length mismatch: expected {args.count}, got {len(telecom_back)}"
            )

        first = telecom_back[0]["value"]
        last = telecom_back[-1]["value"]
        expected_first = "555-100000"
        expected_last = f"555-{100000 + args.count - 1}"

        if first != expected_first or last != expected_last:
            raise RuntimeError(
                f"boundary mismatch: first={first} last={last} expected_first={expected_first} expected_last={expected_last}"
            )
        timings["verify_boundaries"] = time.perf_counter() - t0

        t0 = time.perf_counter()
        recurrence_json_a = appointment2[Appointment.RECURRENCETEMPLATE].to_json()
        recurrence_back_a = json.loads(recurrence_json_a)
        recurrence_json_b = appointment3[Appointment.RECURRENCETEMPLATE].to_json()
        recurrence_back_b = json.loads(recurrence_json_b)

        if len(recurrence_back_a) != 1:
            raise RuntimeError(f"appointment-1 recurrenceTemplate length mismatch: expected 1, got {len(recurrence_back_a)}")
        if len(recurrence_back_b) != 1:
            raise RuntimeError(f"appointment-2 recurrenceTemplate length mismatch: expected 1, got {len(recurrence_back_b)}")

        excluding_back_a = recurrence_back_a[0].get("excludingRecurrenceId", [])
        excluding_back_b = recurrence_back_b[0].get("excludingRecurrenceId", [])

        if len(excluding_back_a) != args.count:
            raise RuntimeError(
                f"appointment-1 excludingRecurrenceId length mismatch: expected {args.count}, got {len(excluding_back_a)}"
            )
        if len(excluding_back_b) != args.count:
            raise RuntimeError(
                f"appointment-2 excludingRecurrenceId length mismatch: expected {args.count}, got {len(excluding_back_b)}"
            )

        if excluding_back_a[0] != 1 or excluding_back_a[-1] != args.count:
            raise RuntimeError(
                f"appointment-1 excludingRecurrenceId boundary mismatch: first={excluding_back_a[0]} last={excluding_back_a[-1]} expected_first=1 expected_last={args.count}"
            )

        expected_b_first = args.count + 1
        expected_b_last = args.count * 2
        if excluding_back_b[0] != expected_b_first or excluding_back_b[-1] != expected_b_last:
            raise RuntimeError(
                f"appointment-2 excludingRecurrenceId boundary mismatch: first={excluding_back_b[0]} last={excluding_back_b[-1]} expected_first={expected_b_first} expected_last={expected_b_last}"
            )
        timings["verify_scalar_arrays"] = time.perf_counter() - t0

    mem2.close()

    size_bytes = os.path.getsize(ffhr_path)
    total_timing = sum(timings.values())
    print("Queue array stress PASS")
    print(f"  file: {ffhr_path}")
    print(f"  telecom count: {args.count}")
    print(f"  capacity source: {capacity_source}")
    print(f"  capacity bytes: {capacity_bytes:,}")
    print(f"  estimated payload bytes: {estimated_payload_bytes:,}")
    print(f"  first: {expected_first}")
    print(f"  last:  {expected_last}")
    print(f"  scalar arrays (ingest/roundtrip): appointment-1 recurrenceTemplate[0].excludingRecurrenceId ({args.count})")
    print(f"  scalar arrays (ingest/roundtrip): appointment-2 recurrenceTemplate[0].excludingRecurrenceId ({args.count})")
    print(f"  archive size: {size_bytes:,} bytes")
    if size_bytes == capacity_bytes:
        print("  note: archive size equals capacity (file-backed arena preallocation)")
    print("  timings:")
    for k in (
        "create_memory",
        "ingest_bundle",
        "build_array_python",
        "patch_array_assignment",
        "finalize",
        "reopen_memory",
        "find_patient_after_reopen",
        "decode_telecom_json",
        "verify_boundaries",
        "verify_scalar_arrays",
    ):
        print(f"    {k:26s} {timings.get(k, 0.0) * 1000.0:.3f} ms")
    print(f"    {'total_measured':26s} {total_timing * 1000.0:.3f} ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
