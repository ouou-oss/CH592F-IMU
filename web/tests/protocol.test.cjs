// 与浏览器使用同一份模块，固定外部向量与明确数值验证协议含义。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const P = require('../js/protocol.js');
const bytes = (hex) => Uint8Array.from(hex.match(/[a-f\d]{2}/gi).map((x) => parseInt(x, 16)));
const concat = (...arrays) => Uint8Array.from(arrays.flatMap((array) => Array.from(array)));

function rfBytes(kind = 'data', options = {}) {
  const types = { data: 1, status: 2, poll: 5 };
  const n = options.n === undefined ? 3 : options.n;
  const data = new Uint8Array(kind === 'data' ? 36 + 6 * n : kind === 'status' ? 63 : 20);
  const view = new DataView(data.buffer);
  data[0] = 0x10 | types[kind];
  data[1] = options.nodeId || 1;
  data[2] = options.flags || 0;
  data[3] = options.cfg === undefined ? 0xbd : options.cfg;
  view.setUint16(4, options.bootId || 6, true);
  view.setUint32(6, options.seq || 0, true);
  view.setUint32(10, options.oldestSeq || 0, true);
  if (kind === 'data') {
    view.setUint32(14, options.first || 0, true);
    data[18] = n;
    data[19] = options.segment || 0;
    data[20] = options.attempt || 1;
    data[21] = 9;
    view.setUint32(22, options.anchorIdx === undefined ? 0xfedcba98 : options.anchorIdx, true);
    view.setUint32(26, options.anchorUs === undefined ? 0x11223344 : options.anchorUs, true);
    view.setUint32(30, 0x55667788, true);
    data[34] = 254;
    data[35] = 2;
    for (let i = 0; i < n; i++) {
      view.setInt16(36 + i * 6, i + 1, true);
      view.setInt16(38 + i * 6, -32768 + i, true);
      view.setInt16(40 + i * 6, 2048 + i, true);
    }
  } else if (kind === 'status') {
    view.setUint32(14, options.sampleIdxNow || 12345, true);
    view.setUint16(18, 3700, true);
    view.setInt16(20, -512, true);
    for (const [offset, name, fallback] of [[22, 'txOk', 100], [26, 'txRetry', 12], [30, 'txBackoff', 8], [34, 'ringDropPkts', 4], [38, 'ringDropSamples', 140], [42, 'fifoGapSamples', 8192]]) {
      view.setUint32(offset, options[name] === undefined ? fallback : options[name], true);
    }
    view.setUint16(46, options.fifoFullCnt || 2, true);
    data[48] = 22;
    data[49] = 10;
    view.setUint16(50, 0x42, true);
    view.setInt8(52, -77);
    data[53] = 1;
    data[54] = 1;
    data[55] = 3;
    data[56] = 1;
    data[57] = 0;
    data[58] = 1;
    view.setUint16(59, 4800, true);
    view.setUint16(61, 7, true);
  } else {
    view.setUint32(14, 0xdeadbeef, true);
    data[18] = 255;
    data[19] = 3;
  }
  return data;
}

function packet(kind = 'data', options = {}) {
  return Object.assign(P.decodeRf(rfBytes(kind, options)), { gatewayId: 1, rxUs: 123, rssi: -53 });
}

function timedPacket(options = {}) {
  const n = options.n === undefined ? 3 : options.n;
  const first = options.first || 0;
  return packet('data', Object.assign({ anchorIdx: (first + n - 1) >>> 0 }, options));
}

function close(actual, expected, message) {
  assert.ok(Math.abs(actual - expected) < 1e-9, `${message || ''}: ${actual} != ${expected}`);
}

function rfRxPayload(rf) {
  const payload = new Uint8Array(7 + rf.length);
  const view = new DataView(payload.buffer);
  payload[0] = 2;
  view.setUint32(1, 0x87654321, true);
  view.setInt8(5, -93);
  payload[6] = rf.length;
  payload.set(rf, 7);
  return payload;
}

