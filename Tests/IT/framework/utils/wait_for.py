"""Synchronous polling helper used in behave step definitions."""

import time
from typing import Callable


def wait_for(
    condition: Callable[[], bool],
    timeout_sec: float,
    poll_interval_sec: float = 0.1,
) -> bool:
    """Block until *condition()* returns True or *timeout_sec* elapses.

    Returns True if the condition became True within the timeout, False otherwise.
    """
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(poll_interval_sec)
    return False
