// 网关字节流与节点会话：偏移与 firmware/common/proto.h 保持一致。
(function (root, factory) {
  'use strict';
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.BmiProtocol = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  const HOST_PAYLOAD_MAX = 512;
  const U32 = 0x100000000;
  const RTC_MODULUS = 0xa8c00000;
  const ODR = Object.freeze({ 11: 800, 12: 1600, 13: 3200, 14: 6400 });
  const RANGE_G = Object.freeze([2, 4, 8, 16]);
  const HOST = Object.freeze({ RF_RX: 1, TIME_RESP: 2, GW_STATUS: 3, CMD_RESP: 4, HELLO: 5, TIME_REQ: 0x81, QUEUE_DOWNLINK: 0x82, GET_INFO: 0x83 });

  function crc16(bytes) {
    let crc = 0xffff;
    for (const byte of bytes) {
      crc ^= byte << 8;
      for (let bit = 0; bit < 8; bit++) crc = ((crc << 1) ^ (crc & 0x8000 ? 0x1021 : 0)) & 0xffff;
    }
    return crc;
  }

  function makeHostFrame(type, seq, payload = new Uint8Array(0)) {
    if (payload.length > HOST_PAYLOAD_MAX) throw new Error('主机负载超过 512 字节');
    const bytes = new Uint8Array(payload.length + 8);
    bytes.set([0xa5, 0x5a, type, seq, payload.length & 255, payload.length >>> 8]);
    bytes.set(payload, 6);
    const crc = crc16(bytes.subarray(2, bytes.length - 2));
    bytes[bytes.length - 2] = crc & 255;
    bytes[bytes.length - 1] = crc >>> 8;
    return bytes;
  }

  class StreamParser {
    constructor(options = {}) {
      this.onFrame = typeof options === 'function' ? options : options.onFrame || function () {};
      this.buffer = new Uint8Array(HOST_PAYLOAD_MAX + 8);
      this.reset();
    }

    reset() {
      this.used = 0;
      this.stats = { frames: 0, crcErrors: 0, discardedBytes: 0, invalidLengths: 0 };
    }

    push(bytes) {
      // 帧长有界，逐字节喂入不会因任意大的串口 chunk 扩大内部缓存。
      for (const byte of bytes) {
        this.buffer[this.used++] = byte;
        this._drain();
      }
    }

    _discard(count) {
      this.buffer.copyWithin(0, count, this.used);
      this.used -= count;
      this.stats.discardedBytes += count;
    }

    _drain() {
      while (this.used) {
        if (this.buffer[0] !== 0xa5) { this._discard(1); continue; }
        if (this.used < 2) return;
        if (this.buffer[1] !== 0x5a) { this._discard(1); continue; }
        if (this.used < 6) return;
        const length = this.buffer[4] | this.buffer[5] << 8;
        if (length > HOST_PAYLOAD_MAX) {
          this.stats.invalidLengths++;
          this._discard(1);
          continue;
        }
        const end = 8 + length;
        if (this.used < end) return;
        const stored = this.buffer[end - 2] | this.buffer[end - 1] << 8;
        if (crc16(this.buffer.subarray(2, end - 2)) !== stored) {
          this.stats.crcErrors++;
          this._discard(1);
          continue;
        }
        const frame = { type: this.buffer[2], seq: this.buffer[3], payload: this.buffer.slice(6, end - 2) };
        this.buffer.copyWithin(0, end, this.used);
        this.used -= end;
        this.stats.frames++;
        this.onFrame(frame);
      }
    }
  }

  function expectLength(bytes, expected, name) {
    if (bytes.length !== expected) throw new Error(`${name} 长度应为 ${expected} B，实际 ${bytes.length} B`);
  }

  function decodeRf(bytes) {
    if (bytes.length < 14) throw new Error('RF 公共头长度不足');
    if (bytes[0] >>> 4 !== 1) throw new Error(`不支持 RF 协议版本 ${bytes[0] >>> 4}`);
    const type = bytes[0] & 15;
    if (![1, 2, 5].includes(type)) throw new Error(`不支持 RF 包类型 ${type}`);
    if (bytes[1] === 0 || bytes[1] === 255) throw new Error('RF 节点编号无效');
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const cfg = bytes[3];
    const rangeG = RANGE_G[cfg >>> 4 & 3];
    const packet = {
      kind: { 1: 'data', 2: 'status', 5: 'poll' }[type], type,
      version: 1, nodeId: bytes[1], flags: bytes[2], cfg,
      bootId: view.getUint16(4, true), seq: view.getUint32(6, true), oldestSeq: view.getUint32(10, true),
      odr: ODR[cfg & 15] || null, rangeG, lsbPerG: 32768 / rangeG,
      timeUnit: cfg & 0x80 ? 'rtc32768' : 'us',
    };
    if (type === 1) {
      if (bytes.length < 36) throw new Error('DATA 包头长度不足');
      const n = bytes[18];
      if (n < 1 || n > 35) throw new Error('DATA 样本数应为 1～35');
      if (bytes[20] < 1) throw new Error('DATA attempt 必须从 1 开始');
      if (!packet.odr) throw new Error('DATA 采样率代码不支持');
      expectLength(bytes, 36 + n * 6, 'DATA');
      Object.assign(packet, {
        firstSampleIdx: view.getUint32(14, true), nSamples: n, segmentId: bytes[19], attempt: bytes[20], cmdAck: bytes[21],
        anchorIdx: view.getUint32(22, true), anchorUs: view.getUint32(26, true), syncTxdoneUs: view.getUint32(30, true),
        syncSeqLo: bytes[34], syncAttempt: bytes[35], samples: [],
      });
      for (let i = 0; i < n; i++) {
        const offset = 36 + i * 6;
        packet.samples.push({ x: view.getInt16(offset, true), y: view.getInt16(offset + 2, true), z: view.getInt16(offset + 4, true) });
      }
    } else if (type === 2) {
      expectLength(bytes, 63, 'STATUS');
      Object.assign(packet, {
        sampleIdxNow: view.getUint32(14, true), battMv: view.getUint16(18, true), bmiTempRaw: view.getInt16(20, true),
        txOk: view.getUint32(22, true), txRetry: view.getUint32(26, true), txBackoff: view.getUint32(30, true),
        ringDropPkts: view.getUint32(34, true), ringDropSamples: view.getUint32(38, true), fifoGapSamples: view.getUint32(42, true),
        fifoFullCnt: view.getUint16(46, true), ringMaxUsed: bytes[48], lastCmdId: bytes[49], bmiErrReg: view.getUint16(50, true),
        ackRssiAvg: view.getInt8(52), fwMajor: bytes[53], fwMinor: bytes[54], bmiReinitCnt: bytes[55],
        currentGatewayId: bytes[56], currentChannelIndex: bytes[57], mode: bytes[58], solarMv: view.getUint16(59, true), roamCnt: view.getUint16(61, true),
      });
      packet.temperatureC = packet.bmiTempRaw === -32768 ? null : 23 + packet.bmiTempRaw / 512;
      packet.motionSleeping = (packet.flags & 0x80) !== 0 && packet.mode === 2;
    } else {
      expectLength(bytes, 20, 'POLL');
      Object.assign(packet, { syncTxdoneUs: view.getUint32(14, true), syncSeqLo: bytes[18], syncAttempt: bytes[19] });
    }
    return packet;
  }

  function decodeRfRx(payload) {
    if (payload.length < 7 || payload[6] > 246 || payload.length !== 7 + payload[6]) throw new Error('RF_RX 长度与 rf_len 不符');
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    return Object.assign(decodeRf(payload.subarray(7)), { gatewayId: payload[0], rxUs: view.getUint32(1, true), rssi: view.getInt8(5) });
  }

  function decodeFrame(frame) {
    const { type, seq, payload } = frame;
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    const result = { type, seq };
    if (type === HOST.RF_RX) {
      const rf = decodeRfRx(payload);
      return Object.assign(result, { kind: 'rf_rx', gatewayId: rf.gatewayId, rxUs: rf.rxUs, rssi: rf.rssi, rf });
    }
    if (type === HOST.TIME_RESP) {
      expectLength(payload, 12, 'TIME_RESP');
      return Object.assign(result, { kind: 'time_resp', reqId: view.getUint32(0, true), tReqUs: view.getUint32(4, true), tRespUs: view.getUint32(8, true) });
    }
    if (type === HOST.GW_STATUS) {
      expectLength(payload, 28, 'GW_STATUS');
      const names = ['rxOk', 'crcErr', 'typeErr', 'queueFull', 'netDrop', 'rearmLatMaxUs', 'tcpReconnect'];
      result.kind = 'gw_status';
      names.forEach((name, i) => { result[name] = view.getUint32(i * 4, true); });
      return result;
    }
    if (type === HOST.CMD_RESP) {
      expectLength(payload, 2, 'CMD_RESP');
      return Object.assign(result, { kind: 'cmd_resp', reqType: payload[0], result: payload[1] });
    }
    if (type === HOST.HELLO) {
      expectLength(payload, 8, 'HELLO');
      return Object.assign(result, { kind: 'hello', gatewayId: payload[0], fwMajor: payload[1], fwMinor: payload[2], frequencyKhz: view.getUint32(3, true), phy: payload[7] });
    }
    return Object.assign(result, { kind: 'unknown', payload });
  }

  function unwrap32(raw, reference) {
    if (reference === null || reference === undefined) return raw;
    return raw + Math.round((reference - raw) / U32) * U32;
  }

  const COUNTERS = { txOk: U32, txRetry: U32, txBackoff: U32, ringDropPkts: U32, ringDropSamples: U32, fifoGapSamples: U32, fifoFullCnt: 65536, bmiReinitCnt: 256, roamCnt: 65536 };

  class Session {
    constructor(packet, capacity) {
      this.key = `${packet.nodeId}:${packet.bootId}`;
      this.nodeId = packet.nodeId;
      this.bootId = packet.bootId;
      this.capacity = capacity;
      this.odr = packet.odr;
      this.rangeG = packet.rangeG;
      this.lsbPerG = packet.lsbPerG;
      this.lastRssi = null;
      this.gatewayId = null;
      this.receivedPackets = 0;
      this.duplicatePackets = 0;
      this.receivedSamples = 0;
      this.missingSamples = 0;
      this.outOfOrderPackets = 0;
      this.stalePackets = 0;
      this.conflictingPackets = 0;
      this.segments = 0;
      this.latestStatus = null;
      this.status = null;
      this.statusDelta = null;
      this.sampleCount = 0;
      // DATA 没有上电后的完整时钟周期数；首个锚点只证明模内运行秒数。
      this.timeWrapUnknown = true;
      this.nodeTime = null;
      this.nodeClockUnit = null;
      this.nodeClockHz = null;
      this.nodeClockModulus = null;
      this._blocks = [];
      this._seen = new Set();
      this._seenOrder = [];
      this._seenLimit = Math.max(4096, Math.ceil(capacity / 35) * 4);
      this._maxSeq = null;
      this._maxDataSeq = null;
      this._minIndex = null;
      this._maxIndex = null;
      this._originIndex = null;
      this._originSeq = null;
      this._trimmedSeq = -Infinity;
      this._statusSeq = -Infinity;
      this._statusBaseline = null;
      this._segmentCounts = new Map();
      this._nodeClockFloor = null;
    }

    get samples() { return this.getSamples(); }
    get uniqueSamples() { return this.receivedSamples; }

    ingest(packet) {
      if (Number.isFinite(packet.rssi)) this.lastRssi = packet.rssi;
      if (packet.gatewayId !== undefined) this.gatewayId = packet.gatewayId;
      const event = { session: this, kind: packet.kind, added: 0, duplicate: false, outOfOrder: false };
      if (packet.kind === 'poll') return event;
      const seq = unwrap32(packet.seq, this._maxSeq);
      if (this._seen.has(seq)) {
        this.duplicatePackets++;
        event.duplicate = true;
        return event;
      }
      // 缓存之外的超迟到包不再重插；避免有限去重表过期后把古老副本重复计入。
      if (seq <= this._trimmedSeq || (this._maxSeq !== null && seq <= this._maxSeq - this._seenLimit)) {
        this.stalePackets++;
        event.stale = true;
        return event;
      }
      this._seen.add(seq);
      this._seenOrder.push(seq);
      if (this._seenOrder.length > this._seenLimit) this._seen.delete(this._seenOrder.shift());
      event.outOfOrder = this._maxSeq !== null && seq < this._maxSeq;
      if (event.outOfOrder) this.outOfOrderPackets++;
      this._maxSeq = this._maxSeq === null ? seq : Math.max(seq, this._maxSeq);
      this.receivedPackets++;
      if (packet.kind === 'status') {
        if (seq > this._statusSeq) {
          this._statusSeq = seq;
          this.latestStatus = this.status = packet;
          if (!this._statusBaseline) this._statusBaseline = packet;
          this.statusDelta = {};
          for (const [name, modulus] of Object.entries(COUNTERS)) this.statusDelta[name] = (packet[name] - this._statusBaseline[name] + modulus) % modulus;
        }
        return event;
      }
      if (packet.kind !== 'data') return event;

      let first = unwrap32(packet.firstSampleIdx, this._maxIndex);
      if (first < 0) {
        // 首包恰在回绕之后，迟到的回绕前包会补充之前未知的 epoch。
        const shift = Math.ceil(-first / U32) * U32;
        for (const cached of this._blocks) { cached.first += shift; cached.last += shift; }
        if (this._nodeClockFloor) this._nodeClockFloor.nodeAnchorIndex += shift;
        this._originIndex += shift;
        this._minIndex += shift;
        this._maxIndex += shift;
        first += shift;
      }
      const last = first + packet.samples.length - 1;
      let low = 0;
      let high = this._blocks.length;
      while (low < high) {
        const middle = low + high >>> 1;
        if (this._blocks[middle].seq < seq) low = middle + 1;
        else high = middle;
      }
      const before = this._blocks[low - 1];
      const after = this._blocks[low];
      if ((before && before.last >= first) || (after && last >= after.first)) {
        this.conflictingPackets++;
        event.conflict = true;
        return event;
      }
      const block = { packet, seq, first, last, segment: packet.segmentId, startTime: 0 };
      this._blocks.splice(low, 0, block);
      if (this._originIndex === null) { this._originIndex = first; this._originSeq = seq; }
      // 按包序列前向展开 u8 段号：0→128 是 +128，不能按最近有符号距离变成 -128。
      if (before) block.segment = before.segment + (packet.segmentId - before.packet.segmentId + 256) % 256;
      else if (after) {
        block.segment = after.segment - (after.packet.segmentId - packet.segmentId + 256) % 256;
        if (block.segment < 0) {
          const shift = Math.ceil(-block.segment / 256) * 256;
          for (const current of this._blocks) current.segment += shift;
          this._segmentCounts = new Map(Array.from(this._segmentCounts, ([segment, count]) => [segment + shift, count]));
        }
      }
      for (let i = low + 1; i < this._blocks.length; i++) {
        const previous = this._blocks[i - 1];
        const current = this._blocks[i];
        current.segment = previous.segment + (current.packet.segmentId - previous.packet.segmentId + 256) % 256;
      }
      this._updateTimes(low);
      this._updateNodeTimes(low);
      if (low === this._blocks.length - 1) {
        if (!this._segmentCounts.has(block.segment)) this.segments++;
        this._segmentCounts.set(block.segment, (this._segmentCounts.get(block.segment) || 0) + 1);
      } else {
        const counts = new Map();
        for (const current of this._blocks) counts.set(current.segment, (counts.get(current.segment) || 0) + 1);
        for (const segment of counts.keys()) if (!this._segmentCounts.has(segment)) this.segments++;
        this._segmentCounts = counts;
      }
      this._minIndex = this._minIndex === null ? first : Math.min(first, this._minIndex);
      this._maxIndex = this._maxIndex === null ? last : Math.max(last, this._maxIndex);
      this.receivedSamples += packet.samples.length;
      this.sampleCount += packet.samples.length;
      this.missingSamples = this._maxIndex - this._minIndex + 1 - this.receivedSamples;
      if (this._maxDataSeq === null || seq > this._maxDataSeq) {
        this._maxDataSeq = seq;
        this.odr = packet.odr;
        this.rangeG = packet.rangeG;
        this.lsbPerG = packet.lsbPerG;
      }
      this._trim();
      event.added = packet.samples.length;
      return event;
    }

    _updateTimes(start) {
      // 只表示按配置 ODR 推导的相对采样时间，不使用 PC 到达时间冒充 UTC。
      const blocks = this._blocks;
      if (start === 0) {
        const first = blocks[0];
        if (blocks.length === 1 || first.seq === this._originSeq) first.startTime = (first.first - this._originIndex) / first.packet.odr;
        else {
          const next = blocks[1];
          first.startTime = next.startTime - (next.first - first.first) / first.packet.odr;
        }
      }
      for (let i = Math.max(1, start); i < blocks.length; i++) {
        const previous = blocks[i - 1];
        const current = blocks[i];
        current.startTime = previous.startTime + (current.first - previous.first) / previous.packet.odr;
      }
    }

    _updateNodeTimes(start) {
      // 时钟按 DATA seq 而非主机到达顺序展开。保留每包锚点的真实停顿，
      // 相邻样本仅用 ODR 从该锚点内插，不用名义采样时间代替设备运行时间。
      for (let i = start; i < this._blocks.length; i++) {
        const block = this._blocks[i];
        const packet = block.packet;
        const previous = i ? this._blocks[i - 1] : this._nodeClockFloor;
        const rtc = !!(packet.cfg & 0x80);
        const hz = rtc ? 32768 : 1000000;
        const modulus = rtc ? RTC_MODULUS : U32;
        const period = modulus / hz;
        const rawSeconds = packet.anchorUs / hz;
        block.nodeAnchorIndex = unwrap32(packet.anchorIdx, block.last);
        let epoch = 0;
        if (previous) {
          // 最小前向周期保留跨 GAP/变速/暂停的真实时钟差；同一连续段的
          // 样本进度还能辨认两个 DATA 之间跨过多个完整周期的情况。
          const minimumEpoch = Math.ceil((previous.nodeAnchorTime - rawSeconds) / period - 1e-12);
          const previousCfg = previous.packet ? previous.packet.cfg : previous.cfg;
          const previousOdr = previous.packet ? previous.packet.odr : previous.odr;
          const sameRateSegment = block.segment === previous.segment && packet.cfg === previousCfg && !(packet.flags & 0x48);
          const expected = previous.nodeAnchorTime + (block.nodeAnchorIndex - previous.nodeAnchorIndex) / previousOdr;
          epoch = Math.max(0, minimumEpoch, sameRateSegment ? Math.round((expected - rawSeconds) / period) : 0);
        }
        block.nodeAnchorTime = rawSeconds + epoch * period;
        block.nodeClockEpoch = epoch;
        block.nodeClockUnit = rtc ? 'rtc32768' : 'us';
        block.nodeClockHz = hz;
        block.nodeClockModulus = modulus;
      }
      const latest = this._blocks[this._blocks.length - 1];
      if (latest) {
        this.nodeTime = latest.nodeAnchorTime + (latest.last - latest.nodeAnchorIndex) / latest.packet.odr;
        this.nodeClockUnit = latest.nodeClockUnit;
        this.nodeClockHz = latest.nodeClockHz;
        this.nodeClockModulus = latest.nodeClockModulus;
      }
    }

    _trim() {
      while (this.sampleCount > this.capacity && this._blocks.length) {
        const block = this._blocks[0];
        const remove = Math.min(this.sampleCount - this.capacity, block.packet.samples.length);
        this.sampleCount -= remove;
        if (remove < block.packet.samples.length) {
          block.packet = Object.assign({}, block.packet, { samples: block.packet.samples.slice(remove) });
          block.first += remove;
          block.startTime += remove / block.packet.odr;
        } else {
          this._blocks.shift();
          // 缓存裁剪不能把已展开的设备时钟重新归零或退回模内时间。
          this._nodeClockFloor = {
            seq: block.seq, cfg: block.packet.cfg, odr: block.packet.odr, segment: block.segment,
            nodeAnchorTime: block.nodeAnchorTime, nodeAnchorIndex: block.nodeAnchorIndex,
          };
          this._trimmedSeq = Math.max(this._trimmedSeq, block.seq);
          const count = this._segmentCounts.get(block.segment) - 1;
          if (count) this._segmentCounts.set(block.segment, count);
          else this._segmentCounts.delete(block.segment);
        }
      }
    }

    get firstSampleTime() {
      const first = this._blocks[0];
      return first ? first.nodeAnchorTime + (first.first - first.nodeAnchorIndex) / first.packet.odr : null;
    }

    getSamples(limit = this.capacity, range = null) {
      const count = Math.max(0, Math.min(this.sampleCount, Math.floor(limit)));
      if (!count) return [];
      const output = [];
      let skip = this.sampleCount - count;
      let previous = null;
      for (const block of this._blocks) {
        const packet = block.packet;
        if (skip >= packet.samples.length) { skip -= packet.samples.length; continue; }
        const firstOffset = block.first - block.nodeAnchorIndex;
        if (range && (block.nodeAnchorTime + (firstOffset + skip) / packet.odr > range.end ||
            block.nodeAnchorTime + (block.last - block.nodeAnchorIndex) / packet.odr < range.start)) {
          skip = 0;
          previous = null;
          continue;
        }
        for (let i = skip; i < packet.samples.length; i++) {
          const nodeTime = block.nodeAnchorTime + (firstOffset + i) / packet.odr;
          if (range && (nodeTime < range.start || nodeTime > range.end)) { previous = null; continue; }
          const raw = packet.samples[i];
          const index = block.first + i;
          const relativeTime = block.startTime + i / packet.odr;
          const continuous = !!previous && index === previous.index + 1 && packet.cfg === previous.cfg && block.segment === previous.segment && !(i === 0 && (packet.flags & 0x49));
          const sample = {
            index, rawIndex: index >>> 0, seq: packet.seq, nodeId: this.nodeId, bootId: this.bootId,
            segment: block.segment, rawSegment: packet.segmentId, cfg: packet.cfg, flags: packet.flags,
            odr: packet.odr, rangeG: packet.rangeG, lsbPerG: packet.lsbPerG,
            x: raw.x, y: raw.y, z: raw.z, gx: raw.x / packet.lsbPerG, gy: raw.y / packet.lsbPerG, gz: raw.z / packet.lsbPerG,
            relativeTime, time: relativeTime, continuous, nodeTime,
            nodeClockRaw: packet.anchorUs, nodeClockUnit: block.nodeClockUnit,
            nodeClockHz: block.nodeClockHz, nodeClockModulus: block.nodeClockModulus,
            nodeClockEpoch: block.nodeClockEpoch, nodeAnchorIndex: block.nodeAnchorIndex,
            nodeAnchorTime: block.nodeAnchorTime,
          };
          output.push(sample);
          previous = sample;
        }
        skip = 0;
      }
      return output;
    }

    getContinuousSamples(limit = this.capacity) {
      const samples = this.getSamples(limit);
      let start = samples.length - 1;
      while (start > 0 && samples[start].continuous) start--;
      return samples.slice(Math.max(0, start));
    }
  }

  class SessionStore {
    constructor(options = {}) {
      this.capacity = options.capacity || options.maxSamples || 65536;
      if (!Number.isInteger(this.capacity) || this.capacity < 1) throw new Error('样本缓存容量必须为正整数');
      this.sessions = new Map();
    }

    ingest(decoded) {
      const packet = decoded && decoded.kind === 'rf_rx' ? decoded.rf : decoded;
      if (!packet || !['data', 'status', 'poll'].includes(packet.kind)) return null;
      const key = `${packet.nodeId}:${packet.bootId}`;
      if (!this.sessions.has(key)) this.sessions.set(key, new Session(packet, this.capacity));
      return this.sessions.get(key).ingest(packet);
    }

    clear() { this.sessions.clear(); }
  }

  return Object.freeze({ HOST, ODR, RANGE_G, RTC_MODULUS, crc16, makeHostFrame, StreamParser, decodeRf, decodeRfRx, decodeFrame, SessionStore, unwrap32 });
});
