#!/usr/bin/env python3
"""Allowlisted, resource-bounded launcher for approved FlashNext diagnostics."""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import time

ROOT = Path("/home/user/quortex").resolve()
LOCK = ROOT / "quortex" / "autonomy" / "inference.lock"
ALLOWED = {
    "quortex": ROOT / "native" / "debug" / "quortex-flashnext-layer0",
    "quortex-build": ROOT / "native" / "build" / "quortex-flashnext-layer0",
    "chain": ROOT / "native" / "debug" / "quortex-flashnext-chain",
    "reference": Path("/home/user/bitnet-cortex/cortex-stream-forge/runtime-flashnext-layer-exp-build/bin/llama-flashnext-owned-layer"),
    "two-range": Path("/tmp/quortex-qies109-wt.Tgz8zp/build-qies116/bin/llama-flashnext-two-range"),
}
APPROVED_SHA256 = {
    "two-range": "41ad54216564564e17610a6bee5de14e4a4fe04f23d97f8db2a526575d250c75",
}
APPROVED_LIBRARY_SHA256 = {
    "two-range": "f5f72adbb22bfdfb86c82b64942a6bcb47716e5383d2d266b49acfad4e7fc649",
}
APPROVED_DEPENDENCY_SHA256 = {
    "two-range": {
        "libggml-base.so.0.25.3": "426046f381f53747aa42c46686fcebafb3b3d3f7a2993545a56f171259ba9db2",
    },
}
MAX_AS = 1536 * 1024 * 1024
TIMEOUT = 90
ALLOWED_ENV = {
    "CORTEX_EXTERNAL_GLOBAL_TENSORS", "CORTEX_FLASHNEXT_LAYER_START",
    "CORTEX_FLASHNEXT_LAYER_END", "CORTEX_FLASHNEXT_LAYER_RANGE",
    "CORTEX_FLASHNEXT_PRE_MOE", "CORTEX_FLASHNEXT_SEGMENT_B",
    "CORTEX_FLASHNEXT_SEGMENT_B_INPUT_WIDTH", "CORTEX_FLASHNEXT_INTERNAL_TENSOR_DIAGNOSTICS",
    "CORTEX_REFERENCE_TWO_TOKEN_ONLY", "CORTEX_FLASHNEXT_CHAIN_1_2", "QUORTEX_TWO_TOKEN_STATE",
    "QUORTEX_FULL_LAYER0_STREAMED", "CORTEX_FLASHNEXT_TWO_RANGE", "FLASHNEXT_TENSOR_INDEX", "FLASHNEXT_COMPARE_DIR",
    "QUORTEX_CHAIN_RANGE",
}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def validate(args: argparse.Namespace) -> tuple[Path, Path, Path, dict[str, str]]:
    exe = ALLOWED.get(args.target)
    if exe is None or not exe.is_file() or not os.access(exe, os.X_OK):
        raise ValueError(f"target is not an approved executable: {args.target}")
    expected_hash = APPROVED_SHA256.get(args.target)
    if expected_hash is not None and sha256(exe) != expected_hash:
        raise ValueError("approved executable hash mismatch")
    expected_library_hash = APPROVED_LIBRARY_SHA256.get(args.target)
    if expected_library_hash is not None:
        library = exe.parent / "libllama.so"
        if not library.is_file() or sha256(library) != expected_library_hash:
            raise ValueError("approved Cortex library hash mismatch")
    for name, expected_hash in APPROVED_DEPENDENCY_SHA256.get(args.target, {}).items():
        dependency = exe.parent / name
        if not dependency.is_file() or sha256(dependency) != expected_hash:
            raise ValueError(f"approved dependency hash mismatch: {name}")
    model = Path(args.model).resolve()
    if not model.is_file() or not model.name.endswith(".gguf"):
        raise ValueError("model must be an existing GGUF file")
    out = Path(args.output).resolve()
    if out == ROOT or ROOT not in out.parents:
        raise ValueError("output must be below the QUORTEX repository")
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        raise ValueError("output directory must be fresh and empty")
    index = Path(args.index).resolve() if args.index else None
    if index is not None and (not index.is_file() or index.suffix != ".json"):
        raise ValueError("tensor index must be an existing JSON file")
    if args.target in ("reference", "chain") and index is None:
        raise ValueError("target requires a tensor index")
    env = {k: v for k, v in os.environ.items() if k in ALLOWED_ENV}
    env.update({"FLASHNEXT_COMPARE_DIR": str(out)})
    if args.host_context:
        # Fixed host user-manager endpoints; never accept these from a request.
        env.update({"XDG_RUNTIME_DIR": "/run/user/1000", "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus"})
    if index:
        env["FLASHNEXT_TENSOR_INDEX"] = str(index)
    if args.target == "two-range":
        env["CORTEX_FLASHNEXT_TWO_RANGE"] = "1"
        env["CORTEX_FLASHNEXT_GRAPH_FORENSICS"] = "1"
    elif args.target.startswith("quortex") or args.target == "chain":
        env.update({"QUORTEX_TWO_TOKEN_STATE": "1", "CORTEX_FLASHNEXT_LAYER_START": "0", "CORTEX_FLASHNEXT_LAYER_END": "1", "CORTEX_FLASHNEXT_LAYER_RANGE": "1"})
    else:
        if args.mode == "two-layer":
            env["CORTEX_FLASHNEXT_CHAIN_1_2"] = "1"
        else:
            env["CORTEX_REFERENCE_TWO_TOKEN_ONLY"] = "1"
    return exe, model, out, env


