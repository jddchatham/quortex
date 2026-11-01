import pathlib
import unittest

FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "staged_capture_source_contract.txt"

class StagedCaptureGateTests(unittest.TestCase):
    def test_staged_mode_materializes_diagnostics_without_internal_flag(self):
        source = FIXTURE.read_text()
        self.assertIn('std::getenv("CORTEX_FLASHNEXT_STAGED_MOE")', source)
        self.assertIn('flashnext_diag_owned[it.first]', source)

    def test_staged_launcher_pin_and_flag(self):
        source = pathlib.Path("quortex/autonomy/guarded_inference.py").read_text()
        self.assertIn('"staged": "53df80306688a700aa02f5370c131b8e9064a1e139f7a7ad826463dd660de934"', source)
        self.assertIn('env["CORTEX_FLASHNEXT_STAGED_MOE"] = "1"', source)
        self.assertIn('env["FLASHNEXT_TENSOR_INDEX"] = str(validate_staged_index(model))', source)


if __name__ == "__main__":
    unittest.main()
