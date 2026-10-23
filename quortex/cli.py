"""Interactive QUORTEX launcher around a llama.cpp-compatible CLI."""

import argparse
import json
from pathlib import Path
import sys

from .inference import executable_available, make_plan, run_chat


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Run an admitted QUORTEX conversation")
    parser.add_argument("-m", "--model", required=True, help="path to a GGUF model")
    parser.add_argument("--backend", default="llama-cli", help="trusted llama.cpp-compatible executable")
    parser.add_argument("-p", "--prompt", help="optional initial prompt/system prompt")
    parser.add_argument("--require", action="append", default=[], help="context requirement; repeatable")
    parser.add_argument("--property", action="append", dest="properties", default=["model", "runtime", "chat"], help="candidate property; repeatable")
    parser.add_argument("--json-decision", action="store_true", help="print the officiation decision and exit")
    parser.add_argument("--single-turn", action="store_true", help="use deterministic non-conversation backend mode")
    args, extra = parser.parse_known_args(argv)

    if not Path(args.model).is_file():
        print(json.dumps({"accepted": False, "reason": "model-not-found", "model": args.model}), file=sys.stderr)
        return 2
    if not executable_available(args.backend):
        print(json.dumps({"accepted": False, "reason": "backend-not-found", "backend": args.backend}), file=sys.stderr)
        return 2

    plan, candidate, context, decision = make_plan(
        args.backend, args.model, args.require, args.properties, args.prompt, not args.single_turn
    )
    record = {
        "candidate": candidate.key,
        "quadrant": list(decision.quadrant.bits),
        "accepted": decision.accepted,
        "reason": decision.reason,
        "requirements": sorted(context.requirements),
        "backend": str(plan.executable),
    }
    if args.json_decision:
        print(json.dumps(record, sort_keys=True))
        return 0 if decision.accepted else 3
    if not decision.accepted:
        print(json.dumps(record, sort_keys=True))
        return 3
    return run_chat(plan, extra)


if __name__ == "__main__":
    raise SystemExit(main())
