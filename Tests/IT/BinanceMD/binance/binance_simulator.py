"""Binance-protocol exchange simulator.

Implements the Binance REST endpoints that BinanceMD uses on startup and the
WebSocket stream protocol (SUBSCRIBE / UNSUBSCRIBE + periodic stream updates).

REST endpoints served:
  GET /api/v3/ping
  GET /api/v3/time
  GET /api/v3/exchangeInfo      ← getTradableInstruments
  GET /api/v3/depth             ← getDepth (snapshot)
  GET /api/v3/trades            ← getRecentTrades (snapshot)

WebSocket protocol (path /stream):
  Client → {"method": "SUBSCRIBE",   "id": N, "params": ["btcusdt@trade"]}
  Client → {"method": "UNSUBSCRIBE", "id": N, "params": ["btcusdt@trade"]}
  Server → {"result": null, "id": N}  (ack)
  Server → {"stream": "btcusdt@trade", "data": {...}}  (periodic updates)
"""

import asyncio
import json
import time
from typing import Any

from aiohttp import web

from IT.framework.simulator.exchange_simulator import ExchangeSimulator


# Default instrument list returned by /exchangeInfo.
# BinanceMD reads the "symbol" field from each entry to populate symbolStore.
_DEFAULT_INSTRUMENTS = [
    {"symbol": "BTCUSDT", "status": "TRADING", "baseAsset": "BTC", "quoteAsset": "USDT"},
    {"symbol": "ETHUSDT", "status": "TRADING", "baseAsset": "ETH", "quoteAsset": "USDT"},
]

# How often (seconds) the simulator pushes an update per subscribed stream.
_UPDATE_INTERVAL_SEC = 0.2  # 5 updates/sec — within wsThrottleRatePerSec=5


