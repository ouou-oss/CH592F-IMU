// 协议部分的页面渲染：字节布局图、字段表、解码器
'use strict';

const $ = (sel) => document.querySelector(sel);
const escapeHtml = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

function fieldAt(fields, off) {
  return fields.find((f) => off >= f.off && off < f.off + SIZES[f.t]);
}

function byteCell(off, f, extraCls) {
  const first = f && off === f.off;
  const cls = `byte g-${f ? f.g : 'none'}${first ? ' first' : ''}${extraCls || ''}`;
  return `<button type="button" class="${cls}" data-field="${f ? f.n : ''}" title="${f ? f.n + '：' + f.d : ''}" ` +
    `aria-label="偏移 ${off}，${f ? f.n + '：' + f.d : '保留'}"><span class="off">${off}</span></button>`;
}

function tailCells(p, base) {
  if (p.samples) {
    const f = { off: base, n: 'sample[0]', g: 'data', d: 'x、y、z 各一个 int16，小端，原始 LSB', t: 'u8' };
    const cells = Array.from({ length: 6 }, (_, i) => byteCell(base + i, f));
    return cells.join('') + `<div class="byte more g-data">… 共 n 个样本（≤ ${p.samples.max}）</div>`;
  }
  if (p.conf) {
    const conf = [{ n: 'conf[0].node_id', s: 1 }, { n: 'seq_lo16', s: 2 }];
    let off = base;
    const cells = conf.map((c) => {
      const f = { off, n: c.n, g: 'seq', d: '确认表条目：节点号 + seq 低 16 位，最新的在前', t: c.s === 1 ? 'u8' : 'u16' };
      const html = Array.from({ length: c.s }, (_, i) => byteCell(off + i, f)).join('');
      off += c.s;
      return html;
    });
    return cells.join('') + `<div class="byte more g-seq">… 共 conf_n 条（≤ ${p.conf.max}）</div>`;
  }
  return '';
}

function renderLayout(name) {
  const p = PROTO.packets[name];
  const last = p.fields[p.fields.length - 1];
  const hdrLen = last.off + SIZES[last.t];
  let html = '';
  for (let off = 0; off < hdrLen; off++) html += byteCell(off, fieldAt(p.fields, off));
  html += tailCells(p, hdrLen);
  $('#pkt-layout').innerHTML = html;
  $('#pkt-meta').innerHTML = `<b>${p.title}</b>（type = 0x${p.type.toString(16)}，${p.dir}）` +
    `<br>长度：${p.size}<br>${p.desc}`;
  const rows = p.fields.map((f) => `<tr data-field="${f.n}"><td>${f.off}</td><td><code>${f.t}</code></td>` +
    `<td><code>${f.n}</code></td><td>${f.d}</td></tr>`).join('');
  $('#pkt-fields tbody').innerHTML = rows;
  $('#pkt-hint').textContent = '数字为字节偏移；悬停或聚焦字节查看字段。完整定义见下表。';
  bindHover();
}

function bindHover() {
  document.querySelectorAll('#protocol [data-field]').forEach((el) => {
    el.addEventListener('mouseenter', () => highlight(el.dataset.field, true));
    el.addEventListener('mouseleave', () => highlight(el.dataset.field, false));
    el.addEventListener('focus', () => highlight(el.dataset.field, true));
    el.addEventListener('blur', () => highlight(el.dataset.field, false));
  });
}

function highlight(name, on) {
  if (!name) return;
  document.querySelectorAll(`#protocol [data-field="${name}"]`).forEach((el) => el.classList.toggle('hl', on));
  if (on) {
    const p = PROTO.packets[$('#pkt-tabs .on').dataset.pkt];
    const f = p.fields.find((x) => x.n === name);
    $('#pkt-hint').textContent = f ? `${f.n} · 偏移 ${f.off} · ${f.t} — ${f.d}` : name;
  }
}

