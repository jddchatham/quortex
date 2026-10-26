"""Persistent QIES Director backlog selection and bounded failure handling."""

from __future__ import annotations

import json
import os
from pathlib import Path


def load(path: Path) -> dict:
    return json.loads(path.read_text())


def save(path: Path, data: dict) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, path)


def eligible(data: dict) -> list[dict]:
    done = {a["id"] for a in data["assignments"] if a["status"] == "completed"}
    return [a for a in data["assignments"] if a["status"] in {"pending", "in_progress"} and set(a["dependencies"]).issubset(done)]


def select(data: dict) -> dict | None:
    choices = eligible(data)
    if not choices:
        return None
    return sorted(choices, key=lambda a: (a["status"] != "in_progress", a["priority"]))[0]


def begin(data: dict, assignment_id: str) -> dict:
    for assignment in data["assignments"]:
        if assignment["id"] == assignment_id:
            assignment["status"] = "in_progress"
            assignment["attempts"] += 1
            return assignment
    raise KeyError(assignment_id)


def complete(data: dict, assignment_id: str, evidence: str, commit: str | None = None) -> dict:
    for assignment in data["assignments"]:
        if assignment["id"] == assignment_id:
            assignment["status"] = "completed"
            assignment["evidence"].append(evidence)
            assignment["source_commit"] = commit
            return assignment
    raise KeyError(assignment_id)


def fail(data: dict, assignment_id: str, reason: str, max_attempts: int = 3) -> dict:
    for assignment in data["assignments"]:
        if assignment["id"] == assignment_id:
            assignment["evidence"].append(reason)
            if assignment["attempts"] >= max_attempts:
                assignment["status"] = "blocked"
                assignment["next_action"] = "Diagnose blocker or obtain required authorization"
            return assignment
    raise KeyError(assignment_id)

