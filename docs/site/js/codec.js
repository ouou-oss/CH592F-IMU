// 包编解码：小端序，字段定义来自 protocol.js
'use strict';

const SIZES = { u8: 1, i8: 1, u16: 2, i16: 2, u32: 4, 'u8[8]': 8 };

function readField(b, f) {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  switch (f.t) {
    case 'u8': return dv.getUint8(f.off);
    case 'i8': return dv.getInt8(f.off);
    case 'u16': return dv.getUint16(f.off, true);
    case 'i16': return dv.getInt16(f.off, true);
    case 'u32': return dv.getUint32(f.off, true);
    default: return Array.from(b.slice(f.off, f.off + SIZES[f.t]));
  }
}

function writeField(dv, f, v) {
  switch (f.t) {
    case 'u8': dv.setUint8(f.off, v); break;
    case 'i8': dv.setInt8(f.off, v); break;
    case 'u16': dv.setUint16(f.off, v, true); break;
    case 'i16': dv.setInt16(f.off, v, true); break;
    case 'u32': dv.setUint32(f.off, v, true); break;
    default: break;
  }
}

// CRC-16/CCITT-FALSE：poly=0x1021, init=0xFFFF, refin/refout=false, xorout=0
// 主机帧传入 type..payload，结果以低字节、高字节追加；不含 A5 5A。
function crc16(bytes) {
  let c = 0xFFFF;
  for (const x of bytes) {
    c ^= x << 8;
    for (let i = 0; i < 8; i++) {
      c = (c & 0x8000) ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
    }
  }
  return c;
}