test('普通浏览器脚本暴露 BmiProtocol，无 CommonJS 依赖', () => {
  const context = vm.createContext({});
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../js/protocol.js'), 'utf8'), context);
  assert.equal(typeof context.BmiProtocol.StreamParser, 'function');
});

test('CRC-16/CCITT-FALSE 标准 check、空值与独立请求向量', () => {
  assert.equal(P.crc16(Buffer.from('123456789')), 0x29b1);
  assert.equal(P.crc16([]), 0xffff);
  assert.deepEqual(P.makeHostFrame(0x83, 0), bytes('a5 5a 83 00 00 00 24 c2'));
  assert.deepEqual(P.makeHostFrame(0x81, 1, Uint8Array.of(1, 0, 0, 0)), bytes('a5 5a 81 01 04 00 01 00 00 00 18 43'));
  assert.throws(() => P.makeHostFrame(1, 0, new Uint8Array(513)), /512/);
});

test('STREAM_CTRL 独立下行向量包含目标节点，CMD_RESP 仅表示网关排队结果', () => {
  assert.equal(P.HOST.QUEUE_DOWNLINK, 0x82);
  for (const [value, hex] of [[0, 'a5 5a 82 04 05 00 07 19 02 01 00 38 c5'], [1, 'a5 5a 82 04 05 00 07 19 02 01 01 19 d5']]) {
    assert.deepEqual(P.makeHostFrame(P.HOST.QUEUE_DOWNLINK, 4, Uint8Array.of(7, 25, 2, 1, value)), bytes(hex));
  }
  const response = P.decodeFrame({ type: P.HOST.CMD_RESP, seq: 9, payload: Uint8Array.of(0x82, 0) });
  assert.deepEqual(response, { type: P.HOST.CMD_RESP, seq: 9, kind: 'cmd_resp', reqType: 0x82, result: 0 });
});

test('逐字节分片、跨帧头的分片和粘包都恢复同样帧', () => {
  const frames = [];
  const parser = new P.StreamParser({ onFrame: (frame) => frames.push(frame) });
  const frame = bytes('a5 5a 81 01 04 00 01 00 00 00 18 43');
  for (const byte of frame) parser.push(Uint8Array.of(byte));
  parser.push(concat(frame, frame));
  assert.equal(frames.length, 3);
  assert.equal(frames[0].type, 0x81);
  assert.equal(frames[0].seq, 1);
  assert.deepEqual(frames[0].payload, Uint8Array.of(1, 0, 0, 0));
  assert.equal(parser.stats.frames, 3);
  assert.equal(parser.used, 0);
});

test('坏 CRC 后重新同步；A5 A5 5A 的重叠帧头不丢好帧', () => {
  const frames = [];
  const parser = new P.StreamParser((frame) => frames.push(frame));
  const good = bytes('a5 5a 83 00 00 00 24 c2');
  const bad = good.slice();
  bad[3] = 1;
  parser.push(concat(Uint8Array.of(0, 0xa5), bad, good));
  assert.equal(frames.length, 1);
  assert.equal(parser.stats.crcErrors, 1);
  assert.equal(parser.stats.discardedBytes, bad.length + 2);
});

test('非法长度立即重同步，截断帧暂存到补齐，不提前解码', () => {
  const frames = [];
  const parser = new P.StreamParser((frame) => frames.push(frame));
  const good = bytes('a5 5a 83 00 00 00 24 c2');
  parser.push(bytes('a5 5a 01 00 ff ff'));
  parser.push(good.subarray(0, 7));
  assert.equal(frames.length, 0);
  assert.equal(parser.stats.invalidLengths, 1);
  parser.push(good.subarray(7));
  assert.equal(frames.length, 1);
  parser.reset();
  assert.equal(parser.used, 0);
  assert.equal(parser.stats.frames, 0);
});

test('受损的合理长度帧在最多 520 字节后重新找到嵌套有效帧', () => {
  const frames = [];
  const parser = new P.StreamParser((frame) => frames.push(frame));
  parser.push(concat(bytes('a5 5a 01 00 00 02'), bytes('a5 5a 83 00 00 00 24 c2'), new Uint8Array(506)));
  assert.equal(frames.length, 1);
  assert.equal(parser.stats.crcErrors, 1);
  assert.equal(parser.used, 0);
});

