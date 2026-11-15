"""M-MAC-06: evidence-chain integrity checks (backward compatible)."""

import hashlib
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHAIN = ROOT / "quortex/autonomy/evidence/m-mac-06/CHAIN.json"

ACCEPTED = {
    "LOGITS0": "1b84aa062ad14b9c69ab0fd6ccc7d34806552ce7f404d4ae3495657a11f74b66",
}

class EvidenceChainTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not CHAIN.is_file():
            raise unittest.SkipTest("M-MAC-06 chain not generated on this host")
        cls.doc = json.loads(CHAIN.read_text())

    def test_chain_links_recompute(self):
        prev = "0" * 64
        for link in self.doc["links"]:
            path = ROOT / "quortex" / "autonomy" / "evidence" / link["file"].split("evidence/", 1)[-1]
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertEqual(digest, link["sha256"])
            link_hash = hashlib.sha256((prev + digest).encode()).hexdigest()
            self.assertEqual(link_hash, link["chain_hash"])
            prev = link_hash
        self.assertEqual(prev, self.doc["head"])

    def test_acceptance_gate_still_independent(self):
        # Chaining must not have altered acceptance behavior.
        from quortex.autonomy.acceptance import two_token_pass
        self.assertEqual(two_token_pass(ROOT), (True, []))

if __name__ == "__main__":
    unittest.main()
