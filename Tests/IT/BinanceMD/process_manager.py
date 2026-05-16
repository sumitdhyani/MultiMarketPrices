"""Manages the BinanceMD subprocess lifecycle for integration tests.

The binary is launched with a single positional argument: the appId.

ConfigLib hardcodes the config path as `./config/config.json` relative to the
process's working directory.  BinanceMDProcess therefore sets cwd to the
Tests/IT/ directory so that ConfigLib finds `Tests/IT/config/config.json`.

Usage:
    proc = BinanceMDProcess()
    proc.start()
    ...
    proc.stop()
"""

import os
import signal
import subprocess
from typing import Optional

# Absolute path to Tests/IT/BinanceMD/ — the working directory for BinanceMD so
# that ConfigLib finds ./config/config.json (= Tests/IT/BinanceMD/config/config.json).
_IT_DIR = os.path.dirname(os.path.abspath(__file__))

# Resolve the binary relative to this file's location:
#   Tests/IT/utils/process_manager.py  →  build/MDGateways/Binance/BinanceMD
_DEFAULT_BINARY = os.environ.get(
    "BINANCE_MD_BINARY",
    os.path.normpath(
        os.path.join(
            os.path.dirname(__file__),           # Tests/IT/utils/
            "../../../build/MDGateways/Binance/BinanceMD",
        )
    ),
)


class BinanceMDProcess:
    """Wraps a BinanceMD subprocess for use in behave steps."""

    def __init__(
        self,
        app_id: str = "BinanceMD_1",
        binary_path: str = _DEFAULT_BINARY,
    ) -> None:
        self._app_id = app_id
        self._binary_path = binary_path
        self._process: Optional[subprocess.Popen] = None

    def start(self) -> None:
        """Launch BinanceMD with cwd=Tests/IT/ so ConfigLib finds config/config.json."""
        if self._process and self._process.poll() is None:
            return  # already running
        self._process = subprocess.Popen(
            [self._binary_path, self._app_id],
            cwd=_IT_DIR,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def stop(self, timeout_sec: float = 5.0) -> None:
        """Send SIGTERM and wait; SIGKILL if the process does not exit in time."""
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
