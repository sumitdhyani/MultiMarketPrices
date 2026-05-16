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