test('主机有效帧 payload 独立拷贝，不被后续输入覆盖', () => {
  const frames = [];
  const parser = new P.StreamParser((frame) => frames.push(frame));
  parser.push(P.makeHostFrame(0x81, 4, Uint8Array.of(10, 20, 30, 40)));
  parser.push(P.makeHostFrame(0x81, 5, Uint8Array.of(1, 2, 3, 4)));
  assert.deepEqual(frames[0].payload, Uint8Array.of(10, 20, 30, 40));
});

test('DATA 偏移、小端、有符号三轴与不舍入的 g 转换', () => {
  const data = packet('data', { seq: 0x12345678, first: 0xabcdef01, segment: 128, attempt: 7, oldestSeq: 0x11223344 });
  assert.equal(data.seq, 0x12345678);
  assert.equal(data.oldestSeq, 0x11223344);
  assert.equal(data.firstSampleIdx, 0xabcdef01);
  assert.equal(data.anchorIdx, 0xfedcba98);
  assert.equal(data.anchorUs, 0x11223344);
  assert.equal(data.syncTxdoneUs, 0x55667788);
  assert.equal(data.syncSeqLo, 254);
  assert.equal(data.syncAttempt, 2);
  assert.equal(data.attempt, 7);
  assert.equal(data.cmdAck, 9);
  assert.equal(data.segmentId, 128);
  assert.equal(data.timeUnit, 'rtc32768');
  assert.deepEqual(data.samples[0], { x: 1, y: -32768, z: 2048 });
  const store = new P.SessionStore();
  const session = store.ingest(data).session;
  assert.equal(session.getSamples()[0].gx, 1 / 2048);
  assert.equal(session.getSamples()[0].gz, 1);
});

test('RF_RX 元数据符号、完整解码与非法长度拒绝', () => {
  const payload = rfRxPayload(rfBytes('data'));
  const direct = P.decodeRfRx(payload);
  assert.equal(direct.gatewayId, 2);
  assert.equal(direct.rxUs, 0x87654321);
  assert.equal(direct.rssi, -93);
  const wrapped = P.decodeFrame({ type: 1, seq: 99, payload });
  assert.equal(wrapped.kind, 'rf_rx');
  assert.equal(wrapped.rf.kind, 'data');
  assert.equal(wrapped.rf.nodeId, 1);
  assert.throws(() => P.decodeRfRx(payload.slice(0, -1)), /长度/);
  assert.throws(() => P.decodeRfRx(new Uint8Array(6)), /长度/);
});

test('STATUS 所有健康计数、温度及固件版本准确', () => {
  const status = packet('status');
  for (const [name, expected] of Object.entries({ sampleIdxNow: 12345, battMv: 3700, bmiTempRaw: -512, temperatureC: 22, txOk: 100, txRetry: 12, txBackoff: 8, ringDropPkts: 4, ringDropSamples: 140, fifoGapSamples: 8192, fifoFullCnt: 2, ringMaxUsed: 22, lastCmdId: 10, bmiErrReg: 0x42, ackRssiAvg: -77, fwMajor: 1, fwMinor: 1, bmiReinitCnt: 3, currentGatewayId: 1, currentChannelIndex: 0, mode: 1, solarMv: 4800, roamCnt: 7 })) assert.equal(status[name], expected, name);
  const poll = packet('poll');
  assert.equal(poll.syncTxdoneUs, 0xdeadbeef);
  assert.equal(poll.syncSeqLo, 255);
  assert.equal(poll.syncAttempt, 3);
});

test('STATUS 区分自动休眠、远程停采和恢复采集', () => {
  const rf = rfBytes('status');
  rf[2] |= 0x80;
  rf[54] = 6;
  rf[58] = 2;
  new DataView(rf.buffer, rf.byteOffset).setInt16(20, -32768, true);
  assert.equal(P.decodeRf(rf).motionSleeping, true);
  assert.equal(P.decodeRf(rf).temperatureC, null);
  rf[58] = 1;
  assert.equal(P.decodeRf(rf).motionSleeping, false);
  rf[2] &= ~0x80;
  assert.equal(P.decodeRf(rf).motionSleeping, false);
});

