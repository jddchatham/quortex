import unittest
from pathlib import Path

from quortex.autonomy.guarded_inference import ALLOWED


class ReferenceBuildIdentityTests(unittest.TestCase):
    def test_reference_allowlist_uses_owned_layer_target(self):
        ref = ALLOWED["reference"]
        self.assertEqual(ref.name, "llama-flashnext-owned-layer")
        self.assertIn("runtime-flashnext-layer-exp-build", str(ref))

    def test_reference_source_and_target_are_present(self):
        source = Path("/home/user/bitnet-cortex/cortex-stream-forge/runtime-flashnext-layer-exp/examples/flashnext-owned-layer.cpp")
        self.assertTrue(source.is_file())
        self.assertTrue(ALLOWED["reference"].is_file())
