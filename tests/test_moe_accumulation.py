import math
import unittest


class MoeAccumulationTest(unittest.TestCase):
    def test_selected_plus_shared_order(self):
        selected = [1.0, -2.0, 3.5]
        shared = [0.25, 2.0, -0.5]
        complete = [a + b for a, b in zip(selected, shared)]
        self.assertEqual(complete, [1.25, 0.0, 3.0])
        self.assertTrue(all(math.isfinite(x) for x in complete))


if __name__ == "__main__":
    unittest.main()
