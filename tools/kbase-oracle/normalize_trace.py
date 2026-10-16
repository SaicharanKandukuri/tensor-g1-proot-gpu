#!/usr/bin/env python3

import argparse
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path


JOB_TYPES = {
    0: "not_started",
    1: "null",
    2: "write_value",
    3: "cache_flush",
    4: "compute",
    5: "vertex",
    6: "geometry",
    7: "tiler",
    8: "fused",
    9: "fragment",
    10: "indexed_vertex",
    11: "malloc_vertex",
}

EVENT_NAMES = {
    0x00: "not_started",
    0x01: "done",
    0x03: "stopped",
    0x04: "terminated",
    0x08: "active",
    0x40: "job_config_fault",
    0x41: "job_power_fault",
    0x42: "job_read_fault",
    0x43: "job_write_fault",
    0x44: "job_affinity_fault",
    0x48: "job_bus_fault",
    0x50: "instr_invalid_pc",
    0x51: "instr_invalid_enc",
    0x52: "instr_type_mismatch",
    0x53: "instr_operand_fault",
    0x54: "instr_tls_fault",
    0x55: "instr_barrier_fault",
    0x56: "instr_align_fault",
    0x58: "data_invalid_fault",
    0x59: "tile_range_fault",
    0x5A: "state_fault",
    0x60: "out_of_memory",
    0x7F: "unknown",
    0x80: "delayed_bus_fault",
    0x88: "shareability_fault",
    0xC1: "translation_fault_level1",
    0xC2: "translation_fault_level2",
    0xC3: "translation_fault_level3",
    0xC4: "translation_fault_level4",
    0xC8: "permission_fault",
    0xD1: "transtab_bus_fault_level1",
    0xD2: "transtab_bus_fault_level2",
    0xD3: "transtab_bus_fault_level3",
    0xD4: "transtab_bus_fault_level4",
    0xD8: "access_flag",
}

CORE_REQ_FLAGS = {
    0: "fragment",
    1: "compute_shader",
    2: "tiler",
    3: "cache_flush",
    4: "value_writeback",
    5: "event_coalesce",
    6: "coherent_group",
    7: "permon",
    8: "external_resources",
    10: "only_compute",
    11: "specific_coherent_group",
    12: "event_only_on_failure",
    13: "fragment_afbc",
    14: "event_never",
    15: "skip_cache_start",
    16: "skip_cache_end",
    17: "job_slot",
    18: "start_renderpass",
    19: "end_renderpass",
    20: "limited_core_mask",
}

SOFT_JOB_TYPES = {
    1: "dump_cpu_gpu_time",
    2: "fence_trigger",
    3: "fence_wait",
    5: "event_wait",
    6: "event_set",
    7: "event_reset",
    8: "debug_copy",
    9: "jit_alloc",
    10: "jit_free",
    11: "external_resource_map",
    12: "external_resource_unmap",
}


def decode_core_req(value):
    if value & (1 << 9):
        return ["soft_job", SOFT_JOB_TYPES.get(value & 0x1F, "unknown_soft")]
    if value == 0:
        return ["dependency"]
    return [name for bit, name in CORE_REQ_FLAGS.items() if value & (1 << bit)]


def read_events(path):
    events = []
    for line_number, line in enumerate(Path(path).read_text().splitlines(), 1):
        if not line.strip():
            continue
        event = json.loads(line)
        if event.get("schema") != 1 or "type" not in event:
            raise ValueError(f"{path}:{line_number}: unsupported event")
        events.append(event)
    return events


