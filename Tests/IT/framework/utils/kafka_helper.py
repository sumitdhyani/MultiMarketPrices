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
        from confluent_kafka import TopicPartition
        # Use assign() at the current end-offset rather than subscribe() so
        # there is no rebalance delay and no risk of missing early messages.
        admin = AdminClient({"bootstrap.servers": brokers})
        meta = admin.list_topics(topic, timeout=10)
        partitions = list(meta.topics[topic].partitions.keys())

        group_id = f"IT_upd_{uuid.uuid4().hex[:12]}"
        self._consumer = Consumer({
            "bootstrap.servers":  brokers,
            "group.id":           group_id,
            "enable.auto.commit": "false",
        })
        tps = [TopicPartition(topic, p) for p in partitions]
        for tp in tps:
            _lo, hi = self._consumer.get_watermark_offsets(
                TopicPartition(topic, tp.partition), timeout=5.0, cached=False
            )
            tp.offset = hi
        self._consumer.assign(tps)
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

    def seek_to_end(self) -> None:
        """Advance the Kafka consumer position to the current high watermark and
        clear the in-memory update list.  Call this after an unsubscribe sync
        point so that any messages published *before* the unsubscribe took
        effect are skipped and do not pollute the subsequent silence window."""
        from confluent_kafka import TopicPartition
        assignment = self._consumer.assignment()
        for tp in assignment:
            _lo, hi = self._consumer.get_watermark_offsets(
                TopicPartition(tp.topic, tp.partition), timeout=5.0, cached=False
            )
            self._consumer.seek(TopicPartition(tp.topic, tp.partition, hi))
        self._updates.clear()

    def close(self) -> None:
        self._consumer.close()


# ── Status consumer (receives gateway_status messages from BinanceMD) ─────────

class KafkaStatusConsumer:
    """Polls the gateway_status topic and flags when an 'operational' status_update
    is received.

    Message format (produced by PlatformComm::buildStatusMsg):
      Header: message_type = "status_update"
      Value:  JSON { "message_type": "status_update",
                     "appId": "<appId>",
                     "appGroup": "<appGroup>",
                     "md_gw_status": "operational" | "degraded" | "disconnected" | "init",
                     "status_detail": "<free text>" }
    """

    STATUS_FIELD = "md_gw_status"
    OPERATIONAL  = "operational"
    MSG_TYPE     = "status_update"

    def __init__(self, brokers: str, status_topic: str) -> None:
        from confluent_kafka import TopicPartition
        # Use assign() with the current end-offset rather than subscribe().
        # This sidesteps the rebalance protocol entirely: the partition is
        # assigned and the fetch position is set atomically before the first
        # poll(), so no BinanceMD message published after __init__ returns
        # can ever be missed.
        admin = AdminClient({"bootstrap.servers": brokers})
        meta = admin.list_topics(status_topic, timeout=10)
        partitions = list(meta.topics[status_topic].partitions.keys())

        group_id = f"IT_status_{uuid.uuid4().hex[:12]}"
        self._consumer = Consumer({
            "bootstrap.servers":  brokers,
            "group.id":           group_id,
            "enable.auto.commit": "false",
        })
        tps = [TopicPartition(status_topic, p) for p in partitions]
        # get_watermark_offsets requires the consumer to have the partition
        # assigned first when using assign(); use the admin watermarks instead.
        wm_futures = {}
        for tp in tps:
            lo, hi = self._consumer.get_watermark_offsets(
                TopicPartition(status_topic, tp.partition), timeout=5.0, cached=False
            )
            tp.offset = hi
        self._consumer.assign(tps)
        self.gateway_operational: bool = False

    def warm_up(self, timeout_sec: float = 5.0) -> bool:
        """No-op: assign() is synchronous — the consumer is ready immediately."""
        return True

    def poll_once(self, timeout_sec: float = 0.5) -> None:
        msg = self._consumer.poll(timeout=timeout_sec)
        if msg is None or msg.error():
            return
        headers = dict(msg.headers() or [])
        msg_type_raw = headers.get("message_type", b"")
        msg_type = msg_type_raw.decode() if isinstance(msg_type_raw, bytes) else msg_type_raw
        if msg_type != self.MSG_TYPE:
            return
        try:
            payload = json.loads(msg.value().decode())
        except (json.JSONDecodeError, UnicodeDecodeError):
            return
        if payload.get(self.STATUS_FIELD) == self.OPERATIONAL:
            self.gateway_operational = True

    def close(self) -> None:
        self._consumer.close()


# ── Topic probe (detects the first message on any topic) ──────────────────────

class KafkaTopicProbe:
    """Assigns to a topic at the current end-offset and flags when any new
    message arrives.

    Used to detect that BinanceMD has published to sync_data_request, which
    confirms the Kafka partition has been assigned and the per-partition FSM
    has sent its sync-data download request to SDPMock.
    After the probe sees this message, SDPMock will have responded (in
    milliseconds) and the FSM will be operational.
    """

    def __init__(self, brokers: str, topic: str) -> None:
        from confluent_kafka import TopicPartition
        admin = AdminClient({"bootstrap.servers": brokers})
        meta = admin.list_topics(topic, timeout=10)
        partitions = list(meta.topics[topic].partitions.keys())

        group_id = f"IT_probe_{uuid.uuid4().hex[:12]}"
        self._consumer = Consumer({
            "bootstrap.servers":  brokers,
            "group.id":           group_id,
            "enable.auto.commit": "false",
        })
        tps = [TopicPartition(topic, p) for p in partitions]
        for tp in tps:
            _lo, hi = self._consumer.get_watermark_offsets(
                TopicPartition(topic, tp.partition), timeout=5.0, cached=False
            )
            tp.offset = hi
        self._consumer.assign(tps)
        self.message_received: bool = False

    def warm_up(self, timeout_sec: float = 5.0) -> bool:
        """No-op: assign() is synchronous — the consumer is ready immediately."""
        return True

    def poll_once(self, timeout_sec: float = 0.5) -> None:
        msg = self._consumer.poll(timeout=timeout_sec)
        if msg is None or msg.error():
            return
        self.message_received = True

    def close(self) -> None:
        self._consumer.close()
