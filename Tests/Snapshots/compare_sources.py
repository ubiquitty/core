#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Compare a composed split catalog to an existing combined checkout's runner.

Imports the reference runner in place; neither runners nor baseline files are
copied or rewritten. No renderer is launched. Both actual loader contracts,
configuration bytes, golden ownership/content and policy bytes must agree.
"""

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import sys
import tempfile

import run


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def logical_golden(path, directories):
    if path is None:
        return None
    matches = [str(path.relative_to(directory)) for directory in directories if path.is_relative_to(directory)]
    if len(matches) != 1:
        raise ValueError(f"golden has no single catalog owner: {path}")
    return matches[0]


def contract(regressions, directories):
    result = {}
    for regression in regressions:
        record = dict(vars(regression))
        golden = record.pop("golden")
        record["golden"] = logical_golden(golden, directories)
        record["golden_sha256"] = digest(golden) if golden else None
        if record["case"] is not None:
            case = dict(record["case"])
            case["config"] = Path(case["config"]).read_text()
            record["case"] = case
        result[regression.identifier] = record
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--core-source-root", required=True)
    parser.add_argument("--kit-source-root", required=True)
    parser.add_argument("--reference-root", required=True)
    parser.add_argument("--backend", choices=run.ALL_BACKENDS, default="swift")
    parser.add_argument("--expected-comparisons", type=int, required=True)
    args = parser.parse_args()
    # The reference may be the live normal-hook checkout. Import/probe caches
    # and generated configurations belong only to this comparison's scratch.
    sys.dont_write_bytecode = True
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    reference_root = Path(args.reference_root).resolve()
    reference_path = reference_root / "Tests" / "Snapshots" / "run.py"
    specification = importlib.util.spec_from_file_location("snapshot_reference_runner", reference_path)
    if specification is None or specification.loader is None:
        raise ValueError(f"reference runner is unavailable: {reference_path}")
    reference = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = reference
    specification.loader.exec_module(reference)
    run.configure_sources(args.source_root, args.core_source_root, args.kit_source_root)
    actual = run.load_regressions()
    actual_contract = contract(actual, run.SOURCE_FILES.directories)
    with tempfile.TemporaryDirectory(prefix="ubiquitty-reference-configs-") as scratch:
        reference._CONFIG_DIRECTORY = Path(scratch)
        expected = reference.load_regressions()
        expected_contract = contract(expected, (reference.HERE,))
    if actual_contract != expected_contract:
        differences = sorted(name for name in actual_contract.keys() | expected_contract.keys()
                             if actual_contract.get(name) != expected_contract.get(name))
        raise ValueError("composed catalog differs from original: " + ", ".join(differences))
    patterns = ("known-failing-*.txt", "exempt-*.txt", "approved.tsv")
    actual_policy = {path.name: path.read_bytes() for pattern in patterns for path in run.manifest_paths(pattern)}
    expected_policy = {path.name: path.read_bytes() for pattern in patterns for path in reference.HERE.glob(pattern)}
    if actual_policy != expected_policy:
        raise ValueError("composed waiver/exemption/approval files differ from original")
    for constant in ("CHANNEL_TOLERANCE", "MAX_DIFFERING_FRACTION", "REPAINT_DELTA", "MAX_REPAINTED_PIXELS", "STRICT_MAX_REPAINTED_PIXELS"):
        if getattr(run, constant) != getattr(reference, constant):
            raise ValueError(f"pixel policy differs from original: {constant}")
    for module, catalog in ((run, actual), (reference, expected)):
        exempt = module.exempt_identifiers(args.backend)
        comparisons = sum(item.backend == args.backend and item.identifier not in exempt for item in catalog)
        if comparisons != args.expected_comparisons:
            raise ValueError(f"{args.backend} comparison count is {comparisons}, expected {args.expected_comparisons}")
    print(f"PASS: {len(actual_contract)} identical catalog entries, original config/golden bytes and all policy files; "
          f"{args.backend} has {args.expected_comparisons} comparisons")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f"REFUSED: {error}", file=sys.stderr)
        sys.exit(1)