def canonicalize(events, include_bytes=False, include_addresses=False):
    processes = [event for event in events if event["type"] == "process"]
    labels = list(dict.fromkeys(event.get("label", "unlabelled") for event in processes))
    ioctl_events = [event for event in events if event["type"] == "ioctl"]
    ioctl_counts = Counter(event["name"] for event in ioctl_events)
    ioctl_failures = Counter(
        event["name"] for event in ioctl_events if event.get("result", 0) < 0
    )
    completions = [event for event in events if event["type"] == "completion"]
    allocations = {
        event["allocation"]: event
        for event in events
        if event["type"] == "allocation" and event.get("allocation")
    }
    mappings = {
        event["allocation"]: event
        for event in events
        if event["type"] == "mapping" and event.get("allocation")
    }
    atoms = defaultdict(list)
    jobs = defaultdict(list)
    submits = []
    for event in events:
        if event["type"] == "submit":
            submits.append(event)
        elif event["type"] == "atom":
            atoms[event["submit"]].append(event)
        elif event["type"] == "job":
            jobs[event["submit"]].append(event)

    canonical_maps = {}

    def map_name(allocation):
        if allocation not in canonical_maps:
            canonical_maps[allocation] = f"map-{len(canonical_maps) + 1}"
        return canonical_maps[allocation]

    normalized_submits = []
    unmapped_jobs = 0
    for submit_index, submit in enumerate(submits):
        submit_id = submit["submit"]
        submit_jobs = sorted(
            jobs[submit_id], key=lambda event: (event["atom_index"], event["chain_index"])
        )
        roots = {}
        normalized_jobs = []
        for job in submit_jobs:
            if not job.get("mapped", True):
                unmapped_jobs += 1
                normalized = {
                    "atom_index": job["atom_index"],
                    "chain_index": job["chain_index"],
                    "mapped": False,
                }
                if include_addresses:
                    normalized["gpu"] = job.get("gpu")
                normalized_jobs.append(normalized)
                continue
            reference = {
                "map": map_name(job["allocation"]),
                "offset": job["offset"],
            }
            if job["chain_index"] == 0:
                roots[job["atom_index"]] = reference
            normalized = {
                "atom_index": job["atom_index"],
                "chain_index": job["chain_index"],
                "mapped": True,
                "reference": reference,
                "header": {
                    "type": JOB_TYPES.get(job["type_id"], f"unknown_{job['type_id']}"),
                    "type_id": job["type_id"],
                    "barrier": job["barrier"],
                    "index": job["index"],
                    "dependency_1": job["dependency_1"],
                    "dependency_2": job["dependency_2"],
                },
            }
            raw = bytes.fromhex(job.get("bytes", ""))
            if raw:
                normalized["snapshot_sha256"] = hashlib.sha256(raw).hexdigest()
            if include_bytes and "bytes" in job:
                normalized["bytes"] = job["bytes"]
            if include_addresses:
                normalized["next_gpu"] = job.get("next_gpu")
            normalized_jobs.append(normalized)

        normalized_atoms = []
        for atom in sorted(atoms[submit_id], key=lambda event: event["atom_index"]):
            normalized = {
                "atom_index": atom["atom_index"],
                "atom_number": atom["atom_number"],
                "core_req": atom["core_req"],
                "core_req_flags": decode_core_req(atom["core_req"]),
                "dependencies": [
                    {
                        "atom": atom["pre_dep_0"],
                        "type": atom["pre_dep_type_0"],
                    },
                    {
                        "atom": atom["pre_dep_1"],
                        "type": atom["pre_dep_type_1"],
                    },
                ],
                "priority": atom["priority"],
                "device": atom["device"],
                "jobslot": atom["jobslot"],
            }
            if atom["atom_index"] in roots:
                normalized["job_root"] = roots[atom["atom_index"]]
            if include_addresses:
                normalized["jc"] = atom.get("jc")
                normalized["seq_nr"] = atom.get("seq_nr")
            normalized_atoms.append(normalized)

        normalized_submits.append(
            {
                "index": submit_index,
                "nr_atoms": submit["nr_atoms"],
                "stride": submit["stride"],
                "atoms": normalized_atoms,
                "jobs": normalized_jobs,
            }
        )

    normalized_allocations = []
    for allocation, name in canonical_maps.items():
        source = allocations.get(allocation, {})
        mapping = mappings.get(allocation, {})
        normalized_allocations.append(
            {
                "map": name,
                "kind": source.get("kind", "unknown"),
                "pages": source.get("pages"),
                "flags": source.get("flags"),
                "mapped_length": mapping.get("length"),
            }
        )

    normalized_completions = [
        {
            "atom_number": event["atom_number"],
            "event_code": event["event_code"],
            "event": EVENT_NAMES.get(event["event_code"], "unclassified"),
        }
        for event in completions
    ]

    return {
        "schema": 1,
        "labels": labels,
        "summary": {
            "events": len(events),
            "submits": len(submits),
            "atoms": sum(len(value) for value in atoms.values()),
            "jobs": sum(len(value) for value in jobs.values()),
            "unmapped_jobs": unmapped_jobs,
            "completions": len(completions),
            "completion_failures": sum(
                event["event_code"] >= 0x40 for event in completions
            ),
            "ioctls": dict(sorted(ioctl_counts.items())),
            "ioctl_failures": dict(sorted(ioctl_failures.items())),
        },
        "allocations": normalized_allocations,
        "completions": normalized_completions,
        "submits": normalized_submits,
    }


def semantic_projection(trace):
    submits = []
    for submit in trace["submits"]:
        submits.append(
            {
                "nr_atoms": submit["nr_atoms"],
                "stride": submit["stride"],
                "atoms": [
                    {
                        "atom_index": atom["atom_index"],
                        "core_req": atom["core_req"],
                        "dependencies": atom["dependencies"],
                        "priority": atom["priority"],
                        "device": atom["device"],
                        "jobslot": atom["jobslot"],
                    }
                    for atom in submit["atoms"]
                ],
                "jobs": [
                    {
                        "atom_index": job["atom_index"],
                        "chain_index": job["chain_index"],
                        "mapped": job["mapped"],
                        "header": job.get("header"),
                    }
                    for job in submit["jobs"]
                ],
            }
        )
    return submits


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace")
    parser.add_argument("-o", "--output")
    parser.add_argument("--include-bytes", action="store_true")
    parser.add_argument("--include-addresses", action="store_true")
    arguments = parser.parse_args()
    normalized = canonicalize(
        read_events(arguments.trace),
        include_bytes=arguments.include_bytes,
        include_addresses=arguments.include_addresses,
    )
    rendered = json.dumps(normalized, indent=2, sort_keys=True) + "\n"
    if arguments.output:
        Path(arguments.output).write_text(rendered)
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()
