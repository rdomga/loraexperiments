#!/usr/bin/env python3
"""Adds the keys of tbConnector_additions.json to the uplink mapping of a ThingsBoard gateway MQTT connector config.
Idempotent: keys that already exist are left alone. Writes a new file, never edits the input.

usage: apply_connector_additions.py <tbConnector.json in> <tbConnector.json out>
"""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
src, dst = sys.argv[1], sys.argv[2]
cfg = json.load(open(src))
add = json.load(open(os.path.join(HERE, "tbConnector_additions.json")))
m = [x for x in cfg["mapping"] if x["topicFilter"].endswith("/up")]
if len(m) != 1:
    sys.exit("expected exactly one uplink mapping (topic ending /up), found %d" % len(m))
ts = m[0]["converter"]["timeseries"]
have = {x["key"] for x in ts}
new = [a for a in add if a["key"] not in have]
ts.extend(new)
json.dump(cfg, open(dst, "w"), indent=2)
print("added %d key(s), %d already present: %s" % (len(new), len(add) - len(new), ", ".join(a["key"] for a in new) or "-"))
