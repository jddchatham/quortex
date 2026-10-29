import pathlib
import unittest


class BatchPositionAllocationTest(unittest.TestCase):
    def test_embedding_batch_allocates_full_position_tuple(self):
        source = pathlib.Path(
            "/home/user/bitnet-cortex/cortex-stream-forge/"
            "runtime-flashnext-layer-exp/src/llama-batch.cpp"
        ).read_text()
        self.assertIn("constexpr int32_t n_pos_alloc = GGML_MROPE_SECTIONS", source)
        self.assertIn("calloc((size_t) n_tokens_alloc * n_pos_alloc", source)


if __name__ == "__main__":
    unittest.main()
