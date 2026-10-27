#!/usr/bin/env python3
"""Independent evidence gates for autonomous roadmap progression."""
from __future__ import annotations
import json
from pathlib import Path


def two_token_pass(root: str | Path) -> tuple[bool, list[str]]:
    path = Path(root) / "quortex" / "autonomy" / "evidence" / "QIES-060-TWO-TOKEN.json"
    errors: list[str] = []
    if not path.is_file():
        return False, [f"missing evidence manifest: {path}"]
    try:
        doc = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        return False, [f"invalid evidence manifest: {exc}"]
    if doc.get("status") != "PASS": errors.append("status is not PASS")
    if doc.get("reference_exit_status") != 0: errors.append("reference did not exit 0")
    if doc.get("quortex_exit_status") != 0: errors.append("QUORTEX did not exit 0")
    if doc.get("state_continuity") is not True: errors.append("state continuity is not proven")
    if doc.get("numerical_comparison") != "PASS": errors.append("numerical comparison is not PASS")
    for name in ("reference", "quortex"):
        if not isinstance(doc.get(name, {}).get("captures"), list) or not doc[name]["captures"]:
            errors.append(f"{name} captures are missing")
    return not errors, errors
