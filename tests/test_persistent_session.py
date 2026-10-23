import sys
import tempfile
import unittest
from pathlib import Path

from benchmarks.persistent_session import run_session


class PersistentSessionTests(unittest.TestCase):
    def test_one_process_receives_both_turns_and_records_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "turns"
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "import sys\n"
                "turns = [sys.stdin.readline().rstrip('\\n') for _ in range(2)]\n"
                "open(sys.argv[1], 'w').write('|'.join(turns))\n"
            )
            record = run_session([sys.executable, str(backend), str(marker)],
                                 ["first", "second"], 2)
            self.assertEqual(record["returncode"], 0)
            self.assertEqual(record["turns_sent"], ["first", "second"])
            self.assertEqual(marker.read_text(), "first|second")
            self.assertEqual(len(record["output_sha256"]), 64)

    def test_dead_backend_is_reported_as_incomplete(self):
        record = run_session([sys.executable, "-c", "raise SystemExit(7)"],
                             ["first", "second"], 1)
        self.assertEqual(record["returncode"], 7)
        self.assertLess(len(record["turns_sent"]), 2)


if __name__ == "__main__":
    unittest.main()
