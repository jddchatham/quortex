#!/usr/bin/env python3
"""Bounded, recoverable QIES supervisor.

One invocation owns one lock and runs at most ``--cycles`` Codex cycles.  The
systemd unit may restart this process, but the supervisor itself never creates
an uncontrolled process tree.  It records every attempt before selecting the
next assignment.  Codex is invoked through documented ``codex exec`` flags;
this program does not bypass approvals or sandbox policy.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from typing import Iterator


ROOT = Path(__file__).resolve().parents[2]
AUTONOMY = ROOT / "quortex" / "autonomy"
STATE_PATH = AUTONOMY / "state.json"
LOG_DIR = AUTONOMY / "logs"
LOCK_PATH = AUTONOMY / "supervisor.lock"
EVENTS_PATH = AUTONOMY / "events.jsonl"
STOP_PATH = AUTONOMY / "stop.requested"


@dataclass(frozen=True)
class Config:
    max_runtime: int = 900
    backoff_base: int = 10
    backoff_max: int = 300
    max_retries: int = 2
    memory_mb: int = 2048
    max_cycles: int = 1
    dry_run: bool = False


def load_state() -> dict:
    return json.loads(STATE_PATH.read_text())


def save_state(state: dict) -> None:
    temporary = STATE_PATH.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, STATE_PATH)


def event(payload: dict) -> None:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    record = {"time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), **payload}
    with EVENTS_PATH.open("a") as stream:
        stream.write(json.dumps(record, sort_keys=True) + "\n")


@contextmanager
def single_instance() -> Iterator[None]:
    AUTONOMY.mkdir(parents=True, exist_ok=True)
    with LOCK_PATH.open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise RuntimeError("another QUORTEX supervisor is already running") from exc
        yield


def stop_requested() -> bool:
    return STOP_PATH.exists()


def prompt_for(state: dict) -> str:
    assignment = state.get("assignment", "No assignment recorded")
    return f"""You are executing one bounded QIES-001 engineering cycle for QUORTEX.

Read QUORTEX.md, QUORTEX_GENESIS.md, all docs/QIES_*.md, and the current Git state.
You are one coordinated team of six personas: Director, Research Scientist,
Implementation Engineer, Verification Officer, Performance Engineer, and
Continuity Engineer. Apply separation of duties internally and report evidence.

Current durable assignment: {assignment}

Do exactly one bounded assignment cycle. Prioritize working inference engineering
over documentation. Inspect before editing. Preserve Cortex and do not add model
weights, checkpoints, credentials, or generated binaries. Use small bounded tests.
Do not claim success without executable evidence. If the assignment cannot be
completed safely, document the blocker and leave the tree recoverable.
Run relevant tests, update the QIES logs/state, and commit and push only verified
source/docs changes to origin/quortex; never force-push. Do not start large-model
runs unless the assignment explicitly justifies them.
"""


def run_codex(state: dict, config: Config, cycle: int, retry: int) -> tuple[int, Path]:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    output_path = LOG_DIR / f"cycle-{cycle:03d}-retry-{retry}-{stamp}.jsonl"
    command = [
        "codex", "exec",
        "-C", str(ROOT),
        "-s", "workspace-write",
        "--json",
        "--ephemeral",
        "-o", str(output_path.with_suffix(".last.txt")),
        prompt_for(state),
    ]
    event({"kind": "cycle-start", "cycle": cycle, "retry": retry, "command": command[:-1]})
    if config.dry_run:
        output_path.write_text(json.dumps({"dry_run": True, "assignment": state.get("assignment")}) + "\n")
        return 0, output_path
    with output_path.open("w") as log:
        try:
            result = subprocess.run(
                command,
                cwd=ROOT,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=config.max_runtime,
                check=False,
            )
            code = result.returncode
        except subprocess.TimeoutExpired:
            log.write("SUPERVISOR_TIMEOUT\n")
            code = 124
    event({"kind": "cycle-finish", "cycle": cycle, "retry": retry, "returncode": code, "log": str(output_path)})
    return code, output_path


def record_cycle(state: dict, cycle: int, code: int, log: Path, config: Config) -> None:
    history = state.setdefault("supervisor_history", [])
    history.append({
        "cycle": cycle,
        "returncode": code,
        "log": str(log.relative_to(ROOT)),
        "time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "dry_run": config.dry_run,
    })
    state["last_supervisor_cycle"] = cycle
    state["last_supervisor_returncode"] = code
    state["last_supervisor_log"] = str(log.relative_to(ROOT))
    state["supervisor_status"] = "cycle-succeeded" if code == 0 else "cycle-failed"
    save_state(state)


def run(config: Config) -> int:
    if config.max_cycles < 1:
        raise ValueError("--cycles must be positive")
    try:
        with single_instance():
            if STOP_PATH.exists():
                STOP_PATH.unlink()
            state = load_state()
            event({"kind": "supervisor-start", "cycles": config.max_cycles, "dry_run": config.dry_run})
            for cycle in range(1, config.max_cycles + 1):
                if stop_requested():
                    event({"kind": "supervisor-stop-requested", "cycle": cycle})
                    return 0
                success = False
                for retry in range(config.max_retries + 1):
                    code, log = run_codex(state, config, cycle, retry)
                    record_cycle(state, cycle, code, log, config)
                    if code == 0:
                        success = True
                        break
                    if retry < config.max_retries and not stop_requested():
                        delay = min(config.backoff_base * (2 ** retry), config.backoff_max)
                        event({"kind": "retry-backoff", "seconds": delay, "cycle": cycle, "retry": retry})
                        time.sleep(delay)
                if not success:
                    state["supervisor_status"] = "paused-unrecoverable-failure"
                    save_state(state)
                    event({"kind": "supervisor-paused", "cycle": cycle})
                    return 1
                state = load_state()
            event({"kind": "supervisor-complete", "cycles": config.max_cycles})
            return 0
    except RuntimeError as exc:
        event({"kind": "supervisor-lock-failure", "error": str(exc)})
        return 75


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=int, default=int(os.environ.get("QUORTEX_MAX_CYCLES", "1")))
    parser.add_argument("--max-runtime", type=int, default=int(os.environ.get("QUORTEX_MAX_RUNTIME", "900")))
    parser.add_argument("--max-retries", type=int, default=int(os.environ.get("QUORTEX_MAX_RETRIES", "2")))
    parser.add_argument("--backoff-base", type=int, default=10)
    parser.add_argument("--backoff-max", type=int, default=300)
    parser.add_argument("--memory-mb", type=int, default=2048, help="recorded limit; systemd enforces service MemoryMax")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--request-stop", action="store_true")
    args = parser.parse_args(argv)
    if args.request_stop:
        STOP_PATH.touch()
        return 0
    return run(Config(args.max_runtime, args.backoff_base, args.backoff_max, args.max_retries, args.memory_mb, args.cycles, args.dry_run))


if __name__ == "__main__":
    raise SystemExit(main())
