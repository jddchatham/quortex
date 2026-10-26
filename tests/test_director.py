import tempfile
import unittest
from pathlib import Path

from quortex.autonomy.director import acceptance_met, begin, complete, eligible, fail, load, save, select


class DirectorTests(unittest.TestCase):
    def data(self):
        return {"assignments":[
            {"id":"A","objective":"first","dependencies":[],"priority":1,"status":"pending","attempts":0,"evidence":[],"source_commit":None,"next_action":"do"},
            {"id":"B","objective":"second","dependencies":["A"],"priority":2,"status":"pending","attempts":0,"evidence":[],"source_commit":None,"next_action":"wait"},
            {"id":"C","objective":"independent","dependencies":[],"priority":3,"status":"pending","attempts":0,"evidence":[],"source_commit":None,"next_action":"do"}
        ]}

    def test_completion_advances(self):
        data=self.data(); self.assertEqual(select(data)["id"],"A"); begin(data,"A"); complete(data,"A","tests", "abc"); self.assertEqual(select(data)["id"],"B")

    def test_failed_task_blocks_after_bound(self):
        data=self.data()
        for _ in range(3):
            begin(data,"A"); fail(data,"A","failure")
        self.assertEqual(data["assignments"][0]["status"],"blocked")
        self.assertEqual(select(data)["id"],"C")

    def test_zero_exit_without_acceptance_cannot_complete(self):
        data=self.data(); begin(data,"A"); self.assertFalse(acceptance_met(data["assignments"][0])); fail(data,"A","codex exited zero but acceptance unmet",max_attempts=1); self.assertEqual(data["assignments"][0]["status"],"blocked")

    def test_retry_pending_is_bounded_and_blocked_is_not_eligible(self):
        data=self.data(); begin(data,"A"); fail(data,"A","recoverable",max_attempts=3); self.assertEqual(data["assignments"][0]["status"],"retry_pending"); begin(data,"A"); fail(data,"A","recoverable",max_attempts=3); begin(data,"A"); fail(data,"A","terminal",max_attempts=3); self.assertEqual(data["assignments"][0]["status"],"blocked"); self.assertEqual(select(data)["id"],"C")

    def test_empty_backlog_is_idle(self):
        data={"assignments":[]}; self.assertIsNone(select(data)); self.assertEqual(eligible(data),[])

    def test_high_priority_independent_work_preempts_retry(self):
        data=self.data(); begin(data,"A"); fail(data,"A","recoverable",max_attempts=3)
        data["assignments"][2]["priority"] = 0
        self.assertEqual(select(data)["id"],"C")

    def test_atomic_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"backlog.json"; save(p,self.data()); self.assertEqual(load(p)["assignments"][0]["id"],"A")

