#!/usr/bin/env python3
"""Host-side polling service for strictly allowlisted inference requests."""
from __future__ import annotations
import json, os, secrets, subprocess, sys, time
from pathlib import Path
try:
    from .guarded_inference import ROOT
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from quortex.autonomy.guarded_inference import ROOT

BASE = ROOT / "quortex" / "autonomy" / "requests"
INBOX, RESPONSES = BASE / "inbox", BASE / "responses"
OPS = {"two_token_reference": "reference", "two_layer_cortex_reference": "reference", "two_token_quortex": "quortex", "two_layer_quortex": "chain", "two_range_flashnext": "two-range"}
MAX_AGE = 120

def atomic(path: Path, value: dict) -> None:
    tmp = path.with_name(f".{path.name}.{secrets.token_hex(4)}")
    tmp.write_text(json.dumps(value, indent=2) + "\n")
    os.replace(tmp, path)

def reject(req_id: str, reason: str) -> None:
    atomic(RESPONSES / f"{req_id}.json", {"request_id": req_id, "status": "REJECTED", "reason": reason})

def validate(doc: dict) -> tuple[str, list[str]]:
    if set(doc) - {"request_id", "operation", "model", "index", "output", "evidence"}:
        raise ValueError("unexpected request fields")
    req_id = doc.get("request_id")
    if not isinstance(req_id, str) or len(req_id) < 8 or len(req_id) > 80 or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for c in req_id):
        raise ValueError("invalid request id")
    target = OPS.get(doc.get("operation"))
    if target is None:
        raise ValueError("operation is not allowlisted")
    for key in ("model", "output", "evidence"):
        if not isinstance(doc.get(key), str) or not doc[key].startswith("/home/user/"):
            raise ValueError(f"invalid {key}")
    index = doc.get("index")
    if index is not None and (not isinstance(index, str) or not index.endswith(".json")):
        raise ValueError("invalid index")
    return req_id, [target, doc["model"], doc["output"], index or "", doc["evidence"]]

def main() -> int:
    INBOX.mkdir(parents=True, exist_ok=True); RESPONSES.mkdir(parents=True, exist_ok=True)
    lock = BASE / "service.lock"
    with lock.open("a+") as lf:
        import fcntl
        try: fcntl.flock(lf.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: return 2
        while True:
            for path in sorted(INBOX.glob("*.json")):
                try:
                    if time.time() - path.stat().st_mtime > MAX_AGE:
                        path.unlink(missing_ok=True); continue
                    doc = json.loads(path.read_text())
                    req_id, vals = validate(doc)
                    response = RESPONSES / f"{req_id}.json"
                    if response.exists(): raise ValueError("duplicate request id")
                    path.unlink()
                    target, model, output, index, evidence = vals
                    cmd = [sys.executable, str(ROOT / "quortex/autonomy/guarded_inference.py"), "--host-context", "--target", target, "--model", model, "--output", output, "--evidence", evidence]
                    if doc.get("operation") == "two_layer_cortex_reference": cmd += ["--mode", "two-layer"]
                    if index: cmd += ["--index", index]
                    proc = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
                    atomic(response, {"request_id": req_id, "status": "COMPLETED" if proc.returncode == 0 else "FAILED", "returncode": proc.returncode, "stdout": proc.stdout[-4000:], "stderr": proc.stderr[-4000:]})
                except Exception as exc:
                    try: reject(doc.get("request_id", path.stem), str(exc))
                    except Exception: pass
                    path.unlink(missing_ok=True)
            time.sleep(0.25)

if __name__ == "__main__": raise SystemExit(main())
