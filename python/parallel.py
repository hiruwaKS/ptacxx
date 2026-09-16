#!/usr/bin/env python3
"""Shared parallel-execution helper extracted from compile.py.

Runs a list of items through a thread pool and, like compile.py, retries
items that time out with progressively longer timeouts. Child processes
are started in their own session so a timeout can kill the whole process
group instead of leaving orphans behind.
"""

import concurrent.futures
import os
import signal
import subprocess
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Optional, Sequence


DEFAULT_TIMEOUT_STEPS = (2.0, 8.0, 32.0, 128.0)


@dataclass
class ProcessResult:
    returncode: int
    stdout: str
    stderr: str
    timed_out: bool = False


def run_process(argv, timeout, stdin=None, cwd=None, env=None) -> ProcessResult:
    """Run `argv` in its own session, killing the process group on timeout."""
    proc = subprocess.Popen(
        argv,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        cwd=cwd,
        env=env,
        start_new_session=True,
    )
    try:
        out, err = proc.communicate(input=stdin, timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except OSError:
            pass
        proc.communicate()
        return ProcessResult(returncode=-1, stdout="", stderr="", timed_out=True)
    return ProcessResult(proc.returncode, out, err)


@dataclass
class TaskResult:
    status: str          # "OK" | "FAIL" | "TIMEOUT" (or any caller-defined status)
    item: Any
    payload: Any = None


class ParallelHelper:
    """Run tasks concurrently, retrying timeouts with longer limits.

    `run_one(item, timeout)` must return a `TaskResult`; a result whose
    status is `"TIMEOUT"` is retried with the next timeout step. Items
    still pending after the last step are reported as `"TIMEOUT"`.
    """

    def __init__(
        self,
        max_workers: int = 0,
        timeout_steps: Sequence[float] = DEFAULT_TIMEOUT_STEPS,
    ):
        self.max_workers = max_workers
        self.timeout_steps = tuple(timeout_steps)

    def worker_count(self, pending: int) -> int:
        if pending <= 0:
            return 0
        workers = self.max_workers or (os.cpu_count() or 4)
        return max(1, min(workers, pending))

    def run(
        self,
        items: Iterable[Any],
        run_one: Callable[[Any, float], TaskResult],
        on_result: Callable[[TaskResult], None],
        on_step: Optional[Callable[[int, float, int], None]] = None,
    ) -> None:
        """Run `items`, calling `on_result` once per item.

        `on_step(step, timeout, pending)` is called before each step.
        """
        pending = list(items)
        for step, timeout in enumerate(self.timeout_steps):
            if not pending:
                break
            if on_step is not None:
                on_step(step, timeout, len(pending))
            still = []
            with concurrent.futures.ThreadPoolExecutor(
                max_workers=self.worker_count(len(pending))
            ) as ex:
                futs = {ex.submit(run_one, item, timeout): item for item in pending}
                for fut in concurrent.futures.as_completed(futs):
                    res = fut.result()
                    if res.status == "TIMEOUT":
                        still.append(res.item)
                    else:
                        on_result(res)
            pending = still
        for item in pending:
            on_result(TaskResult("TIMEOUT", item))
