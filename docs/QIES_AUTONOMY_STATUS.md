# Autonomy status

The installed environment provides `/home/user/.local/bin/codex` with a supported noninteractive `codex exec` command. This session is not itself an indefinitely running agent. A bounded supervisor may invoke one cycle at a time, subject to authentication, approvals, quotas, and host limits.

No unattended cycle is started automatically by this repository. The optional workflow must use a lock, a wall-time limit, a single assignment, and explicit test/commit/push policy. A queue file is durable state, not proof of continuous execution.