function renderPacketTabs() {
  const tabs = Object.keys(PROTO.packets).map((k) => `<button data-pkt="${k}" aria-pressed="false">${k}</button>`).join('');
  $('#pkt-tabs').innerHTML = tabs;
  $('#pkt-tabs').addEventListener('click', (e) => {
    const k = e.target.dataset.pkt;
    if (!k) return;
    document.querySelectorAll('#pkt-tabs button').forEach((b) => {
      b.classList.toggle('on', b.dataset.pkt === k);
      b.setAttribute('aria-pressed', String(b.dataset.pkt === k));
    });
    renderLayout(k);
  });
  $('#pkt-tabs button').click();
}

function renderTable(sel, rows) {
  $(sel + ' tbody').innerHTML = rows.map((r) => '<tr>' + r.map((c) => `<td>${c}</td>`).join('') + '</tr>').join('');
}

function renderFlags() {
  const rows = PROTO.flagBits.map((n, i) => [`b${i}`, `<code>${n}</code>`, PROTO.flagDesc[i]]);
  renderTable('#flag-table', rows);
}

function sampleWaveform(samples) {
  const w = 800, h = 210, left = 50, right = 18, top = 24, bottom = 32;
  const max = Math.max(2, ...samples.flatMap((s) => s.g.map((v) => Math.abs(Number(v)))));
  const scale = Math.ceil(max / 2) * 2;
  const y = (v) => top + (h - top - bottom) * (scale - v) / (scale * 2);
  const x = (i) => left + i * (w - left - right) / Math.max(1, samples.length - 1);
  let svg = `<svg class="waveform" viewBox="0 0 ${w} ${h}" role="img" aria-label="解码样本的 X、Y、Z 三轴加速度波形，纵轴单位 g，横轴包内样本序号">`;
  [-scale, 0, scale].forEach((v) => {
    svg += `<path d="M${left} ${y(v)}H${w - right}" stroke="#e2e9e2"/><text x="8" y="${y(v) + 4}">${v} g</text>`;
  });
  ['#0b7560', '#8b6da8', '#dc744d'].forEach((color, axis) => {
    const points = samples.map((s, i) => `${x(i)},${y(Number(s.g[axis]))}`).join(' ');
    svg += `<polyline points="${points}" fill="none" stroke="${color}" stroke-width="2"/><text class="axis-${axis}" x="${w - 170 + axis * 50}" y="15">${['X', 'Y', 'Z'][axis]}</text>`;
  });
  svg += `<text x="${left}" y="${h - 9}">0</text><text x="${w - 135}" y="${h - 9}">样本 #${samples.length - 1}</text></svg>`;
  return svg;
}

function rfResultHtml(r) {
  const rows = r.rows.map((x) => x.missing
    ? `<tr><td>${x.f.off}</td><td><code>${x.f.n}</code></td><td colspan="2" class="bad">数据不足</td></tr>`
    : `<tr><td>${x.f.off}</td><td><code>${x.f.n}</code></td><td>${Array.isArray(x.v) ? '' : x.v}</td><td>${x.text}</td></tr>`);
  let html = `<div class="result-header"><h3>${r.p.title}</h3><span>V1 · ${r.len} B · 小端序</span></div>`;
  if (r.samples && r.samples.length) html += sampleWaveform(r.samples);
  html += `<div class="table-scroll"><table class="tbl"><thead><tr><th>偏移</th><th>字段</th>` +
    `<th>值</th><th>含义</th></tr></thead><tbody>${rows.join('')}</tbody></table></div>`;
  if (r.samples && r.samples.length) {
    const s = r.samples.map((v, i) => `<tr><td>${i}</td><td>${v.raw.join(', ')}</td><td>${v.g.join(', ')}</td></tr>`);
    html += `<details class="sample-details"><summary>查看 ${s.length} 个样本原始值与 g 值（x, y, z）</summary><div class="table-scroll"><table class="tbl small"><thead><tr><th>包内 #</th><th>原始 LSB</th><th>g</th></tr></thead>` +
      `<tbody>${s.join('')}</tbody></table></div></details>`;
  }
  if (r.conf) html += `<p>确认表：${r.conf.map((c) => `node ${c.node} / seq_lo ${c.seqLo}`).join('；') || '空'}</p>`;
  return html;
}

