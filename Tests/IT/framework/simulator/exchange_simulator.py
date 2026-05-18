"""Abstract base class for simulated exchanges.

Provides an HTTPS REST server and a WSS WebSocket server, both running in the
same asyncio event loop on separate ports.  Subclasses supply the route
definitions for REST and the message handler for WebSocket frames.
"""

import asyncio
import json
import ssl
from abc import ABC, abstractmethod
from typing import Optional

from aiohttp import web, WSMsgType


class ExchangeSimulator(ABC):
    """Reusable base for HTTPS/WSS exchange simulators used in integration tests."""

    def __init__(self, rest_port: int, ws_port: int, certfile: str, keyfile: str) -> None:
        self._rest_port = rest_port
        self._ws_port = ws_port
        self._certfile = certfile
        self._keyfile = keyfile
        self._active_ws: list[web.WebSocketResponse] = []
        self._rest_runner: Optional[web.AppRunner] = None
        self._ws_runner: Optional[web.AppRunner] = None
        # Set to True the first time any REST request is received.
        self.rest_request_received: bool = False

    # ── TLS ──────────────────────────────────────────────────────────────────

    def _make_ssl_context(self) -> ssl.SSLContext:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(self._certfile, self._keyfile)
        return ctx

    # ── Abstract interface for subclasses ─────────────────────────────────────

    @abstractmethod
    def _rest_routes(self) -> list[web.RouteDef]:
        """Return aiohttp route definitions for the REST server."""

    @abstractmethod
    async def _on_ws_message(self, ws: web.WebSocketResponse, msg: dict) -> None:
        """Called once for every JSON text frame received on any WS connection."""

    # ── REST middleware ──────────────────────────────────────────────────────

    @web.middleware
    async def _track_rest_request(self, request: web.Request, handler):
        self.rest_request_received = True
        return await handler(request)

    # ── WebSocket handler ────────────────────────────────────────────────────

    async def _ws_handler(self, request: web.Request) -> web.WebSocketResponse:
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        self._active_ws.append(ws)
        try:
            async for raw in ws:
                if raw.type == WSMsgType.TEXT:
                    try:
                        data = json.loads(raw.data)
                    except json.JSONDecodeError:
                        continue
                    await self._on_ws_message(ws, data)
                elif raw.type == WSMsgType.ERROR:
                    break
        finally:
            if ws in self._active_ws:
                self._active_ws.remove(ws)
        return ws

    # ── Helpers ───────────────────────────────────────────────────────────────

    async def broadcast_ws(self, message: dict) -> None:
        """Send a JSON message to every connected WS client."""
        text = json.dumps(message)
        for ws in list(self._active_ws):
            try:
                await ws.send_str(text)
            except Exception:
                pass

    async def drop_all_ws_connections(self) -> None:
        """Force-close every active WebSocket connection (simulates a server restart)."""
        for ws in list(self._active_ws):
            try:
                await ws.close()
            except Exception:
                pass

    @property
    def ws_connection_count(self) -> int:
        return len(self._active_ws)

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    async def start(self) -> None:
        ssl_ctx = self._make_ssl_context()

        rest_app = web.Application(middlewares=[self._track_rest_request])
        rest_app.add_routes(self._rest_routes())
        self._rest_runner = web.AppRunner(rest_app)
        await self._rest_runner.setup()
        await web.TCPSite(
            self._rest_runner, "127.0.0.1", self._rest_port, ssl_context=ssl_ctx
        ).start()

        ws_app = web.Application()
        ws_app.router.add_get("/stream", self._ws_handler)
        self._ws_runner = web.AppRunner(ws_app)
        await self._ws_runner.setup()
        await web.TCPSite(
            self._ws_runner, "127.0.0.1", self._ws_port, ssl_context=ssl_ctx
        ).start()

    async def reset(self) -> None:
        """Drop all active connections and clear per-scenario state.

        Called between scenarios so the server keeps running (avoiding
        stop/restart port-binding races) while the scenario-specific
        subscription and connection state is wiped clean.
        """
        await self.drop_all_ws_connections()
        self.rest_request_received = False
        await self._on_reset()

    async def _on_reset(self) -> None:
        """Subclass hook called by reset() after connections are dropped."""

    async def stop(self) -> None:
        await self.drop_all_ws_connections()
        if self._rest_runner:
            await self._rest_runner.cleanup()
            self._rest_runner = None
        if self._ws_runner:
            await self._ws_runner.cleanup()
            self._ws_runner = None
