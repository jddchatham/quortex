#!/usr/bin/env python3
"""Darwin (macOS) resource-bounded launcher for approved QUORTEX diagnostics.

Purpose
-------
``guarded_inference.py`` relies on Linux-only machinery: ``systemd-run --user``
with ``MemoryMax``/``MemorySwapMax``/``TasksMax`` and ``RLIMIT_AS``.  Empirical
probing on macOS 15.3.1 (Apple Silicon) shows Darwin here rejects user-space
``RLIMIT_AS``/``RLIMIT_RSS``/``RLIMIT_STACK`` changes entirely, so the Linux
virtual-memory rlimit cannot be ported as-is.  This module provides the
supported macOS equivalents:

* **Resident-set (physical memory) gate**: the supervisor samples the child's
  live resident size via ``proc_pid_rusage`` (libproc, the same interface
  ``top``/``ps`` use; same-user, no privileges required) and kills the whole
  process group with SIGKILL when the cap is exceeded.  Fallback sampler:
  ``ps -o rss=`` if libproc cannot be loaded.
* **CPU backstop**: ``RLIMIT_CPU`` (verified enforced on this host: SIGXCPU).
* **Tasks analog**: ``RLIMIT_NPROC = 64``.
* **Crash artifacts off**: ``RLIMIT_CORE = 0``.
* **Wall-clock bound**: parent-side timeout with ``killpg(SIGKILL)``.

Everything else mirrors the Linux guard fail-closed semantics: fixed target
allowlist, optional SHA-256 pins, fresh-empty output directory inside the
repository, environment allowlist, single-instance flock, and a complete
evidence JSON for every attempt.  No systemd, no cgroup, no hardcoded
``/home/user`` paths.

This module is inert unless invoked; it never runs model inference by itself
and it only ever executes targets that are explicitly allowlisted below.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import fcntl
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / "quortex" / "autonomy" / "inference.darwin.lock"

# Approved executables, resolved relative to this repository.  Hashes are
# optional; when present they are enforced (fail-closed).
ALLOWED = {
    "arm64-smoke": ROOT / "native" / "build" / "macos-arm64-smoke",
    # M-MAC-01: Cortex ggml-0.25.3 diagnostic targets, built natively for
    # arm64 against cortex-stream-forge @ e3a408e (see evidence/m-mac-01/).
    # The model-taking targets receive their GGUF path as remainder argv.
    "gguf-diagnostic": ROOT / "native" / "build-mac" / "quortex-gguf-diagnostic",
    "tensor-allocation": ROOT / "native" / "build-mac" / "quortex-tensor-allocation",
    "flashnext-op": ROOT / "native" / "build-mac" / "quortex-flashnext-op",
    # M-MAC-02: deterministic inference probe against the pinned Cortex
    # llama library.  Remainder argv: MODEL.gguf N_TOKENS(1|2).
    "llama-probe": ROOT / "native" / "build-mac" / "macos-llama-probe",
    # M-MAC-04: persistent two-segment session probe. Remainder argv:
    # MODEL.gguf {session|combined|slow}.
    "llama-session": ROOT / "native" / "build-mac" / "macos-llama-session",
    # M-MAC-08: QUORTEX FlashNext layer-0 (authentic Cortex recovery).
    "flashnext-layer0": ROOT / "native" / "build-mac" / "quortex-flashnext-layer0",
}
APPROVED_SHA256 = {
    # M-MAC-01 pinned build (clang-1700 arm64, ggml 0.25.3, Cortex e3a408e).
    # Any rebuild invalidates the pin and must be re-approved with evidence.
    "gguf-diagnostic": "7e4d291fd1f47b12d2b5d3bc7c2e433dd6943b10310ba9f76fe7c81b5df5c710",
    "tensor-allocation": "0a89aebf9d86f3b57eb1a96aab6a9738c595df55d7eed65d94ab12649eb45099",
    "flashnext-op": "0eb75b386bb9a6fb0da4deb2f84745c5698a9b41a836372849227cea31fabcdd",
    # M-MAC-02 pinned probe (clang-1700 arm64, Cortex libllama 0.5.0-dev @ e3a408e).
    "llama-probe": "3c16d3124759fae4d519c5a8d883df9d5f014c65fb91e4a60f862c9c75333e15",
    # M-MAC-04 pinned session probe (Cortex libllama @ e3a408e, arm64 CPU).
    "llama-session": "a6c231f784748de799f79f3ab8616ffc10b9db12e79e4abc63699485086ab0e5",
    # M-MAC-08 pinned layer-0 (Cortex @ e3a408e + verbatim 4e8a5aa recovery).
    "flashnext-layer0": "ba8d973861c4e95ab0e39198075f2b230b190f12804dc3e3bdd19eef3a64d722",
}

DEFAULT_TIMEOUT = 60
DEFAULT_RSS_CAP_MB = 2048
NPROC_LIMIT = 64
POLL_SECONDS = 0.05

ALLOWED_ENV = {
    "FLASHNEXT_TENSOR_INDEX",
    "FLASHNEXT_COMPARE_DIR",
    "QUORTEX_TWO_TOKEN_STATE",
    "QUORTEX_FULL_LAYER0_STREAMED",
    # M-MAC-08: attestation pair the layer-0 binary requires before it will
    # accept the supervisor-enforced memory cap in place of RLIMIT_AS.
    "QUORTEX_DARWIN_SUPERVISOR",
    "QUORTEX_DARWIN_RSS_CAP_MB",
}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def validate(args: argparse.Namespace) -> tuple[Path, Path, dict[str, str]]:
    exe = ALLOWED.get(args.target)
    if exe is None or not exe.is_file() or not os.access(exe, os.X_OK):
        raise ValueError(f"target is not an approved executable: {args.target}")
    expected = APPROVED_SHA256.get(args.target)
    if expected is not None and sha256(exe) != expected:
        raise ValueError("approved executable hash mismatch")
    model = Path(args.model).resolve() if args.model else None
    if model is not None and (not model.is_file() or not model.name.endswith(".gguf")):
        raise ValueError("model must be an existing GGUF file")
    out = Path(args.output).resolve()
    if out == ROOT or ROOT not in out.parents:
        raise ValueError("output must be below the QUORTEX repository")
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        raise ValueError("output directory must be fresh and empty")
    env = {k: v for k, v in os.environ.items() if k in ALLOWED_ENV}
    env["FLASHNEXT_COMPARE_DIR"] = str(out)
    # Attest the external memory gate for targets that require it (the
    # layer-0 binary refuses to run on Darwin without this pair).
    env["QUORTEX_DARWIN_SUPERVISOR"] = "phys_footprint"
    env["QUORTEX_DARWIN_RSS_CAP_MB"] = str(args.rss_cap_mb)
    return exe, out, env


class ResidentSampler:
    """Live memory sampler for one child pid.

    Primary metric: ``ri_phys_footprint`` via ``proc_pid_rusage`` (libproc) —
    the workload's own internal memory, excluding the Darwin shared cache that
    inflates ``ru_maxrss`` to ~1.2 GB even for trivial binaries.  Fallback
    metric: ``ri_resident_size``; last resort: ``ps -o rss=``.  Returns bytes
    or None when momentarily unavailable.
    """

    METRIC = "phys_footprint"

    def __init__(self) -> None:
        self._libproc = None
        self._struct = None
        path = ctypes.util.find_library("System") or ctypes.util.find_library("libproc")
        if path:
            try:
                lib = ctypes.CDLL(path)
                lib.proc_pid_rusage.restype = ctypes.c_int
                lib.proc_pid_rusage.argtypes = [
                    ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                ]

                class RUsageInfoV0(ctypes.Structure):
                    _fields_ = [
                        ("ri_uuid", ctypes.c_ubyte * 16),
                        ("ri_user_time", ctypes.c_uint64),
                        ("ri_system_time", ctypes.c_uint64),
                        ("ri_pkg_idle_wkups", ctypes.c_uint64),
                        ("ri_interrupt_wkups", ctypes.c_uint64),
                        ("ri_pageins", ctypes.c_uint64),
                        ("ri_wired_size", ctypes.c_uint64),
                        ("ri_resident_size", ctypes.c_uint64),
                        ("ri_phys_footprint", ctypes.c_uint64),
                        ("ri_proc_start_abstime", ctypes.c_uint64),
                        ("ri_proc_exit_abstime", ctypes.c_uint64),
                    ]

                self._libproc = lib
                self._struct = RUsageInfoV0
            except (OSError, AttributeError):
                self._libproc = None

    def resident_bytes(self, pid: int) -> int | None:
        if self._libproc is not None:
            info = self._struct()
            rc = self._libproc.proc_pid_rusage(pid, 0, ctypes.byref(info))
            if rc == 0:
                return int(info.ri_phys_footprint)
        try:
            out = subprocess.run(
                ["ps", "-o", "rss=", "-p", str(pid)],
                capture_output=True, text=True, timeout=5,
            )
            return int(out.stdout.strip()) * 1024
        except (ValueError, subprocess.SubprocessError, OSError):
            return None


def set_darwin_limits(timeout_seconds: int) -> None:
    """Apply the rlimits Darwin actually accepts (verified on macOS 15.3.1)."""
    resource.setrlimit(resource.RLIMIT_NPROC, (NPROC_LIMIT, NPROC_LIMIT))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    # CPU backstop: wall timeout + grace.  Verified enforced (SIGXCPU).
    cpu_cap = max(5, int(timeout_seconds) + 5)
    resource.setrlimit(resource.RLIMIT_CPU, (cpu_cap, cpu_cap))


def run(args: argparse.Namespace) -> int:
    try:
        exe, out, env = validate(args)
    except ValueError as exc:
        print(f"GUARD_REJECTED: {exc}", file=sys.stderr)
        return 2

    timeout = args.timeout
    rss_cap = args.rss_cap_mb * 1024 * 1024  # cap on phys_footprint (see ResidentSampler)
    LOCK.parent.mkdir(parents=True, exist_ok=True)
    with LOCK.open("a+") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("GUARD_REJECTED: another inference is running", file=sys.stderr)
            return 3

        start = time.monotonic()
        command = [str(exe)] + list(args.args or [])
        evidence = {
            "supervisor": "darwin-preexec+rss-monitor",
            "target": args.target,
            "command": command,
            "output": str(out),
            "rlimits": {
                "RLIMIT_NPROC": NPROC_LIMIT,
                "RLIMIT_CORE": 0,
                "RLIMIT_CPU_seconds": max(5, int(timeout) + 5),
                "RLIMIT_AS": "not_supported_on_darwin_user_processes",
                "RLIMIT_RSS": "not_supported_on_darwin_user_processes",
            },
            "rss_cap_bytes": rss_cap,
            "memory_metric": ResidentSampler.METRIC,
            "rss_monitor": "proc_pid_rusage" if ResidentSampler()._libproc is not None else "ps",
            "timeout_seconds": timeout,
            "environment": env,
        }
        sampler = ResidentSampler()
        peak_rss = 0
        timed_out = False
        rss_exceeded = False
        returncode = None
        stdout = ""
        try:
            proc = subprocess.Popen(
                command,
                cwd=ROOT,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,  # own process group for killpg
                preexec_fn=lambda: set_darwin_limits(timeout),
            )
            try:
                while True:
                    try:
                        stdout, _ = proc.communicate(timeout=POLL_SECONDS)
                        returncode = proc.returncode
                        break
                    except subprocess.TimeoutExpired:
                        pass
                    elapsed = time.monotonic() - start
                    rss = sampler.resident_bytes(proc.pid)
                    if rss is not None:
                        peak_rss = max(peak_rss, rss)
                        if rss > rss_cap:
                            rss_exceeded = True
                            os.killpg(proc.pid, signal.SIGKILL)
                            stdout, _ = proc.communicate()
                            returncode = 137
                            break
                    if elapsed > timeout:
                        timed_out = True
                        os.killpg(proc.pid, signal.SIGKILL)
                        stdout, _ = proc.communicate()
                        returncode = 124
                        break
            finally:
                if proc.poll() is None:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except (ProcessLookupError, PermissionError):
                        pass
                    proc.wait(timeout=5)
        except OSError as exc:
            evidence["error"] = repr(exc)
            returncode = 127

        evidence.update({
            "returncode": returncode,
            "stdout": stdout[-4000:],
            "wall_seconds": time.monotonic() - start,
            "peak_memory_bytes": peak_rss,
            "timeout_enforced": timed_out,
            "rss_cap_enforced": rss_exceeded,
        })
        Path(args.evidence).resolve().parent.mkdir(parents=True, exist_ok=True)
        Path(args.evidence).resolve().write_text(json.dumps(evidence, indent=2) + "\n")
        if rss_exceeded:
            return 137
        if timed_out:
            return 124
        return int(returncode if returncode is not None else 127)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--target", choices=sorted(ALLOWED), required=True)
    p.add_argument("--model", help="optional GGUF model (must exist when given)")
    p.add_argument("--output", required=True)
    p.add_argument("--evidence", required=True)
    p.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    p.add_argument("--rss-cap-mb", type=int, default=DEFAULT_RSS_CAP_MB)
    p.add_argument("args", nargs=argparse.REMAINDER, help="extra argv passed to the target after --")
    ns = p.parse_args()
    if ns.args and ns.args[0] == "--":
        ns.args = ns.args[1:]
    return run(ns)


if __name__ == "__main__":
    raise SystemExit(main())
