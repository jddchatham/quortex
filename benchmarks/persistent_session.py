"""Record a bounded persistent llama.cpp session.

The backend is kept alive while both user turns are sent through one PTY.  The
result is JSON evidence, not a correctness claim: callers must compare the
recorded transcript with an independently prepared oracle.
"""

import argparse
import hashlib
import json
import math
import numbers
import os
import pty
import select
import signal
import subprocess
import sys
import termios
import time
from pathlib import Path


def _append_output(output, data, limit):
    remaining = limit - len(output)
    if remaining <= 0:
        return True
    output.extend(data[:remaining])
    return len(data) > remaining


def _wait_for_marker(master, process, output, marker, deadline, search_from,
                     limit):
    """Read until the next marker after search_from, or until the deadline."""
    marker = marker.encode()
    truncated = False
    while time.monotonic() < deadline:
        position = output.find(marker, search_from)
        if position >= 0:
            return position + len(marker), truncated
        if process.poll() is not None:
            # A short-lived backend can exit after writing its final marker
            # while those bytes are still queued in the PTY. Drain that
            # bounded residue before declaring the marker absent.
            truncated |= _drain_available(
                master, output, min(deadline, time.monotonic() + 0.05), limit)
            break
        readable, _, _ = select.select([master], [], [], 0.1)
        if readable:
            try:
                truncated |= _append_output(output, os.read(master, 65536), limit)
            except OSError:
                break
    position = output.find(marker, search_from)
    marker_end = position + len(marker) if position >= 0 else None
    return marker_end, truncated


def _drain_available(master, output, deadline, limit):
    """Drain PTY output during the bounded inter-turn handoff."""
    while time.monotonic() < deadline:
        readable, _, _ = select.select([master], [], [], 0.01)
        if not readable:
            return False
        try:
            truncated = _append_output(output, os.read(master, 65536), limit)
            if truncated:
                return True
        except OSError:
            return False
    return False


def _kill_process_group(process_group):
    """Stop the bounded session and any descendants it created."""
    try:
        os.killpg(process_group, signal.SIGKILL)
    except ProcessLookupError:
        pass


def _write_line(master, line):
    """Write one complete UTF-8 line, tolerating partial PTY writes."""
    payload = (line + "\n").encode()
    written = 0
    while written < len(payload):
        count = os.write(master, payload[written:])
        if count <= 0:
            raise OSError("PTY write made no progress")
        written += count


def _session_sha256(command, turns, timeout, ready_marker, exit_command, limit):
    """Hash the exact bounded session specification independently of output."""
    specification = {
        "command": list(command),
        "turns": list(turns),
        "timeout": timeout,
        "ready_marker": ready_marker,
        "exit_command": exit_command,
        "max_output_bytes": limit,
    }
    encoded = json.dumps(specification, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False).encode()
    return hashlib.sha256(encoded).hexdigest()


