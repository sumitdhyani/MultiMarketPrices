"""Manages the BinanceMD subprocess lifecycle for integration tests.

The binary is launched with a single positional argument: the appId.

ConfigLib hardcodes the config path as `./config/config.json` relative to the
process's working directory.  The process manager therefore sets cwd to the
Tests/IT/BinanceMD/ directory so that ConfigLib finds
`Tests/IT/BinanceMD/config/config.json`.

Usage:
    proc = BinanceMDProcess()
    proc.start()
    ...
    proc.stop()
"""

import os
import signal
import subprocess
from typing import Any, Optional

# Absolute path to Tests/IT/BinanceMD/ — the shared working directory for all
# binaries so that ConfigLib finds ./config/config.json.
_IT_DIR = os.path.dirname(os.path.abspath(__file__))

_DEFAULT_BINANCE_BINARY = os.environ.get(
    "BINANCE_MD_BINARY",
    os.path.normpath(
        os.path.join(_IT_DIR, "../../../build/MDGateways/Binance/BinanceMD")
    ),
)


class _BaseProcess:
    """Shared subprocess lifecycle logic."""

    def __init__(self, binary_path: str, app_id: str) -> None:
        self._binary_path = binary_path
        self._app_id = app_id
        self._process: Optional[subprocess.Popen] = None

    def start(self, env_extra: Optional[dict] = None, log_file: Optional[str] = None) -> None:
        if self._process and self._process.poll() is None:
            return
        env = os.environ.copy()
        if env_extra:
            env.update(env_extra)
        if log_file:
            log_fd = open(log_file, "w")
            stdout_dest: Any = log_fd
            stderr_dest: Any = log_fd
        else:
            stdout_dest = subprocess.DEVNULL
            stderr_dest = subprocess.DEVNULL
        self._process = subprocess.Popen(
            [self._binary_path, self._app_id],
            cwd=_IT_DIR,
            stdout=stdout_dest,
            stderr=stderr_dest,
            env=env,
        )

    def stop(self, timeout_sec: float = 5.0) -> None:
        if self._process is None:
            return
        if self._process.poll() is None:
            self._process.send_signal(signal.SIGTERM)
            try:
                self._process.wait(timeout=timeout_sec)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait()
        self._process = None

    def is_running(self) -> bool:
        return self._process is not None and self._process.poll() is None


class BinanceMDProcess(_BaseProcess):
    """Wraps a BinanceMD subprocess for use in behave steps."""

    def __init__(
        self,
        app_id: str,
        binary_path: str = _DEFAULT_BINANCE_BINARY,
    ) -> None:
        super().__init__(binary_path, app_id)
