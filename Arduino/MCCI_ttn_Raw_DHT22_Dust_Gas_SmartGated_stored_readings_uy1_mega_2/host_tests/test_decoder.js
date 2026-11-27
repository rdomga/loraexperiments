// Evaluates ../payload.txt exactly as the TTN application formatter will, and
// checks the Round 8 §5.3 sentinel case plus the version matrix, Port 1 anchor
// fields and the Port 2 46/43-byte split. Usage: node test_decoder.js
const fs = require('fs'), path = require('path');
eval(fs.readFileSync(path.join(__dirname, '..', 'payload.txt'), 'utf8') + ';globalThis.D = Decoder;');

let fails = 0;
const check = (c, what) => { if (!c) { fails++; console.log('FAIL:', what); } };

// One 11-byte record: temp 0x00FA (25.0 C), hum 0xC8 (100%), dust 200,
// gas 500, batt 360, then the two-byte record time field.
function storedFrame(ver, timeHi, timeLo) {
  return [ver, 0x05, 0x13, 0x88, 0x00, 0x00, 0x01, 0x1E,
          0x00, 0xFA, 0xC8, 0x00, 0xC8, 0x01, 0xF4, 0x01, 0x68,
          timeHi, timeLo];
}

// --- Round 8 §5.3: 0xFF10 sentinel and 0x1234 control -------------------
let f = storedFrame(0x04, 0xFF, 0x10);
let d = D(f, 3);
check(d.valid_frame === true, 'sentinel: valid_frame');
check(d.records.length === 1, 'sentinel: one record');
check(d.records[0].rec_t_min === null, 'sentinel: rec_t_min is null');
check(d.records[0].age_min === null, 'sentinel: age_min is null');
let csv = d.records_csv.split(',');
check(csv.length === 9, 'sentinel: nine CSV fields, got ' + csv.length);
check(csv[8] === '', 'sentinel: ninth CSV field empty');
console.log('sentinel csv: ' + d.records_csv);

f = storedFrame(0x04, 0x12, 0x34);
d = D(f, 3);
check(d.records[0].rec_t_min === 4660, 'control: rec_t_min 4660');
check(d.records[0].age_min === 340, 'control: age_min 340');
console.log('control csv: ' + d.records_csv);

// --- Version matrix ------------------------------------------------------
const matrix = [
  [0x04, 'energy_sag', 4660, 340],
  [0x05, 'outage',     4660, 340],
  [0x06, 'energy_sag', 4660, null],
  [0x07, 'outage',     4660, null]
];
matrix.forEach(function (m) {
  const r = D(storedFrame(m[0], 0x12, 0x34), 3);
  check(r.valid_frame === true, 'v0x0' + m[0] + ': valid');
  check(r.stored_regime === m[1], 'v0x0' + m[0] + ': regime');
  check(r.stored_interval_min === 30, 'v0x0' + m[0] + ': interval');
  check(r.sag_interval_min === undefined, 'v0x0' + m[0] + ': no sag_interval_min');
  check(r.records[0].rec_t_min === m[2], 'v0x0' + m[0] + ': rec_t_min');
  check(r.records[0].age_min === m[3], 'v0x0' + m[0] + ': age_min');
  if (m[0] === 0x06 || m[0] === 0x07) {
    check(r.record_boot_id === 0x05, 'v0x0' + m[0] + ': record_boot_id');
  } else {
    check(r.record_boot_id === undefined, 'v0x0' + m[0] + ': no record_boot_id');
  }
});

// Legacy versions 0x01..0x03 and unknown versions are rejected.
check(D(storedFrame(0x01, 0x00, 0x08), 3).valid_frame === false, 'legacy v0x01 rejected');
check(D(storedFrame(0x02, 0x00, 0x08), 3).valid_frame === false, 'legacy v0x02 rejected');
check(D(storedFrame(0x03, 0x00, 0x08), 3).valid_frame === false, 'legacy v0x03 rejected');

// Guards
check(D([0x04, 0x05, 0x13, 0x88, 0, 0, 0x00, 0x1E], 3).valid_frame === false, 'n == 0 rejected');
check(D([0x04, 0x05, 0x13, 0x88, 0, 0, 0x01, 0x1E, 1, 2], 3).valid_frame === false, 'truncated record rejected');
check(D([0x08, 0x05, 0x13, 0x88, 0, 0, 0x01, 0x1E], 3).valid_frame === false, 'unknown version rejected');

// --- Port 1: 14-byte frame only ------------------------------------------
const p1 = [0x00, 0xFA, 0xC8, 0x00, 0xC8, 0x01, 0xF4, 0x01, 0x68,
            0x07, 0x00, 0x2A, 0x00, 0x35];
let o = D(p1, 1);
check(o.p1_boot_id === 7, 'port1: p1_boot_id');
check(o.reading_seq === 42, 'port1: reading_seq');
check(o.p1_uptime_min === 53, 'port1: p1_uptime_min');
check(o.temperature === 25, 'port1: temperature');
check(D(p1.slice(0, 9), 1).valid_frame === false, 'port1: 9-byte legacy frame rejected');

// --- Port 2: 46-byte frame only ------------------------------------------
function status(n) {
  const b = new Array(n).fill(0);
  b[0] = 0x00; b[1] = 90;          // interval
  b[8] = 0xC8;                      // humidity raw
  b[13] = 0x01; b[14] = 0x68;       // battery raw
  b[16] = 0x00;
  b[33] = 9;                        // buffer_unsent
  b[34] = 0x07;                     // node_boot_id
  b[42] = 0x00; b[43] = 0x35;       // uptime_min 53
  b[44] = 0x00; b[45] = 0x2A;       // anchor_seq 42
  return b;
}
o = D(status(46), 2);
check(o.uptime_min === 53 && o.anchor_seq === 42, 'port2 46: anchor fields');
check(o.buffer_unsent === 9 && o.outage_unsent === undefined, 'port2 46: buffer_unsent only');
check(D(new Array(43).fill(0), 2).valid_frame === false, 'port2: 43-byte legacy frame rejected');
check(o.node_boot_id === 7, 'port2: node_boot_id');

console.log(fails ? '\n' + fails + ' CHECK(S) FAILED' : '\nall payload decoder checks passed');
process.exit(fails ? 1 : 0);