def set_limits() -> None:
    resource.setrlimit(resource.RLIMIT_AS, (MAX_AS, MAX_AS))


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--target", choices=sorted(ALLOWED))
    p.add_argument("--model", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--index")
    p.add_argument("--evidence", required=True)
    p.add_argument("--host-context", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--mode", choices=["two-token", "two-layer"], default="two-token", help=argparse.SUPPRESS)
    args = p.parse_args()
    try:
        exe, model, out, env = validate(args)
    except ValueError as exc:
        print(f"GUARD_REJECTED: {exc}", file=sys.stderr)
        return 2
    LOCK.parent.mkdir(parents=True, exist_ok=True)
    with LOCK.open("a+") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("GUARD_REJECTED: another inference is running", file=sys.stderr)
            return 3
        start = time.monotonic()
        command = [str(exe), str(model)]
        if args.target == "two-range":
            command.append(str(out / "layer0-1-output.bin"))
        if args.target == "chain":
            command += [str(Path(args.index).resolve()), str(out)]
        if args.target == "reference":
            command.append(str(Path(args.index).resolve()))
        evidence = {"target": args.target, "command": command, "model": str(model), "model_sha256": sha256(model), "output": str(out), "rlimit_as_bytes": MAX_AS, "timeout_seconds": TIMEOUT, "cgroup_required": {"MemoryMax": "2G", "MemorySwapMax": "0", "TasksMax": 64}, "environment": env}
        # systemd-run is mandatory: RLIMIT_AS alone does not satisfy the physical-memory gate.
        # systemd-run starts the unit from the user manager environment; the
        # Python client's env is not inherited. Pass only the already-filtered
        # diagnostic variables explicitly, so capture paths and mode flags
        # cannot silently disappear at the privilege boundary.
        systemd = ["systemd-run", "--user", "--quiet", "--wait", "--pipe", "--collect", "-p", "MemoryMax=2G", "-p", "MemorySwapMax=0", "-p", "TasksMax=64"]
        for key, value in sorted(env.items()):
            systemd += [f"--setenv={key}={value}"]
        systemd += ["--"] + command
        if not args.host_context:
            systemd.insert(3, "--machine=user@.host")
        try:
            proc = subprocess.run(systemd, cwd=ROOT, env=env, preexec_fn=set_limits, timeout=TIMEOUT, text=True, capture_output=True)
            evidence.update({"returncode": proc.returncode, "stdout": proc.stdout, "stderr": proc.stderr, "wall_seconds": time.monotonic() - start})
        except (subprocess.TimeoutExpired, OSError) as exc:
            evidence.update({"returncode": 124, "error": repr(exc), "wall_seconds": time.monotonic() - start})
            proc = None
        Path(args.evidence).resolve().parent.mkdir(parents=True, exist_ok=True)
        Path(args.evidence).resolve().write_text(json.dumps(evidence, indent=2) + "\n")
        return int(evidence["returncode"])


if __name__ == "__main__":
    raise SystemExit(main())