test('严格拒绝未知版本/类型、节点编号、样本数、ODR 与损坏长度', () => {
  for (const [offset, value] of [[0, 0x21], [0, 0x13], [1, 0], [1, 255], [18, 0], [18, 36], [20, 0], [3, 0xb1]]) {
    const rf = rfBytes('data');
    rf[offset] = value;
    assert.throws(() => P.decodeRf(rf));
  }
  assert.throws(() => P.decodeRf(rfBytes('data').slice(0, -1)), /长度/);
  assert.throws(() => P.decodeRf(rfBytes('status').slice(0, 62)), /长度/);
  assert.throws(() => P.decodeRf(rfBytes('poll').slice(0, 19)), /长度/);
});

test('HELLO / TIME_RESP / GW_STATUS / CMD_RESP 定长、小端与语义', () => {
  const hello = P.decodeFrame({ type: 5, seq: 1, payload: bytes('01 01 02 50 62 25 00 01') });
  assert.deepEqual(hello, { type: 5, seq: 1, kind: 'hello', gatewayId: 1, fwMajor: 1, fwMinor: 2, frequencyKhz: 2450000, phy: 1 });
  const response = P.decodeFrame({ type: 2, seq: 2, payload: bytes('01 00 00 00 78 56 34 12 f0 de bc 9a') });
  assert.equal(response.reqId, 1);
  assert.equal(response.tReqUs, 0x12345678);
  assert.equal(response.tRespUs, 0x9abcdef0);
  const payload = new Uint8Array(28);
  const view = new DataView(payload.buffer);
  for (let i = 0; i < 7; i++) view.setUint32(i * 4, i + 100, true);
  const status = P.decodeFrame({ type: 3, seq: 3, payload });
  assert.equal(status.rxOk, 100);
  assert.equal(status.rearmLatMaxUs, 105);
  assert.equal(status.tcpReconnect, 106);
  assert.equal(P.decodeFrame({ type: 4, seq: 4, payload: Uint8Array.of(0x83, 2) }).result, 2);
  for (const [type, length] of [[2, 12], [3, 28], [4, 2], [5, 8]]) assert.throws(() => P.decodeFrame({ type, seq: 0, payload: new Uint8Array(length - 1) }), /长度/);
});

test('node+boot+seq 去重，POLL 不占序号且不会吞掉后续 DATA', () => {
  const store = new P.SessionStore();
  store.ingest(packet('poll', { seq: 5 }));
  store.ingest(packet('poll', { seq: 5 }));
  const first = store.ingest(packet('data', { seq: 5 }));
  const duplicate = store.ingest(packet('data', { seq: 5, attempt: 2 }));
  assert.equal(first.added, 3);
  assert.equal(duplicate.added, 0);
  assert.equal(duplicate.duplicate, true);
  assert.equal(first.session.receivedPackets, 1);
  assert.equal(first.session.duplicatePackets, 1);
  store.ingest(packet('data', { seq: 5, bootId: 7 }));
  store.ingest(packet('data', { seq: 5, nodeId: 2 }));
  assert.equal(store.sessions.size, 3);
  assert.equal(first.session.key, '1:6');
});

test('乱序唯一包补齐缺口，样本按采样编号排序，连续性恢复', () => {
  const store = new P.SessionStore();
  const session = store.ingest(packet('data', { seq: 1, first: 100 })).session;
  store.ingest(packet('data', { seq: 3, first: 106 }));
  assert.equal(session.missingSamples, 3);
  assert.equal(session.getContinuousSamples().length, 3);
  const late = store.ingest(packet('data', { seq: 2, first: 103 }));
  assert.equal(late.outOfOrder, true);
  assert.equal(session.missingSamples, 0);
  assert.deepEqual(session.getSamples().map((x) => x.index), [100, 101, 102, 103, 104, 105, 106, 107, 108]);
  assert.equal(session.getContinuousSamples().length, 9);
  assert.equal(session.receivedSamples, 9);
  assert.equal(session.getSamples().at(-1).relativeTime, 8 / 3200);
});

