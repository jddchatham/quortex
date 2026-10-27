import json
import tempfile
import unittest
from pathlib import Path

from quortex.autonomy.acceptance import two_token_pass
from quortex.autonomy.guarded_inference import validate


class OvernightControlFixtures(unittest.TestCase):
    def test_numerical_mismatch_blocks_even_with_zero_exits(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "quortex/autonomy/evidence"
            p.mkdir(parents=True)
            (p / "QIES-060-TWO-TOKEN.json").write_text(json.dumps({
                "status": "FAIL", "reference_exit_status": 0,
                "quortex_exit_status": 0, "state_continuity": True,
                "numerical_comparison": "FAIL",
                "reference": {"captures": ["r"]},
                "quortex": {"captures": ["q"]},
            }))
            ok, reasons = two_token_pass(d)
            self.assertFalse(ok)
            self.assertTrue(reasons)

    def test_missing_capture_blocks(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "quortex/autonomy/evidence"
            p.mkdir(parents=True)
            (p / "QIES-060-TWO-TOKEN.json").write_text(json.dumps({
                "status": "PASS", "reference_exit_status": 0,
                "quortex_exit_status": 0, "state_continuity": True,
                "numerical_comparison": "PASS", "reference": {"captures": []},
                "quortex": {"captures": ["q"]},
            }))
            self.assertFalse(two_token_pass(d)[0])

    def test_launcher_rejects_unapproved_target(self):
        args = type("Args", (), {"target": "bad", "model": "/missing.gguf", "output": "/tmp/out", "index": None})()
        with self.assertRaises(ValueError):
            validate(args)

    def test_launcher_rejects_nonempty_output(self):
        with tempfile.TemporaryDirectory() as d:
            model = Path(d) / "x.gguf"
            model.write_bytes(b"fixture")
            out = Path(d) / "out"
            out.mkdir()
            (out / "existing").write_text("x")
            args = type("Args", (), {"target": "quortex", "model": str(model), "output": str(out), "index": None})()
            with self.assertRaises(ValueError):
                validate(args)
