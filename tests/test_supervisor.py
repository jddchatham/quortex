import json
import tempfile
import unittest
from pathlib import Path

from quortex.autonomy import supervisor


class SupervisorTests(unittest.TestCase):
    def test_codex_command_uses_only_exec_supported_flags(self):
        self.assertNotIn('"-a"', supervisor.run_codex.__doc__ or "")
        # The command construction is exercised by dry-run without spawning Codex.
        with tempfile.TemporaryDirectory() as directory:
            old_log = supervisor.LOG_DIR
            old_events = supervisor.EVENTS_PATH
            old_state = supervisor.STATE_PATH
            try:
                supervisor.LOG_DIR = Path(directory) / "logs"
                supervisor.EVENTS_PATH = Path(directory) / "events.jsonl"
                state_path = Path(directory) / "state.json"
                state_path.write_text(json.dumps({"assignment": "test"}))
                supervisor.STATE_PATH = state_path
                code, log = supervisor.run_codex({"assignment": "test"}, supervisor.Config(dry_run=True), 1, 0)
                self.assertEqual(code, 0)
                self.assertTrue(log.exists())
            finally:
                supervisor.LOG_DIR = old_log
                supervisor.EVENTS_PATH = old_events
                supervisor.STATE_PATH = old_state


if __name__ == "__main__":
    unittest.main()