test('样本和 seq u32 回绕仍单调，迟到回绕前样本正确排序', () => {
  const store = new P.SessionStore();
  const session = store.ingest(packet('data', { seq: 0xfffffffe, first: 0xfffffffd, n: 3 })).session;
  store.ingest(packet('data', { seq: 0, first: 3, n: 3 }));
  assert.equal(session.missingSamples, 3);
  store.ingest(packet('data', { seq: 0xffffffff, first: 0, n: 3 }));
  const samples = session.getSamples();
  assert.equal(samples[3].index, 0x100000000);
  assert.equal(samples[3].rawIndex, 0);
  assert.equal(samples.at(-1).index, 0x100000005);
  assert.equal(session.missingSamples, 0);
  assert.equal(session.getContinuousSamples().length, 9);
  assert.equal(store.ingest(packet('data', { seq: 0xffffffff, first: 0 })).duplicate, true);
});

test('segment 0→128 前向展开为128，255→0 展开为256', () => {
  const store = new P.SessionStore();
  const session = store.ingest(packet('data', { seq: 1, first: 0, segment: 0 })).session;
  store.ingest(packet('data', { seq: 2, first: 3, segment: 128 }));
  store.ingest(packet('data', { seq: 3, first: 6, segment: 255 }));
  store.ingest(packet('data', { seq: 4, first: 9, segment: 0 }));
  assert.deepEqual(session.getSamples().filter((_, i) => i % 3 === 0).map((x) => x.segment), [0, 128, 255, 256]);
  assert.equal(session.segments, 4);
  assert.equal(session.getContinuousSamples().length, 3);
});

test('首包位于回绕之后时，迟到包补充非负 sample/segment epoch，不改变相对时间原点', () => {
  const store = new P.SessionStore();
  const session = store.ingest(packet('data', { seq: 3, first: 0, segment: 0 })).session;
  store.ingest(packet('data', { seq: 2, first: 0xfffffffd, segment: 128 }));
  const samples = session.getSamples();
  assert.equal(samples[0].index, 0xfffffffd);
  assert.equal(samples[3].index, 0x100000000);
  assert.equal(samples[0].segment, 128);
  assert.equal(samples[3].segment, 256);
  assert.equal(samples[3].relativeTime, 0);
  assert.equal(session.missingSamples, 0);
  assert.equal(session.segments, 2);
});

test('迟到段号按 seq 插入，揭示一次额外的 u8 段号回绕', () => {
  const store = new P.SessionStore();
  const session = store.ingest(packet('data', { seq: 1, first: 0, segment: 0 })).session;
  store.ingest(packet('data', { seq: 3, first: 6, segment: 0 }));
  store.ingest(packet('data', { seq: 2, first: 3, segment: 128 }));
  assert.deepEqual(session.getSamples().filter((_, i) => i % 3 === 0).map((x) => x.segment), [0, 128, 256]);
  assert.equal(session.segments, 3);
});

test('FFT 数据不能跨缺口、配置变更、段变化或显式 GAP', () => {
  for (const options of [{ first: 4 }, { cfg: 0xbc }, { segment: 1 }, { flags: 1 }, { flags: 8 }, { flags: 64 }]) {
    const store = new P.SessionStore();
    const session = store.ingest(packet('data', { seq: 1 })).session;
    store.ingest(packet('data', Object.assign({ seq: 2, first: 3 }, options)));
    assert.equal(session.getContinuousSamples().length, 3, JSON.stringify(options));
  }
});

test('STATUS 启动前累计丢样只作基线，本次增量独立且能处理 u32 回绕', () => {
  const store = new P.SessionStore();
  const session = store.ingest(packet('status', { seq: 10, txOk: 0xfffffffe })).session;
  assert.equal(session.statusDelta.ringDropSamples, 0);
  assert.equal(session.statusDelta.fifoGapSamples, 0);
  assert.equal(session.missingSamples, 0);
  store.ingest(packet('status', { seq: 12, txOk: 3, ringDropSamples: 175, fifoGapSamples: 8200 }));
  assert.equal(session.statusDelta.txOk, 5);
  assert.equal(session.statusDelta.ringDropSamples, 35);
  assert.equal(session.statusDelta.fifoGapSamples, 8);
  store.ingest(packet('status', { seq: 11, ringDropSamples: 150 }));
  assert.equal(session.latestStatus.seq, 12);
  assert.equal(session.statusDelta.ringDropSamples, 35);
});

