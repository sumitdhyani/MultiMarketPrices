"""behave environment hooks for the BinanceMD integration test suite.

Lifecycle:
  before_all  – generate TLS cert; start shared asyncio event loop thread;
                ensure all required Kafka topics exist
  before_scenario – start SDPMock; start BinanceSimulator; create Kafka helpers;
                    launch BinanceMD; wait for gateway_status=operational;
                    wait for sync_data_request (FSM partition assigned + SDPMock responded)
  after_scenario  – stop BinanceMD; stop SDPMock; stop simulator; close Kafka helpers
  after_all       – stop event loop; cleanup TLS temp dir

Context attributes set by before_scenario:
  context.simulator       — BinanceSimulator instance
  context.producer        — KafkaCommandProducer
  context.consumer        — KafkaUpdateConsumer
  context.status_consumer — KafkaStatusConsumer
  context.sync_probe      — KafkaTopicProbe (watches sync_data_request)
  context.bmd_process     — BinanceMDProcess
  context.sdpmock_process — SDPMockProcess
  context.loop            — asyncio.AbstractEventLoop (shared, background thread)

Constants (override via env vars):
  KAFKA_BROKERS            default "127.0.0.1:9092"
  BINANCE_IN_TOPIC         default "binance_price_subscriptions"
  BINANCE_IT_TOPIC         default "BinanceMD_IT_1"
  GATEWAY_STATUS_TOPIC     default "gateway_status"
  SYNC_DATA_REQUEST_TOPIC  default "sync_data_request"
  REST_PORT                default 18443
  WS_PORT                  default 18444
"""

import asyncio
import os
import shutil
import tempfile
import threading
import time

import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../.."))  # → Tests/
from IT.BinanceMD.binance.binance_simulator import BinanceSimulator
from IT.framework.simulator.tls_helper import generate_self_signed_cert
from IT.framework.utils.kafka_helper import (
    KafkaCommandProducer,
    KafkaUpdateConsumer,
    KafkaStatusConsumer,
    KafkaLifeCycleConsumer,
    KafkaTopicProbe,
    ensure_topic_exists,
)
from IT.BinanceMD.process_manager import BinanceMDProcess, SDPMockProcess
from IT.framework.utils.wait_for import wait_for

# ── Constants ─────────────────────────────────────────────────────────────────
KAFKA_BROKERS           = os.environ.get("KAFKA_BROKERS",           "127.0.0.1:9092")
BINANCEMD_APP_ID        = os.environ.get("BINANCEMD_APP_ID",        "BinanceMD_1")
BINANCE_IN_TOPIC        = os.environ.get("BINANCE_IN_TOPIC",        "binance_price_subscriptions")
BINANCE_IT_TOPIC        = os.environ.get("BINANCE_IT_TOPIC",        "BinanceMD_IT_1")
GATEWAY_STATUS_TOPIC    = os.environ.get("GATEWAY_STATUS_TOPIC",    "gateway_status")
SYNC_DATA_REQUEST_TOPIC = os.environ.get("SYNC_DATA_REQUEST_TOPIC", "sync_data_request")
REGISTRATIONS_TOPIC     = os.environ.get("REGISTRATIONS_TOPIC",     "registrations")
REST_PORT               = int(os.environ.get("REST_PORT",           "18443"))
WS_PORT                 = int(os.environ.get("WS_PORT",             "18444"))

# Readiness timeouts
OPERATIONAL_TIMEOUT_SEC  = 30   # gateway_status = operational
FSM_READY_TIMEOUT_SEC    = 15   # sync_data_request seen (partition assigned)
FSM_SETTLE_SEC           = 0.5  # SDPMock response pipeline settle window

APP_DOWN_WAIT_SEC        = 0.5  # time to wait for to get an app_down message, after sending a stop_request messaage
PROCESS_KILLED_WAIT_SEC  = 2    # time to wait for the process to be down after it has sent the app_down message
# ── Event-loop thread ─────────────────────────────────────────────────────────

def _run_loop(loop: asyncio.AbstractEventLoop) -> None:
    loop.run_forever()


# ── Suite-level hooks ─────────────────────────────────────────────────────────

def before_all(context) -> None:
    # Shared asyncio loop for the simulator.
    context.loop = asyncio.new_event_loop()
    context.loop_thread = threading.Thread(
        target=_run_loop, args=(context.loop,), daemon=True
    )
    context.loop_thread.start()

    # Generate one TLS cert/key pair for the whole suite.
    context.tls_dir = tempfile.mkdtemp(prefix="binance_it_tls_")
    context.certfile, context.keyfile = generate_self_signed_cert(context.tls_dir)

    # Ensure all Kafka topics used by BinanceMD and the IT harness exist.
    for topic in [
        BINANCE_IN_TOPIC,
        BINANCE_IT_TOPIC,
        GATEWAY_STATUS_TOPIC,
        SYNC_DATA_REQUEST_TOPIC,
        "heartbeats",
        "sync_data",
    ]:
        ensure_topic_exists(KAFKA_BROKERS, topic)

    # Start the exchange simulator once for the whole suite.  It is RESET
    # between scenarios rather than stopped/restarted to avoid port-binding
    # races when the same port is immediately re-bound by a new instance.
    context.simulator = BinanceSimulator(
        REST_PORT, WS_PORT, context.certfile, context.keyfile
    )
    _run_sync(context, context.simulator.start())


def after_all(context) -> None:
    if hasattr(context, "simulator"):
        _run_sync(context, context.simulator.stop())
    context.loop.call_soon_threadsafe(context.loop.stop)
    context.loop_thread.join(timeout=5)
    shutil.rmtree(context.tls_dir, ignore_errors=True)


