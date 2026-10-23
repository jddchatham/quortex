import unittest

from quortex.autonomy import transition


class StateMachineTests(unittest.TestCase):
    def test_normal_cycle(self):
        state = "DISCOVER"
        for target in ("SPECIFY", "RESEARCH", "IMPLEMENT", "BUILD", "TEST", "AUDIT", "BENCHMARK", "ACCEPT", "COMMIT", "PUSH", "NEXT_ASSIGNMENT"):
            state = transition(state, target)
        self.assertEqual(state, "NEXT_ASSIGNMENT")

    def test_rejection_requires_diagnosis(self):
        self.assertEqual(transition("REJECT", "DIAGNOSE"), "DIAGNOSE")
        with self.assertRaises(ValueError):
            transition("REJECT", "COMMIT")

    def test_interrupt_recovery(self):
        state = transition("TEST", "INTERRUPTED")
        state = transition(state, "CHECKPOINT")
        state = transition(state, "RECOVER")
        self.assertEqual(transition(state, "VALIDATE_STATE"), "VALIDATE_STATE")


if __name__ == "__main__":
    unittest.main()

