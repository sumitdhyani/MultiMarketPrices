# IT Framework — Developer Guide

> **Audience:** New team members joining the gateway team.  
> **Goal:** Give you a complete mental model of the generic IT framework — why it exists, what building blocks it provides, and how to add a new integration test suite for a new gateway component.  
> For BinanceMD-specific tests, see [`BinanceMD/IT_GUIDE.md`](BinanceMD/IT_GUIDE.md).

---

## Table of Contents

1. [Core Philosophy](#1-core-philosophy)
2. [Big Picture — How Everything Connects](#2-big-picture--how-everything-connects)
3. [Directory Layout](#3-directory-layout)
4. [Framework Component Deep-Dive](#4-framework-component-deep-dive)
   - 4.1 [ExchangeSimulator (base class)](#41-exchangesimulator-base-class)
   - 4.2 [KafkaCommandProducer](#42-kafkacommandproducer)
   - 4.3 [KafkaUpdateConsumer](#43-kafkaupdateconsumer)
   - 4.4 [KafkaStatusConsumer & KafkaTopicProbe](#44-kafkastatusconsumer--kafkatopicprobe)
   - 4.5 [wait_for](#45-wait_for)
5. [Generic Scenario Lifecycle Pattern](#5-generic-scenario-lifecycle-pattern)
6. [Kafka Wire Protocol](#6-kafka-wire-protocol)
7. [How to Add a New Gateway Component Test Suite](#7-how-to-add-a-new-gateway-component-test-suite)
8. [Common Pitfalls & Lessons Learned](#8-common-pitfalls--lessons-learned)

---

## 1. Core Philosophy

Each integration test treats **the gateway binary as a black box**.

The real compiled binary is launched as a subprocess per scenario. It receives commands through Kafka, connects to a fake exchange over TLS/WSS, transforms market data, and publishes updates back to Kafka. The test harness owns and controls every external system the binary talks to — it does not mock any internal C++ class or link against gateway code.

Two rules follow from this:

| Rule | Rationale |
|------|-----------|
| **The binary is relaunched fresh for every scenario** | Eliminates shared in-process state between tests (global symbol stores, static flags, etc.) |
| **Every touch-point is controlled by the harness** | Assertions are deterministic — no dependency on external network availability or pre-existing Kafka topic state |

Scenarios are written in **Gherkin** and executed with **behave** (Python). This keeps test intent readable in plain English while keeping infrastructure in maintainable Python.

---

## 2. Big Picture — How Everything Connects

```
 ┌────────────────────────────────────────────────────────────────────────┐
 │                       behave test process (Python)                      │
 │                                                                         │
 │  ┌──────────────┐   ┌─────────────────┐   ┌───────────────────────┐   │
 │  │ Feature file │   │ environment.py  │   │   <gw>_steps.py       │   │
 │  │ (Gherkin)    │──►│ before/after    │   │  Given/When/Then      │   │
 │  └──────────────┘   │ hooks           │   └───────────┬───────────┘   │
 │                      └────────┬────────┘               │               │
 │                               │ spawns                 │               │
 │              ┌────────────────┼────────────────────────┤               │
 │              │                │                        │               │
 │              ▼                ▼                        │               │
 │  ┌──────────────────┐  ┌────────────────┐             │               │
 │  │ ExchangeSimulator │  │  SDPSimulator   │             │               │
 │  │ (HTTPS :RPORT)   │  │  (in-process)  │             │               │
 │  │ (WSS  :WPORT)    │  └────────────────┘             │               │
 │  └──────────┬───────┘                                  │               │
 │             │ TLS/WSS                    Kafka produce │               │
 │             │                           (subscribe/    │               │
 │  ┌──────────┴──────────────────────┐     unsubscribe) │               │
 │  │     Gateway binary               │◄─────────────────┘               │
 │  │     (subprocess, cwd=IT/<GW>/)   │                                  │
 │  └──────────┬──────────────────────┘                                  │
 │             │ Kafka produce (trade_update / depth_update)              │
 │             ▼                                                           │
 │  ┌─────────────────────┐                                               │
 │  │ KafkaUpdateConsumer │◄── drain() / poll_once() ── behave steps      │
 │  │ (<GW>_IT_1)         │                                               │
 │  └─────────────────────┘                                               │
 │                                                                         │
 │   All of the above runs inside one OS process.                          │
 │   The simulator's asyncio loop runs in a background thread.             │
 └────────────────────────────────────────────────────────────────────────┘
```

### Full message flow (subscribe → update → assert)

```
behave step               Gateway binary (subprocess)         ExchangeSimulator (Python)
    │                             │                                   │
    │─── Kafka produce ──────────►│                                   │
    │    topic: <gw>_subscriptions                                    │
    │    header: message_type="subscribe"                             │
    │    body: {destination_topic, symbol, type}                      │
    │                             │── HTTPS GET /exchangeInfo ───────►│
    │                             │◄── {symbols:[...]} ───────────────│
    │                             │── TLS WebSocket upgrade ─────────►│
    │                             │── WS SUBSCRIBE ──────────────────►│
    │                             │◄── {result:null} ─────────────────│
    │                             │◄── {stream:"...", data:{...}} ─────│  periodic
    │                    transforms data                               │
    │◄── Kafka consume ───────────│                                   │
    │    topic: <GW>_IT_1                                             │
    │    header: message_type="trade_update"                          │
  assert                          │                                   │
```

---

## 3. Directory Layout

```
Tests/
├── BinanceMD_IT.cpp              ← Legacy C++ IT (kept for reference; runs manually)
│
└── IT/
    ├── IT_DESIGN.md              ← High-level design notes
    ├── IT_FRAMEWORK_GUIDE.md     ← This file (framework-generic)
    │
    ├── framework/                ← Reusable, gateway-agnostic layer
    │   ├── simulator/
    │   │   ├── exchange_simulator.py   ← Abstract HTTPS+WSS server base class
    │   │   └── tls_helper.py           ← Self-signed cert generator
    │   └── utils/
    │       ├── kafka_helper.py         ← Kafka producer/consumer wrappers
    │       └── wait_for.py             ← Synchronous polling helper
    │
    └── BinanceMD/                ← Binance-specific test suite
        ├── IT_GUIDE.md           ← BinanceMD-specific guide (see this for details)
        ├── binance/
        │   └── binance_simulator.py    ← Concrete Binance protocol simulator
        ├── config/
        │   └── config.json             ← Test config for BinanceMD at runtime
        ├── features/
        │   ├── binance_md.feature      ← Gherkin scenarios
        │   ├── environment.py          ← before_*/after_* lifecycle hooks
        │   └── steps/
        │       └── binance_md_steps.py ← Given/When/Then implementations
        ├─── process_manager.py          ← Subprocess lifecycle for BinanceMD
        ├── requirements.txt            ← Python deps (behave, confluent-kafka, ...)
        ├── Dockerfile.it               ← CI Docker image
        └── docker-compose.it.yml       ← Full-stack CI compose file
```

When a new gateway component is added, it gets its own subdirectory under `Tests/IT/` with the same shape as `BinanceMD/`.

---

## 4. Framework Component Deep-Dive

All files in this section live under `Tests/IT/framework/`. These classes are **gateway-agnostic** — every integration test suite reuses them unchanged.

### 4.1 ExchangeSimulator (base class)

**File:** `Tests/IT/framework/simulator/exchange_simulator.py`

The reusable foundation for any exchange simulator. It provides two things:

1. An **HTTPS REST server** (via `aiohttp`) on a configurable port, with a middleware hook that sets `rest_request_received = True` on the first incoming request — used by the harness as the "gateway is alive" latch.
2. A **WSS WebSocket server** on a separate configurable port, at path `/stream`. All incoming JSON frames are dispatched to `_on_ws_message()`, which subclasses implement.

Helpers for test steps:

| Method / Property | Purpose |
|---|---|
| `drop_all_ws_connections()` | Force-closes every active WS connection — triggers reconnect logic in the gateway |
| `broadcast_ws(msg)` | Fan-out a JSON message to all connected clients |
| `ws_connection_count` | Number of currently live WS connections — used to assert reconnect happened |
| `reset()` | Drops WS connections and clears state without stopping/restarting the servers (avoids port-rebinding races between scenarios) |

Both servers share the same `ssl.SSLContext` built from the self-signed cert generated in `before_all`.

To add support for a new exchange protocol, subclass `ExchangeSimulator`, register REST route handlers, and implement `_on_ws_message()`.

### 4.2 KafkaCommandProducer

**File:** `Tests/IT/framework/utils/kafka_helper.py`

Sends subscribe/unsubscribe commands to a gateway. Every command is a Kafka message with:

- **Key:** the symbol string (e.g. `BTCUSDT`)
- **Header:** `message_type = "subscribe"` or `"unsubscribe"`
- **Value:** JSON `{"destination_topic": "<our topic>", "symbol": "...", "type": "trade"|"depth"}`

The `destination_topic` field tells the gateway where to publish updates. The IT harness typically uses a dedicated topic like `<GW>_IT_1`.

### 4.3 KafkaUpdateConsumer

**File:** `Tests/IT/framework/utils/kafka_helper.py`

Receives trade/depth update messages that the gateway publishes.

**Key design decision:** it uses `assign()` at the **current high-watermark offset** rather than `subscribe()`. This means:
- No rebalance delay (ready immediately)
- Only sees messages produced _after_ it was created — no stale messages from previous scenarios

**Important methods:**

| Method | What it does |
|---|---|
| `poll_once(timeout_sec)` | Polls once, appends any trade/depth message to internal list |
| `drain(duration_sec)` | Polls continuously for the given window, collecting all messages |
| `clear()` | Empties the **Python-side list only** — does NOT move the Kafka read pointer |
| `seek_to_end()` | Repositions the Kafka consumer to the current HWM AND clears the list — use this when you need to start a "silence window" assertion after an unsubscribe |
| `has_trade_update()` / `has_depth_update()` | Presence checks |
| `trade_updates()` / `depth_updates()` | Full list access |

> ⚠️ **Critical:** Do not use `clear()` before a silence-window assertion. Use `seek_to_end()` instead. See [Section 8](#8-common-pitfalls--lessons-learned).

### 4.4 KafkaStatusConsumer & KafkaTopicProbe

**File:** `Tests/IT/framework/utils/kafka_helper.py`

Used by `before_scenario` to wait for readiness signals.

- **`KafkaStatusConsumer`** watches the `gateway_status` topic. Its `gateway_operational` flag flips `True` once the gateway publishes `{"md_gw_status": "operational"}`. The harness polls this with a configurable timeout.
- **`KafkaTopicProbe`** watches an arbitrary topic. Its `message_received` flag flips `True` when the first message arrives — used to detect that the per-partition FSM has completed its initial sync handshake with the SDPSimulator. Only after this is the gateway ready to process subscriptions.

Both have a `warm_up(timeout_sec)` method that blocks until Kafka partition assignment is confirmed — this ensures they don't miss very early messages from a fast-starting gateway.

### 4.5 wait_for

**File:** `Tests/IT/framework/utils/wait_for.py`

```python
wait_for(condition: Callable[[], bool], timeout_sec: float, poll_interval_sec: float = 0.1) -> bool
```

A simple synchronous polling loop. Use it anywhere a step needs to wait for an async side-effect to materialise. Returns `True` if the condition became `True` within the timeout, `False` otherwise.

```python
assert wait_for(lambda: context.simulator.ws_connection_count > 0, timeout_sec=15)
```

---

## 5. Generic Scenario Lifecycle Pattern

Every integration test suite follows this pattern in its `environment.py`:

```
before_all (once for the entire behave run)
│
├── Start a background asyncio event-loop thread
│   (the simulator runs its coroutines here, synchronised from behave via
│    asyncio.run_coroutine_threadsafe)
│
├── Generate a self-signed TLS cert + key in a temp dir
│   (valid for 127.0.0.1 SAN; shared across all scenarios)
│
├── Create all required Kafka topics if they don't exist
│
└── Start the ExchangeSimulator subclass
    (started once, reset between scenarios to avoid port-rebinding races)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

before_scenario (once per scenario)
│
├── 1. simulator.reset()
│      Drops all WS connections, clears any per-scenario state
│
├── 2. Create fresh Kafka helpers (producer, consumer, status_consumer, sync_probe)
│      All positioned at current high-watermark → see only messages from this scenario
│
├── 3. warm_up() status_consumer and sync_probe
│      Waits for Kafka partition assignment before any process starts
│
├── 4. Create the SDPSimulator (in-process; uses assign() — no rebalance delay)
│      Must be created BEFORE the gateway so it is ready to respond to the
│      first sync_data_request immediately after the partition rebalance fires
│
├── 5. Start gateway binary subprocess
│      Env: SSL_CA_CERT = path to self-signed cert
│
├── 6. Wait for gateway_status = operational
│      Confirms: Kafka middleware up, REST connections established, WS connected
│
├── 7. Wait for sync_data_request message + SDPSimulator responds
│      Confirms: per-partition FSM assigned, download request sent and answered
│      → FSM is now in Operational state, ready to process subscriptions
│
└── 8. Brief settle sleep
       Allows the gateway's individual Kafka consumer rebalance to complete

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

after_scenario
│
├── Send request_stop to gateway via Kafka
├── Wait for app_down on registrations topic
└── Close all Kafka helpers + SDPSimulator

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

after_all
│
├── simulator.stop()
├── Stop asyncio event-loop thread
└── Delete TLS temp directory
```

> **Why create SDPSimulator before starting the gateway?**  
> The gateway's Kafka partition rebalance fires on startup and immediately emits a `sync_data_request`. The SDPSimulator uses `assign()` (no consumer group, no rebalance), so it is ready to respond the moment it is created. Creating it in-process before the gateway subprocess starts guarantees it is listening before the first request lands on the topic. See also [Section 8 — SDPSimulator ordering](#sdpsimulator-must-be-created-before-the-gateway).

---

## 6. Kafka Wire Protocol

This section documents the common message shape. Concrete topic names, header values, and JSON body keys vary per gateway — see the component-specific guide.

### Subscribe / Unsubscribe command (test → gateway)

```
Topic:   <gw>_price_subscriptions      (e.g. binance_price_subscriptions)
Key:     <SYMBOL>                       e.g. BTCUSDT
Headers: message_type = "subscribe"   |   "unsubscribe"
Value:   {
           "destination_topic": "<IT topic>",
           "symbol":            "<SYMBOL>",
           "type":              "trade"   |   "depth"
         }
```

### Market data update (gateway → test)

```
Topic:   <IT topic>                     e.g. BinanceMD_IT_1
Key:     <SYMBOL>:<type>                e.g. BTCUSDT:trade
Headers: message_type = "trade_update" | "depth_update"
Value:   { "symbol": "...", ... }       (gateway-specific fields)
```

### Gateway status (gateway → monitoring)

```
Topic:   gateway_status
Headers: message_type = "status_update"
Value:   { "appId": "<id>", "md_gw_status": "operational", ... }
```

> Any mismatch between the harness-side keys and the C++ `Constants.h` definitions will result in commands being silently ignored by the gateway.

---

## 7. How to Add a New Gateway Component Test Suite

Use `BinanceMD/` as the reference implementation. The steps below create a parallel suite for a new gateway called `<GW>`.

### Step 1 — Create the directory skeleton

```
Tests/IT/<GW>/
├── IT_GUIDE.md             ← Component-specific guide (see BinanceMD/IT_GUIDE.md as template)
├── <gw>/
│   └── <gw>_simulator.py  ← Subclass of ExchangeSimulator
├── config/
│   └─── config.json         ← Config read by the gateway binary at runtime
├── features/
│   ├── <gw>.feature        ← Gherkin scenarios
│   ├── environment.py      ← before_*/after_* lifecycle hooks
│   └── steps/
│       └── <gw>_steps.py   ← Given/When/Then implementations
├── process_manager.py      ← Subprocess lifecycle wrappers
└── requirements.txt        ← Python deps (can copy from BinanceMD/)
```

### Step 2 — Implement the simulator

```python
# Tests/IT/<GW>/<gw>/<gw>_simulator.py
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'framework'))

from simulator.exchange_simulator import ExchangeSimulator

class <GW>Simulator(ExchangeSimulator):
    def __init__(self, rest_port, ws_port):
        super().__init__(rest_port, ws_port)
        # Register REST routes
        self._app.router.add_get('/api/v1/ping', self._handle_ping)

    async def _handle_ping(self, request):
        from aiohttp import web
        return web.json_response({})

    async def _on_ws_message(self, ws, data):
        # Handle SUBSCRIBE / UNSUBSCRIBE frames
        pass
```

### Step 3 — Copy and adapt environment.py

Copy `Tests/IT/BinanceMD/features/environment.py`. Replace:
- Kafka topic names (constants at top of file)
- REST/WS port numbers
- Simulator class import
- Binary names and env-var overrides

### Step 4 — Write the feature file

```gherkin
Feature: <GW> gateway forwards market data to Kafka

  Background:
    Given <GW> is running and connected to the exchange simulator

  Scenario: Trade stream delivers updates to Kafka after subscribe
    When a client subscribes to "XYZABC" trade updates
    Then Kafka receives at least 1 trade update for "XYZABC" within 20 seconds
```

### Step 5 — Implement step definitions

Implement `Given/When/Then` functions in `features/steps/<gw>_steps.py` following the same pattern as `BinanceMD/features/steps/binance_md_steps.py`.

### Step 6 — Write config.json

Set Kafka broker, REST/WS ports, and `session.timeout.ms: 6000` in `middleware_params` (see [Section 8](#consumer-group-sessiontimeoutms-must-be-low)).

### Step 7 — Run

```bash
cd Tests/IT/<GW>
python -m behave features/<gw>.feature --no-capture
```

---

## 8. Common Pitfalls & Lessons Learned

These are real bugs encountered while building the framework. Read this before debugging a flaky test.

---

### `consumer.clear()` is NOT a Kafka seek

**Symptom:** "Unexpected trade update received after unsubscribe" even though the gateway correctly sent the WS UNSUBSCRIBE.

**Root cause:** `clear()` only empties the Python-side list. Messages already written to the Kafka topic but not yet polled are still waiting at the pre-`clear()` offset. The next `drain()` reads them immediately.

**Fix:** Use `consumer.seek_to_end()` before a silence-window assertion. It repositions the Kafka consumer to the current high-watermark atomically, skipping all pre-unsubscribe messages.

```python
# Wrong — will pick up pre-unsubscribe messages from Kafka
context.consumer.clear()
context.consumer.drain(duration_sec=4)

# Correct
context.consumer.seek_to_end()
context.consumer.drain(duration_sec=4)
```

---

### SDPSimulator must be created before the gateway

**Symptom:** The gateway starts, but the FSM never reaches `Operational`. The `sync_data_request` message is never answered.

**Root cause:** The gateway's Kafka partition rebalance fires on startup and it immediately emits a `sync_data_request`. If the SDPSimulator is not yet listening, nobody answers the request and the FSM stays stuck in `Downloading` forever.

**Fix:** Create the SDPSimulator (in-process, using `assign()`) before launching the gateway subprocess. Because `assign()` does not require a Kafka group rebalance it is ready instantly, eliminating the timing race entirely.

---

### Consumer group `session.timeout.ms` must be low

**Symptom:** The second scenario hangs for ~45 seconds then fails waiting for gateway readiness.

**Root cause:** Kafka's default `session.timeout.ms` is 45 seconds. When the previous scenario's gateway process dies, the broker keeps that consumer group membership live for 45 seconds. The next scenario's gateway tries to rejoin the same group; the rebalance stalls for the full session expiry window.

**Fix:** Set `session.timeout.ms: 6000` (and `heartbeat.interval.ms: 2000`) in `config.json` under `middleware_params` for the gateway. This reduces the maximum hang to 6 seconds.

> **Note:** This only applies to the gateway's own Kafka consumer group. The SDPSimulator uses `assign()` and therefore has no group and no session timeout issue.

---

### The TLS cert must have a SAN for `127.0.0.1`

**Symptom:** The gateway fails the TLS handshake immediately on startup.

**Root cause:** Boost.Beast validates the Subject Alternative Name (SAN) field. Without a SAN entry for `127.0.0.1`, the handshake fails regardless of peer-verification settings.

**Fix:** `tls_helper.py` always adds `127.0.0.1` as a SAN. Do not replace it with a plain CN-only cert.

---

### `SSL_clear()` is needed on reconnect

**Symptom:** After the simulator drops the WS connection, the gateway's reconnect attempt fails with "protocol is shutdown" from OpenSSL.

**Root cause:** A graceful TLS shutdown marks the `SSL*` object as shut down. It cannot be reused for a new handshake without `SSL_clear()`.

**Fix:** Call `SSL_clear(stream.next_layer().native_handle())` in `closeConnection()` after closing the WS layer. This was implemented in `WebSockets.h` for BinanceMD — replicate the pattern in any new gateway that manages its own `ssl::stream`.

---

### Simulator update loop has inertia after UNSUBSCRIBE

**Symptom:** The gateway sends WS UNSUBSCRIBE, the simulator ACKs it, but the simulator still pushes one or two more frames.

**Root cause:** The simulator's update task checks for empty subscribers only on its next `asyncio.sleep()` wake-up (every 200 ms). One extra frame can be sent in that window.

**Fix:** Add a sync point: poll the simulator's `is_subscribed()` flag until `False` (confirms the stream is gone from the server side), sleep briefly for in-flight WS frames to traverse the Kafka pipeline, then call `seek_to_end()` before the silence window. See how `step_assert_no_trade_update` handles this in `BinanceMD/features/steps/binance_md_steps.py`.
