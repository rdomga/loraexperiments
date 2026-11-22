// Script for the "Explode stored readings" transform node (rule chain "Stored Readings").
// Input: the telemetry message of a port 3 uplink, as mapped by tbConnector.json:
//   msg.type = "stored", msg.stored_csv = "seq,age_min,temp,hum,dust,gas,batt,vmask;seq,..." (see payload.txt, decodeStored),
//   msg.stored_boot_id, msg.stored_first_seq, msg.stored_uptime_min, msg.stored_count
//   metadata.ts = arrival time of the uplink in ms (the connector uses TTN's received_at).
// Output: an ARRAY of {ts, values}, one entry per saved reading, timestamped at the time of the reading, ready for a
// Save Timeseries node. ES5 on purpose (pastes into a TB script node).
//
// Not copied from the live chain, on purpose: co_ppm and the AQI keys. co_ppm uses placeholder calibration that is still
// changing, and the AQI keys come from a stateful hourly window ("Get AQI State" / "Save AQI State") that replayed
// readings must not touch. dust_ugm3 and battery_voltage / battery_soc_pct are stateless and are reproduced here.

var MINUTE_MS = 60000;
// Node's night reading interval, only used to place records whose age is unknown. Source, in order:
//   1) msg.night_interval_min: byte 7 of a v2 port 3 frame, so the frame carries its own interval;
//   2) metadata.night_interval_min: the latest status/frame value fetched by the 'Latest night interval' node (v1 frames);
//   3) 60 minutes, the firmware default.
var nightMin = parseInt(msg.night_interval_min, 10);
if (!(nightMin > 0)) { nightMin = parseInt(metadata.night_interval_min, 10); }
if (!(nightMin > 0)) { nightMin = 60; }
var NIGHT_INTERVAL_MS = nightMin * MINUTE_MS;

// Keep in sync with the "Calculate Battery Voltage" node of the Root Rule Chain.
var BATTERY_VCC_REF = 5.168;
var BATTERY_DIVIDER_RATIO = 1.996;
var SOC_EMPTY_V = 3.3;
var SOC_FULL_V = 4.2;

var arrival = parseInt(metadata.ts, 10);
if (!(arrival > 0)) { arrival = new Date().getTime(); }

function toInt(s) { return (s === '' || s === undefined) ? null : parseInt(s, 10); }
function toNum(s) { return (s === '' || s === undefined) ? null : parseFloat(s); }

var rows = String(msg.stored_csv).split(';');
var recs = [];
for (var i = 0; i < rows.length; i++) {
    var f = rows[i].split(',');
    if (f.length < 8) { continue; }                          // malformed row: skip it, keep the rest
    recs.push({
        seq: toInt(f[0]), age: toInt(f[1]), temp: toNum(f[2]), hum: toNum(f[3]),
        dust: toInt(f[4]), gas: toInt(f[5]), batt: toInt(f[6]), vmask: toInt(f[7]),
        ts: null, estimated: 0
    });
}

// 1) timestamps of records with a known age: the arrival time minus the age, rounded to the minute so that the same
//    record arriving twice (a re-send) lands on almost the same timestamp
var firstKnown = -1;
for (var k = 0; k < recs.length; k++) {
    if (recs[k].age !== null) {
        recs[k].ts = Math.round((arrival - recs[k].age * MINUTE_MS) / MINUTE_MS) * MINUTE_MS;
        if (firstKnown < 0) { firstKnown = k; }
    }
}
// 2) records with an unknown age (stored before a reset, the node's clock restarted): space them by the night interval,
//    backwards from the first record with a known age, or backwards from the arrival time if there is none. Flagged as estimated.
for (var j = 0; j < recs.length; j++) {
    if (recs[j].ts !== null) { continue; }
    recs[j].estimated = 1;
    if (firstKnown >= 0) {
        recs[j].ts = (j < firstKnown) ? recs[firstKnown].ts - (firstKnown - j) * NIGHT_INTERVAL_MS
                                      : Math.min(arrival, recs[firstKnown].ts + (j - firstKnown) * NIGHT_INTERVAL_MS);
    } else {
        recs[j].ts = arrival - (recs.length - j) * NIGHT_INTERVAL_MS;
    }
}

var out = [];
for (var m = 0; m < recs.length; m++) {
    var r = recs[m];
    var v = {};
    v.night_replayed = 1;                                    // lets queries and widgets tell replayed readings from live ones
    v.night_seq = r.seq;
    v.night_boot = msg.stored_boot_id;
    v.night_ts_estimated = r.estimated;
    if ((r.vmask & 1) && r.temp !== null) { v.temperature = r.temp; v.humidity = r.hum; }
    v.dht22_valid = !!(r.vmask & 1);
    v.dust_adc = r.dust;   v.dust_valid = !!(r.vmask & 2);
    v.gas_adc = r.gas;     v.gas_valid = !!(r.vmask & 4);
    v.battery_adc = r.batt; v.battery_valid = !!(r.vmask & 8);
    if (v.dust_valid && r.dust !== null) {
        v.dust_ugm3 = Math.round(Math.max(0, 170.0 * r.dust * (5.0 / 1023.0)) * 100) / 100;   // same formula as the AQI node
    }
    if (v.battery_valid && r.batt !== null) {
        var voltage = r.batt * (BATTERY_VCC_REF / 1023.0) * BATTERY_DIVIDER_RATIO;
        v.battery_voltage = Math.round(voltage * 100) / 100;
        var soc = (voltage - SOC_EMPTY_V) / (SOC_FULL_V - SOC_EMPTY_V) * 100.0;
        v.battery_soc_pct = Math.round(Math.max(0, Math.min(100, soc)));
    }
    out.push({ ts: r.ts, values: v });
}

return { msg: out, metadata: metadata, msgType: 'POST_TELEMETRY_REQUEST' };
