"""Kafka producer/consumer helpers for integration tests.

The Middleware encodes the message type as a Kafka header with the key
"message_type".  TradeUpdate and DepthUpdate produced by BinanceMD arrive
on the topic that was specified as "destination_topic" in the subscribe
message sent by the IT client.

Subscribe message format (sent TO BinanceMD on its in_topic):
  Header:  message_type = "subscribe"
  Key:     <symbol>            e.g. "BTCUSDT"
  Value:   JSON {"destination_topic": "<our_topic>",
                 "symbol":            "<SYMBOL>",
                 "type":              "trade" | "depth"}

Update message format (received FROM BinanceMD on our_topic):
  Header:  message_type = "trade_update" | "depth_update"
  Key:     <SYMBOL>:trade | <SYMBOL>:depth
  Value:   JSON market-data payload
"""

import json
import time
import uuid
from typing import Optional

from confluent_kafka import Consumer, Producer, KafkaError, KafkaException
from confluent_kafka.admin import AdminClient, NewTopic


# ── Topic administration ──────────────────────────────────────────────────────

def ensure_topic_exists(brokers: str, topic: str, num_partitions: int = 1) -> None:
    """Create *topic* if it does not already exist (idempotent)."""
    admin = AdminClient({"bootstrap.servers": brokers})
    fs = admin.create_topics([NewTopic(topic, num_partitions=num_partitions, replication_factor=1)])
    for t, f in fs.items():
        try:
            f.result()
        except KafkaException as e:
            # TOPIC_ALREADY_EXISTS is not an error for us
            if e.args[0].code() != KafkaError.TOPIC_ALREADY_EXISTS:
                raise


# ── Producer (sends subscribe/unsubscribe commands to BinanceMD) ─────────────

class KafkaCommandProducer:
    """Sends subscribe / unsubscribe commands to BinanceMD via Kafka."""

    def __init__(self, brokers: str) -> None:
        self._producer = Producer({"bootstrap.servers": brokers})

    def subscribe(
        self,
        in_topic: str,
        symbol: str,
        subscription_type: str,  # "trade" or "depth"
        destination_topic: str,
    ) -> None:
        """Send a subscribe command to BinanceMD."""
        self._send(in_topic, symbol, subscription_type, destination_topic, is_sub=True)

    def unsubscribe(
        self,
        in_topic: str,
        symbol: str,
        subscription_type: str,
        destination_topic: str,
    ) -> None:
        """Send an unsubscribe command to BinanceMD."""
        self._send(in_topic, symbol, subscription_type, destination_topic, is_sub=False)

    def _send(
        self,
        topic: str,
        symbol: str,
        subscription_type: str,
        destination_topic: str,
        is_sub: bool,
    ) -> None:
        msg_type = "subscribe" if is_sub else "unsubscribe"
        payload = json.dumps({
            "destination_topic": destination_topic,
            "symbol":            symbol,
            "type":              subscription_type,
        }).encode()
        self._producer.produce(
            topic,
            key=symbol.encode(),
            value=payload,
            headers=[("message_type", msg_type.encode())],
        )
        self._producer.flush()

    def close(self) -> None:
        self._producer.flush()


# ── Consumer (receives trade/depth updates from BinanceMD) ───────────────────

class KafkaUpdateConsumer:
    """Polls a Kafka topic for trade_update / depth_update messages from BinanceMD."""

    MSG_TYPE_TRADE = "trade_update"
    MSG_TYPE_DEPTH = "depth_update"

    def __init__(self, brokers: str, topic: str) -> None:
        # Use a unique group_id so each test gets a fresh offset position.
        group_id = f"IT_{uuid.uuid4().hex[:12]}"
        self._consumer = Consumer({
            "bootstrap.servers": brokers,
            "group.id":          group_id,
            "auto.offset.reset": "latest",
            "enable.auto.commit": "true",
        })
        self._consumer.subscribe([topic])
        self._updates: list[dict] = []

    def poll_once(self, timeout_sec: float = 0.5) -> None:
        """Poll Kafka once and append any received update to the internal list."""
        msg = self._consumer.poll(timeout=timeout_sec)
        if msg is None or msg.error():
            return
        headers = dict(msg.headers() or [])
        msg_type_raw = headers.get("message_type", b"")
        msg_type = msg_type_raw.decode() if isinstance(msg_type_raw, bytes) else msg_type_raw
        if msg_type in (self.MSG_TYPE_TRADE, self.MSG_TYPE_DEPTH):
            try:
                payload = json.loads(msg.value().decode())
            except (json.JSONDecodeError, UnicodeDecodeError):
                return
            self._updates.append({"msg_type": msg_type, "payload": payload})

    def drain(self, duration_sec: float = 2.0) -> None:
        """Poll continuously for *duration_sec* seconds, collecting all messages."""
        deadline = time.monotonic() + duration_sec
        while time.monotonic() < deadline:
            self.poll_once(timeout_sec=0.1)

    # ── Accessors ─────────────────────────────────────────────────────────────

    def updates(self) -> list[dict]:
        return list(self._updates)

    def trade_updates(self) -> list[dict]:
        return [u for u in self._updates if u["msg_type"] == self.MSG_TYPE_TRADE]

    def depth_updates(self) -> list[dict]:
        return [u for u in self._updates if u["msg_type"] == self.MSG_TYPE_DEPTH]

    def has_trade_update(self) -> bool:
        return any(u["msg_type"] == self.MSG_TYPE_TRADE for u in self._updates)

    def has_depth_update(self) -> bool:
        return any(u["msg_type"] == self.MSG_TYPE_DEPTH for u in self._updates)

    def clear(self) -> None:
        self._updates.clear()

    def close(self) -> None:
        self._consumer.close()
