"""Record a bounded two-turn persistent llama.cpp session.

The backend is kept alive while both user turns are sent through one PTY.  The
result is JSON evidence, not a correctness claim: callers must compare the
recorded transcript with an independently prepared oracle.
"""

import argparse
import hashlib
import json
import os
import pty
import select
import subprocess
import sys
import time
from pathlib import Path


def run_session(command, turns, timeout):
    started = time.perf_counter()
    master, slave = pty.openpty()
    process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
                               close_fds=True)
    os.close(slave)
    output = bytearray()
    sent = []
    deadline = time.monotonic() + timeout
    try:
        for turn in turns:
            if process.poll() is not None:
                break
            os.write(master, (turn + "\n").encode())
            sent.append(turn)
            # Give the backend a bounded opportunity to consume this turn.
            time.sleep(0.05)
        while process.poll() is None and time.monotonic() < deadline:
            readable, _, _ = select.select([master], [], [], 0.1)
            if readable:
                try:
                    output.extend(os.read(master, 65536))
                except OSError:
                    break
        timed_out = process.poll() is None and time.monotonic() >= deadline
        if process.poll() is None:
            process.kill()
        try:
            output.extend(os.read(master, 65536))
        except OSError:
            pass
        return {
            "command": command,
            "turns_requested": turns,
            "turns_sent": sent,
            "returncode": process.wait(),
            "timed_out": timed_out,
            "seconds": round(time.perf_counter() - started, 3),
            "stdout_stderr": output.decode(errors="replace"),
            "output_sha256": hashlib.sha256(output).hexdigest(),
        }
    finally:
        os.close(master)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--backend", required=True)
    parser.add_argument("--turn", action="append", required=True)
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    command = [args.backend, "-m", args.model, "-t", "2", "-ngl", "0",
               "-cnv", "--simple-io", "-n", "8", "--seed", "123",
               "--no-warmup", "--no-display-prompt"]
    record = run_session(command, args.turn, args.timeout)
    encoded = json.dumps(record, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded)
    print(encoded, end="")
    return 0 if record["returncode"] == 0 and len(record["turns_sent"]) == len(args.turn) else 1


if __name__ == "__main__":
    raise SystemExit(main())
