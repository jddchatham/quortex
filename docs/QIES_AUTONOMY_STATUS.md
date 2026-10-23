# Autonomy status

The installed environment provides `/home/user/.local/bin/codex` with a supported noninteractive `codex exec` command. This session is not itself an indefinitely running agent. A bounded supervisor may invoke one cycle at a time, subject to authentication, approvals, quotas, and host limits.

The bounded supervisor is implemented at `quortex/autonomy/supervisor.py` and a user-service unit is provided at `deploy/quortex-autonomy.service`. Two dry-run cycles passed, and one real Codex cycle completed; the child could not commit because its managed environment reported `.git` read-only. The controlling session must commit verified changes before enabling the service. A queue file is durable state, not proof of continuous execution.
