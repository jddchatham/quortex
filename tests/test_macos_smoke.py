"""Native ARM64 smoke target verification (macOS bring-up).

Builds native/build/macos-arm64-smoke with the host toolchain when missing,
then verifies:
  1. the binary is a Mach-O arm64 executable;
  2. it self-checks against the embedded float64 reference and exits 0;
  3. an independent Python float64 implementation reproduces the reference
     (cross-implementation numerical agreement);
  4. it completes under the Darwin supervisor with no guard tripped.
"""

import json
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from quortex.autonomy import macos_inference

ROOT = Path(macos_inference.ROOT)
NATIVE = ROOT / "native"
BINARY = NATIVE / "build" / "macos-arm64-smoke"
REFERENCE_DOT = -840.45440460774125


def build_smoke() -> None:
    subprocess.run(
        ["sh", str(NATIVE / "build_macos_smoke.sh")],
        check=True, capture_output=True, text=True, timeout=120,
    )


class Arm64SmokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BINARY.is_file():
            build_smoke()
        cls.file_output = subprocess.run(
            ["file", "-b", str(BINARY)], capture_output=True, text=True
        ).stdout

    def test_binary_is_mach_o_arm64(self):
        self.assertIn("Mach-O", self.file_output)
        self.assertIn("arm64", self.file_output)

    def test_python_recomputes_reference(self):
        # Independent reimplementation: same LCG, same accumulation order.
        def gen(seed):
            x = seed & 0xFFFFFFFFFFFFFFFF
            while True:
                x = (x * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
                yield ((x >> 33) / 2147483648.0) * 2.0 - 1.0

        ga, gb = gen(0x5DEECE66D), gen(0x2545F4914F6CDD1D)
        dot = 0.0
        for _ in range(1 << 20):
            dot += next(ga) * next(gb)
        rel = (dot - REFERENCE_DOT) / REFERENCE_DOT
        self.assertLess(abs(rel), 1e-12)

    def test_binary_self_check_passes(self):
        proc = subprocess.run(
            [str(BINARY)], capture_output=True, text=True, timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        fields = dict(
            line.split("=", 1) for line in proc.stdout.strip().splitlines() if "=" in line
        )
        self.assertEqual(fields["SMOKE_PASS"], "1")
        dot = float(fields["SMOKE_DOT"])
        self.assertLess(abs((dot - REFERENCE_DOT) / REFERENCE_DOT), 1e-9)
        # SMOKE_RSS_KB is raw getrusage(RUSAGE_SELF).ru_maxrss, which on
        # Darwin includes the shared-cache region (~1.2 GB even for trivial
        # binaries); assert only a sane ceiling.  The precise workload
        # footprint is enforced via phys_footprint in the supervisor test.
        self.assertLess(int(fields["SMOKE_RSS_KB"]), 8 * 1024 * 1024)

    def test_runs_under_darwin_supervisor(self):
        outdir = ROOT / "quortex" / "autonomy" / "evidence" / f"macos-smoke-{time.time_ns()}"
        ev_path = outdir.with_suffix(".evidence.json")
        args = type("Args", (), {
            "target": "arm64-smoke",
            "model": None,
            "output": str(outdir),
            "evidence": str(ev_path),
            "timeout": 60,
            "rss_cap_mb": 2048,
            "args": [],
        })()
        try:
            rc = macos_inference.run(args)
            self.assertEqual(rc, 0)
            ev = json.loads(ev_path.read_text())
            self.assertFalse(ev["timeout_enforced"])
            self.assertFalse(ev["rss_cap_enforced"])
            self.assertLess(ev["peak_memory_bytes"], 256 * 1024 * 1024)
            self.assertIn("SMOKE_PASS=1", ev["stdout"])
            self.assertEqual(
                ev["rlimits"]["RLIMIT_NPROC"], 64,
                "guard rlimits must be recorded and applied",
            )
        finally:
            ev_path.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