test('有界缓存精确截断包中部，累计样本不减少，旧副本不重复计入', () => {
  const store = new P.SessionStore({ capacity: 5 });
  const session = store.ingest(packet('data', { seq: 1, first: 0 })).session;
  store.ingest(packet('data', { seq: 2, first: 3 }));
  assert.equal(session.sampleCount, 5);
  assert.equal(session.receivedSamples, 6);
  assert.deepEqual(session.getSamples().map((x) => x.index), [1, 2, 3, 4, 5]);
  assert.equal(session.getSamples()[0].relativeTime, 1 / 3200);
  assert.equal(session.getSamples(2).length, 2);
  assert.equal(session.getSamples(0).length, 0);
  store.ingest(packet('data', { seq: 3, first: 6 }));
  assert.equal(store.ingest(packet('data', { seq: 1, first: 0 })).duplicate, true);
  assert.equal(session.receivedSamples, 9);
  assert.equal(session.sampleCount, 5);
  assert.equal(session.missingSamples, 0);
});

test('有限去重窗口过期后的古老副本标为 stale，不增加样本数', () => {
  const store = new P.SessionStore({ maxSamples: 35 });
  const session = store.ingest(packet('data', { seq: 1, first: 0, n: 35 })).session;
  for (let seq = 2; seq <= 4200; seq++) store.ingest(packet('data', { seq, first: (seq - 1) * 35, n: 35 }));
  const before = session.receivedSamples;
  const event = store.ingest(packet('data', { seq: 1, first: 0, n: 35 }));
  assert.equal(event.stale, true);
  assert.equal(session.receivedSamples, before);
  assert.equal(session.sampleCount, 35);
  assert.equal(session._seen.size <= 4096, true);
  assert.equal(session._blocks.length, 1);
});

test('不同 seq 的重叠样本作为协议冲突拒绝，不伪造重复的样本', () => {
  const store = new P.SessionStore();
  const session = store.ingest(packet('data', { seq: 1, first: 100 })).session;
  const event = store.ingest(packet('data', { seq: 2, first: 102 }));
  assert.equal(event.conflict, true);
  assert.equal(event.added, 0);
  assert.equal(session.conflictingPackets, 1);
  assert.equal(session.receivedSamples, 3);
});

test('完整主机流到会话的连通检查，不把 HELLO 当作重启清数据', () => {
  const store = new P.SessionStore();
  const parser = new P.StreamParser((frame) => store.ingest(P.decodeFrame(frame)));
  parser.push(P.makeHostFrame(1, 7, rfRxPayload(rfBytes('data', { seq: 1, first: 100 }))));
  parser.push(P.makeHostFrame(5, 8, bytes('01 01 02 50 62 25 00 01')));
  parser.push(P.makeHostFrame(1, 9, rfRxPayload(rfBytes('data', { seq: 2, first: 103 }))));
  assert.equal(store.sessions.get('1:6').receivedSamples, 6);
  assert.equal(store.ingest(P.decodeFrame({ type: 5, seq: 0, payload: bytes('01 01 02 50 62 25 00 01') })), null);
});

test('连接时节点已运行 2h：nodeTime 使用设备锚点，relativeTime 仍从首样本起算', () => {
  const store = new P.SessionStore();
  const session = store.ingest(timedPacket({ seq: 20, first: 23040000, anchorUs: 7200 * 32768 })).session;
  const samples = session.getSamples();
  close(samples[0].nodeTime, 7200 - 2 / 3200);
  close(samples[2].nodeTime, 7200);
  assert.equal(samples[0].relativeTime, 0);
  assert.equal(samples[2].relativeTime, 2 / 3200);
  assert.equal(samples[0].time, samples[0].relativeTime);
  assert.equal(samples[0].nodeClockRaw, 7200 * 32768);
  assert.equal(samples[0].nodeClockUnit, 'rtc32768');
  assert.equal(samples[0].nodeClockHz, 32768);
  assert.equal(samples[0].nodeClockModulus, 0xa8c00000);
  assert.equal(samples[0].nodeAnchorIndex, 23040002);
  assert.equal(samples[0].nodeAnchorTime, 7200);
  assert.equal(session.nodeTime, 7200);
  assert.equal(session.nodeClockUnit, 'rtc32768');
});

