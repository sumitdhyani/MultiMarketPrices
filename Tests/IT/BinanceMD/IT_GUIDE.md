# BinanceMD Integration Test Suite — Developer Guide

> **Audience:** Team members working on the BinanceMD gateway tests.  
> **Scope:** BinanceMD-specific: simulator endpoints, Gherkin scenarios, step definitions, log files, config, and how to run/extend the suite.  
> For framework-level concepts (ExchangeSimulator base class, KafkaUpdateConsumer, wait_for, lifecycle pattern, common pitfalls), see [`../IT_FRAMEWORK_GUIDE.md`](../IT_FRAMEWORK_GUIDE.md).

---

## Table of Contents

1. [Directory Layout](#1-directory-layout)
2. [BinanceSimulator (concrete)](#2-binancesimulator-concrete)
3. [BinanceMDProcess & SDPMockProcess](#3-binancemdprocess--sdpmockprocess)
4. [Scenario Lifecycle Details](#4-scenario-lifecycle-details)
5. [The Feature File & Gherkin Scenarios](#5-the-feature-file--gherkin-scenarios)
6. [Step Definitions Reference](#6-step-definitions-reference)
7. [Kafka Wire Protocol (BinanceMD-specific)](#7-kafka-wire-protocol-binancemd-specific)
8. [Mock Exchange Endpoints](#8-mock-exchange-endpoints)
9. [Log Files](#9-log-files)
10. [How to Run the Tests](#10-how-to-run-the-tests)
11. [How to Add a New Test](#11-how-to-add-a-new-test)
12. [How to Modify an Existing Test](#12-how-to-modify-an-existing-test)
13. [Configuration Reference](#13-configuration-reference)
14. [BinanceMD-Specific Pitfalls](#14-binancemd-specific-pitfalls)

---

## 1. Directory Layout

```
Tests/IT/BinanceMD/
├── IT_GUIDE.md                   ← This file
├── binance/
│   └── binance_simulator.py      ← BinanceSimulator: REST + WS protocol mock
├── config/
│   └── config.json               ← Config read by BinanceMD and SDPMock at runtime
├── features/
│   ├── binance_md.feature        ← Gherkin scenarios (the source of truth)
│   ├── environment.py            ← before_*/after_* lifecycle hooks
│   └── steps/
│       └── binance_md_steps.py   ← Given/When/Then implementations
├── process_manager.py            ← Subprocess lifecycle wrappers
├── requirements.txt              ← Python deps
├── Dockerfile.it                 ← CI Docker image
└── docker-compose.it.yml         ← Full-stack CI compose file
```

---

## 2. BinanceSimulator (concrete)

**File:** `Tests/IT/BinanceMD/binance/binance_simulator.py`

Extends `ExchangeSimulator` (from `framework/simulator/`) with the Binance-specific REST and WebSocket protocol that BinanceMD expects.

### REST endpoints

| Path | Purpose | Called by BinanceMD |
|---|---|---|
| `GET /api/v3/ping` | Liveness probe | startup |
| `GET /api/v3/time` | Server timestamp | startup |
| `GET /api/v3/exchangeInfo` | Returns `{"symbols":[...]}` — populates BinanceMD's `SymbolStore` | `getTradableInstruments` |
| `GET /api/v3/depth?symbol=X` | Order-book snapshot | `getDepth` (per-partition FSM download phase) |
| `GET /api/v3/trades?symbol=X` | Recent trades snapshot | `getRecentTrades` |

The default instrument list is `[BTCUSDT, ETHUSDT]` (controlled by `_DEFAULT_INSTRUMENTS`).

### WebSocket protocol

```
Client → {"method": "SUBSCRIBE",   "id": N, "params": ["btcusdt@trade"]}
Server → {"result": null, "id": N}
Server → {"stream": "btcusdt@trade", "data": {...}}   ← every 200 ms

Client → {"method": "UNSUBSCRIBE", "id": N, "params": ["btcusdt@trade"]}
Server → {"result": null, "id": N}
```

On SUBSCRIBE the simulator spawns a per-stream asyncio `Task` that pushes one update frame every `_UPDATE_INTERVAL_SEC = 0.2 s` until the subscriber set empties. On UNSUBSCRIBE it removes the client from `_stream_subscribers[stream]`; when the set is empty, `subscribed_streams` is updated and the task exits naturally.

### Introspection properties used by step definitions

| Property / Method | Type | Meaning |
|---|---|---|
| `subscribed_streams` | `set[str]` | Streams with at least one live subscriber |
| `is_subscribed(stream)` | `bool` | Convenience check — used as a sync point after UNSUBSCRIBE |
| `ws_connection_count` | `int` | Inherited from base — used to detect reconnects |

---

## 3. BinanceMDProcess & SDPMockProcess

**File:** `Tests/IT/BinanceMD/process_manager.py`

Thin wrappers around `subprocess.Popen`. Both share a `_BaseProcess` base class.

**Binary resolution order:**
1. Environment variable `BINANCE_MD_BINARY` / `SDPMOCK_BINARY`
2. Default: `../../../build/MDGateways/Binance/BinanceMD` and `../../../build/SDPMock/SDPMock` (relative to `Tests/IT/BinanceMD/`)

Both binaries are launched with:
- `cwd = Tests/IT/BinanceMD/` — so that ConfigLib finds `./config/config.json`
- A single positional argument: the `appId` (`BinanceMD_1` or `SDPMock_1`)
- `stdout` and `stderr` redirected to `/tmp/bmd_<scenario>.log` / `/tmp/sdp_<scenario>.log`

`stop()` sends `SIGTERM` and waits up to 5 s before escalating to `SIGKILL`.

---

## 4. Scenario Lifecycle Details

The generic pattern is described in `IT_FRAMEWORK_GUIDE.md §5`. BinanceMD-specific values:

```
before_all
├── Kafka topics created: binance_price_subscriptions, BinanceMD_IT_1,
│   gateway_status, sync_data_request, heartbeats, sync_data
└── BinanceSimulator started on HTTPS :18443, WSS :18444

before_scenario
├── status_consumer watches: gateway_status
├── sync_probe watches: sync_data_request
├── SDPMock launched with appId: SDPMock_1
├── BinanceMD launched with appId: BinanceMD_1
│   Env: SSL_CA_CERT = path to self-signed cert
├── Wait ≤30 s for gateway_status = "operational"
├── Wait ≤15 s for sync_data_request message
└── Sleep 3.5 s settle (BinanceMD's K consumer rebalance)

after_scenario
├── SIGTERM BinanceMD_1 (SIGKILL after 5 s)
└── SIGTERM SDPMock_1
```

> **Tip:** If `before_scenario` raises a timeout error ("BinanceMD did not reach operational status"), the last 3000 characters of `/tmp/bmd_<scenario>.log` are automatically printed in the assertion message.

---

## 5. The Feature File & Gherkin Scenarios

**File:** `Tests/IT/BinanceMD/features/binance_md.feature`

```gherkin
Feature: BinanceMD gateway forwards market data to Kafka

  Background:
    Given BinanceMD is running and connected to the exchange simulator

  Scenario: Trade stream delivers updates to Kafka after subscribe
    When a client subscribes to "BTCUSDT" trade updates
    Then Kafka receives at least 1 trade update for "BTCUSDT" within 20 seconds

  Scenario: Depth stream delivers updates to Kafka after subscribe
    When a client subscribes to "BTCUSDT" depth updates
    Then Kafka receives at least 1 depth update for "BTCUSDT" within 20 seconds

  Scenario: Unsubscribe stops further updates on Kafka
    Given a client has subscribed to "BTCUSDT" trade updates
    And trade updates for "BTCUSDT" are arriving on Kafka
    When the client unsubscribes from "BTCUSDT" trade updates
    Then no new trade updates for "BTCUSDT" arrive on Kafka within 4 seconds

  Scenario: BinanceMD reconnects after WebSocket server disconnect
    Given a client has subscribed to "BTCUSDT" trade updates
    And trade updates for "BTCUSDT" are arriving on Kafka
    When the exchange simulator drops all WebSocket connections
    Then BinanceMD reconnects to the simulator within 15 seconds
    And trade updates for "BTCUSDT" resume on Kafka within 20 seconds
```

The `Background` step runs before every scenario and verifies the preconditions set up by `before_scenario`.

---

## 6. Step Definitions Reference

**File:** `Tests/IT/BinanceMD/features/steps/binance_md_steps.py`

### Background

| Gherkin | What it does |
|---|---|
| `Given BinanceMD is running and connected to the exchange simulator` | Asserts `bmd_process.is_running()`, `sdpmock_process.is_running()`, `gateway_operational`, and `sync_probe.message_received` |

### Subscribe / Unsubscribe

| Gherkin | What it does |
|---|---|
| `When a client subscribes to "{symbol}" trade updates` | `producer.subscribe(topic, symbol, "trade", destination_topic)` |
| `When a client subscribes to "{symbol}" depth updates` | `producer.subscribe(topic, symbol, "depth", destination_topic)` |
| `Given a client has subscribed to "{symbol}" trade updates` | Identical to the `When` subscribe step — used in `Given` position to set up a precondition |
| `When the client unsubscribes from "{symbol}" trade updates` | `producer.unsubscribe(topic, symbol, "trade", destination_topic)` |

### Precondition

| Gherkin | What it does |
|---|---|
| `And trade updates for "{symbol}" are arriving on Kafka` | Polls until at least one trade update is received (timeout 20 s), then `consumer.clear()` — confirms the pipeline is flowing before the next assertion |

### Assertions

| Gherkin | What it does |
|---|---|
| `Then Kafka receives at least 1 trade update for "{symbol}" within {N} seconds` | Polls `consumer` until `has_trade_update()` or timeout, then asserts |
| `Then Kafka receives at least 1 depth update for "{symbol}" within {N} seconds` | Same for depth; also logs `simulator.subscribed_streams` on every 2 s tick for diagnostics |
| `Then no new trade updates for "{symbol}" arrive on Kafka within {N} seconds` | (1) Polls `simulator.is_subscribed(stream)` until `False` (max 10 s) — waits for UNSUBSCRIBE to reach the simulator. (2) Sleeps 0.5 s for in-flight WS frames to traverse the Kafka pipeline. (3) `consumer.seek_to_end()` to skip pre-unsubscribe messages. (4) `consumer.drain(N)` to observe the silence window. (5) Asserts `not has_trade_update()` |
| `Then BinanceMD reconnects to the simulator within {N} seconds` | `wait_for(lambda: simulator.ws_connection_count > 0, N)` |
| `Then trade updates for "{symbol}" resume on Kafka within {N} seconds` | `consumer.clear()` + poll until trade update arrives |

### Actions

| Gherkin | What it does |
|---|---|
| `When the exchange simulator drops all WebSocket connections` | Calls `simulator.drop_all_ws_connections()` via the asyncio bridge |

---

## 7. Kafka Wire Protocol (BinanceMD-specific)

Topic names and header values must match `MDGateways/Binance/Constants.h`.

### Subscribe / Unsubscribe command (IT → BinanceMD)

```
Topic:   binance_price_subscriptions
Key:     BTCUSDT
Headers: message_type = "subscribe"  |  "unsubscribe"
Value:   {
           "destination_topic": "BinanceMD_IT_1",
           "symbol":            "BTCUSDT",
           "type":              "trade"  |  "depth"
         }
```

### Trade update (BinanceMD → IT)

```
Topic:   BinanceMD_IT_1
Key:     BTCUSDT:trade
Headers: message_type = "trade_update"
Value:   { "symbol": "BTCUSDT", "price": "50000.00", "quantity": "0.100", ... }
```

### Depth update (BinanceMD → IT)

```
Topic:   BinanceMD_IT_1
Key:     BTCUSDT:depth
Headers: message_type = "depth_update"
Value:   { "symbol": "BTCUSDT", "bids": [...], "asks": [...] }
```

### Gateway status (BinanceMD → monitoring)

```
Topic:   gateway_status
Headers: message_type = "status_update"
Value:   { "appId": "BinanceMD_1", "md_gw_status": "operational", ... }
```

---

## 8. Mock Exchange Endpoints

`BinanceSimulator` speaks the same protocol BinanceMD uses in production.

### REST responses

| Endpoint | Response (abbreviated) |
|---|---|
| `GET /api/v3/ping` | `{}` |
| `GET /api/v3/time` | `{"serverTime": <epoch_ms>}` |
| `GET /api/v3/exchangeInfo` | `{"symbols": [{"symbol":"BTCUSDT","status":"TRADING",...}, ...]}` |
| `GET /api/v3/depth?symbol=X&limit=5` | `{"lastUpdateId":1, "bids":[["50000","1"]], "asks":[["50001","1"]]}` |
| `GET /api/v3/trades?symbol=X&limit=1` | `{"data": [{...}]}` |

> `disablePeerVerification: true` is set in `config.json` so BinanceMD accepts the self-signed cert without a CA chain.

### WebSocket update frames

**Trade stream** (`btcusdt@trade`):
```json
{
  "stream": "btcusdt@trade",
  "data": {
    "e": "trade",
    "s": "BTCUSDT",
    "p": "50000.00",
    "q": "0.100",
    "T": 1716000000000
  }
}
```

**Depth stream** (`btcusdt@depth5`):
```json
{
  "stream": "btcusdt@depth5",
  "data": {
    "e": "depthUpdate",
    "s": "BTCUSDT",
    "bids": [["50000.00", "1.000"]],
    "asks": [["50001.00", "1.000"]]
  }
}
```

Updates are pushed at **5 Hz (every 200 ms)**, within the `wsThrottleRatePerSec: 5` limit in `config.json`.

---

## 9. Log Files

Each scenario creates two log files in `/tmp/`, capturing **stdout + stderr** of both binary processes. The filename is derived from the scenario name (first 40 characters, spaces replaced with `_`).

| Log file | Process |
|---|---|
| `/tmp/bmd_<scenario_tag>.log` | BinanceMD binary — NanoLog binary output + `fprintf(stderr, ...)` traces |
| `/tmp/sdp_<scenario_tag>.log` | SDPMock binary — same format |

**Examples for the 4 current scenarios:**
```
/tmp/bmd_Trade_stream_delivers_updates_to_Kafka_a.log
/tmp/bmd_Depth_stream_delivers_updates_to_Kafka_a.log
/tmp/bmd_Unsubscribe_stops_further_updates_on_Kaf.log
/tmp/bmd_BinanceMD_reconnects_after_WebSocket_ser.log
```

### NanoLog binary log files

BinanceMD and SDPMock also write a **NanoLog binary log** (distinct from the stderr file above). Files are named:

```
<appId>_<YYYYMMDD>.<rotation_number>
e.g.  BinanceMD_1_20260517.0
```

Placed in `./logs/` relative to the binary CWD — i.e. `Tests/IT/BinanceMD/logs/`.

To read them:
```bash
ThirdParty/NanoLog/runtime/decompressor \
  Tests/IT/BinanceMD/logs/BinanceMD_1_$(date +%Y%m%d).0 \
  | less
```

---

## 10. How to Run the Tests

### Prerequisites

```bash
# 1. Kafka running locally on 127.0.0.1:9092
docker compose up -d

# 2. Build the binaries
cd build && make BinanceMD SDPMock -j$(nproc)

# 3. Activate the Python virtual environment
cd /home/admin/code/MultiMarketPrices
source .venv/bin/activate

# 4. Install Python dependencies (one time)
cd Tests/IT/BinanceMD
pip install -r requirements.txt
```

### Run the full suite

```bash
cd Tests/IT/BinanceMD
python -m behave features/binance_md.feature
```

### Run a single scenario by line number

```bash
python -m behave features/binance_md.feature:6    # Trade stream
python -m behave features/binance_md.feature:10   # Depth stream
python -m behave features/binance_md.feature:14   # Unsubscribe
python -m behave features/binance_md.feature:20   # Reconnect
```

### Run a single scenario by name

```bash
python -m behave features/binance_md.feature --name "Unsubscribe stops further updates on Kafka"
```

### Show all output (process traces etc.)

```bash
python -m behave features/binance_md.feature --no-capture
```

### Override Kafka broker or simulator ports

```bash
KAFKA_BROKERS=10.0.0.5:9092 REST_PORT=28443 WS_PORT=28444 \
  python -m behave features/binance_md.feature
```

### Point at a custom binary

```bash
BINANCE_MD_BINARY=/path/to/custom/BinanceMD \
  python -m behave features/binance_md.feature
```

---

## 11. How to Add a New Test

### Step 1 — Write the Gherkin scenario

Add a new `Scenario:` block to `features/binance_md.feature`. Reuse existing steps where they fit.

```gherkin
  Scenario: Subscribing to multiple symbols delivers independent updates
    When a client subscribes to "BTCUSDT" trade updates
    And a client subscribes to "ETHUSDT" trade updates
    Then Kafka receives at least 1 trade update for "BTCUSDT" within 20 seconds
    And Kafka receives at least 1 trade update for "ETHUSDT" within 20 seconds
```

### Step 2 — Implement any new steps

Add missing steps to `features/steps/binance_md_steps.py`:

```python
@when('a client subscribes to "{symbol}" trade updates')
def step_subscribe_trade(context, symbol):
    context.producer.subscribe(
        in_topic=BINANCE_IN_TOPIC,
        symbol=symbol,
        subscription_type="trade",
        destination_topic=BINANCE_IT_TOPIC,
    )
```

### Step 3 — Extend the simulator if needed

If your scenario requires a new exchange behaviour (e.g. a new REST endpoint or a new WS frame format), extend `BinanceSimulator` in `binance/binance_simulator.py`. Keep it minimal — replicate the real Binance protocol, don't add business logic.

### Step 4 — Run and iterate

```bash
python -m behave features/binance_md.feature --name "Your new scenario" --no-capture
```

Check `/tmp/bmd_<scenario>.log` if the gateway doesn't behave as expected.

---

## 12. How to Modify an Existing Test

### Change a scenario timeout

Timeouts are parsed directly from the Gherkin step text:

```gherkin
# Before
Then Kafka receives at least 1 trade update for "BTCUSDT" within 20 seconds
# After (faster environment)
Then Kafka receives at least 1 trade update for "BTCUSDT" within 10 seconds
```

### Change gateway-readiness timeouts

Edit the constants at the top of `features/environment.py`:

```python
OPERATIONAL_TIMEOUT_SEC  = 30   # increase if BinanceMD is slow to start
FSM_READY_TIMEOUT_SEC    = 15   # increase if SDPMock handshake is slow
FSM_SETTLE_SEC           = 0.5  # brief sleep after sync_data_request seen
```

### Change the simulator update rate

In `binance/binance_simulator.py`:

```python
_UPDATE_INTERVAL_SEC = 0.2  # 5 updates/sec — must be ≤ 1/wsThrottleRatePerSec
```

The value must stay within BinanceMD's `wsThrottleRatePerSec` config limit.

### Change the instrument list

```python
# binance/binance_simulator.py
_DEFAULT_INSTRUMENTS = [
    {"symbol": "BTCUSDT", "status": "TRADING", "baseAsset": "BTC", "quoteAsset": "USDT"},
    {"symbol": "ETHUSDT", "status": "TRADING", "baseAsset": "ETH", "quoteAsset": "USDT"},
    {"symbol": "SOLUSDT", "status": "TRADING", "baseAsset": "SOL", "quoteAsset": "USDT"},  # new
]
```

---

## 13. Configuration Reference

**File:** `Tests/IT/BinanceMD/config/config.json`

Both BinanceMD and SDPMock read this file at runtime (ConfigLib resolves it as `./config/config.json` relative to process CWD = `Tests/IT/BinanceMD/`).

| Field | Value | Notes |
|---|---|---|
| `brokers` | `127.0.0.1:9092` | Local Kafka |
| `restHost` / `restPort` | `127.0.0.1` / `18443` | Must match `REST_PORT` in `environment.py` |
| `wsHost` / `wsPort` | `127.0.0.1` / `18444` | Must match `WS_PORT` in `environment.py` |
| `wsThrottleRatePerSec` | `5` | Must be ≥ `1 / _UPDATE_INTERVAL_SEC` |
| `disablePeerVerification` | `true` | Accept the self-signed TLS cert |
| `middleware_params.session.timeout.ms` | `6000` | Fast consumer group rebalance between scenarios — **do not raise above the Kafka broker's `group.max.session.timeout.ms`** |
| `middleware_params.heartbeat.interval.ms` | `2000` | Must be `< session.timeout.ms / 3` |
| `logLevel` | `DEBUG` | Change to `INFO` or `WARNING` to reduce NanoLog noise |

---

## 14. BinanceMD-Specific Pitfalls

The generic pitfalls (consumer.clear(), SDPMock ordering, session.timeout.ms, TLS SAN, SSL_clear) are documented in [`../IT_FRAMEWORK_GUIDE.md §8`](../IT_FRAMEWORK_GUIDE.md#8-common-pitfalls--lessons-learned). The following are specific to this suite.

---

### Unsubscribe silence assertion requires three guards, in order

The unsubscribe silence check is not a simple `seek + drain`. It requires all three steps in sequence:

1. **`wait_for(lambda: not simulator.is_subscribed(stream), ...)`** — waits until the WS UNSUBSCRIBE has reached the simulator and been processed. Without this, the Kafka pipeline may still be carrying pre-unsubscribe updates.

2. **`sleep(0.5)`** — allows the last in-flight WS frame (the one the update task sent during the `asyncio.sleep(0.2)` interval before it noticed the empty subscriber set) to traverse the Kafka pipeline and be published by BinanceMD.

3. **`consumer.seek_to_end()`** — discards any messages published to `BinanceMD_IT_1` before the silence window starts. This is the step that prevents false failures.

Removing any of these three guards will make the test flaky or always-fail.

---

### BinanceMD's per-symbol FSM resubscribes on reconnect

After a WS reconnect, BinanceMD replays all active subscriptions. If your test drives a WS disconnect while a symbol is subscribed, expect BinanceMD to re-send a WS SUBSCRIBE frame after reconnecting. The simulator's `subscribed_streams` will become `True` again automatically. The reconnect scenario asserts on `ws_connection_count > 0` (base class), not on `subscribed_streams`, which is the correct approach.

---

### `exchangeInfo` is fetched once per binary launch, not per subscribe

`SymbolStore` is populated from `GET /api/v3/exchangeInfo` at startup. If you want to test a symbol that isn't in `_DEFAULT_INSTRUMENTS`, add it **before** the scenario starts (i.e. before `before_scenario` launches BinanceMD). Modifying the simulator's instrument list at subscribe time has no effect — BinanceMD won't see the new symbol until it restarts.