# ── Scenario-level hooks ──────────────────────────────────────────────────────

def before_scenario(context, scenario) -> None:
    # 1. Reset the shared exchange simulator (drops WS connections, clears
    #    subscription state) without stopping/restarting the server so that
    #    port-binding races between scenarios are avoided.
    _run_sync(context, context.simulator.reset())

    # 2. Set up Kafka helpers.
    #    Status consumer and sync probe must be created BEFORE BinanceMD starts
    #    so they sit at 'latest' offset and will catch the very first messages.
    context.producer            = KafkaCommandProducer(KAFKA_BROKERS)
    context.consumer            = KafkaUpdateConsumer(KAFKA_BROKERS, BINANCE_IT_TOPIC)
    context.status_consumer     = KafkaStatusConsumer(KAFKA_BROKERS, GATEWAY_STATUS_TOPIC)
    context.lifecycle_consumer  = KafkaLifeCycleConsumer(KAFKA_BROKERS, REGISTRATIONS_TOPIC)
    context.sync_probe          = KafkaTopicProbe(KAFKA_BROKERS, SYNC_DATA_REQUEST_TOPIC)

    # 3. Warm up the monitoring consumers so their partition assignments are live
    #    before any process starts.  Without this, messages published immediately
    #    on startup (before the rebalance completes) are missed.
    assert context.status_consumer.warm_up(5.0), \
        "KafkaStatusConsumer failed to get partition assignment within 5s"
    assert context.sync_probe.warm_up(5.0), \
        "KafkaTopicProbe failed to get partition assignment within 5s"

    # 4. Start SDPMock BEFORE BinanceMD so that the sync_data_request consumer
    #    group is registered by the time BinanceMD's partition rebalance fires.
    scenario_tag = scenario.name.replace(" ", "_")[:40]
    bmd_log = f"/tmp/bmd_{scenario_tag}.log"
    sdp_log = f"/tmp/sdp_{scenario_tag}.log"
    context.sdpmock_process = SDPMockProcess()
    context.sdpmock_process.start(log_file=sdp_log)
    context.bmd_process = BinanceMDProcess(BINANCEMD_APP_ID)
    context.bmd_process.start(
        env_extra={"SSL_CA_CERT": context.certfile},
        log_file=bmd_log,
    )

    # 6. Wait for BinanceMD to publish gateway_status = operational.
    #    This confirms: middleware is up, Kafka producers/consumers are live,
    #    and both REST + WS exchange connections are established.
    if not wait_for(
        lambda: _poll_status(context),
        timeout_sec=OPERATIONAL_TIMEOUT_SEC,
        poll_interval_sec=0.2,
    ):
        still_up = context.bmd_process.is_running()
        try:
            with open(bmd_log) as f:
                log_tail = f.read()[-3000:]
        except OSError:
            log_tail = "(log unavailable)"
        raise AssertionError(
            f"BinanceMD did not reach operational status within {OPERATIONAL_TIMEOUT_SEC}s "
            f"(process {'still running' if still_up else 'has already exited'})\n"
            f"--- BinanceMD log ({bmd_log}) ---\n{log_tail}"
        )

    # 7. Wait for sync_data_request message.
    #    This confirms the Kafka partition was assigned, the per-partition FSM
    #    sent its download request to SDPMock, and SDPMock has responded
    #    (SDPMock responds synchronously, so the FSM reaches operational
    #    state within milliseconds of this message appearing on Kafka).
    assert wait_for(
        lambda: _poll_sync_probe(context),
        timeout_sec=FSM_READY_TIMEOUT_SEC,
        poll_interval_sec=0.1,
    ), f"BinanceMD partition FSM did not initialise within {FSM_READY_TIMEOUT_SEC}s"

    # Brief settle window for SDPMock's response to complete the FSM transition.
    time.sleep(FSM_SETTLE_SEC)

    # Extra settle: give BinanceMD's individual consumer (BinanceMD_1 topic) enough
    # time to complete its Kafka group rebalance and start polling, so that the
    # sync_data response already on the topic is picked up before any subscribe
    # command arrives from the test.
    time.sleep(3.0)


def after_scenario(context, scenario) -> None:
    if hasattr(context, "bmd_process"):
        context.producer.stop(BINANCEMD_APP_ID)
        assert context.lifecycle_consumer.appDown(APP_DOWN_WAIT_SEC), f"app_down message not received after sending stop request and waiting for{APP_DOWN_WAIT_SEC} sec"
        time.sleep(PROCESS_KILLED_WAIT_SEC)
        assert not context.bmd_process.is_running(), f"BinanceMD not down after {PROCESS_KILLED_WAIT_SEC} sec after sending app_down message" 
    if hasattr(context, "sdpmock_process"):
        context.sdpmock_process.stop()
    for attr in ("producer", "consumer", "status_consumer", "sync_probe"):
        if hasattr(context, attr):
            getattr(context, attr).close()


# ── Helpers ───────────────────────────────────────────────────────────────────

def _poll_status(context) -> bool:
    context.status_consumer.poll_once(timeout_sec=0.1)
    return context.status_consumer.gateway_operational

def _poll_sync_probe(context) -> bool:
    context.sync_probe.poll_once(timeout_sec=0.1)
    return context.sync_probe.message_received


def _run_sync(context, coro, timeout: float = 10.0):
    """Submit *coro* to the background asyncio loop and block until it completes."""
    future = asyncio.run_coroutine_threadsafe(coro, context.loop)
    return future.result(timeout=timeout)