test('首连之前整天数无从确定：保留模内秒数与显式 timeWrapUnknown', () => {
  const store = new P.SessionStore();
  const session = store.ingest(timedPacket({ seq: 100, first: (2 * 86400 + 7200) * 3200, anchorUs: 7200 * 32768 })).session;
  assert.equal(session.timeWrapUnknown, true);
  assert.equal(session.getSamples().at(-1).nodeTime, 7200);
  assert.equal(session.getSamples().at(-1).nodeClockEpoch, 0);
});

test('RTC 按 0xA8C00000 日回绕，展开运行时间而非按 u32 回绕', () => {
  const store = new P.SessionStore();
  const session = store.ingest(timedPacket({ seq: 1, first: 100, anchorUs: 0xa8c00000 - 1 })).session;
  store.ingest(timedPacket({ seq: 2, first: 103, anchorUs: 32 }));
  const samples = session.getSamples();
  close(samples[2].nodeTime, 86400 - 1 / 32768);
  close(samples[5].nodeTime, 86400 + 32 / 32768);
  assert.equal(samples[5].nodeClockRaw, 32);
  assert.equal(samples[5].nodeClockEpoch, 1);
  assert.equal(session.getContinuousSamples().length, 6);
});

test('旧 DATA 乱序到达按 seq 重算时钟，不让当前运行时间倒退或多加一天', () => {
  const store = new P.SessionStore();
  const session = store.ingest(timedPacket({ seq: 1, first: 100, anchorUs: 0xa8c00000 - 32 })).session;
  store.ingest(timedPacket({ seq: 3, first: 106, anchorUs: 32 }));
  const latestBefore = session.nodeTime;
  store.ingest(timedPacket({ seq: 2, first: 103, anchorUs: 0 }));
  const samples = session.getSamples();
  close(samples[5].nodeTime, 86400);
  close(samples[8].nodeTime, 86400 + 32 / 32768);
  close(session.nodeTime, latestBefore);
  assert.equal(session.missingSamples, 0);
});

test('首包在日回绕后，迟到的日前 DATA 补全时钟 epoch', () => {
  const store = new P.SessionStore();
  const session = store.ingest(timedPacket({ seq: 2, first: 103, anchorUs: 32 })).session;
  close(session.nodeTime, 32 / 32768);
  store.ingest(timedPacket({ seq: 1, first: 100, anchorUs: 0xa8c00000 - 1 }));
  const samples = session.getSamples();
  close(samples[2].nodeTime, 86400 - 1 / 32768);
  close(samples[5].nodeTime, 86400 + 32 / 32768);
  assert.equal(samples[3].relativeTime, 0);
  assert.equal(session.timeWrapUnknown, true);
});

test('us 时钟按 u32 回绕，迟到包不再触发额外 epoch', () => {
  const store = new P.SessionStore();
  const session = store.ingest(timedPacket({ seq: 10, first: 100, cfg: 0x3d, anchorUs: 0x100000000 - 1000 })).session;
  store.ingest(timedPacket({ seq: 12, first: 106, cfg: 0x3d, anchorUs: 875 }));
  store.ingest(timedPacket({ seq: 11, first: 103, cfg: 0x3d, anchorUs: 0x100000000 - 62 }));
  const sample = session.getSamples().at(-1);
  close(sample.nodeTime, (0x100000000 + 875) / 1000000);
  assert.equal(sample.nodeClockUnit, 'us');
  assert.equal(sample.nodeClockHz, 1000000);
  assert.equal(sample.nodeClockModulus, 0x100000000);
  assert.equal(sample.nodeClockEpoch, 1);
});

