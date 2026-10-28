import unittest

from quortex.autonomy.overnight_supervisor import dependency_route


class SchedulerDependencyTests(unittest.TestCase):
    def test_blocked_gate_routes_before_dependent_launch(self):
        self.assertEqual(dependency_route({"two_token_gate": "BLOCKED", "dependency_routes": 0}, "QIES-061-LONGER-SEQUENCE"), "route")

    def test_blocked_gate_stops_after_bounded_routes(self):
        self.assertEqual(dependency_route({"two_token_gate": "BLOCKED", "dependency_routes": 2}, "QIES-062-FULL-LAYER-TRAVERSAL"), "stop")

    def test_passed_gate_allows_dependents(self):
        self.assertEqual(dependency_route({"two_token_gate": "PASS", "dependency_routes": 0}, "QIES-063-PERSISTENT-PERFORMANCE"), "allow")

    def test_crash_repair_is_independent(self):
        self.assertEqual(dependency_route({"two_token_gate": "BLOCKED", "dependency_routes": 2}, "QIES-067-CRASH-REPAIR"), "allow")
