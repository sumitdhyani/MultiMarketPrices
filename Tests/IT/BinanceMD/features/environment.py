"""behave environment hooks for the BinanceMD integration test suite.

Lifecycle:
  before_all  – generate TLS cert; start shared asyncio event loop thread
  before_scenario – start BinanceSimulator; create Kafka helpers; launch BinanceMD;
                    wait for BinanceMD to connect to the simulator
  after_scenario  – stop BinanceMD; stop simulator; close Kafka helpers
  after_all       – stop event loop; cleanup TLS temp dir

Context attributes set by before_scenario:
  context.simulator   — BinanceSimulator instance
  context.producer    — KafkaCommandProducer
  context.consumer    — KafkaUpdateConsumer
  context.bmd_process — BinanceMDProcess
  context.loop        — asyncio.AbstractEventLoop (shared, background thread)

Constants (override via env vars):
  KAFKA_BROKERS  default "127.0.0.1:9092"
  BINANCE_IN_TOPIC   default "binance_price_subscriptions"
  BINANCE_IT_TOPIC   default "BinanceMD_IT_1"  (the topic BinanceMD publishes updates to)
  REST_PORT      default 18443
  WS_PORT        default 18444
"""

import asyncio
import os
import shutil
import tempfile
import threading

# IT package lives at Tests/IT/; we need Tests/ on sys.path for "import IT" to work.
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../.."))  # → Tests/
from IT.BinanceMD.binance.binance_simulator import BinanceSimulator
from IT.framework.simulator.tls_helper import generate_self_signed_cert
from IT.framework.utils.kafka_helper import (
    KafkaCommandProducer,
    KafkaUpdateConsumer,
    ensure_topic_exists,
)
from IT.BinanceMD.process_manager import BinanceMDProcess
from IT.framework.utils.wait_for import wait_for

# ── Constants ─────────────────────────────────────────────────────────────────
KAFKA_BROKERS    = os.environ.get("KAFKA_BROKERS",   "127.0.0.1:9092")
BINANCE_IN_TOPIC = os.environ.get("BINANCE_IN_TOPIC","binance_price_subscriptions")
BINANCE_IT_TOPIC = os.environ.get("BINANCE_IT_TOPIC","BinanceMD_IT_1")
REST_PORT        = int(os.environ.get("REST_PORT",   "18443"))
WS_PORT          = int(os.environ.get("WS_PORT",     "18444"))

# How long to wait for BinanceMD to connect to the simulator on startup.
CONNECT_TIMEOUT_SEC = 30


# ── Event-loop thread ─────────────────────────────────────────────────────────

def _run_loop(loop: asyncio.AbstractEventLoop) -> None:
    loop.run_forever()


# ── Suite-level hooks ─────────────────────────────────────────────────────────

def before_all(context) -> None:
    # Shared asyncio loop that runs the simulator in a background thread so
    # the synchronous behave steps can interact with it via run_coroutine_threadsafe.
    context.loop = asyncio.new_event_loop()
    context.loop_thread = threading.Thread(
        target=_run_loop, args=(context.loop,), daemon=True
    )
    context.loop_thread.start()

    # Generate one TLS cert/key pair for the whole suite.
    context.tls_dir = tempfile.mkdtemp(prefix="binance_it_tls_")
    context.certfile, context.keyfile = generate_self_signed_cert(context.tls_dir)

    # Ensure Kafka topics exist before any scenario runs.
    ensure_topic_exists(KAFKA_BROKERS, BINANCE_IN_TOPIC)
    ensure_topic_exists(KAFKA_BROKERS, BINANCE_IT_TOPIC)


def after_all(context) -> None:
    context.loop.call_soon_threadsafe(context.loop.stop)
    context.loop_thread.join(timeout=5)
    shutil.rmtree(context.tls_dir, ignore_errors=True)


# ── Scenario-level hooks ──────────────────────────────────────────────────────

def before_scenario(context, scenario) -> None:
    # 1. Start the exchange simulator.
    context.simulator = BinanceSimulator(
        REST_PORT, WS_PORT, context.certfile, context.keyfile
    )
    _run_sync(context, context.simulator.start())

    # 2. Set up Kafka helpers.
    context.producer = KafkaCommandProducer(KAFKA_BROKERS)
    context.consumer = KafkaUpdateConsumer(KAFKA_BROKERS, BINANCE_IT_TOPIC)

    # 3. Launch BinanceMD.
    context.bmd_process = BinanceMDProcess()
    context.bmd_process.start()

    # 4. Wait for BinanceMD to connect: the REST /exchangeInfo call is the first
    #    sign of life — once that arrives the WS client is also starting up.
    assert wait_for(
        lambda: context.simulator.rest_request_received,
        timeout_sec=CONNECT_TIMEOUT_SEC,
    ), f"BinanceMD did not connect to simulator within {CONNECT_TIMEOUT_SEC}s"


def after_scenario(context, scenario) -> None:
    # Stop BinanceMD first so it flushes / disconnects cleanly.
    if hasattr(context, "bmd_process"):
        context.bmd_process.stop()

    # Stop the simulator.
    if hasattr(context, "simulator"):
        _run_sync(context, context.simulator.stop())

    # Close Kafka helpers.
    if hasattr(context, "producer"):
        context.producer.close()
    if hasattr(context, "consumer"):
        context.consumer.close()


# ── Helper ────────────────────────────────────────────────────────────────────

def _run_sync(context, coro, timeout: float = 10.0):
    """Submit *coro* to the background asyncio loop and block until it completes."""
    future = asyncio.run_coroutine_threadsafe(coro, context.loop)
    return future.result(timeout=timeout)
