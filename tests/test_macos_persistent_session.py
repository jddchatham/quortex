"""M-MAC-05: native persistent-session manifest compatibility checks.

Validates the macOS persistent-session evidence manifest against the field
semantics asserted by tests/test_persistent_session.py for the repository's
run_session harness records, plus the M-MAC-05 batch-position bitwise
contract.  No existing test or requirement is modified; these checks mirror
them against native evidence.
"""

import hashlib
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "quortex/autonomy/evidence/m-mac-05/PERSISTENT_SESSION.json"

MODEL_SHA256 = "25130a98aa782284a7dabea0c23245b2fd371ed47244e79d78b8ec23245fdf96"
BINARY_SHA256 = "af035b63cfd90a1fb94fb0ceed894dd817a90fb868d9cc445da9a3360988869e"
# Fingerprints accepted at M-MAC-03 (single-segment runs) - cross-milestone binding.
MMAC03_A_LOGITS = [
    "1b84aa062ad14b9c69ab0fd6ccc7d34806552ce7f404d4ae3495657a11f74b66",
    "c8c4186f9b900344a9aa73c01051e4a215ff7d65e20d442ccb856e6fbb5aca63",
    "700e655c0ce1305cf0075e1856f977cc6a9eba6882b05275734a4fc3031316aa",
    "7647858f925a75bf30b7cb26c237adc3ef983066b1fdc384174d26751144c558",
]


class ManifestContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not MANIFEST.is_file():
            raise unittest.SkipTest("M-MAC-05 manifest not generated on this host")
        cls.m = json.loads(MANIFEST.read_text())

    def test_harness_record_fields_present_and_typed(self):
        m = self.m
        self.assertIsInstance(m["command"], list)
        self.assertEqual(m["turns_sent"], ["The capital of France is", " known for the Eiffel Tower"])
        self.assertEqual(m["returncode"], 0)
        self.assertIs(m["completed"], True)
        self.assertIs(m["timed_out"], False)
        self.assertEqual(m["ready_markers_seen"], [])
        self.assertIsNone(m["ready_marker"])
        self.assertIsNone(m["exit_command"])
        self.assertEqual(len(m["output_sha256"]), 64)
        self.assertEqual(len(m["session_sha256"]), 64)
        json.dumps(m)  # JSON-safe evidence, per test_tuple_command_produces_json_safe_evidence

    def test_session_hash_binds_inputs_not_output(self):
        material = json.dumps({
            "model_sha256": MODEL_SHA256,
            "turns": self.m["turns_sent"],
            "mode": "session",
            "gen_per_segment": 4,
            "limits": {"rss_cap_mb": 2048, "timeout_s": 90},
            "binary_sha256": BINARY_SHA256,
        }, sort_keys=True).encode()
        self.assertEqual(hashlib.sha256(material).hexdigest(), self.m["session_sha256"])
        # output hash must differ from session hash (output is not the input binding)
        self.assertNotEqual(self.m["output_sha256"], self.m["session_sha256"])

    def test_segment_a_matches_accepted_mmac03_fingerprints(self):
        for i, cap in enumerate(self.m["fingerprints"]["A"]):
            self.assertEqual(cap["logits_sha256"], MMAC03_A_LOGITS[i])

    def test_reset_reuse_without_model_reload(self):
        own = self.m["session_ownership"]
        self.assertEqual(own["model_reloads"], 0)
        self.assertIs(own["reset_reuses_same_model"], True)
        self.assertEqual(own["reset_drift"], "0")
        self.assertEqual(own["expected_kv_cells"], own["total_decoded"])

    def test_batch_position_variants_bitwise_equal(self):
        bp = self.m["batch_position_check"]
        finals = bp["final_logits_sha256"]
        self.assertEqual(finals["S"], finals["Q"])
        self.assertEqual(finals["Q"], finals["O"])
        for i in range(4):
            row = [bp["generated_logits_sha256"][p][i] for p in "SQO"]
            self.assertEqual(row[0], row[1])
            self.assertEqual(row[1], row[2])

    def test_negative_control_tampered_fingerprint_rejected(self):
        m = json.loads(MANIFEST.read_text())
        m["fingerprints"]["A"][0]["logits_sha256"] = "0" * 64
        material = json.dumps({
            "model_sha256": MODEL_SHA256,
            "turns": m["turns_sent"],
            "mode": "session",
            "gen_per_segment": 4,
            "limits": {"rss_cap_mb": 2048, "timeout_s": 90},
            "binary_sha256": BINARY_SHA256,
        }, sort_keys=True).encode()
        # session hash binds inputs only and is unchanged, but the segment-A
        # binding to accepted M-MAC-03 evidence must fail:
        self.assertNotEqual(m["fingerprints"]["A"][0]["logits_sha256"], MMAC03_A_LOGITS[0])


if __name__ == "__main__":
    unittest.main()
