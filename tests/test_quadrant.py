import unittest

from quortex import Candidate, Context, Officiator, Quadrant


class QuadrantTests(unittest.TestCase):
    def setUp(self):
        self.judge = Officiator()
        self.context = Context.from_requirements({"basket", "ball", "official-2026"})

    def test_all_completeness_states(self):
        self.assertEqual(Candidate("none").quadrant, Quadrant.NEITHER)
        self.assertEqual(Candidate("first", first="basket").quadrant, Quadrant.FIRST_ONLY)
        self.assertEqual(Candidate("second", second="ball").quadrant, Quadrant.SECOND_ONLY)
        self.assertEqual(Candidate("both", first="basket", second="ball").quadrant, Quadrant.COMPLETE)

    def test_complete_is_not_automatically_correct(self):
        candidate = Candidate("dollar", "basket", "ball", frozenset({"basket", "ball"}))
        decision = self.judge.officiate(candidate, self.context)
        self.assertEqual(decision.quadrant, Quadrant.COMPLETE)
        self.assertFalse(decision.accepted)
        self.assertIn("official-2026", decision.reason)

    def test_contextually_correct_candidate_is_accepted(self):
        candidate = Candidate(
            "nba-2026", "basket", "ball", frozenset({"basket", "ball", "official-2026"})
        )
        decision = self.judge.officiate(candidate, self.context)
        self.assertTrue(decision.accepted)
        self.assertEqual(decision.reason, "contextual-match")

    def test_changed_requirements_revoke_prior_acceptance(self):
        candidate = Candidate(
            "nba-2026", "basket", "ball", frozenset({"basket", "ball", "official-2026"})
        )
        old = self.judge.officiate(candidate, self.context)
        new_context = Context.from_requirements({"basket", "ball", "official-2027"}, version="1")
        new = self.judge.reevaluate(candidate, old, new_context)
        self.assertTrue(old.accepted)
        self.assertFalse(new.accepted)
        self.assertNotEqual(old.context, new.context)

    def test_rejected_convergence_for_incomplete_candidate(self):
        decision = self.judge.officiate(Candidate("basket", first="basket"), self.context)
        self.assertFalse(decision.accepted)
        self.assertEqual(decision.reason, "incomplete")

    def test_pre_inference_gate_is_deterministic(self):
        metadata = {"properties": ["basket", "ball", "official-2026"]}
        self.assertTrue(self.judge.admit(metadata, self.context))
        self.assertTrue(self.judge.admit(metadata, self.context))
        self.assertFalse(self.judge.admit({"properties": ["basket", "ball"]}, self.context))


if __name__ == "__main__":
    unittest.main()

