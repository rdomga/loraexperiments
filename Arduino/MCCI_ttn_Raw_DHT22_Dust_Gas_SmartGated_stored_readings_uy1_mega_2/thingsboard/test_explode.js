// Runs the filter and explode scripts exactly as they sit inside Stored_Readings_rule_chain.json, on a real port 3 frame
// (decoded by ../payload.txt and mapped the way tbConnector_additions.json maps it). Usage: node test_explode.js [frames.json]
const fs = require('fs'), path = require('path');
const H = __dirname;
const chain = JSON.parse(fs.readFileSync(path.join(H, 'Stored_Readings_rule_chain.json')));
const script = n => chain.metadata.nodes.find(x => x.name === n).configuration.jsScript;
const filter = new Function('msg', 'metadata', 'msgType', script('Is stored frame?'));
const explode = new Function('msg', 'metadata', 'msgType', script('Explode stored readings'));
eval(fs.readFileSync(path.join(H, '..', 'payload.txt'), 'utf8') + ';globalThis.D = Decoder;');

let fails = 0;
const check = (c, what) => { if (!c) { fails++; console.log('FAIL:', what); } };
// what the gateway connector produces for a port 3 uplink
const asMsg = (frame) => { const d = D(frame, 3);
  return { type: d.type, stored_csv: d.records_csv, stored_boot_id: d.boot_id, stored_first_seq: d.first_seq,
           stored_uptime_min: d.sender_uptime_min, stored_count: d.record_count, night_interval_min: d.night_interval_min,
           f_port: 3, f_cnt: 100, rssi: -90 }; };

const frames = JSON.parse(fs.readFileSync(process.argv[2] || path.join(H, 'fixture_p3_frames.json')));
const ARR = 1793579000000;   // arrival, ms
let msg = asMsg(frames[0]), meta = { ts: String(ARR) };
check(filter(msg, meta, 'POST_TELEMETRY_REQUEST') === true, 'filter accepts a stored frame');
const o = explode(msg, meta, 'POST_TELEMETRY_REQUEST');
check(Array.isArray(o.msg) && o.msg.length === 5, '5 entries'); check(o.msgType === 'POST_TELEMETRY_REQUEST', 'msgType');
const ages = [4, 3, 2, 2, 1];
o.msg.forEach((e, i) => {
  check(e.ts === Math.round((ARR - ages[i] * 60000) / 60000) * 60000, 'ts of record ' + i);
  check(e.values.night_replayed === 1 && e.values.night_seq === 5 + i, 'seq/flag of record ' + i);
  check(e.values.battery_voltage > 3 && e.values.battery_voltage < 4.5, 'battery_voltage computed');
  check(e.values.dust_ugm3 > 0, 'dust_ugm3 computed'); check(!('co_ppm' in e.values) && !('aqi' in e.values), 'no co_ppm / AQI in replayed values');
  check(e.values.temperature === undefined, 'no temperature when the DHT22 is invalid');
});
console.log('sample entry:', JSON.stringify(o.msg[0]));

// unknown ages (stored before a reset) mixed with known ones: placed backwards from the first known one, flagged
msg = { type: 'stored', stored_csv: '3,,25.0,60,200,300,360,15;4,,25.1,60,201,301,359,15;5,10,25.2,61,202,302,358,15;6,5,25.3,61,203,303,357,15', stored_boot_id: 2 };
const u = explode(msg, { ts: String(ARR) }, 'x').msg;
check(u[2].ts === Math.round((ARR - 10 * 60000) / 60000) * 60000, 'first known'); check(u[1].ts === u[2].ts - 3600000 && u[0].ts === u[2].ts - 2 * 3600000, 'unknown placed hourly backwards');
check(u[0].values.night_ts_estimated === 1 && u[2].values.night_ts_estimated === 0, 'estimated flag');
check(u[0].values.temperature === 25.0 && u[0].values.humidity === 60, 'dht22 values kept when valid');
// all unknown
const a = explode({ type: 'stored', stored_csv: '0,,,,10,20,360,14;1,,,,11,21,360,14', stored_boot_id: 1 }, { ts: String(ARR) }, 'x').msg;
check(a[1].ts === ARR - 3600000 && a[0].ts === ARR - 2 * 3600000, 'all unknown: backwards from arrival');
// malformed row skipped, the rest kept
const b = explode({ type: 'stored', stored_csv: '0,5,,,10,20,360,14;garbage;2,3,,,12,22,360,14', stored_boot_id: 1 }, { ts: String(ARR) }, 'x').msg;
check(b.length === 2, 'malformed row skipped');
// night interval source: v2 frame byte 7, then the latest status value in metadata, then 60 min
const mkV2 = (intervalMin, recs) => { const b = [2, 1, 0, 9, 0, 3, recs.length, intervalMin]; recs.forEach(r => { b.push(0x80, 0, 0xFF, 0, 10, 0, 20, 1, 104, r >> 8, r & 255); }); return b; };
const v2 = asMsg(mkV2(30, [0xFFFF, 0xFFFF, 6]));
check(v2.night_interval_min === 30 && v2.stored_count === 3, 'v2 frame carries the night interval');
const w = explode(v2, { ts: String(ARR) }, 'x').msg;
check(w[2].ts === Math.round((ARR - 6 * 60000) / 60000) * 60000, 'v2 known age');
check(w[1].ts === w[2].ts - 30 * 60000 && w[0].ts === w[2].ts - 60 * 60000, 'v2: unknown ages spaced by the frame interval (30 min), not 60');
const v1m = asMsg(frames[0]);   // old v1 frame: no interval in the frame
check(v1m.night_interval_min === undefined, 'v1 frame has no interval');
msg = { ...v1m, stored_csv: '3,,,,200,300,360,14;4,5,,,201,301,359,14' };
let q = explode(msg, { ts: String(ARR), night_interval_min: '20' }, 'x').msg;
check(q[0].ts === q[1].ts - 20 * 60000, 'v1: interval from the fetched status value (20 min)');
q = explode(msg, { ts: String(ARR) }, 'x').msg;
check(q[0].ts === q[1].ts - 60 * 60000, 'v1 without any interval: 60 min default');
q = explode({ ...msg, night_interval_min: 15 }, { ts: String(ARR), night_interval_min: '20' }, 'x').msg;
check(q[0].ts === q[1].ts - 15 * 60000, 'the frame value wins over the fetched one');
// the filter rejects normal frames
check(filter({ type: 'sensor', dust_adc: 1 }, {}, 'x') === false, 'filter rejects port 1');
check(filter({ type: 'stored', stored_csv: '' }, {}, 'x') === false, 'filter rejects empty csv');
console.log(fails ? fails + ' CHECK(S) FAILED' : 'all stored-readings script checks passed');
process.exit(fails ? 1 : 0);
