#!/usr/bin/env python3
"""Builds Stored_Readings_rule_chain.json, a standalone rule chain for ThingsBoard (Rule chains > Import).

Same pattern as "Tiered Alarms": a small separate chain, reached from the Root Rule Chain through ONE "Rule chain" node.
Nodes: Is stored frame? (filter) -> Latest night interval (originator attributes, optional) -> Explode stored readings (transform, stored_readings_explode.js) -> Save stored readings
(Save Timeseries, NOT connected to Tiered Alarms, so replayed readings never raise or clear alarms). A failure of the
transform goes to a log node.

usage: build_stored_readings_chain.py [out.json]
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "Stored_Readings_rule_chain.json")
explode = open(os.path.join(HERE, "stored_readings_explode.js")).read()
filt = ("return msg.type === 'stored' && typeof msg.stored_csv === 'string' && msg.stored_csv.length > 0;")
logjs = ("return 'Stored readings: could not explode the frame\\n' + JSON.stringify(msg) + '\\n' + JSON.stringify(metadata);")

def node(typ, name, cfg, x, y, desc=""):
    return {"type": typ, "name": name, "debugSettings": None, "singletonMode": False, "queueName": None,
            "configurationVersion": 1 if "Timeseries" in typ else 0, "configuration": cfg, "externalId": None,
            "additionalInfo": {"description": desc, "layoutX": x, "layoutY": y}}

nodes = [
    node("org.thingsboard.rule.engine.filter.TbJsFilterNode", "Is stored frame?",
         {"scriptLang": "JS", "jsScript": filt, "tbelScript": "return msg.type == 'stored';"}, 300, 150,
         "Port 3 uplink carrying saved night readings (msg.type == 'stored' and a stored_csv string)."),
    node("org.thingsboard.rule.engine.metadata.TbGetAttributesNode", "Latest night interval",
         {"tellFailureIfAbsent": False, "fetchTo": "METADATA", "clientAttributeNames": [], "sharedAttributeNames": [],
          "serverAttributeNames": [], "latestTsKeyNames": ["night_interval_min"], "getLatestValueWithTs": False}, 460, 150,
         "Fetches the device's latest night_interval_min telemetry into metadata. Only used for old v1 port 3 frames, which carry no interval."),
    node("org.thingsboard.rule.engine.transform.TbTransformMsgNode", "Explode stored readings",
         {"scriptLang": "JS", "jsScript": explode, "tbelScript": "return {msg: msg, metadata: metadata, msgType: msgType};"}, 700, 150,
         "One {ts, values} entry per saved reading, timestamped at the reading time (arrival minus age)."),
    node("org.thingsboard.rule.engine.telemetry.TbMsgTimeseriesNode", "Save stored readings",
         {"defaultTTL": 0, "useServerTs": False, "processingSettings": {"type": "ON_EVERY_MESSAGE"}}, 1040, 150,
         "Deliberately has no connection to Tiered Alarms: replayed night readings must not raise or clear alarms."),
    node("org.thingsboard.rule.engine.action.TbLogNode", "Log explode failure",
         {"scriptLang": "JS", "jsScript": logjs, "tbelScript": logjs}, 1040, 300),
]
conns = [{"fromIndex": 0, "toIndex": 1, "type": "True"},
         {"fromIndex": 1, "toIndex": 2, "type": "Success"},
         {"fromIndex": 1, "toIndex": 2, "type": "Failure"},     # no interval available: carry on, the script defaults to 60 min
         {"fromIndex": 2, "toIndex": 3, "type": "Success"},
         {"fromIndex": 2, "toIndex": 4, "type": "Failure"}]
chain = {
    "ruleChain": {"name": "Stored Readings", "type": "CORE", "root": False, "debugMode": False, "configuration": None,
                  "additionalInfo": {"description": "Saves the night readings a node stored in EEPROM and sent on port 3, at the time they were taken. No alarms, no AQI state."}},
    "metadata": {"firstNodeIndex": 0, "nodes": nodes, "connections": conns, "ruleChainConnections": None},
}
json.dump(chain, open(OUT, "w"), indent=2)
print("wrote", OUT)