test('同一连续段可用样本进度辨认 DATA 之间跨过多个完整 RTC 日', () => {
  const store = new P.SessionStore();
  const session = store.ingest(timedPacket({ seq: 1, first: 100, anchorUs: 100 * 32768 })).session;
  store.ingest(timedPacket({ seq: 2, first: 100 + 2 * 86400 * 3200, anchorUs: 100 * 32768 }));
  const latest = session.getSamples().at(-1);
  close(latest.nodeTime, 100 + 2 * 86400);
  assert.equal(latest.nodeClockEpoch, 2);
  assert.equal(session.timeWrapUnknown, true);
});

test('变采样率和跨 GAP 停顿保留真实运行时间，不拼成名义 ODR 时长', () => {
  const store = new P.SessionStore();
  const session = store.ingest(timedPacket({ seq: 1, first: 100, anchorUs: 10 * 32768 })).session;
  store.ingest(timedPacket({ seq: 2, first: 103, cfg: 0xbb, anchorUs: 15 * 32768, segment: 1, flags: 0x49 }));
  const samples = session.getSamples();
  close(samples[3].nodeTime, 15 - 2 / 800);
  close(samples[5].nodeTime, 15);
  close(samples[3].relativeTime, 3 / 3200);
  close(samples[5].relativeTime, 3 / 3200 + 2 / 800);
  assert.equal(session.getContinuousSamples().length, 3);
  assert.equal(session.odr, 800);
});

test('跨日后缓存裁剪、再插入乱序包，设备时间不重置且相对时间原点不变', () => {
  const store = new P.SessionStore({ capacity: 4 });
  const session = store.ingest(timedPacket({ seq: 1, first: 100, anchorUs: 0xa8c00000 - 1 })).session;
  store.ingest(timedPacket({ seq: 2, first: 103, anchorUs: 32 }));
  store.ingest(timedPacket({ seq: 4, first: 109, anchorUs: 96 }));
  close(session.nodeTime, 86400 + 96 / 32768);
  store.ingest(timedPacket({ seq: 3, first: 106, anchorUs: 64 }));
  const samples = session.getSamples();
  assert.equal(samples.length, 4);
  assert.equal(samples[0].index, 108);
  close(samples[0].nodeTime, 86400 + 64 / 32768);
  close(samples.at(-1).nodeTime, 86400 + 96 / 32768);
  close(samples[0].relativeTime, 8 / 3200);
  assert.equal(samples.at(-1).nodeClockEpoch, 1);
});

test('样本 idx 和 RTC 同时回绕时锚点样本编号同样展开', () => {
  const store = new P.SessionStore();
  const session = store.ingest(timedPacket({ seq: 0xffffffff, first: 0xfffffffd, anchorUs: 0xa8c00000 - 1 })).session;
  store.ingest(timedPacket({ seq: 0, first: 0, anchorUs: 32 }));
  const sample = session.getSamples().at(-1);
  assert.equal(sample.nodeAnchorIndex, 0x100000002);
  close(sample.nodeTime, 86400 + 32 / 32768);
  assert.equal(session.missingSamples, 0);
});

test('波形读取可按节点时间访问较旧样本，保留缓存及包间连续性', () => {
  const store = new P.SessionStore({ capacity: 8 });
  const session = store.ingest(packet('data', { seq: 1, first: 100 })).session;
  store.ingest(packet('data', { seq: 2, first: 103 }));
  store.ingest(packet('data', { seq: 3, first: 106 }));
  const all = session.getSamples();
  assert.equal(session.firstSampleTime, all[0].nodeTime);
  const range = { start: all[1].nodeTime, end: all[4].nodeTime };
  const view = session.getSamples(session.capacity, range);
  assert.deepEqual(view.map(sample => sample.index), [102, 103, 104, 105]);
  assert.deepEqual(view.map(sample => sample.continuous), [false, true, true, true]);
  assert.deepEqual(session.getSamples(session.capacity, { start: range.start - 2, end: range.start - 1 }), []);
  assert.deepEqual(session.getSamples(), all);
});