function hexToBytes(s) {
  const clean = s.trim().replace(/0x(?=[0-9a-f]{2})/gi, '');
  if (/[^0-9a-f\s,:-]/i.test(clean)) throw new Error('含非十六进制字符，请仅粘贴 HEX 数据，不要包含日志文字');
  const h = clean.replace(/[\s,:-]/g, '');
  if (h.length === 0) throw new Error('没有输入数据');
  if (h.length % 2) throw new Error('十六进制字符数为奇数');
  const out = new Uint8Array(h.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(h.substr(i * 2, 2), 16);
  return out;
}

const bytesToHex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join(' ');
const hex = (v, w) => '0x' + v.toString(16).toUpperCase().padStart(w, '0');

function explainCfg(v) {
  const odr = PROTO.odr[v & 0x0F];
  const unit = (v & 0x80) ? 'RTC 计数' : 'µs';
  return `ODR ${odr ? odr + ' Hz' : '未知'}，±${PROTO.rangeG[(v >> 4) & 3]} g，带宽 ODR/${(v & 0x40) ? 4 : 2}，时间单位 ${unit}`;
}

function explainValue(f, v, cfg) {
  if (f.n === 'ver_type') return `版本 ${v >> 4}，类型 ${hex(v & 0x0F, 1)}`;
  if (f.bits === 'cfg') return explainCfg(v);
  if (f.bits === 'flags') {
    const on = PROTO.flagBits.filter((_, i) => v & (1 << i));
    return on.length ? on.join(' | ') : '无';
  }
  if (Array.isArray(v)) return bytesToHex(v);
  if (f.n === 'bmi_temp_raw') return v === -32768 ? '无效（传感器休眠）' : `${(23 + v / 512).toFixed(2)} °C`;
  if (f.n === 'anchor_us' || f.n === 'sync_txdone_us') {
    return (cfg & 0x80) ? `${(v / 32768).toFixed(6)} s（节点 RTC 计数 ${v}）` : `${v} µs（节点时间）`;
  }
  return String(v);
}

function decodeSamples(b, cfg) {
  const lsbPerG = 32768 / PROTO.rangeG[(cfg >> 4) & 3];
  const n = b[18];
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const out = [];
  for (let i = 0; i < n; i++) {
    const o = 36 + i * 6;
    const raw = [0, 2, 4].map((k) => dv.getInt16(o + k, true));
    out.push({ raw, g: raw.map((r) => (r / lsbPerG).toFixed(3)) });
  }
  return out;
}

function decodeConf(b) {
  const n = b[14];
  const out = [];
  for (let i = 0; i < n && 15 + i * 3 + 3 <= b.length; i++) {
    const o = 15 + i * 3;
    out.push({ node: b[o], seqLo: b[o + 1] | (b[o + 2] << 8) });
  }
  return out;
}

function decodeRf(b) {
  if (!b.length) throw new Error('无线负载为空');
  if ((b[0] >> 4) !== 1) throw new Error(`不支持协议版本 ${b[0] >> 4}（当前为 V1）`);
  if ([3, 4].includes(b[0] & 0x0F)) throw new Error('SUMMARY / EVENT 属于后续模式 B，请查阅 05 边缘模式文档');
  const entry = Object.entries(PROTO.packets).find(([, p]) => p.type === (b[0] & 0x0F));
  if (!entry) throw new Error(`未知包类型 ${hex(b[0] & 0x0F, 1)}`);
  const [name, p] = entry;
  const fixed = { DATA: 36, STATUS: 63, POLL: 20, ACK: 15 }[name];
  if (b.length < fixed) throw new Error(`${name} 长度不足：至少 ${fixed} B，实际 ${b.length} B`);
  let expected = fixed;
  if (name === 'DATA') {
    if (b[18] < 1 || b[18] > 35) throw new Error('DATA 的 n_samples 必须为 1～35');
    expected += b[18] * 6;
  }
  if (name === 'ACK') {
    if (b[5] > 8 || b[14] > 8) throw new Error('ACK 的 cmd_len 或 conf_n 超过 8');
    expected += b[14] * 3;
  }
  if (b.length !== expected) throw new Error(`${name} 长度不符：应为 ${expected} B，实际 ${b.length} B`);
  const rows = p.fields.map((f) => {
    if (f.off + SIZES[f.t] > b.length) return { f, missing: true };
    const v = readField(b, f);
    return { f, v, text: explainValue(f, v, b[3]) };
  });
  const res = { name, p, rows, len: b.length };
  if (name === 'DATA' && b.length >= 36) res.samples = decodeSamples(b, b[3]);
  if (name === 'ACK' && b.length >= 15) res.conf = decodeConf(b);
  return res;
}

function decodeHost(b) {
  if (b.length < 8 || b[0] !== 0xA5 || b[1] !== 0x5A) throw new Error('不是主机帧（应以 A5 5A 开头）');
  const len = b[4] | (b[5] << 8);
  if (b.length !== 8 + len) throw new Error(`主机帧长度不符：len=${len}，整帧应为 ${8 + len} B，实际 ${b.length} B；请只输入一帧`);
  const pl = b.slice(6, 6 + len);
  const crcStored = b[6 + len] | (b[7 + len] << 8);
  const crcCalc = crc16(b.slice(2, 6 + len));
  const res = { type: b[2], seq: b[3], len, crcStored, crcCalc, payload: pl };
  // CRC 失败仍展示校验结果，但不把受损负载作为可信字段继续解码。
  if (crcStored !== crcCalc) return res;
  const dv = new DataView(pl.buffer, pl.byteOffset, pl.byteLength);
  const sizes = { 0x02: 12, 0x03: 28, 0x04: 2, 0x05: 8, 0x81: 4, 0x83: 0 };
  if (sizes[b[2]] !== undefined && pl.length !== sizes[b[2]]) {
    res.payloadError = `type ${hex(b[2], 2)} 负载应为 ${sizes[b[2]]} B，实际 ${pl.length} B`;
    return res;
  }
  if (b[2] === 0x01) {
    if (pl.length < 7 || pl.length !== 7 + pl[6] || pl[6] > 251) {
      res.payloadError = 'RF_RX 负载长度与 rf_len 不符（应为 7 + rf_len，且 rf_len ≤ 251）';
      return res;
    }
    res.rx = { gw: pl[0], rxUs: dv.getUint32(1, true), rssi: dv.getInt8(5), rfLen: pl[6] };
    try { res.rf = decodeRf(pl.slice(7)); }
    catch (e) { res.payloadError = e.message; }
  } else if (b[2] === 0x02) {
    res.fields = [['req_id', dv.getUint32(0, true)], ['t_req_us', dv.getUint32(4, true)], ['t_resp_us', dv.getUint32(8, true)]];
  } else if (b[2] === 0x03) {
    const names = ['rx_ok', 'crc_err', 'type_err', 'queue_full', 'net_drop', 'rearm_lat_max_us', 'tcp_reconnect'];
    res.fields = names.map((n, i) => [n, dv.getUint32(i * 4, true)]);
  } else if (b[2] === 0x04) {
    res.fields = [['req_type', hex(pl[0], 2)], ['result', pl[1]]];
  } else if (b[2] === 0x05) {
    res.fields = [['gw_id', pl[0]], ['fw_major', pl[1]], ['fw_minor', pl[2]],
      ['frequency_khz', dv.getUint32(3, true)], ['phy', pl[7]]];
  } else if (b[2] === 0x81) {
    res.fields = [['req_id', dv.getUint32(0, true)]];
  } else if (b[2] === 0x82) {
    if (pl.length < 4 || pl[3] > 8 || pl.length !== 4 + pl[3]) {
      res.payloadError = 'QUEUE_DOWNLINK 应为 node_id、cmd_id、cmd_code、cmd_len 四个 u8 + cmd_len 字节负载，cmd_len ≤ 8';
      return res;
    }
    res.fields = [['node_id', pl[0]], ['cmd_id', pl[1]], ['cmd_code', hex(pl[2], 2)], ['cmd_len', pl[3]], ['cmd_payload', bytesToHex(pl.slice(4))]];
  }
  return res;
}

function makeHostFrame(type, seq, payload) {
  const f = new Uint8Array(8 + payload.length);
  f.set([0xA5, 0x5A, type, seq, payload.length & 0xFF, payload.length >> 8]);
  f.set(payload, 6);
  const c = crc16(f.subarray(2, 6 + payload.length));
  f[6 + payload.length] = c & 0xFF;
  f[7 + payload.length] = c >> 8;
  return f;
}

function makeExampleAck() {
  // 两条确认表，保留固定 8 B 命令区；无待下行命令。
  return Uint8Array.from([0x18, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 0, 4, 1, 0xff, 3]);
}

// 生成一个示例 DATA 包：静止 Z≈1 g，下标 20 的样本叠加一次冲击
function makeExampleData() {
  const n = 35;
  const b = new Uint8Array(36 + n * 6);
  const dv = new DataView(b.buffer);
  const vals = { ver_type: 0x11, node_id: 1, flags: 0x00, cfg: 0xFD, boot_id: 7, seq: 1024,
    oldest_seq: 1021, first_sample_idx: 35840, n_samples: n, segment_id: 0, attempt: 1, cmd_ack: 0,
    anchor_idx: 35874, anchor_us: 1234567, sync_txdone_us: 1234100, sync_seq_lo: 0xFF, sync_attempt: 1 };
  PROTO.packets.DATA.fields.forEach((f) => writeField(dv, f, vals[f.n]));
  for (let i = 0; i < n; i++) {
    const spike = i === 20 ? 30000 : (i === 21 ? -12000 : 0);
    const noise = ((i * 37) % 11) - 5;
    dv.setInt16(36 + i * 6, noise, true);
    dv.setInt16(38 + i * 6, -noise, true);
    dv.setInt16(40 + i * 6, Math.max(-32767, Math.min(32767, 2048 + noise + spike)), true);
  }
  return b;
}

function wrapHostRfRx(rf, gw, rxUs, rssi) {
  const pl = new Uint8Array(7 + rf.length);
  const dv = new DataView(pl.buffer);
  pl[0] = gw; dv.setUint32(1, rxUs, true); dv.setInt8(5, rssi); pl[6] = rf.length;
  pl.set(rf, 7);
  return makeHostFrame(0x01, 0x2A, pl);
}
