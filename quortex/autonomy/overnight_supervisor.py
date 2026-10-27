#!/usr/bin/env python3
"""Bounded, single-instance QUORTEX overnight Codex runner.

This process deliberately runs sequential workers only.  It never runs model
inference itself and never pushes to GitHub.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timedelta, timezone
try:
    from quortex.autonomy.acceptance import two_token_pass
except ModuleNotFoundError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from quortex.autonomy.acceptance import two_token_pass

ROOT = Path("/home/user/quortex")
AUTONOMY = ROOT / "quortex" / "autonomy"
STATE = AUTONOMY / "overnight_state.json"
LOCK = AUTONOMY / "overnight.lock"
SESSION = "quortex-overnight"
WORKER_TIMEOUT = 55 * 60
POLL_SECONDS = 5

TASKS = [
    {
        "id": "QIES-060-TWO-TOKEN-COMPARE",
        "title": "Run the first guarded two-token numerical comparison",
        "prompt": """Execute the first guarded Cortex/QUORTEX two-token numerical comparison using the existing diagnostic modes. Inspect the current QIES-060 implementation first. Do not push to GitHub, do not run competing processes, and do not retry an unchanged failed inference. Preserve MemoryMax=2G, MemorySwapMax=0, TasksMax=64, RLIMIT_AS=1536 MiB, and 90 seconds per inference process. If preflight is unsafe, stop model execution and record the exact blocker. Compare genuine position-0 and position-1 state/output captures, report evidence, and make only a narrowly justified source fix if static diagnosis proves one is needed. Run lightweight tests afterward.""",
    },
    {
        "id": "QIES-060-DIAGNOSE-TWO-TOKEN",
        "title": "Diagnose or repair the first two-token blocker",
        "prompt": """Review the prior two-token comparison evidence. If it failed, diagnose the earliest concrete state, capture, or reference mismatch without repeating the same inference. Implement the smallest safe correction, preserving QIES-056 layer-0 correctness and all memory guards. If it passed, freeze the evidence and prepare the next sequence-length test. Do not push to GitHub or run inference without a new justified test case.""",
    },
    {
        "id": "QIES-061-LONGER-SEQUENCE",
        "title": "Extend verified persistent state to positions 2 through 5",
        "prompt": """Advance from a verified two-token result toward positions 2-5 using one persistent context and real state. First inspect and preserve prior evidence. Use at most one guarded execution per new explicitly named case, no automatic retries, and the established memory limits. Compare matching Cortex and QUORTEX checkpoints and stop at the first divergence. Do not push to GitHub.""",
    },
    {
        "id": "QIES-062-FULL-LAYER-TRAVERSAL",
        "title": "Prepare and verify extension toward all 48 layers",
        "prompt": """Investigate the smallest next implementation step from verified short-sequence state continuity toward all 48 FlashNext layers. Prefer code and focused tests over broad documentation. Do not claim full-model inference until actual multi-token, multi-layer outputs are captured and compared. Preserve memory guards, avoid retries, and do not push to GitHub.""",
    },
    {
        "id": "QIES-063-PERSISTENT-PERFORMANCE",
        "title": "Profile the verified persistent path",
        "prompt": """Only if correctness evidence is intact, profile persistent-context overhead, repeated initialization, tensor-index parsing, duplicate reads, and buffer reuse. Measure rather than speculate. Do not trade away correctness, do not push to GitHub, and do not run unbounded model work.""",
    },
]


def now() -> datetime:
    return datetime.now(timezone.utc)


def atomic_write(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, sort_keys=True)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(name, path)
    finally:
        try:
            os.unlink(name)
        except FileNotFoundError:
            pass


def load_state(start: datetime, deadline: datetime) -> dict:
    if STATE.exists():
        try:
            prior = json.loads(STATE.read_text())
        except (OSError, json.JSONDecodeError):
            prior = {}
    else:
        prior = {}
    fresh = prior.get("status") in {"COMPLETE", "BLOCKED", "DEADLINE", "STOPPED"} or prior.get("deadline_utc", "") <= start.isoformat()
    return {
        "schema": 1,
        "runner": "quortex-overnight",
        "start_utc": start.isoformat(),
        "deadline_utc": deadline.isoformat(),
        "status": "RUNNING",
        "task_index": 0 if fresh else int(prior.get("task_index", 0)),
        "cycles_completed": 0 if fresh else int(prior.get("cycles_completed", 0)),
        "no_progress": 0 if fresh else int(prior.get("no_progress", 0)),
        "two_token_gate": None if fresh else prior.get("two_token_gate"),
        "two_token_gate_reasons": [] if fresh else prior.get("two_token_gate_reasons", []),
        "current_task": None,
        "worker_pid": None,
        "last_returncode": None,
        "history": prior.get("history", []),
        "safety": {
            "no_github_push": True,
            "max_worker_seconds": WORKER_TIMEOUT,
            "inference_memory_max": "2G",
            "inference_swap_max": "0",
            "inference_tasks_max": 64,
            "inference_rlimit_as": "1536MiB",
            "inference_timeout_seconds": 90,
        },
    }


def append_log(log: Path, text: str) -> None:
    with log.open("a", encoding="utf-8") as f:
        f.write(text)
        if not text.endswith("\n"):
            f.write("\n")
        f.flush()


def stop_handler(signum, _frame):
    raise KeyboardInterrupt(f"signal {signum}")


def ensure_host_inference_service(log: Path) -> bool:
    """Start the allowlisted service in the host user manager, outside Codex."""
    check = subprocess.run(["systemctl", "--user", "--machine=user@.host", "is-active", "--quiet", "quortex-inference-service.service"])
    if check.returncode == 0:
        append_log(log, "HOST_INFERENCE_SERVICE=already-active")
        return True
    command = [
        "systemd-run", "--user", "--machine=user@.host", "--unit=quortex-inference-service.service",
        "--collect", "--working-directory=/home/user/quortex", "--no-block", "--",
        "/usr/bin/python3", str(AUTONOMY / "inference_service.py"),
    ]
    result = subprocess.run(command, text=True, capture_output=True)
    append_log(log, f"HOST_INFERENCE_SERVICE_START rc={result.returncode} stdout={result.stdout.strip()} stderr={result.stderr.strip()}")
    return result.returncode == 0


def run(args: argparse.Namespace) -> int:
    AUTONOMY.mkdir(parents=True, exist_ok=True)
    start = now()
    deadline = start + timedelta(hours=args.hours)
    log = AUTONOMY / "logs" / f"overnight-{start.strftime('%Y%m%dT%H%M%SZ')}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    with LOCK.open("a+") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            append_log(log, "REFUSED: another overnight supervisor holds the lock")
            return 2
        state = load_state(start, deadline)
        state["log"] = str(log)
        state["pid"] = os.getpid()
        atomic_write(STATE, state)
        append_log(log, f"START pid={os.getpid()} start={start.isoformat()} deadline={deadline.isoformat()}")
        if not ensure_host_inference_service(log):
            state["status"] = "BLOCKED"
            state["blocker"] = "host inference service could not be started"
            atomic_write(STATE, state)
            return 1
        signal.signal(signal.SIGTERM, stop_handler)
        signal.signal(signal.SIGINT, stop_handler)
        try:
            while now() < deadline and state["task_index"] < len(TASKS):
                task = TASKS[state["task_index"]]
                state["current_task"] = task["id"]
                state["worker_pid"] = None
                atomic_write(STATE, state)
                prompt = f"""You are the bounded QUORTEX overnight engineering worker.\nRepository: {ROOT}\nBranch must remain qies-059-position1 unless a local development branch is already required.\nRead QIES standards and current project state before acting.\nThis is sequential cycle {state['cycles_completed'] + 1}, task {task['id']}: {task['title']}.\n{task['prompt']}\nSubmit model runs through /home/user/quortex/quortex/autonomy/request_client.py using its allowlisted operations; do not invoke systemd-run or inference binaries directly. For QIES-060, write the independently verifiable gate manifest to /home/user/quortex/quortex/autonomy/evidence/QIES-060-TWO-TOKEN.json with the required exit statuses, capture inventory, state_continuity boolean, and numerical_comparison value. At the end, run relevant tests and summarize exact results. Do not claim equivalence without evidence."""
                cmd = [
                    "codex", "exec", "-C", str(ROOT), "--add-dir", "/home/user/bitnet-cortex",
                    "--approve-for-me", "--skip-git-repo-check", prompt,
                ]
                append_log(log, f"\nCYCLE_START task={task['id']} index={state['task_index']}\nCOMMAND={' '.join(cmd[:12])} ...\n")
                before = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True, capture_output=True).stdout.strip()
                try:
                    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
                    state["worker_pid"] = proc.pid
                    atomic_write(STATE, state)
                    try:
                        text, _ = proc.communicate(timeout=WORKER_TIMEOUT)
                        output = text.splitlines()
                        for line in output:
                            append_log(log, line)
                        rc = proc.returncode
                    except subprocess.TimeoutExpired as exc:
                        proc.terminate()
                        append_log(log, "WORKER_TIMEOUT: terminated gracefully")
                        try:
                            text, _ = proc.communicate(timeout=30)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            text, _ = proc.communicate()
                        output = (exc.output or b"").decode(errors="replace").splitlines() if isinstance(exc.output, bytes) else (exc.output or "").splitlines()
                        rc = 124
                except OSError as exc:
                    append_log(log, f"CLI_ERROR: {exc}")
                    state["status"] = "BLOCKED"
                    state["current_task"] = None
                    atomic_write(STATE, state)
                    return 1
                after = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True, capture_output=True).stdout.strip()
                state["worker_pid"] = None
                state["last_returncode"] = rc
                record = {"task": task["id"], "cycle": state["cycles_completed"] + 1, "returncode": rc, "start": now().isoformat(), "head_before": before, "head_after": after}
                state["history"].append(record)
                if rc != 0:
                    append_log(log, f"STOP: codex exec returned {rc}; no automatic retry")
                    state["status"] = "BLOCKED"
                    state["current_task"] = None
                    atomic_write(STATE, state)
                    return 1
                state["cycles_completed"] += 1
                if task["id"] == "QIES-060-TWO-TOKEN-COMPARE":
                    passed, reasons = two_token_pass(ROOT)
                    state["two_token_gate"] = "PASS" if passed else "BLOCKED"
                    state["two_token_gate_reasons"] = reasons
                    if not passed:
                        append_log(log, "ACCEPTANCE_BLOCKED: " + "; ".join(reasons))
                    else:
                        append_log(log, "ACCEPTANCE_PASS: QIES-060-TWO-TOKEN")
                elif task["id"] in {"QIES-061-LONGER-SEQUENCE", "QIES-062-FULL-LAYER-TRAVERSAL", "QIES-063-PERSISTENT-PERFORMANCE"} and state.get("two_token_gate") != "PASS":
                    append_log(log, "STOP: dependent task cannot run before two-token acceptance PASS")
                    state["status"] = "BLOCKED"
                    state["current_task"] = None
                    atomic_write(STATE, state)
                    return 1
                if after == before and not any(marker in "".join(output[-20:]) for marker in ("PASS", "COMPLETED", "implemented", "evidence")):
                    state["no_progress"] += 1
                else:
                    state["no_progress"] = 0
                if state["no_progress"] >= 2:
                    append_log(log, "STOP: repeated no-progress cycles")
                    state["status"] = "BLOCKED"
                    state["current_task"] = None
                    atomic_write(STATE, state)
                    return 1
                if task["id"] == "QIES-060-TWO-TOKEN-COMPARE" and state.get("two_token_gate") == "PASS":
                    state["task_index"] = 2
                else:
                    state["task_index"] += 1
                if task["id"] == "QIES-060-DIAGNOSE-TWO-TOKEN" and state.get("two_token_gate") != "PASS":
                    append_log(log, "STOP: diagnostic cycle completed without acceptance evidence")
                    state["status"] = "BLOCKED"
                    state["current_task"] = None
                    atomic_write(STATE, state)
                    return 1
                state["current_task"] = None
                atomic_write(STATE, state)
                append_log(log, f"CYCLE_ACCEPTED task={task['id']} next_index={state['task_index']}")
            state["status"] = "DEADLINE" if now() >= deadline else "COMPLETE"
            state["current_task"] = None
            atomic_write(STATE, state)
            append_log(log, f"STOP status={state['status']} cycles={state['cycles_completed']}")
            return 0
        except KeyboardInterrupt as exc:
            state["status"] = "STOPPED"
            state["current_task"] = None
            state["worker_pid"] = None
            atomic_write(STATE, state)
            append_log(log, f"STOPPED: {exc}")
            return 130


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hours", type=float, default=8.0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.dry_run:
        print(json.dumps({"session": SESSION, "tasks": [t["id"] for t in TASKS], "worker_timeout": WORKER_TIMEOUT}, indent=2))
        return 0
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
