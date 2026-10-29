#!/usr/bin/env python3
"""Submit one allowlisted inference request and wait for its response."""
from __future__ import annotations
import argparse, json, os, secrets, time
from pathlib import Path
try:
    from .inference_service import BASE, INBOX, RESPONSES
except ImportError:
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from quortex.autonomy.inference_service import BASE, INBOX, RESPONSES

def main() -> int:
    p=argparse.ArgumentParser(); p.add_argument("--operation", choices=["two_token_reference", "two_token_quortex", "two_layer_quortex"], required=True); p.add_argument("--model", required=True); p.add_argument("--output", required=True); p.add_argument("--evidence", required=True); p.add_argument("--index"); p.add_argument("--timeout", type=int, default=120); a=p.parse_args()
    rid=f"req-{int(time.time())}-{secrets.token_hex(6)}"; req={"request_id":rid,"operation":a.operation,"model":a.model,"output":a.output,"evidence":a.evidence}
    if a.index: req["index"]=a.index
    INBOX.mkdir(parents=True, exist_ok=True); RESPONSES.mkdir(parents=True, exist_ok=True)
    tmp=INBOX/f".{rid}.tmp"; tmp.write_text(json.dumps(req)); os.replace(tmp, INBOX/f"{rid}.json")
    end=time.time()+a.timeout; response=RESPONSES/f"{rid}.json"
    while time.time()<end:
        if response.exists(): print(response.read_text()); return 0 if json.loads(response.read_text()).get("status")=="COMPLETED" else 1
        time.sleep(.25)
    print(json.dumps({"request_id":rid,"status":"TIMEOUT"})); return 124
if __name__ == "__main__": raise SystemExit(main())
