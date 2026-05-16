# BinanceMD Integration Test Framework

## 1. Core Philosophy

The integration tests treat **BinanceMD as a black box**.  The real compiled
binary is exercised end-to-end: it receives commands via Kafka, connects to an
exchange over TLS/HTTPS and TLS/WSS, transforms market data, and publishes
updates back to Kafka.  The tests own and control every external system the
binary talks to — they do **not** mock any internal C++ class.

The two key enforcement rules that follow from this:

| Rule | Rationale |
|------|-----------|
| The binary is launched as a subprocess per scenario. | Eliminates shared in-process state between tests (global `SymbolStore`, static `connectedOnce` flags, etc.) |
| Every external touch-point (exchange, TLS, Kafka topic) is controlled or created by the test harness. | Assertions are deterministic — no dependency on external network availability or pre-existing topic state. |

Scenarios are written in **Gherkin** and executed with **behave-python**.  This
keeps the test intent readable in plain English while keeping infrastructure code
in maintainable Python.

---

## 2. High-Level Design

### 2.1 Environment Setup

```
before_all (once per suite)
│
├── Generate self-signed TLS certificate + key  →  stored in tmp dir
├── Start shared asyncio event-loop in background thread
└── Ensure Kafka topics exist (binance_price_subscriptions, BinanceMD_IT_1)

before_scenario (once per scenario)
│
├── Start BinanceSimulator  (HTTPS :18443, WSS :18444)
├── Create KafkaCommandProducer
├── Create KafkaUpdateConsumer  (subscribes to BinanceMD_IT_1 at latest offset)
├── Launch BinanceMD subprocess  (cwd = Tests/IT/,  Config reads ./config/config.json)
└── Wait up to 30 s for BinanceMD to make its first REST request to the simulator
    → confirms BinanceMD is live and the simulator is reachable

after_scenario
│
├── SIGTERM BinanceMD  (SIGKILL after 5 s)
├── Stop BinanceSimulator
└── Close Kafka producer / consumer

after_all
│
├── Stop shared asyncio event loop
└── Delete TLS temp directory
```

### 2.2 Interaction / Data Flow

The diagram below shows the full message path exercised by a `subscribe →
receive trade_update` scenario.

```
 behave step              BinanceMD binary             BinanceSimulator (Python)
     │                         │                              │
     │── Kafka produce ────────►                              │
     │   topic: binance_price_subscriptions                   │
     │   header: message_type = "subscribe"                   │
     │   payload: { destination_topic, symbol, type }         │
     │                         │                              │
     │                    (PlatformComm reads Kafka)          │
     │                         │── HTTPS GET /api/v3/exchangeInfo ──►
     │                         │                              │
     │                         │◄── { symbols: [...] } ───────
     │                         │                              │
     │                         │── TLS WebSocket upgrade ────►│
     │                         │   wss://127.0.0.1:18444/stream
     │                         │                              │
     │                         │── WS SUBSCRIBE ─────────────►
     │                         │   { method: "SUBSCRIBE",     │
     │                         │     params: ["btcusdt@trade"]}
     │                         │                              │
     │                         │◄── { result: null } ─────────│
     │                         │                              │
     │                         │◄── { stream: "btcusdt@trade", ─── (every 200 ms)
     │                         │     data: { e, s, p, q, T } }
     │                         │                              │
     │                    (onPriceUpdate transforms data)      │
     │                         │                              │
     │◄── Kafka consume ────────                              │
     │   topic: BinanceMD_IT_1                                │
     │   header: message_type = "trade_update"                │
     │   payload: { symbol, price, quantity }                 │
     │                         │                              │
 assert update received        │                              │
```

**TLS peer-verification** is disabled in `config.test.json`
(`"disablePeerVerification": true`) because the simulator uses a self-signed
certificate.  The C++ client therefore accepts the cert without a CA chain
check — only for test builds.

---

## 3. Low-Level Design

### Directory Layout

