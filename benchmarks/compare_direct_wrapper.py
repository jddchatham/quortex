"""Compare direct and QUORTEX single-turn output with fixed backend settings."""

import argparse
import hashlib
import subprocess
import sys
import time


def run(command):
    started = time.perf_counter()
    result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=90)
    return {
        "returncode": result.returncode,
        "seconds": round(time.perf_counter() - started, 3),
        "stdout": result.stdout,
        "stderr": result.stderr,
        "stdout_sha256": hashlib.sha256(result.stdout.encode()).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--backend", required=True)
    parser.add_argument("--prompt", default="Write one short sentence about quadrants.")
    args = parser.parse_args()
    common = ["-m", args.model, "-p", args.prompt, "-n", "16", "-t", "2", "-ngl", "0", "--seed", "123", "--no-warmup", "--no-display-prompt"]
    direct = run([args.backend, *common])
    wrapper = run([sys.executable, "-m", "quortex.cli", "-m", args.model, "--backend", args.backend, "--single-turn", "-p", args.prompt, "-n", "16", "--seed", "123", "--no-warmup", "--no-display-prompt"])
    result = {
        "direct": {key: value for key, value in direct.items() if key != "stderr"},
        "wrapper": {key: value for key, value in wrapper.items() if key != "stderr"},
        "stdout_identical": direct["stdout"] == wrapper["stdout"],
        "direct_text": direct["stdout"],
        "wrapper_text": wrapper["stdout"],
    }
    import json
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["stdout_identical"] and direct["returncode"] == wrapper["returncode"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

