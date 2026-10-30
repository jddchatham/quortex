import pathlib
import unittest


class StagedGraphIdentityTests(unittest.TestCase):
    def test_reservation_diagnostics_are_cleared_before_execution_graph(self):
        source = pathlib.Path(
            "/tmp/quortex-qies109-wt.Tgz8zp/runtime-flashnext-layer-exp/src/llama-context.cpp"
        ).read_text()
        clear = source.index("flashnext_diag_tensors.clear();")
        build = source.index("model.build_graph(gparams)")
        self.assertLess(clear, build)

    def test_execution_capture_requires_backing_storage(self):
        source = pathlib.Path(
            "/tmp/quortex-qies109-wt.Tgz8zp/runtime-flashnext-layer-exp/src/llama-context.cpp"
        ).read_text()
        self.assertIn("capture=BACKEND_SYNCED", source)
        self.assertIn("t->buffer", source)
        self.assertIn("FLASHNEXT_STAGED_DIAG_CAPTURE", source)


if __name__ == "__main__":
    unittest.main()
