"""Step definitions for BinanceMD integration tests.

All steps operate on context attributes set up by environment.py:
  context.simulator   — BinanceSimulator
  context.producer    — KafkaCommandProducer
  context.consumer    — KafkaUpdateConsumer
  context.bmd_process — BinanceMDProcess
  context.loop        — asyncio event loop (background thread)

Kafka protocol constants (must match Constants.h):
  Header "message_type":
    subscribe/unsubscribe commands → "subscribe" / "unsubscribe"
    BinanceMD outputs              → "trade_update" / "depth_update"
  Payload field "destination_topic" → the topic our consumer listens on
  Payload field "type"             → "trade" | "depth"
"""

import asyncio
import os
import sys
import time

from behave import given, when, then

# Ensure the IT package root is importable from step files.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../../.."))

from IT.framework.utils.wait_for import wait_for

# Kafka topic constants (must agree with environment.py).
BINANCE_IN_TOPIC = os.environ.get("BINANCE_IN_TOPIC", "binance_price_subscriptions")
BINANCE_IT_TOPIC = os.environ.get("BINANCE_IT_TOPIC", "BinanceMD_IT_1")


# ── Background ────────────────────────────────────────────────────────────────

@given('BinanceMD is running and connected to the exchange simulator')
def step_bmd_running(context):
    """Verified by before_scenario in environment.py — assert the explicit service contract."""
    assert context.bmd_process.is_running(), "BinanceMD process is not running"
    assert context.sdp_simulator is not None, "SDPSimulator not initialized"
    assert context.status_consumer.gwyOperational(0.0), (
        "BinanceMD has not published gateway_status=operational"
    )


# ── Subscribe / unsubscribe ───────────────────────────────────────────────────

@when('a client subscribes to "{symbol}" trade updates')
def step_subscribe_trade(context, symbol):
    context.producer.subscribe(
        in_topic=BINANCE_IN_TOPIC,
        symbol=symbol,
        subscription_type="trade",
        destination_topic=BINANCE_IT_TOPIC,
    )
    context.last_symbol = symbol
    context.last_sub_type = "trade"


@when('a client subscribes to "{symbol}" depth updates')
def step_subscribe_depth(context, symbol):
    context.producer.subscribe(
        in_topic=BINANCE_IN_TOPIC,
        symbol=symbol,
        subscription_type="depth",
        destination_topic=BINANCE_IT_TOPIC,
    )
    print(f"[depth subscribe] sent subscribe for {symbol} to {BINANCE_IN_TOPIC}", flush=True)
    context.last_symbol = symbol
    context.last_sub_type = "depth"


@given('a client has subscribed to "{symbol}" trade updates')
def step_given_subscribed_trade(context, symbol):
    step_subscribe_trade(context, symbol)


@when('the client unsubscribes from "{symbol}" trade updates')
def step_unsubscribe_trade(context, symbol):
    context.producer.unsubscribe(
        in_topic=BINANCE_IN_TOPIC,
        symbol=symbol,
        subscription_type="trade",
        destination_topic=BINANCE_IT_TOPIC,
    )


# ── "Updates are arriving" precondition ──────────────────────────────────────

@given('trade updates for "{symbol}" are arriving on Kafka')
def step_updates_arriving(context, symbol):
    """Wait until at least one trade update has been received, then clear."""
    _poll_until_trade_update(context, timeout_sec=20)
    context.consumer.clear()


# ── Assertions ────────────────────────────────────────────────────────────────

@then('Kafka receives at least 1 trade update for "{symbol}" within {timeout:d} seconds')
def step_assert_trade_update(context, symbol, timeout):
    _poll_until_trade_update(context, timeout_sec=timeout)
    assert context.consumer.has_trade_update(), (
        f"No trade update for {symbol} received within {timeout}s"
    )


@then('Kafka receives at least 1 depth update for "{symbol}" within {timeout:d} seconds')
def step_assert_depth_update(context, symbol, timeout):
    _poll_until_depth_update(context, timeout_sec=timeout)
    sim_subs = getattr(context.simulator, "subscribed_streams", set())
    assert context.consumer.has_depth_update(), (
        f"No depth update for {symbol} received within {timeout}s. "
        f"Simulator subscribed_streams={sim_subs}"
    )


@then('no new trade updates for "{symbol}" arrive on Kafka within {timeout:d} seconds')
def step_assert_no_trade_update(context, symbol, timeout):
    # Wait for the unsubscribe to propagate all the way to the simulator.
    # The simulator removes the stream from subscribed_streams once the last
    # subscriber is gone, so polling here gives us a reliable sync point.
    stream = symbol.lower() + "@trade"
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if not context.simulator.is_subscribed(stream):
            break
        time.sleep(0.1)
    # Allow any WS frames already in-flight (simulator → BinanceMD → Kafka)
    # to finish draining before we start the assertion window.
    time.sleep(0.5)
    # Seek the Kafka consumer to the current end-of-topic so that any trade
    # updates published *before* the unsubscribe fully propagated are skipped.
    # plain clear() only empties the Python list; it does not advance the
    # Kafka offset, so those messages would still be picked up by drain().
    context.consumer.seek_to_end()
    context.consumer.drain(duration_sec=timeout)
    assert not context.consumer.has_trade_update(), (
        f"Unexpected trade update for {symbol} received after unsubscribe"
    )


# ── Reconnect scenario ────────────────────────────────────────────────────────

@when('the exchange simulator drops all WebSocket connections')
def step_drop_ws(context):
    _run_sync(context, context.simulator.drop_all_ws_connections())


@then('BinanceMD reconnects to the simulator within {timeout:d} seconds')
def step_assert_reconnect(context, timeout):
    assert wait_for(
        lambda: context.simulator.ws_connection_count > 0,
        timeout_sec=timeout,
    ), f"BinanceMD did not reconnect within {timeout}s"


@then('trade updates for "{symbol}" resume on Kafka within {timeout:d} seconds')
def step_assert_updates_resume(context, symbol, timeout):
    context.consumer.clear()
    _poll_until_trade_update(context, timeout_sec=timeout)
    assert context.consumer.has_trade_update(), (
        f"Trade updates for {symbol} did not resume within {timeout}s"
    )


# ── Helpers ───────────────────────────────────────────────────────────────────

def _poll_until_trade_update(context, timeout_sec: float) -> None:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        context.consumer.poll_once(timeout_sec=0.5)
        if context.consumer.has_trade_update():
            return


def _poll_until_depth_update(context, timeout_sec: float) -> None:
    deadline = time.monotonic() + timeout_sec
    last_log = time.monotonic()
    while time.monotonic() < deadline:
        context.consumer.poll_once(timeout_sec=0.5)
        now = time.monotonic()
        if now - last_log >= 2.0:
            sim_subs = getattr(context.simulator, "subscribed_streams", set())
            print(f"[depth poll] elapsed={now-deadline+timeout_sec:.1f}s, subs={sim_subs}, updates={len(context.consumer.depth_updates())}", flush=True)
            last_log = now
        if context.consumer.has_depth_update():
            return


def _run_sync(context, coro, timeout: float = 10.0):
    future = asyncio.run_coroutine_threadsafe(coro, context.loop)
    return future.result(timeout=timeout)