function hostResultHtml(h) {
  const ok = h.crcStored === h.crcCalc;
  const type = PROTO.hostTypes.find((r) => parseInt(r[0], 16) === h.type);
  let html = `<div class="decode-status${ok ? '' : ' error'}"><strong>${type ? type[1] : '未知主机类型'} · ${hex(h.type, 2)}</strong>` +
    `<span>frame_seq ${h.seq} · payload ${h.len} B</span><b class="${ok ? 'good' : 'bad'}">CRC ${ok ? '正确' : '错误'}</b></div>` +
    `<p class="subtle">帧内 CRC：${hex(h.crcStored, 4)} · 计算 CRC：${hex(h.crcCalc, 4)} · CCITT-FALSE（低字节先存）</p>`;
  if (!ok) return html + '<p class="decode-note">校验未通过，停止解析负载。检查帧边界、CRC 参数与字节是否完整。</p>';
  if (h.payloadError) html += `<p class="bad">负载格式错误：${escapeHtml(h.payloadError)}</p>`;
  if (h.rx) html += `<p>RF_RX：网关 ${h.rx.gw} · rx_us ${h.rx.rxUs} µs · RSSI ${h.rx.rssi} dBm · rf_len ${h.rx.rfLen} B</p>`;
  if (h.rf) return html + rfResultHtml(h.rf);
  if (h.fields) {
    return html + `<div class="table-scroll"><table class="tbl"><thead><tr><th>字段</th><th>值</th></tr></thead><tbody>` +
      h.fields.map(([k, v]) => `<tr><td><code>${k}</code></td><td>${escapeHtml(v)}</td></tr>`).join('') + '</tbody></table></div>';
  }
  if (h.type === 0x83 && !h.payloadError) return html + '<p>GET_INFO 请求：无负载。</p>';
  if ([0x05, 0x84].includes(h.type)) html += '<p class="decode-note">源文档尚未定义此类型的完整字节布局，以下显示原始负载。</p>';
  else if (!type) html += '<p class="decode-note">类型未定义，以下显示原始负载。</p>';
  return html + `<div class="raw-payload">${bytesToHex(h.payload) || '空负载'}</div>`;
}

function decodeInput() {
  const out = $('#dec-out');
  try {
    const b = hexToBytes($('#dec-in').value);
    if (b[0] === 0xA5 && b[1] === 0x5A) {
      out.innerHTML = hostResultHtml(decodeHost(b));
      return;
    }
    out.innerHTML = '<p class="decode-note">无线负载本身不带本页可校验的主机 CRC；无线硬件 CRC 不在输入范围内。</p>' + rfResultHtml(decodeRf(b));
  } catch (e) {
    out.innerHTML = '<div class="decode-status error"><p class="bad"></p></div>';
    out.querySelector('p').textContent = `解码失败：${e.message}`;
  }
}

function loadExample(bytes) {
  $('#dec-in').value = bytesToHex(bytes);
  decodeInput();
}

function initDecoder() {
  $('#dec-run').addEventListener('click', decodeInput);
  $('#dec-ex-rf').addEventListener('click', () => loadExample(makeExampleData()));
  $('#dec-ex-host').addEventListener('click', () => loadExample(wrapHostRfRx(makeExampleData(), 1, 987654321, -67)));
  $('#dec-ex-ack').addEventListener('click', () => loadExample(makeExampleAck()));
  $('#dec-ex-time').addEventListener('click', () => loadExample(hexToBytes('A5 5A 81 01 04 00 01 00 00 00 18 43')));
  $('#load-vector').addEventListener('click', () => loadExample(hexToBytes('A5 5A 83 00 00 00 24 C2')));
  $('#dec-in').addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); decodeInput(); }
  });
}