def run_session(command, turns, timeout, ready_marker=None, exit_command=None,
                max_output_bytes=1024 * 1024):
    if not isinstance(command, (list, tuple)) or not command:
        raise ValueError("persistent-session command must be a non-empty sequence")
    if any(not isinstance(part, str) for part in command):
        raise ValueError("persistent-session command entries must be strings")
    # Keep the returned evidence JSON-safe even when callers provide a tuple.
    command = list(command)
    turns = list(turns)
    if any(not isinstance(turn, str) for turn in turns):
        raise ValueError("persistent-session turns must be strings")
    if any("\n" in turn or "\r" in turn for turn in turns):
        raise ValueError("persistent-session turns must be single-line strings")
    if (isinstance(timeout, bool) or not isinstance(timeout, numbers.Real)
            or not math.isfinite(timeout) or timeout <= 0):
        raise ValueError("persistent-session timeout must be positive")
    if (isinstance(max_output_bytes, bool)
            or not isinstance(max_output_bytes, numbers.Integral)
            or max_output_bytes <= 0):
        raise ValueError("persistent-session output limit must be positive")
    if ready_marker is not None:
        if not isinstance(ready_marker, str):
            raise ValueError("persistent-session ready marker must be a string")
        if "\n" in ready_marker or "\r" in ready_marker:
            raise ValueError("persistent-session ready marker must be single-line")
    if exit_command is not None:
        if not isinstance(exit_command, str):
            raise ValueError("persistent-session exit command must be a string")
        if "\n" in exit_command or "\r" in exit_command:
            raise ValueError("persistent-session exit command must be single-line")
    # Match the CLI contract: an empty control string disables that phase.
    ready_marker = ready_marker or None
    exit_command = exit_command or None
    session_sha256 = _session_sha256(command, turns, timeout, ready_marker,
                                     exit_command, max_output_bytes)
    started = time.perf_counter()
    master, slave = pty.openpty()
    terminal = termios.tcgetattr(slave)
    terminal[3] &= ~(termios.ECHO | termios.ECHONL)
    termios.tcsetattr(slave, termios.TCSANOW, terminal)
    process = None
    process_group = None
    try:
        process = subprocess.Popen(command, stdin=slave, stdout=slave,
                                   stderr=slave, close_fds=True,
                                   start_new_session=True)
        process_group = os.getpgid(process.pid)
    except BaseException:
        if process is not None and process.poll() is None:
            process.kill()
        if process is not None:
            process.wait()
        os.close(slave)
        os.close(master)
        raise
    os.close(slave)
    output = bytearray()
    sent = []
    ready_markers_seen = 0
    marker_search_from = 0
    exit_command_sent = False
    final_ready = ready_marker is None
    output_truncated = False
    deadline = time.monotonic() + timeout
    try:
        for turn in turns:
            if process.poll() is not None or time.monotonic() >= deadline:
                break
            if ready_marker:
                marker_end, marker_truncated = _wait_for_marker(
                    master, process, output, ready_marker, deadline,
                    marker_search_from, max_output_bytes)
                output_truncated |= marker_truncated
                if marker_end is None:
                    break
                marker_search_from = marker_end
                ready_markers_seen += 1
            try:
                _write_line(master, turn)
            except OSError:
                # The backend may exit after emitting its marker but before
                # consuming the next turn. Preserve structured evidence.
                break
            sent.append(turn)
            # Give the backend a bounded opportunity to consume this turn.
            handoff_deadline = min(deadline, time.monotonic() + 0.05)
            output_truncated |= _drain_available(master, output, handoff_deadline,
                                                 max_output_bytes)
            if process.poll() is not None:
                break
        if (not turns and exit_command is not None and ready_marker
                and process.poll() is None):
            marker_end, marker_truncated = _wait_for_marker(
                master, process, output, ready_marker, deadline,
                marker_search_from, max_output_bytes)
            output_truncated |= marker_truncated
            if marker_end is not None:
                marker_search_from = marker_end
                ready_markers_seen += 1
                final_ready = True
        if (exit_command is not None and ready_marker and not final_ready
                and len(sent) == len(turns) and process.poll() is None):
            marker_end, marker_truncated = _wait_for_marker(
                master, process, output, ready_marker, deadline,
                marker_search_from, max_output_bytes)
            output_truncated |= marker_truncated
            if marker_end is not None:
                marker_search_from = marker_end
                ready_markers_seen += 1
                final_ready = True
        if (exit_command is not None and len(sent) == len(turns)
                and final_ready and process.poll() is None):
            try:
                _write_line(master, exit_command)
                exit_command_sent = True
            except OSError:
                # Preserve evidence when the backend closes after its final
                # readiness marker but before consuming the exit command.
                pass
        while process.poll() is None and time.monotonic() < deadline:
            readable, _, _ = select.select([master], [], [], 0.1)
            if readable:
                try:
                    output_truncated |= _append_output(
                        output, os.read(master, 65536), max_output_bytes)
                except OSError:
                    break
        # A child can exit in the small race between the deadline check and
        # this observation (for example after receiving a terminal hangup).
        # Preserve the actual wall-clock contract rather than reporting a
        # clean completion merely because poll() happened to reap first.
        timed_out = time.monotonic() >= deadline
        # The session leader may have exited while a descendant is still
        # alive in its dedicated process group.  The group ID was captured
        # before launch could complete, so cleanup remains scoped to this
        # session even when poll() already observed an exit.
        leader_returncode = process.poll()
        _kill_process_group(process_group)
        try:
            output_truncated |= _append_output(
                output, os.read(master, 65536), max_output_bytes)
        except OSError:
            pass
        # Preserve a clean leader result when descendant cleanup races with
        # the leader's already-observed exit.  The process group is dedicated
        # to this session, so it remains safe to kill for descendant cleanup.
        returncode = (leader_returncode if leader_returncode is not None
                      else process.wait())
        graceful_empty_exit = (exit_command is not None and not turns
                               and final_ready and returncode == 0)
        completed = (returncode == 0 and not timed_out and len(sent) == len(turns)
                     and (exit_command is None or exit_command_sent
                          or graceful_empty_exit))
        return {
            "command": command,
            "turns_requested": turns,
            "session_sha256": session_sha256,
            "turns_sent": sent,
            "ready_marker": ready_marker,
            "ready_markers_seen": ready_markers_seen,
            "exit_command": exit_command,
            "exit_command_sent": exit_command_sent,
            "returncode": returncode,
            "timed_out": timed_out,
            "completed": completed,
            "seconds": round(time.perf_counter() - started, 3),
            "stdout_stderr": output.decode(errors="replace"),
            "output_sha256": hashlib.sha256(output).hexdigest(),
            "max_output_bytes": max_output_bytes,
            "output_truncated": output_truncated,
        }
    finally:
        if process is not None and process.poll() is None:
            _kill_process_group(process_group)
            process.wait()
        os.close(master)


def build_command(backend, model, system_prompt):
    return [backend, "-m", model, "-t", "2", "-ngl", "0",
            "-cnv", "--interactive-first", "--simple-io", "-n", "8", "--seed", "123",
            "--no-warmup", "--no-display-prompt", "-p", system_prompt]


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--backend", required=True)
    parser.add_argument("--system-prompt", default="You are a concise assistant.",
                        help="conversation system prompt required by llama.cpp")
    parser.add_argument("--ready-marker", default="> ",
                        help="prompt marker to await before each turn; empty disables waiting")
    parser.add_argument("--exit-command", default="/exit",
                        help="command sent after all turns; empty disables graceful exit")
    parser.add_argument("--turn", action="append", required=True)
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--max-output-bytes", type=int, default=1024 * 1024)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    command = build_command(args.backend, args.model, args.system_prompt)
    record = run_session(command, args.turn, args.timeout, args.ready_marker or None,
                         args.exit_command or None, args.max_output_bytes)
    encoded = json.dumps(record, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded)
    print(encoded, end="")
    return 0 if record["returncode"] == 0 and record["completed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
