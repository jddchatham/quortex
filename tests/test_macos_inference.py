"""Darwin supervisor bring-up tests: guards, rlimits, RSS monitor, timeout.

Every test here is bounded, synthetic, and model-free.  No guard is relaxed
to make a test pass; the tests assert the guards *work* on this host.
"""

import json
import resource
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from quortex.autonomy import macos_inference

ROOT = Path(macos_inference.ROOT)


class GuardTests(unittest.TestCase):
    def test_unapproved_target_rejected(self):
        args = type("Args", (), {"target": "bad"})()
        with self.assertRaises(ValueError):
            macos_inference.validate(args)

    def test_output_must_be_fresh_and_empty_and_inside_repo(self):
        with tempfile.TemporaryDirectory() as d:
            args = type("Args", (), {
                "target": None, "model": None,
                "output": str(Path(d) / "escape"),
            })()
            with self.assertRaises(ValueError):
                macos_inference.validate(args)
            inside = ROOT / "quortex" / "autonomy" / "evidence" / "macos-guard-test"
            inside.mkdir(parents=True, exist_ok=True)
            try:
                (inside / "stale.txt").write_text("not fresh")
                args = type("Args", (), {
                    "target": None, "model": None, "output": str(inside),
                })()
                with self.assertRaises(ValueError):
                    macos_inference.validate(args)
            finally:
                (inside / "stale.txt").unlink(missing_ok=True)
                inside.rmdir()

    def test_model_must_be_gguf_when_given(self):
        with tempfile.TemporaryDirectory() as d:
            not_model = Path(d) / "weights.bin"
            not_model.write_bytes(b"x")
            args = type("Args", (), {
                "target": None, "model": str(not_model), "output": None,
            })()
            with self.assertRaises(ValueError):
                macos_inference.validate(args)


class LimitTests(unittest.TestCase):
    def test_darwin_rlimits_applied_in_child(self):
        code = (
            "import resource, json;"
            "print(json.dumps({"
            " 'nproc': resource.getrlimit(resource.RLIMIT_NPROC)[0],"
            " 'core': resource.getrlimit(resource.RLIMIT_CORE)[0],"
            " 'cpu': resource.getrlimit(resource.RLIMIT_CPU)[0]}))"
        )
        out = subprocess.run(
            [sys.executable, "-c", code],
            preexec_fn=lambda: macos_inference.set_darwin_limits(30),
            capture_output=True, text=True, timeout=20,
        )
        limits = json.loads(out.stdout.strip().splitlines()[-1])
        self.assertEqual(limits["nproc"], 64)
        self.assertEqual(limits["core"], 0)
        self.assertEqual(limits["cpu"], 35)

    def test_resident_sampler_returns_sane_value(self):
        sampler = macos_inference.ResidentSampler()
        probe = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(3)"])
        try:
            value = None
            for _ in range(20):
                value = sampler.resident_bytes(probe.pid)
                if value and 1024 * 1024 < value < 4 * 1024**3:
                    break
                time.sleep(0.1)
            self.assertIsNotNone(value)
            self.assertGreater(value, 1024 * 1024)
            self.assertLess(value, 4 * 1024**3)
        finally:
            probe.terminate()
            probe.wait(timeout=5)


class EnforcementTests(unittest.TestCase):
    """End-to-end through macos_inference.run with a synthetic child."""

    def _run_helper(self, helper_code: str, timeout: int, rss_cap_mb: int):
        helper = ROOT / "quortex" / "autonomy" / "evidence" / "macos-enforce-helper.py"
        helper.write_text(helper_code)
        outdir = ROOT / "quortex" / "autonomy" / "evidence" / f"macos-enforce-{time.time_ns()}"
        ev_path = outdir.with_suffix(".evidence.json")
        try:
            # The approved-target allowlist is bypassed here by monkeypatching
            # ALLOWED to the interpreter itself: the guards under test are the
            # resource enforcers, not the allowlist (covered above).
            macos_inference.ALLOWED["helper"] = Path(sys.executable)
            args = type("Args", (), {
                "target": "helper",
                "model": None,
                "output": str(outdir),
                "evidence": str(ev_path),
                "timeout": timeout,
                "rss_cap_mb": rss_cap_mb,
                "args": [str(helper)],
            })()
            rc = macos_inference.run(args)
            evidence = json.loads(ev_path.read_text())
            return rc, evidence
        finally:
            macos_inference.ALLOWED.pop("helper", None)
            helper.unlink(missing_ok=True)
            ev_path.unlink(missing_ok=True)

    def test_rss_cap_kills_hungry_child(self):
        rc, ev = self._run_helper(
            "b = bytearray(96 * 1024 * 1024)\n"
            "for i in range(0, len(b), 4096):\n"
            "    b[i] = 1\n"
            "import time\n"
            "while True:\n"
            "    time.sleep(0.05)\n",
            timeout=30,
            rss_cap_mb=32,
        )
        self.assertEqual(rc, 137)
        self.assertTrue(ev["rss_cap_enforced"])
        self.assertGreater(ev["peak_memory_bytes"], 32 * 1024 * 1024)

    def test_timeout_kills_sleeping_child(self):
        rc, ev = self._run_helper(
            "import time\nwhile True:\n    time.sleep(1)\n",
            timeout=3,
            rss_cap_mb=2048,
        )
        self.assertEqual(rc, 124)
        self.assertTrue(ev["timeout_enforced"])
        self.assertLess(ev["wall_seconds"], 20)

    def test_well_behaved_child_completes(self):
        rc, ev = self._run_helper(
            "print('CHILD_OK')\n",
            timeout=30,
            rss_cap_mb=2048,
        )
        self.assertEqual(rc, 0)
        self.assertFalse(ev["timeout_enforced"])
        self.assertFalse(ev["rss_cap_enforced"])
        self.assertIn("CHILD_OK", ev["stdout"])


if __name__ == "__main__":
    unittest.main()
