import sys
import json
import math
import os
import tempfile
import time
import unittest
from unittest import mock
from pathlib import Path

import benchmarks.persistent_session as persistent_session
from benchmarks.persistent_session import build_command, run_session


class PersistentSessionTests(unittest.TestCase):
    def test_line_write_retries_partial_pty_writes(self):
        read_fd, write_fd = os.pipe()
        try:
            original_write = persistent_session.os.write
            calls = []

            def partial_write(fd, payload):
                calls.append(bytes(payload))
                return original_write(fd, payload[:1])

            with mock.patch.object(persistent_session.os, "write",
                                   side_effect=partial_write):
                persistent_session._write_line(write_fd, "turn")
            self.assertEqual(os.read(read_fd, 64), b"turn\n")
            self.assertGreater(len(calls), 1)
        finally:
            os.close(read_fd)
            os.close(write_fd)

    def test_invalid_command_is_rejected_before_backend_launch(self):
        with self.assertRaisesRegex(ValueError, "non-empty sequence"):
            run_session([], [], 1)
        with self.assertRaisesRegex(ValueError, "command entries"):
            run_session([sys.executable, 3], [], 1)

    def test_backend_launch_failure_is_propagated(self):
        with self.assertRaises(FileNotFoundError):
            run_session(["/definitely/missing/quortex-backend"], [], 1)

    def test_backend_that_exits_during_launch_is_reaped(self):
        record = run_session([sys.executable, "-c", "raise SystemExit(7)"], [], 1)
        self.assertEqual(record["returncode"], 7)
        self.assertFalse(record["completed"])

    def test_non_string_turn_is_rejected_before_backend_launch(self):
        with self.assertRaisesRegex(ValueError, "turns must be strings"):
            run_session([sys.executable, "-c", ""], [1], 1)

    def test_multiline_turn_is_rejected_before_backend_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "launched"
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "from pathlib import Path\n"
                "Path(__import__('sys').argv[1]).write_text('launched')\n"
            )
            with self.assertRaisesRegex(ValueError, "single-line"):
                run_session([sys.executable, str(backend), str(marker)],
                            ["first\nsecond"], 1)
            self.assertFalse(marker.exists())

    def test_invalid_timeout_and_exit_command_are_rejected_before_launch(self):
        with self.assertRaisesRegex(ValueError, "timeout must be positive"):
            run_session([sys.executable, "-c", ""], [], 0)
        with self.assertRaisesRegex(ValueError, "timeout must be positive"):
            run_session([sys.executable, "-c", ""], [], "1")
        with self.assertRaisesRegex(ValueError, "timeout must be positive"):
            run_session([sys.executable, "-c", ""], [], True)
        with self.assertRaisesRegex(ValueError, "timeout must be positive"):
            run_session([sys.executable, "-c", ""], [], math.nan)
        with self.assertRaisesRegex(ValueError, "exit command must be single-line"):
            run_session([sys.executable, "-c", ""], [], 1, exit_command="/exit\nquit")
        with self.assertRaisesRegex(ValueError, "output limit must be positive"):
            run_session([sys.executable, "-c", ""], [], 1, max_output_bytes=0)

    def test_marker_and_exit_command_must_be_strings(self):
        with self.assertRaisesRegex(ValueError, "ready marker must be a string"):
            run_session([sys.executable, "-c", ""], [], 1, ready_marker=3)
        with self.assertRaisesRegex(ValueError, "exit command must be a string"):
            run_session([sys.executable, "-c", ""], [], 1, exit_command=3)

    def test_multiline_ready_marker_is_rejected_before_backend_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "launched"
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "from pathlib import Path\n"
                "Path(__import__('sys').argv[1]).write_text('launched')\n"
            )
            with self.assertRaisesRegex(ValueError, "ready marker must be single-line"):
                run_session([sys.executable, str(backend), str(marker)],
                            [], 1, ready_marker="> \nnext")
            self.assertFalse(marker.exists())

    def test_command_supplies_conversation_system_prompt(self):
        command = build_command("llama-cli", "model.gguf", "test system")
        self.assertEqual(command[-2:], ["-p", "test system"])
        self.assertIn("-cnv", command)
        self.assertIn("--interactive-first", command)

    def test_record_preserves_session_control_configuration(self):
        record = run_session(
            [sys.executable, "-c", "print('> ', end='', flush=True); input()"],
            ["first"], 1, ready_marker="> ", exit_command="/exit"
        )
        self.assertEqual(record["ready_marker"], "> ")
        self.assertEqual(record["exit_command"], "/exit")

    def test_empty_control_strings_disable_readiness_and_exit(self):
        record = run_session(
            [sys.executable, "-c", "input()"], ["first"], 1,
            ready_marker="", exit_command=""
        )
        self.assertTrue(record["completed"])
        self.assertIsNone(record["ready_marker"])
        self.assertIsNone(record["exit_command"])

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

    def test_tuple_command_produces_json_safe_evidence(self):
        record = run_session((sys.executable, "-c", "input()"), ["first"], 1)
        self.assertIsInstance(record["command"], list)
        json.dumps(record)

    def test_identical_session_replays_have_identical_evidence(self):
        command = [sys.executable, "-c",
                   "import sys; print('ready', flush=True); "
                   "first = input(); print('reply:' + first, flush=True); "
                   "print('ready', flush=True); second = input(); "
                   "print('reply:' + second, flush=True)"]
        first = run_session(command, ["alpha", "beta"], 1,
                            ready_marker="ready")
        second = run_session(command, ["alpha", "beta"], 1,
                             ready_marker="ready")
        self.assertTrue(first["completed"])
        self.assertEqual(first["turns_sent"], second["turns_sent"])
        self.assertEqual(first["ready_markers_seen"],
                         second["ready_markers_seen"])
        self.assertEqual(first["stdout_stderr"], second["stdout_stderr"])
        self.assertEqual(first["output_sha256"], second["output_sha256"])

    def test_session_hash_binds_inputs_not_backend_output(self):
        command = [sys.executable, "-c", "print('ready', flush=True)"]
        first = run_session(command, ["alpha"], 1, ready_marker="ready")
        same = run_session(command, ["alpha"], 1, ready_marker="ready")
        different = run_session(command, ["beta"], 1, ready_marker="ready")
        self.assertEqual(first["session_sha256"], same["session_sha256"])
        self.assertNotEqual(first["session_sha256"],
                            different["session_sha256"])

    def test_dead_backend_is_reported_as_incomplete(self):
        record = run_session([sys.executable, "-c", "raise SystemExit(7)"],
                             ["first", "second"], 1)
        self.assertEqual(record["returncode"], 7)
        self.assertFalse(record["completed"])

    def test_timeout_bounds_turn_writes_without_readiness_marker(self):
        record = run_session(
            [sys.executable, "-c", "import sys,time; input(); time.sleep(1)"],
            ["first", "second"], 0.01
        )
        self.assertEqual(record["turns_sent"], ["first"])
        self.assertTrue(record["timed_out"])
        self.assertFalse(record["completed"])

    def test_inter_turn_output_is_drained_without_readiness_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "turns"
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "import sys\n"
                "first = sys.stdin.readline().rstrip('\\n')\n"
                "print('x' * 100000, flush=True)\n"
                "second = sys.stdin.readline().rstrip('\\n')\n"
                "open(sys.argv[1], 'w').write(first + '|' + second)\n"
            )
            record = run_session([sys.executable, str(backend), str(marker)],
                                 ["first", "second"], 2)
            self.assertTrue(record["completed"])
            self.assertEqual(marker.read_text(), "first|second")

    def test_timeout_kills_backend_descendants(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "child-alive"
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "import subprocess, sys, time\n"
                "child = subprocess.Popen([sys.executable, '-c', "
                "'from pathlib import Path; import time; "
                "time.sleep(0.2); Path(sys.argv[1]).write_text(\"alive\")', "
                "sys.argv[1]])\n"
                "input()\n"
                "time.sleep(1)\n"
            )
            record = run_session([sys.executable, str(backend), str(marker)],
                                 ["first"], 0.05)
            self.assertTrue(record["timed_out"])
            self.assertFalse(record["completed"])
            time.sleep(0.3)
            self.assertFalse(marker.exists())

    def test_clean_backend_exit_kills_descendants(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "child-alive"
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "import subprocess, sys, time\n"
                "child_code = (\"import sys, time; time.sleep(0.2); \"\n"
                "             \"open(sys.argv[1], 'w').write('alive')\")\n"
                "subprocess.Popen([sys.executable, '-c', child_code,\n"
                "                 sys.argv[1]])\n"
                "input()\n"
            )
            record = run_session([sys.executable, str(backend), str(marker)],
                                 ["first"], 1)
            self.assertEqual(record["returncode"], 0)
            self.assertTrue(record["completed"])
            time.sleep(0.3)
            self.assertFalse(marker.exists())

    def test_readiness_marker_gates_each_turn(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "turns"
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "import sys, time\n"
                "print('> ', end='', flush=True)\n"
                "first = sys.stdin.readline().rstrip('\\n')\n"
                "print('reply', flush=True)\n"
                "print('> ', end='', flush=True)\n"
                "second = sys.stdin.readline().rstrip('\\n')\n"
                "open(sys.argv[1], 'w').write(first + '|' + second)\n"
            )
            record = run_session([sys.executable, str(backend), str(marker)],
                                 ["first", "second"], 2, ready_marker="> ")
            self.assertEqual(record["returncode"], 0)
            self.assertEqual(record["turns_sent"], ["first", "second"])
            self.assertEqual(record["ready_markers_seen"], 2)
            self.assertEqual(marker.read_text(), "first|second")

    def test_output_evidence_is_bounded_and_marks_truncation(self):
        record = run_session(
            [sys.executable, "-c", "print('x' * 100, flush=True); input()"],
            ["turn"], 1, ready_marker="", exit_command="", max_output_bytes=16)
        self.assertTrue(record["completed"])
        self.assertTrue(record["output_truncated"])
        self.assertLessEqual(len(record["stdout_stderr"].encode()), 16)

    def test_readiness_wait_marks_truncation(self):
        record = run_session(
            [sys.executable, "-c",
             "print('x' * 100, end='', flush=True); input()"],
            ["turn"], 1, ready_marker="ready", exit_command="",
            max_output_bytes=16)
        self.assertFalse(record["completed"])
        self.assertTrue(record["output_truncated"])
        self.assertLessEqual(len(record["stdout_stderr"].encode()), 16)

    def test_readiness_marker_is_consumed_not_recounted(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "turns"
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "import sys\n"
                "print('banner > ', flush=True)\n"
                "first = sys.stdin.readline().rstrip('\\n')\n"
                "print('reply', flush=True)\n"
                "second = sys.stdin.readline().rstrip('\\n')\n"
                "open(sys.argv[1], 'w').write(first + '|' + second)\n"
            )
            record = run_session([sys.executable, str(backend), str(marker)],
                                 ["first", "second"], 1, ready_marker="> ")
            self.assertEqual(record["returncode"], -9)
            self.assertEqual(record["turns_sent"], ["first"])
            self.assertEqual(record["ready_markers_seen"], 1)
            self.assertTrue(record["timed_out"])

    def test_turn_text_cannot_supply_a_readiness_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "import sys\n"
                "print('> ', end='', flush=True)\n"
                "first = sys.stdin.readline().rstrip('\\n')\n"
                "print('reply', flush=True)\n"
                "second = sys.stdin.readline().rstrip('\\n')\n"
                "open(sys.argv[1], 'w').write(first + '|' + second)\n"
            )
            record = run_session(
                [sys.executable, str(backend), str(Path(directory) / "turns")],
                ["text > marker", "second"], 1, ready_marker="> "
            )
            self.assertEqual(record["ready_markers_seen"], 1)
            self.assertEqual(record["turns_sent"], ["text > marker"])
            self.assertTrue(record["timed_out"])

    def test_backend_exit_after_marker_is_reported_as_incomplete(self):
        record = run_session(
            [sys.executable, "-c", "print('> ', end='', flush=True)"],
            ["first", "second"], 1, ready_marker="> ")
        self.assertEqual(record["returncode"], 0)
        self.assertLess(len(record["turns_sent"]), 2)
        self.assertEqual(record["ready_markers_seen"], 1)
        self.assertFalse(record["completed"])

    def test_marker_emitted_before_clean_exit_is_consumed(self):
        record = run_session(
            [sys.executable, "-c", "print('> ', end='', flush=True)"],
            [], 1, ready_marker="> ", exit_command="/exit")
        self.assertEqual(record["returncode"], 0)
        self.assertEqual(record["ready_markers_seen"], 1)
        self.assertTrue(record["completed"])

    def test_buffered_marker_is_consumed_after_immediate_clean_exit(self):
        record = run_session(
            [sys.executable, "-c", "import os; os.write(1, b'> '); os._exit(0)"],
            [], 1, ready_marker="> ", exit_command="/exit"
        )
        self.assertEqual(record["returncode"], 0)
        self.assertEqual(record["ready_markers_seen"], 1)
        self.assertTrue(record["completed"])

    def test_exit_command_allows_clean_session_shutdown(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "turns"
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "import sys\n"
                "print('> ', end='', flush=True)\n"
                "first = sys.stdin.readline().rstrip('\\n')\n"
                "print('reply', flush=True)\n"
                "print('> ', end='', flush=True)\n"
                "second = sys.stdin.readline().rstrip('\\n')\n"
                "print('reply', flush=True)\n"
                "print('> ', end='', flush=True)\n"
                "command = sys.stdin.readline().rstrip('\\n')\n"
                "open(sys.argv[1], 'w').write(first + '|' + second + '|' + command)\n"
                "sys.exit(0 if command == '/exit' else 3)\n"
            )
            record = run_session(
                [sys.executable, str(backend), str(marker)],
                ["first", "second"], 2, ready_marker="> ", exit_command="/exit"
            )
            self.assertEqual(record["returncode"], 0)
            self.assertFalse(record["timed_out"])
            self.assertTrue(record["completed"])
            self.assertTrue(record["exit_command_sent"])
            self.assertEqual(record["ready_markers_seen"], 3)
            self.assertEqual(marker.read_text(), "first|second|/exit")

    def test_empty_session_can_exit_after_initial_readiness(self):
        record = run_session(
            [sys.executable, "-c",
             "print('> ', end='', flush=True); input(); raise SystemExit(0)"],
            [], 1, ready_marker="> ", exit_command="/exit"
        )
        self.assertEqual(record["returncode"], 0)
        self.assertEqual(record["ready_markers_seen"], 1)
        self.assertTrue(record["exit_command_sent"])
        self.assertTrue(record["completed"])

    def test_empty_session_waits_for_readiness_before_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "command"
            backend = Path(directory) / "backend.py"
            backend.write_text(
                "import sys, time\n"
                "time.sleep(0.05)\n"
                "print('> ', end='', flush=True)\n"
                "command = sys.stdin.readline().rstrip('\\n')\n"
                "open(sys.argv[1], 'w').write(command)\n"
            )
            record = run_session(
                [sys.executable, str(backend), str(marker)], [], 1,
                ready_marker="> ", exit_command="/exit"
            )
            self.assertTrue(record["completed"])
            self.assertEqual(record["ready_markers_seen"], 1)
            self.assertEqual(marker.read_text(), "/exit")

    def test_exit_command_write_failure_is_structured(self):
        record = run_session(
            [sys.executable, "-c", "print('> ', end='', flush=True)"],
            ["first"], 1, ready_marker="> ", exit_command="/exit"
        )
        self.assertEqual(record["returncode"], 0)
        self.assertEqual(record["turns_sent"], ["first"])
        self.assertFalse(record["exit_command_sent"])
        self.assertFalse(record["completed"])

    def test_exit_command_requires_final_readiness_marker(self):
        record = run_session(
            [sys.executable, "-c", "print('> ', end='', flush=True); input(); input()"],
            ["first"], 0.1, ready_marker="> ", exit_command="/exit"
        )
        self.assertFalse(record["exit_command_sent"])
        self.assertFalse(record["completed"])

    def test_nonzero_backend_exit_is_not_complete(self):
        record = run_session(
            [sys.executable, "-c", "print('> ', end='', flush=True); input(); print('> ', end='', flush=True); input(); raise SystemExit(3)"],
            ["first"], 1, ready_marker="> ", exit_command="/exit"
        )
        self.assertEqual(record["returncode"], 3)
        self.assertTrue(record["exit_command_sent"])
        self.assertFalse(record["completed"])


if __name__ == "__main__":
    unittest.main()
