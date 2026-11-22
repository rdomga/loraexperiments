#!/usr/bin/env python3
"""Adds the night / battery-strategy rows to the "Applied config" widget of an exported ThingsBoard dashboard.
Idempotent. Reads the export, writes a new file, never edits the input.

usage: patch_applied_config_widget.py <dashboard.json in> <dashboard.json out>

What it does, inside the widget titled "Applied config" (markdown card):
  - appends 7 timeseries data keys to datasource 0 (battery_strategy, night_active, critical_floor, night_enter_adc,
    night_exit_adc, night_interval_min, night_unsent);
  - inserts 7 rows into markdownTextFunction right after the "Batt. psave recover" row. The row() helper already shows a dash
    for a missing value, so nodes that still send the old 29-byte status frame just show dashes in the new rows.
"""
import json, sys

KEYS = ["battery_strategy", "night_active", "critical_floor", "night_enter_adc", "night_exit_adc", "night_interval_min", "night_unsent"]
ANCHOR = "    row('Batt. psave recover', 'batt_psave_recover') +\n"
ROWS = (
    "    row('Battery strategy', 'battery_strategy') +\n"
    "    row('Night mode active', 'night_active', bool) +\n"
    "    row('Critical floor', 'critical_floor', bool) +\n"
    "    row('Night: store below (ADC)', 'night_enter_adc') +\n"
    "    row('Night: resume at (ADC)', 'night_exit_adc') +\n"
    "    row('Night interval (min)', 'night_interval_min') +\n"
    "    row('Saved readings waiting', 'night_unsent') +\n"
)

def walk(o):
    if isinstance(o, dict):
        yield o
        for v in o.values():
            yield from walk(v)
    elif isinstance(o, list):
        for v in o:
            yield from walk(v)

src, dst = sys.argv[1], sys.argv[2]
dash = json.load(open(src))
found = 0
for d in walk(dash):
    if not isinstance(d.get("widgets"), dict):
        continue
    for wid, w in d["widgets"].items():
        cfg = w.get("config", {})
        if cfg.get("title") != "Applied config":
            continue
        found += 1
        fn = cfg["settings"]["markdownTextFunction"]
        if "night_active" in fn:
            print("widget %s: rows already present, left alone" % wid)
        else:
            if ANCHOR not in fn:
                sys.exit("widget %s: anchor row not found, the widget changed; edit by hand" % wid)
            cfg["settings"]["markdownTextFunction"] = fn.replace(ANCHOR, ANCHOR + ROWS, 1)
            print("widget %s: 7 rows inserted" % wid)
        keys = cfg["datasources"][0]["dataKeys"]
        have = {k["name"] for k in keys}
        for k in KEYS:
            if k not in have:
                keys.append({"name": k, "type": "timeseries", "label": k, "color": "#305680", "settings": {}})
        print("widget %s: %d data keys now" % (wid, len(keys)))
if not found:
    sys.exit('no widget titled "Applied config" found')
json.dump(dash, open(dst, "w"), indent=2)
