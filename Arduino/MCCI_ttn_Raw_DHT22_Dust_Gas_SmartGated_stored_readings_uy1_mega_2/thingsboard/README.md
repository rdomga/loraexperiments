# ThingsBoard Stored Readings Rule Chain

Rule chain configuration and transformation scripts for processing multi-record stored telemetry frames into discrete time-series points in ThingsBoard.

## Components

| File | Purpose |
|---|---|
| `Stored_Readings_rule_chain.json` | ThingsBoard rule chain definition for processing Port 3 stored reading frames. |
| `stored_readings_explode.js` | JavaScript function used in the rule chain to explode the received batch CSV into individual timestamped telemetry records. |
| `test_explode.js` | Unit test for verifying `stored_readings_explode.js` against sample Port 3 frame payloads. |
| `tbConnector_additions.json` | Gateway connector configuration mapping uplink payloads to ThingsBoard telemetry attributes. |

## Telemetry Ingestion

When a Port 3 stored reading frame arrives:
1. The rule chain unpacks the record batch (`stored_csv`).
2. `stored_readings_explode.js` parses the entries, calculating discrete record timestamps based on frame arrival time and record age.
3. Telemetry values (`temperature`, `humidity`, `dust_ugm3`, `battery_voltage`, `battery_soc_pct`) are persisted to the device timeseries with their calculated historical timestamps.
