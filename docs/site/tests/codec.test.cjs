// 零依赖测试：与浏览器共用普通脚本，以固定外部向量核验 CRC。
'use strict';
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const context = vm.createContext({});
vm.runInContext(['protocol.js', 'codec.js'].map((name) => fs.readFileSync(path.join(root, 'js', name), 'utf8')).join('\n'), context);
const api = vm.runInContext('({ crc16, hexToBytes, bytesToHex, decodeRf, decodeHost, makeHostFrame, makeExampleData, makeExampleAck, wrapHostRfRx })', context);
let passed = 0;
function test(name, fn) {
  fn();
  passed++;
  console.log(`✓ ${name}`);
}
const bytes = (s) => api.hexToBytes(s);
const array = (b) => Array.from(b);
const host = (type, payload = []) => api.makeHostFrame(type, 1, Uint8Array.from(payload));

test('CRC 标准 check 与空输入', () => {
  assert.equal(api.crc16(Buffer.from('123456789')), 0x29b1);
  assert.equal(api.crc16([]), 0xffff);
});
test('独立主机向量 GET_INFO 与 TIME_REQ', () => {
  // 固定值用 Python binascii.crc_hqx(data, 0xFFFF) 独立计算。
  for (const [s, crc] of [['A5 5A 83 00 00 00 24 C2', 0xc224], ['A5 5A 81 01 04 00 01 00 00 00 18 43', 0x4318]]) {
    const result = api.decodeHost(bytes(s));
    assert.equal(result.crcCalc, crc);
    assert.equal(result.crcStored, crc);
  }
  assert.equal(api.decodeHost(bytes('A5 5A 81 01 04 00 01 00 00 00 18 43')).fields[0][1], 1);
});
test('CRC 覆盖 frame_seq 和 len，不含帧头；CRC 小端', () => {
  const f = host(0x81, [1, 0, 0, 0]);
  assert.equal(api.bytesToHex(f), 'a5 5a 81 01 04 00 01 00 00 00 18 43');
  f[3] ^= 1;
  const h = api.decodeHost(f);
  assert.notEqual(h.crcCalc, h.crcStored);
  assert.equal(h.fields, undefined);
  assert.throws(() => api.decodeHost(bytes('00 5A 83 00 00 00 24 C2')), /不是主机帧/);
});
test('HEX 接受前缀、连续字节和分隔符', () => {
  assert.deepEqual(array(bytes('0xA5, 0x5a:83-00\n00 00 24 C2')), array(bytes('A55a8300000024c2')));
});
test('拒绝空输入、奇数字符与混入日志文字', () => {
  assert.throws(() => bytes(' '), /没有输入/);
  assert.throws(() => bytes('A5 5'), /奇数/);
  assert.throws(() => bytes('RX: A5 5A'), /非十六进制/);
  assert.throws(() => bytes('<script>'), /非十六进制/);
});
test('主机帧长度：截断、尾随、拼接均不静默接受', () => {
  const good = bytes('A5 5A 83 00 00 00 24 C2');
  assert.throws(() => api.decodeHost(good.slice(0, 7)), /不是主机帧/);
  assert.throws(() => api.decodeHost(bytes('A5 5A 81 00 04 00 00 00')), /长度不符/);
  assert.throws(() => api.decodeHost(bytes(api.bytesToHex(good) + ' 00')), /只输入一帧/);
  assert.throws(() => api.decodeHost(bytes(api.bytesToHex(good) + ' ' + api.bytesToHex(good))), /只输入一帧/);
});
test('DATA 全字段小端与 ±16 g 样本转换', () => {
  const r = api.decodeRf(api.makeExampleData());
  assert.equal(r.len, 246);
  assert.equal(r.samples.length, 35);
  assert.equal(r.rows.find((r) => r.f.n === 'seq').v, 1024);
  assert.equal(r.rows.find((r) => r.f.n === 'cfg').text.includes('RTC'), true);
  assert.equal(r.samples[5].raw[2], 2052);
  assert.equal(r.samples[5].g[2], '1.002');
  assert.equal(r.samples[20].raw[2], 32046);
});
test('DATA 声明样本数与实际长度必须严格一致', () => {
  const b = api.makeExampleData();
  assert.throws(() => api.decodeRf(b.slice(0, 35)), /长度不足/);
  assert.throws(() => api.decodeRf(b.slice(0, -1)), /长度不符/);
  b[18] = 36;
  assert.throws(() => api.decodeRf(b), /n_samples/);
  const empty = b.slice(0, 36);
  empty[18] = 0;
  assert.throws(() => api.decodeRf(empty), /n_samples/);
  assert.match(api.decodeHost(api.wrapHostRfRx(empty, 1, 0, -50)).payloadError, /n_samples/);
});
test('不同量程与微秒时间单位', () => {
  const b = api.makeExampleData();
  b[3] = 0x4d; // ±2 g，µs，3.2 kHz。
  const r = api.decodeRf(b);
  assert.equal(r.samples[5].g[2], '0.125');
  assert.match(r.rows.find((r) => r.f.n === 'anchor_us').text, /µs/);
});
test('STATUS / POLL 固定长度，温度与有符号 RSSI', () => {
  const b = new Uint8Array(63);
  const dv = new DataView(b.buffer);
  b[0] = 0x12;
  b[3] = 0xfd;
  dv.setInt16(20, -512, true);
  dv.setInt8(52, -80);
  const r = api.decodeRf(b);
  assert.equal(r.rows.find((x) => x.f.n === 'bmi_temp_raw').text, '22.00 °C');
  assert.equal(r.rows.find((x) => x.f.n === 'ack_rssi_avg').v, -80);
  b[2] = 0x80;
  dv.setInt16(20, -32768, true);
  const paused = api.decodeRf(b);
  assert.equal(paused.rows.find((x) => x.f.n === 'flags').text, 'PAUSED');
  assert.equal(paused.rows.find((x) => x.f.n === 'bmi_temp_raw').text, '无效（传感器休眠）');
  assert.throws(() => api.decodeRf(b.slice(0, 62)), /长度不足/);
  const poll = new Uint8Array(20); poll[0] = 0x15;
  assert.equal(api.decodeRf(poll).name, 'POLL');
  assert.throws(() => api.decodeRf(poll.slice(0, 19)), /长度不足/);
});
test('ACK 确认表、条数和命令长度', () => {
  const b = api.makeExampleAck();
  const r = api.decodeRf(b);
  assert.equal(r.conf.length, 2);
  assert.equal(r.conf[0].seqLo, 1024);
  assert.throws(() => api.decodeRf(b.slice(0, -1)), /长度不符/);
  b[5] = 9;
  assert.throws(() => api.decodeRf(b), /超过 8/);
  b[5] = 0; b[14] = 9;
  assert.throws(() => api.decodeRf(b), /超过 8/);
});
test('空无线负载、未知版本、未知类型与模式 B 有明确提示', () => {
  assert.throws(() => api.decodeRf(new Uint8Array()), /为空/);
  assert.throws(() => api.decodeRf(bytes('21')), /协议版本 2/);
  assert.throws(() => api.decodeRf(bytes('1F')), /未知包类型/);
  assert.throws(() => api.decodeRf(bytes('13')), /模式 B/);
});
test('RF_RX 主机封装与嵌套 DATA', () => {
  const f = api.wrapHostRfRx(api.makeExampleData(), 4, 987654321, -67);
  const h = api.decodeHost(f);
  assert.equal(f.length, 261);
  assert.equal(h.len, 253);
  assert.equal(h.crcStored, h.crcCalc);
  assert.equal(h.rx.gw, 4);
  assert.equal(h.rx.rxUs, 987654321);
  assert.equal(h.rx.rssi, -67);
  assert.equal(h.rf.samples.length, 35);
});
test('CRC 损坏停止解码，不把负载当作有效数据', () => {
  const f = api.wrapHostRfRx(api.makeExampleData(), 1, 50, -42);
  f[10] ^= 1;
  const h = api.decodeHost(f);
  assert.notEqual(h.crcCalc, h.crcStored);
  assert.equal(h.rf, undefined);
  assert.equal(h.rx, undefined);
});
test('主机 CRC 正确但内层 rf_len 或 RF 数据损坏', () => {
  assert.match(api.decodeHost(host(1, [1, 0, 0, 0, 0, 0, 20])).payloadError, /rf_len/);
  assert.match(api.decodeHost(host(1, [1, 0, 0, 0, 0, 0, 1, 0x11])).payloadError, /DATA 长度不足/);
});
test('解析主机 TIME_RESP / GW_STATUS / CMD_RESP', () => {
  const time = api.decodeHost(host(2, [1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0]));
  assert.deepEqual(Array.from(time.fields, (row) => row[1]), [1, 2, 3]);
  assert.equal(api.decodeHost(host(3, new Uint8Array(28))).fields.length, 7);
  assert.equal(api.decodeHost(host(4, [0x82, 0])).fields[0][1], '0x82');
  assert.match(api.decodeHost(host(2, [])).payloadError, /应为 12/);
  assert.match(api.decodeHost(host(0x83, [0])).payloadError, /应为 0/);
});
test('QUEUE_DOWNLINK 解析与命令区长度校验', () => {
  const h = api.decodeHost(host(0x82, [1, 2, 3, 1, 0xff]));
  assert.equal(h.fields[0][1], 1);
  assert.equal(h.fields[4][1], 'ff');
  assert.match(api.decodeHost(host(0x82, [1, 2, 3, 2, 0xff])).payloadError, /QUEUE_DOWNLINK/);
});
test('HELLO 固定布局、小端频点与长度校验', () => {
  const payload = new Uint8Array(8);
  const dv = new DataView(payload.buffer);
  payload.set([4, 1, 0]);
  dv.setUint32(3, 2450000, true);
  payload[7] = 1;
  const h = api.decodeHost(host(5, payload));
  assert.deepEqual(Array.from(h.fields, (row) => row[1]), [4, 1, 0, 2450000, 1]);
  assert.match(api.decodeHost(host(5, [4, 1, 0])).payloadError, /应为 8/);
});
test('未明确布局或未知主机类型保留原始负载', () => {
  for (const t of [0x84, 0x99]) {
    const h = api.decodeHost(host(t, [1, 2, 3]));
    assert.deepEqual(array(h.payload), [1, 2, 3]);
    assert.equal(h.fields, undefined);
  }
});
test('文档中的整帧向量均可通过 CRC', () => {
  const doc = fs.readFileSync(path.join(root, '..', '03_网关与上位机.md'), 'utf8');
  const vectors = Array.from(doc.matchAll(/`(A5 5A (?:[0-9A-F]{2} ?)+)`/g), (m) => m[1]);
  assert.equal(vectors.length, 2);
  vectors.forEach((s) => { const h = api.decodeHost(bytes(s)); assert.equal(h.crcCalc, h.crcStored); });
});
console.log(`\n${passed} 项测试通过。`);
