"""Pre-inference QUORTEX gate for an existing llama.cpp-compatible runtime."""

from dataclasses import dataclass
from pathlib import Path
import shutil
import subprocess
from typing import Iterable, Optional

from .quadrant import Candidate, Context, Decision, Officiator


@dataclass(frozen=True)
class LaunchPlan:
    executable: Path
    model: Path
    prompt: Optional[str]
    requirements: frozenset[str]
    properties: frozenset[str]


def make_plan(
    executable: str,
    model: str,
    requirements: Iterable[str] = (),
    properties: Iterable[str] = ("model", "runtime", "chat"),
    prompt: Optional[str] = None,
) -> tuple[LaunchPlan, Candidate, Context, Decision]:
    exe = Path(executable)
    model_path = Path(model)
    candidate = Candidate(
        key=str(model_path),
        first=str(model_path),
        second=str(exe),
        properties=frozenset(properties),
    )
    context = Context.from_requirements(requirements)
    decision = Officiator().officiate(candidate, context)
    return LaunchPlan(exe, model_path, prompt, context.requirements, candidate.properties), candidate, context, decision


def run_chat(plan: LaunchPlan, extra_args: Iterable[str] = ()) -> int:
    """Run the trusted backend only after the caller has accepted the decision."""
    command = [
        str(plan.executable),
        "-m", str(plan.model),
        "-cnv",
        "--simple-io",
        "-t", "2",
        "-ngl", "0",
    ]
    if plan.prompt is not None:
        command.extend(["-p", plan.prompt])
    command.extend(extra_args)
    return subprocess.call(command)


def executable_available(executable: str) -> bool:
    return Path(executable).is_file() or shutil.which(executable) is not None

