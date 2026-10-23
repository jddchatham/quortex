"""Small deterministic prototype for contextual convergence.

This module does not inspect or modify model tensors.  Its intended inference
seam is ``Officiator.admit``: a caller can run this cheap check before a costly
candidate producer or Cortex segment.  Admission is never treated as model
correctness; it is a contextual eligibility decision.
"""

from dataclasses import dataclass
from enum import Enum
from typing import FrozenSet, Iterable, Mapping, Optional


class Quadrant(Enum):
    NEITHER = (0, 0)
    FIRST_ONLY = (1, 0)
    SECOND_ONLY = (0, 1)
    COMPLETE = (1, 1)

    @property
    def bits(self) -> tuple[int, int]:
        return self.value


@dataclass(frozen=True)
class Context:
    """Immutable requirements; equality is the context identity."""

    requirements: FrozenSet[str]
    version: str = "0"

    @classmethod
    def from_requirements(cls, requirements: Iterable[str], version: str = "0") -> "Context":
        return cls(frozenset(requirements), version)


@dataclass(frozen=True)
class Candidate:
    key: str
    first: Optional[str] = None
    second: Optional[str] = None
    properties: FrozenSet[str] = frozenset()

    @property
    def quadrant(self) -> Quadrant:
        return Quadrant((self.first is not None, self.second is not None))


@dataclass(frozen=True)
class Decision:
    candidate_key: str
    quadrant: Quadrant
    accepted: bool
    reason: str
    context: Context


class Officiator:
    """Contextual judge with no hidden state and deterministic decisions."""

    def officiate(self, candidate: Candidate, context: Context) -> Decision:
        if candidate.quadrant is not Quadrant.COMPLETE:
            return Decision(candidate.key, candidate.quadrant, False, "incomplete", context)
        missing = context.requirements - candidate.properties
        if missing:
            return Decision(
                candidate.key,
                candidate.quadrant,
                False,
                "missing:" + ",".join(sorted(missing)),
                context,
            )
        return Decision(candidate.key, candidate.quadrant, True, "contextual-match", context)

    def admit(self, metadata: Mapping[str, object], context: Context) -> bool:
        """Cheap pre-inference eligibility gate.

        Producers may expose candidate properties as metadata.  No expensive
        answer computation is performed here; missing metadata is rejected.
        """
        properties = frozenset(str(value) for value in metadata.get("properties", ()))
        return context.requirements.issubset(properties)

    def reevaluate(self, candidate: Candidate, old: Decision, context: Context) -> Decision:
        if old.context == context:
            return old
        return self.officiate(candidate, context)

