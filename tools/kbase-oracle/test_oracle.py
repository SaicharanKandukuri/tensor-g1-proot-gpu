#!/usr/bin/env python3

import json
import tempfile
import unittest
from pathlib import Path

from compare_traces import compare
from normalize_trace import canonicalize, decode_core_req, read_events


def events(allocation, gpu):
    descriptor = bytearray(32)
    descriptor[16:20] = (11 << 1 | 7 << 16).to_bytes(4, "little")
    descriptor[20:24] = (3 | 4 << 16).to_bytes(4, "little")
    return [
        {"schema": 1, "type": "process", "label": "sample"},
        {
            "schema": 1,
            "type": "allocation",
            "allocation": allocation,
            "kind": "native",
            "pages": 1,
            "flags": "0x2000",
        },
        {
            "schema": 1,
            "type": "mapping",
            "allocation": allocation,
            "gpu": gpu,
            "length": 4096,
        },
        {"schema": 1, "type": "submit", "submit": 9, "nr_atoms": 1, "stride": 64},
        {
            "schema": 1,
            "type": "atom",
            "submit": 9,
            "atom_index": 0,
            "atom_number": 22,
            "core_req": 16,
            "jc": gpu,
            "pre_dep_0": 0,
            "pre_dep_type_0": 0,
            "pre_dep_1": 0,
            "pre_dep_type_1": 0,
            "priority": 1,
            "device": 0,
            "jobslot": 0,
            "seq_nr": 123,
        },
        {
            "schema": 1,
            "type": "job",
            "submit": 9,
            "atom_index": 0,
            "chain_index": 0,
            "allocation": allocation,
            "offset": 128,
            "type_id": 11,
            "barrier": False,
            "index": 7,
            "dependency_1": 3,
            "dependency_2": 4,
            "next_gpu": "0x0",
            "bytes": descriptor.hex(),
        },
        {
            "schema": 1,
            "type": "ioctl",
            "name": "JOB_SUBMIT",
            "result": 0,
        },
    ]


class OracleTest(unittest.TestCase):
    def test_addresses_and_allocation_ids_do_not_change_semantics(self):
        left = canonicalize(events(41, "0x100000"))
        right = canonicalize(events(97, "0x900000"))
        self.assertTrue(compare(left, right)["equal"])
        self.assertNotIn("jc", left["submits"][0]["atoms"][0])
        self.assertEqual(left["submits"][0]["jobs"][0]["reference"]["map"], "map-1")

    def test_header_difference_is_reported(self):
        left = canonicalize(events(41, "0x100000"))
        changed = events(97, "0x900000")
        changed[-2]["type_id"] = 9
        changed[-2]["index"] = 8
        report = compare(left, canonicalize(changed))
        self.assertFalse(report["equal"])
        paths = {difference["path"] for difference in report["differences"]}
        self.assertIn("$[0].jobs[0].header.type_id", paths)
        self.assertIn("$[0].jobs[0].header.index", paths)

    def test_jsonl_schema_is_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text(json.dumps({"schema": 2, "type": "process"}) + "\n")
            with self.assertRaises(ValueError):
                read_events(path)

    def test_core_requirements_are_decoded(self):
        self.assertEqual(
            decode_core_req(78),
            [
                "compute_shader",
                "tiler",
                "cache_flush",
                "coherent_group",
            ],
        )
        self.assertEqual(decode_core_req(521), ["soft_job", "jit_alloc"])

    def test_completion_fault_is_named_and_counted(self):
        trace = events(41, "0x100000")
        trace.append(
            {
                "schema": 1,
                "type": "completion",
                "atom_number": 22,
                "event_code": 0x58,
            }
        )
        normalized = canonicalize(trace)
        self.assertEqual(normalized["summary"]["completion_failures"], 1)
        self.assertEqual(normalized["completions"][0]["event"], "data_invalid_fault")


if __name__ == "__main__":
    unittest.main()