class BinanceSimulator(ExchangeSimulator):
    """Full Binance-protocol simulator for integration testing of BinanceMD."""

    def __init__(self, rest_port: int, ws_port: int, certfile: str, keyfile: str) -> None:
        super().__init__(rest_port, ws_port, certfile, keyfile)
        # stream_name → set of WebSocketResponse that have subscribed
        self._stream_subscribers: dict[str, set[web.WebSocketResponse]] = {}
        # stream_name → running asyncio.Task that pushes updates
        self._update_tasks: dict[str, asyncio.Task] = {}
        # set of stream names that have received at least one SUBSCRIBE from the client
        self.subscribed_streams: set[str] = set()

    # ── REST routes ──────────────────────────────────────────────────────────

    def _rest_routes(self) -> list[web.RouteDef]:
        return [
            web.get("/api/v3/ping",         self._ping),
            web.get("/api/v3/time",         self._server_time),
            web.get("/api/v3/exchangeInfo", self._exchange_info),
            web.get("/api/v3/depth",        self._depth),
            web.get("/api/v3/trades",       self._trades),
        ]

    async def _ping(self, _request: web.Request) -> web.Response:
        return web.json_response({})

    async def _server_time(self, _request: web.Request) -> web.Response:
        return web.json_response({"serverTime": int(time.time() * 1000)})

    async def _exchange_info(self, _request: web.Request) -> web.Response:
        # BinanceMD's on_read checks obj.contains("symbols") and stores them.
        return web.json_response({"symbols": _DEFAULT_INSTRUMENTS})

    async def _depth(self, request: web.Request) -> web.Response:
        symbol = request.rel_url.query.get("symbol", "BTCUSDT")
        # BinanceMD wraps this as result["data"] = obj and calls getDepth callback.
        # depthHandler does copy["data"].as_object()["symbol"] = symbol.
        return web.json_response({
            "lastUpdateId": 1,
            "bids": [["50000.00", "1.000"]],
            "asks": [["50001.00", "1.000"]],
        })

    async def _trades(self, request: web.Request) -> web.Response:
        # NOTE: BinanceMD's on_read parses the body as a json::object, which
        # would throw for a raw JSON array.  The getRecentTrades / tradeHandler
        # path expects result["data"] to be an array.  Return a wrapped object
        # so that on_read succeeds; the tradeHandler's as_array() path is only
        # reached via getSnapshot → getRecentTrades, which is not exercised in
        # the current ITs (only depth snapshots are).  Returning a minimal valid
        # object prevents crashes in BinanceMD if it ever hits this endpoint.
        symbol = request.rel_url.query.get("symbol", "BTCUSDT")
        return web.json_response({
            "data": [
                {
                    "id": 1,
                    "price": "50000.00",
                    "qty": "0.500",
                    "quoteQty": "25000.00",
                    "time": int(time.time() * 1000),
                    "isBuyerMaker": False,
                }
            ]
        })

    # ── WebSocket protocol ───────────────────────────────────────────────────

    async def _on_ws_message(self, ws: web.WebSocketResponse, msg: dict) -> None:
        method = msg.get("method", "")
        msg_id = msg.get("id", 0)
        params: list[str] = msg.get("params", [])

        if method == "SUBSCRIBE":
            for stream in params:
                print(f"[SIMULATOR TRACE] SUBSCRIBE received for stream={stream!r}", flush=True)
                self._stream_subscribers.setdefault(stream, set()).add(ws)
                self.subscribed_streams.add(stream)
                if stream not in self._update_tasks:
                    self._update_tasks[stream] = asyncio.create_task(
                        self._push_stream_updates(stream)
                    )
            await ws.send_str(json.dumps({"result": None, "id": msg_id}))

        elif method == "UNSUBSCRIBE":
            for stream in params:
                subs = self._stream_subscribers.get(stream)
                if subs:
                    subs.discard(ws)
                    if not subs:  # last subscriber left
                        self.subscribed_streams.discard(stream)
            await ws.send_str(json.dumps({"result": None, "id": msg_id}))

    # ── Streaming update loop ────────────────────────────────────────────────

    async def _push_stream_updates(self, stream: str) -> None:
        """Continuously push market-data frames for *stream* until unsubscribed."""
        while True:
            subscribers = self._stream_subscribers.get(stream, set())
            if not subscribers:
                break
            frame = json.dumps(self._make_update_frame(stream))
            for ws in list(subscribers):
                try:
                    await ws.send_str(frame)
                except Exception:
                    subscribers.discard(ws)
            await asyncio.sleep(_UPDATE_INTERVAL_SEC)
        self._update_tasks.pop(stream, None)

    def _make_update_frame(self, stream: str) -> dict[str, Any]:
        """Build the Binance-format market-data frame for *stream*."""
        parts = stream.split("@", 1)
        symbol = parts[0].upper()
        stream_type = parts[1] if len(parts) > 1 else ""

        if "trade" in stream_type:
            # BinanceMD's transformTradeForPlatForm maps: s→symbol, p→price, q→quantity
            return {
                "stream": stream,
                "data": {
                    "e": "trade",
                    "s": symbol,
                    "p": "50000.00",
                    "q": "0.100",
                    "T": int(time.time() * 1000),
                },
            }
        else:  # depth
            # BinanceMD's transformDepthForPlatForm adds symbol field to data.
            return {
                "stream": stream,
                "data": {
                    "e": "depthUpdate",
                    "s": symbol,
                    "bids": [["50000.00", "1.000"]],
                    "asks": [["50001.00", "1.000"]],
                },
            }

    def is_subscribed(self, stream: str) -> bool:
        """Return True if at least one WS client has subscribed to *stream*."""
        return stream in self.subscribed_streams

    # ── Lifecycle override ───────────────────────────────────────────────────

    async def _on_reset(self) -> None:
        """Cancel streaming tasks and clear subscription state."""
        for task in list(self._update_tasks.values()):
            task.cancel()
        self._update_tasks.clear()
        self._stream_subscribers.clear()
        self.subscribed_streams.clear()

    async def stop(self) -> None:
        for task in list(self._update_tasks.values()):
            task.cancel()
        self._update_tasks.clear()
        await super().stop()
