#!/usr/bin/env python3
"""Regenerate the PTO 0.58.3 CUBE type matrix from locked authority."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def expected_contract(authority: dict[str, object], current: dict[str, object]) -> dict[str, object]:
    classes = authority["ordinary_classes"]
    ordinary = [name for values in classes.values() for name in values]
    ordinary_pairs = sum(len(values) ** 2 for values in classes.values())
    mx_types = authority["mx_side_types"]
    result = dict(current)
    result.update({
        "ordinary_side_types": ordinary,
        "ordinary_pair_rule": "same-class ordered pairs",
        "ordinary_ordered_operand_pairs": ordinary_pairs,
        "ordinary_architectural_acc_types": list(
            authority["ordinary_accumulator_by_class"].values()
        ),
        "ordinary_internal_acc_types": ["FP32", "S64", "U64"],
        "mx_side_types": mx_types,
        "mx_scaled_side_types": authority["mx_scaled_side_types"],
        "mx_ordered_operand_pairs": len(mx_types) ** 2,
        "mx_acc_type": authority["mx_accumulator"],
        "mx_scale_type": authority["mx_scale"],
    })
    return result


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    authority_path = root / "tests/linxisa/pto-v0583-matrix-type-authority.json"
    conformance_path = root / "tests/linxisa/pto-v058-cube-profile-conformance.json"
    authority = json.loads(authority_path.read_text(encoding="utf-8"))
    conformance = json.loads(conformance_path.read_text(encoding="utf-8"))
    expected = expected_contract(authority, conformance["normative_contract"])
    if conformance["normative_contract"] == expected:
        print("ok: PTO 0.58.3 CUBE conformance matches locked type authority")
        return 0
    if args.check:
        raise SystemExit("error: CUBE conformance JSON is stale")
    conformance["normative_contract"] = expected
    conformance_path.write_text(
        json.dumps(conformance, indent=2) + "\n", encoding="utf-8"
    )
    print(f"updated {conformance_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
