"""Small auditable state machine for one bounded QIES cycle."""

STATES = (
    "DISCOVER", "SPECIFY", "RESEARCH", "IMPLEMENT", "BUILD", "TEST",
    "AUDIT", "BENCHMARK", "ACCEPT", "REJECT", "COMMIT", "PUSH",
    "NEXT_ASSIGNMENT", "DIAGNOSE", "REVISE", "INTERRUPTED", "CHECKPOINT",
    "RECOVER", "VALIDATE_STATE",
)

TRANSITIONS = {
    "DISCOVER": {"SPECIFY", "INTERRUPTED"},
    "SPECIFY": {"RESEARCH", "INTERRUPTED"},
    "RESEARCH": {"IMPLEMENT", "INTERRUPTED"},
    "IMPLEMENT": {"BUILD", "INTERRUPTED"},
    "BUILD": {"TEST", "IMPLEMENT", "INTERRUPTED"},
    "TEST": {"AUDIT", "IMPLEMENT", "INTERRUPTED"},
    "AUDIT": {"BENCHMARK", "REJECT", "IMPLEMENT", "INTERRUPTED"},
    "BENCHMARK": {"ACCEPT", "REJECT", "IMPLEMENT", "INTERRUPTED"},
    "ACCEPT": {"COMMIT"},
    "REJECT": {"DIAGNOSE"},
    "DIAGNOSE": {"REVISE"},
    "REVISE": {"IMPLEMENT"},
    "COMMIT": {"PUSH", "NEXT_ASSIGNMENT"},
    "PUSH": {"NEXT_ASSIGNMENT", "INTERRUPTED"},
    "NEXT_ASSIGNMENT": {"DISCOVER", "SPECIFY"},
    "INTERRUPTED": {"CHECKPOINT"},
    "CHECKPOINT": {"RECOVER"},
    "RECOVER": {"VALIDATE_STATE"},
    "VALIDATE_STATE": {"DISCOVER", "SPECIFY", "REJECT"},
}


def transition(current: str, target: str) -> str:
    if current not in STATES or target not in STATES:
        raise ValueError("unknown QIES state")
    if target not in TRANSITIONS[current]:
        raise ValueError(f"invalid QIES transition: {current} -> {target}")
    return target

