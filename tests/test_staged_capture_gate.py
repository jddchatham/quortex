import pathlib
import unittest


class StagedCaptureGateTests(unittest.TestCase):
    def test_staged_mode_materializes_diagnostics_without_internal_flag(self):
        source = pathlib.Path(
            "/tmp/quortex-qies109-wt.Tgz8zp/runtime-flashnext-layer-exp/src/llama-context.cpp"
        ).read_text()
        self.assertIn('std::getenv("CORTEX_FLASHNEXT_STAGED_MOE")', source)
        self.assertIn('flashnext_diag_owned[it.first]', source)

    def test_staged_launcher_pin_and_flag(self):
        source = pathlib.Path("quortex/autonomy/guarded_inference.py").read_text()
        self.assertIn('"staged": "bd28bcda383b24c3fce19ba5df99bf83c2282f4da350c44dbbba2dffed11167d"', source)
        self.assertIn('env["CORTEX_FLASHNEXT_STAGED_MOE"] = "1"', source)


if __name__ == "__main__":
    unittest.main()
