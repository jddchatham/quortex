import tempfile
import unittest
from pathlib import Path
import subprocess
import sys

from quortex.inference import make_plan


class InferenceGateTests(unittest.TestCase):
    def test_json_decision_does_not_launch_backend(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "model.gguf"
            marker = root / "launched"
            backend = root / "backend"
            model.write_bytes(b"test placeholder")
            backend.write_text("#!/bin/sh\ntouch '%s'\n" % marker)
            backend.chmod(0o755)
            result = subprocess.run(
                [sys.executable, "-m", "quortex.cli", "-m", str(model), "--backend", str(backend), "--json-decision"],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0)
            self.assertFalse(marker.exists())

    def test_rejected_before_backend_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.gguf"
            model.write_bytes(b"test placeholder")
            _, _, _, decision = make_plan(
                "/bin/true", str(model), requirements={"official-2027"}
            )
            self.assertFalse(decision.accepted)
            self.assertIn("official-2027", decision.reason)

    def test_accepted_plan_is_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.gguf"
            model.write_bytes(b"test placeholder")
            plan, candidate, context, decision = make_plan(
                "/bin/true", str(model), requirements={"model", "runtime"}
            )
            self.assertTrue(decision.accepted)
            self.assertEqual(decision.quadrant.bits, (1, 1))
            self.assertEqual(plan.model, model)
            self.assertEqual(context.requirements, frozenset({"model", "runtime"}))
            self.assertEqual(candidate.quadrant.bits, (1, 1))


if __name__ == "__main__":
    unittest.main()