```
Tests/IT/
│
├── simulator/                   ← Reusable infrastructure layer
│   ├── exchange_simulator.py    ← Abstract base class (HTTPS + WSS server)
│   └── tls_helper.py            ← Self-signed cert generator
│
├── binance/                     ← Binance-specific layer
│   └── binance_simulator.py     ← Concrete Binance-protocol simulator
│
├── utils/                       ← Shared test utilities
│   ├── wait_for.py              ← Synchronous polling helper
│   ├── kafka_helper.py          ← Kafka producer + consumer wrappers
│   └── process_manager.py       ← BinanceMD subprocess lifecycle
│
├── features/                    ← behave artefacts
│   ├── binance_md.feature       ← Gherkin scenarios
│   ├── environment.py           ← before_*/after_* hooks
│   └── steps/
│       └── binance_md_steps.py  ← Given / When / Then implementations
│
├── config/
│   └── config.test.json         ← Test-specific config for BinanceMD
└── requirements.txt
```

---

### 3.1 Reusable Classes

#### `ExchangeSimulator`  (`simulator/exchange_simulator.py`)

| Concern | Detail |
|---------|--------|
| **Purpose** | Abstract base providing a self-contained HTTPS REST server and a WSS WebSocket server in a single asyncio event loop. Subclasses implement the protocol; the base handles TLS, connection tracking, lifecycle. |
| **Transport** | Two independent `aiohttp` TCP sites on separate ports, both wrapped in the same `ssl.SSLContext` loaded from the cert/key generated by `tls_helper`. |
| **REST surface** | `_rest_routes()` is abstract — subclasses return a list of `aiohttp.web.RouteDef`. A middleware hook sets `rest_request_received = True` on the first request, which the test harness uses as the "BinanceMD is alive" latch. |
| **WS surface** | A single `/stream` route handles all WS upgrades. The base keeps `_active_ws` — a live list of connected `WebSocketResponse` objects. Each text frame is JSON-decoded and dispatched to `_on_ws_message()` (abstract). |
| **Helpers** | `broadcast_ws(msg)` fans out to all connected clients.  `drop_all_ws_connections()` force-closes every socket, triggering reconnect logic in BinanceMD.  `ws_connection_count` lets steps assert reconnection happened. |
| **Lifecycle** | `start()` / `stop()` are async; the test harness calls them via `asyncio.run_coroutine_threadsafe` from the synchronous behave thread. |

---

#### `generate_self_signed_cert`  (`simulator/tls_helper.py`)

| Concern | Detail |
|---------|--------|
| **Purpose** | Generates a 2048-bit RSA self-signed certificate valid for `127.0.0.1` (Subject Alternative Name) for exactly 1 day. Called once in `before_all` and the resulting paths are shared across all scenarios. |
| **Why SAN?** | Modern TLS clients (including Boost.Beast) validate the SAN, not just the CN.  Without the SAN entry the TLS handshake fails, even with peer-verification disabled. |
| **Output** | Writes `test_cert.pem` and `test_key.pem` into a temp directory; returns both paths. |

---

#### `wait_for`  (`utils/wait_for.py`)

| Concern | Detail |
|---------|--------|
| **Purpose** | Synchronous, polling-based "eventually" helper.  Takes a `Callable[[], bool]`, a timeout, and an optional poll interval.  Returns `True` if the condition is met, `False` on timeout. |
| **Usage pattern** | Used everywhere a step must wait for an async side-effect to materialise in the synchronous behave thread: `wait_for(lambda: simulator.rest_request_received, 30)`. |

---

### 3.2 Component-Specific Classes

#### `BinanceSimulator`  (`binance/binance_simulator.py`)

