"""M-MAC-08: authentic Cortex layer-0 recovery verification.

Asserts the recovered llama_cortex_* interface is exported from the arm64
libllama and that the layer-0 executable is a pinned Mach-O binary.  No
model weights are used; all checks are structural (nm/file/sha256).
"""

import hashlib
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIBLLAMA = Path("/Users/user/workspace/cortex-build-mac/bin/libllama.0.5.0.dylib")
LAYER0 = ROOT / "native/build-mac/quortex-flashnext-layer0"

RECOVERED = [
    "llama_cortex_token_embd_rows",
    "llama_cortex_copy_layer0_boundary",
    "llama_cortex_copy_layer0_router",
    "llama_cortex_copy_layer0_post_segment_b",
    "llama_cortex_copy_layer0_pre_hc",
]
LAYER0_SHA256 = "ba8d973861c4e95ab0e39198075f2b230b190f12804dc3e3bdd19eef3a64d722"


@unittest.skipUnless(LIBLLAMA.is_file(), "Cortex arm64 build not present on this host")
class CortexRecoveryTests(unittest.TestCase):
    def test_recovered_symbols_exported(self):
        out = subprocess.run(["nm", "-gU", str(LIBLLAMA)], capture_output=True, text=True, check=True).stdout
        for sym in RECOVERED:
            self.assertIn(f"_{sym}", out, f"missing recovered symbol: {sym}")

    def test_layer0_binary_mach_o_arm64_pinned(self):
        if not LAYER0.is_file():
            self.skipTest("layer-0 executable not built")
        info = subprocess.run(["file", "-b", str(LAYER0)], capture_output=True, text=True, check=True).stdout
        self.assertIn("Mach-O", info)
        self.assertIn("arm64", info)
        digest = hashlib.sha256(LAYER0.read_bytes()).hexdigest()
        self.assertEqual(digest, LAYER0_SHA256, "layer-0 binary diverges from the approved pin")

    def test_supervisor_allowlist_includes_layer0(self):
        from quortex.autonomy import macos_inference
        self.assertIn("flashnext-layer0", macos_inference.ALLOWED)
        self.assertEqual(macos_inference.APPROVED_SHA256["flashnext-layer0"], LAYER0_SHA256)


if __name__ == "__main__":
    unittest.main()
