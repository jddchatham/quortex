import json
import tempfile
import unittest
from pathlib import Path

from quortex.autonomy.acceptance import two_token_pass


class AcceptanceGateTests(unittest.TestCase):
    def test_missing_evidence_blocks(self):
        with tempfile.TemporaryDirectory() as d:
            ok, errors = two_token_pass(d)
            self.assertFalse(ok)
            self.assertTrue(errors)

    def test_zero_exit_and_report_do_not_pass(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "quortex/autonomy/evidence"
            p.mkdir(parents=True)
            (p / "QIES-060-TWO-TOKEN.json").write_text(json.dumps({"status": "COMPLETED", "reference_exit_status": 0, "quortex_exit_status": 0}))
            ok, _ = two_token_pass(d)
            self.assertFalse(ok)

    def test_full_evidence_passes(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "quortex/autonomy/evidence"
            p.mkdir(parents=True)
            (p / "QIES-060-TWO-TOKEN.json").write_text(json.dumps({"status": "PASS", "reference_exit_status": 0, "quortex_exit_status": 0, "state_continuity": True, "numerical_comparison": "PASS", "reference": {"captures": ["a"]}, "quortex": {"captures": ["b"]}}))
            self.assertEqual(two_token_pass(d), (True, []))