| Concern | Detail |
|---------|--------|
| **Purpose** | Concrete implementation of the Binance exchange protocol.  Speaks exactly the same HTTP and WebSocket dialect that BinanceMD expects, allowing the real binary to run without any code changes. |
| **REST endpoints** | `/api/v3/ping`, `/api/v3/time`, `/api/v3/exchangeInfo` (returns `{ "symbols": [...] }` list that populates BinanceMD's `SymbolStore`), `/api/v3/depth` (order-book snapshot), `/api/v3/trades` (recent trades snapshot). |
| **WS SUBSCRIBE** | On receiving `{ method: "SUBSCRIBE", params: [...] }`, adds the client to `_stream_subscribers[stream]`, records the stream in `subscribed_streams`, and spawns a per-stream asyncio `Task` that pushes updates every 200 ms. |
| **WS UNSUBSCRIBE** | Removes the client from `_stream_subscribers[stream]`.  When the subscriber set empties the update task exits naturally. |
| **Update frames** | `_make_update_frame(stream)` builds the correct envelope.  Trade streams produce `{ stream, data: { e:"trade", s, p, q, T } }`.  Depth streams produce `{ stream, data: { e:"depthUpdate", s, bids, asks } }`.  Both match what BinanceMD's `onPriceUpdate()` / `transformTradeForPlatForm()` / `transformDepthForPlatForm()` expect. |
| **Introspection** | `is_subscribed(stream)` / `subscribed_streams` let step definitions assert that BinanceMD actually sent the WS subscribe command. |

---

#### `KafkaCommandProducer`  (`utils/kafka_helper.py`)

| Concern | Detail |
|---------|--------|
| **Purpose** | Sends subscribe / unsubscribe commands to BinanceMD via Kafka, replicating exactly what the IT client process does in the old C++ IT. |
| **Wire format** | Topic = `binance_price_subscriptions`.  Header `message_type = "subscribe"` or `"unsubscribe"`.  Payload = `{ destination_topic, symbol, type }` — the exact fields that `PlatformComm::msgCb` parses to build the subscription routing key. |

---

#### `KafkaUpdateConsumer`  (`utils/kafka_helper.py`)

| Concern | Detail |
|---------|--------|
| **Purpose** | Receives `trade_update` / `depth_update` messages from BinanceMD's output topic.  Provides `poll_once()`, `drain()`, typed accessors (`trade_updates()`, `depth_updates()`), and a `clear()` for silence-window assertions. |
| **Isolation** | Each scenario creates a fresh consumer with a UUID group id and `auto.offset.reset = latest` so it only sees messages produced after it was created. |
| **Wire format** | Header `message_type = "trade_update"` or `"depth_update"`.  Payload = JSON market-data object with `symbol`, `price`, `quantity` (trade) or `symbol`, `bids`, `asks` (depth). |

---

#### `BinanceMDProcess`  (`utils/process_manager.py`)

| Concern | Detail |
|---------|--------|
| **Purpose** | Wraps `subprocess.Popen` to launch, monitor, and gracefully stop the real BinanceMD binary. |
| **Working directory** | BinanceMD is launched with `cwd = Tests/IT/`.  `ConfigLib` hard-codes the config path as `./config/config.json` relative to the process CWD — so it automatically picks up `Tests/IT/config/config.json`. |
| **Shutdown** | `stop()` sends `SIGTERM` and waits up to 5 s; escalates to `SIGKILL` if the process does not exit. |
| **Binary path** | Resolved as `build/MDGateways/Binance/BinanceMD` relative to the workspace root, overridable via the `BINANCE_MD_BINARY` environment variable. |

---

#### `environment.py`  (`features/environment.py`)

| Concern | Detail |
|---------|--------|
| **Purpose** | behave's lifecycle hook module.  Orchestrates the entire test environment setup and teardown at both the suite level and individual scenario level. |
| **asyncio bridge** | The exchange simulator is fully async but behave steps are synchronous.  `before_all` starts a single `asyncio` event loop in a daemon background thread.  The helper `_run_sync(context, coro)` submits a coroutine to that loop via `run_coroutine_threadsafe` and blocks until done — giving steps clean synchronous access to async simulator methods. |
| **Suite-level** (`before_all`) | TLS cert generation (once), asyncio loop start, Kafka topic creation. |
| **Scenario-level** (`before_scenario`) | Simulator start, Kafka helpers creation, BinanceMD launch, startup latch (wait for first REST hit). |
| **Teardown** (`after_scenario`) | BinanceMD SIGTERM, simulator stop, Kafka close — always runs even if the scenario fails. |

---

#### `binance_md_steps.py`  (`features/steps/binance_md_steps.py`)

| Concern | Detail |
|---------|--------|
| **Purpose** | Implements every `Given / When / Then` step referenced in `binance_md.feature`. |
| **Subscribe steps** | Calls `KafkaCommandProducer.subscribe()` with the symbol, type, and the IT consumer's topic as the destination.  Stores the symbol/type on `context` for reuse by later steps. |
| **Assertion steps** | Poll `KafkaUpdateConsumer` in a deadline loop (not a busy-wait) until the required message type is seen or timeout expires.  Fail with a descriptive message on timeout. |
| **Reconnect step** | Calls `context.simulator.drop_all_ws_connections()` via `_run_sync`, then asserts `ws_connection_count > 0` within a timeout. |
| **Silence assertion** | Clears the consumer buffer, then `drain()`s for the full window — verifying that unsubscribe actually stopped the flow. |

---

#### `binance_md.feature`  (`features/binance_md.feature`)

Four scenarios covering the primary behavioural contracts of BinanceMD:

| Scenario | What it proves |
|----------|---------------|
| **Trade stream subscribe** | BinanceMD subscribes to the WS trade stream and forwards updates to Kafka. |
| **Depth stream subscribe** | Same for the order-book (depth) stream. |
| **Unsubscribe stops updates** | After an unsubscribe command BinanceMD sends no further updates for that stream. |
| **WS reconnect** | When the exchange drops the connection BinanceMD reconnects automatically and resumes updates. |

---

## 4. System Prerequisites

### Runtime infrastructure

| Component | Requirement | Notes |
|-----------|-------------|-------|
| **Apache Kafka** | Running locally on `127.0.0.1:9092` | The framework creates required topics automatically on first run (`adminClient.createTopics`). Override broker address via `KAFKA_BROKERS`. |
| **BinanceMD binary** | Built at `build/MDGateways/Binance/BinanceMD` | Build with `cmake --build build` from the workspace root.  Override path via `BINANCE_MD_BINARY`. |
| **Python** | 3.11+ | Required for `tuple[str, str]` built-in type hints (3.9+), `asyncio.TaskGroup` not used — 3.9 minimum technically, 3.11 recommended. |

### Python packages

Install in the project venv:

```bash
cd Tests/IT
pip install -r requirements.txt
```

| Package | Version | Role |
|---------|---------|------|
| `behave` | ≥ 1.2.6 | BDD test runner |
| `aiohttp` | ≥ 3.9.0 | Async HTTP + WebSocket server for the simulator |
| `cryptography` | ≥ 41.0.0 | RSA key + X.509 cert generation for TLS |
| `confluent-kafka` | ≥ 2.3.0 | Kafka producer and consumer (wraps librdkafka) |

`librdkafka` must be present on the system:

```bash
# Ubuntu / Debian
sudo apt-get install librdkafka-dev
```

### Test config

`Tests/IT/config/config.test.json` must exist before running.  It must contain:
- `brokers` pointing to the local Kafka instance.
- `restHost` / `restPort` pointing to `127.0.0.1:18443` (the simulator REST port).
- `wsHost` / `wsPort` pointing to `127.0.0.1:18444` (the simulator WS port).
- `"disablePeerVerification": true` so BinanceMD accepts the self-signed cert.
- `in_topic` for the `BinanceMD` group pointing to `binance_price_subscriptions`.

Ports can be overridden via `REST_PORT` / `WS_PORT` environment variables (must match the config).

### How to run

```bash
# from the workspace root, with the venv active:
cd Tests/IT
behave features/
```

To run a single scenario by name:

```bash
behave features/ --name "Trade stream delivers updates"
```

To increase verbosity:

```bash
behave features/ --no-capture --verbose
```
