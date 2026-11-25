// 页面层只负责串口生命周期、视图和导出；协议与数学分析保持独立。
(function () {
  'use strict';
  const P = globalThis.BmiProtocol;
  const A = globalThis.BmiAnalysis;
  const T = globalThis.BmiTimeline;
  const AT = globalThis.BmiAttitude;
  const $ = id => document.getElementById(id);
  const AXES = ['x', 'y', 'z'];
  const COLORS = { x: '#f2a47e', y: '#a9dfb8', z: '#89b7f0' };
  const CHART = {
    grid: '#29343b', gridStrong: '#3a484e', gridMinor: '#253139', label: '#7c909d',
    selectionFill: '#609cf343', selectionLine: '#8ab8ff', marker: '#ffd278',
  };
  const CAPACITY = 96000;
  const MAX_SESSIONS = 4;
  const STREAM_CTRL = 0x02;
  const STREAM_TIMEOUT_MS = 6000;
  const STATUS_FRESH_MS = 5000;
  const nf = new Intl.NumberFormat('zh-CN');

  const data = {
    store: new P.SessionStore({ capacity: CAPACITY }), selected: '',
    parser: new P.StreamParser({ onFrame: handleFrame }),
    hello: null, gatewayStatus: null, formatErrors: 0, bytes: 0, lastFrameAt: 0,
    lastDataAt: new Map(), histories: new Map(), qualities: new Map(), rttMs: null, pendingRequests: new Map(), evicted: 0,
  };
  let mode = 'disconnected';
  let port = null;
  let reader = null;
  let writer = null;
  let readTask = null;
  let closing = null;
  let writeChain = Promise.resolve();
  let timer = null;
  let hostSeq = 0;
  let requestId = 0;
  let connectedAt = 0;
  let sessionOptions = '';
  let cachedAnalysis = { key: '', stats: null, spectra: {} };
  const spectrumToggles = Object.fromEntries(AXES.map(axis => [axis, $(`spectrumToggle-${axis}`)]));
  let dirty = true;
  let lastDraw = 0;
  let lastMetrics = 0;
  let lastByteCheck = { at: performance.now(), bytes: 0, rate: 0 };
  let noticeKey = '';
  let customWindowSeconds = 20;
  const waveCanvas = $('waveCanvas');
  const attitudeCanvas = $('attitudeCanvas');
  const attitudeView = new AT.SensorRenderer(attitudeCanvas);
  const attitudeFilter = new AT.AttitudeFilter({ tau: 0.12 });
  let attitudeTarget = null;
  let attitudeOrbit = null;
  let lastAttitudeDraw = 0;
  let themeLocked = false;
  try { themeLocked = !!localStorage.getItem('bmi-theme'); } catch (_) { themeLocked = false; }
  const viewport = new T.WaveViewport();
  let waveSession = null;
  let waveChart = null;
  let waveDrag = null;
  let waveMarker = null;
  let waveDirty = false;
  const streamNodes = new Map();
  let streamPort = null;
  const streamRequests = new Set();
  let streamReplyRequest = null;
  let streamCommandId = 0;
  const sleepDialog = $('sleepDialog');
  let sleepDialogNode = null;

  function notice(message, style = '') {
    if (noticeKey === `${style}:${message}`) return;
    noticeKey = `${style}:${message}`;
    $('notice').className = `notice ${style}`;
    $('noticeText').textContent = message;
  }
  function chosenSession() { return data.store.sessions.get(data.selected) || null; }
  function count(value) { return nf.format(value || 0); }
  function text(id, value) { const el = $(id); if (el.textContent !== String(value)) el.textContent = value; }
  function setMode(next) {
    mode = next;
    document.body.dataset.mode = next;
    const live = next === 'live';
    const busy = next === 'connecting' || next === 'disconnecting';
    const badge = $('connectionBadge');
    badge.classList.toggle('live', live);
    badge.querySelector('span').textContent = { live: '串口已连接', disconnected: '未连接', connecting: '正在连接', disconnecting: '正在断开' }[next];
    $('connectButton').querySelector('span').textContent = live ? '断开连接' : busy ? '请稍候' : '连接网关';
    $('connectButton').disabled = busy || !('serial' in navigator) || !isSecureContext;
    renderStreamControls();
    dirty = true;
  }

  function cssVariable(name, fallback) {
    const value = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return value || fallback;
  }

  function syncThemeColors() {
    COLORS.x = cssVariable('--x', COLORS.x);
    COLORS.y = cssVariable('--y', COLORS.y);
    COLORS.z = cssVariable('--z', COLORS.z);
    CHART.grid = cssVariable('--chart-grid', CHART.grid);
    CHART.gridStrong = cssVariable('--chart-grid-strong', CHART.gridStrong);
    CHART.gridMinor = cssVariable('--chart-grid-minor', CHART.gridMinor);
    CHART.label = cssVariable('--chart-label', CHART.label);
    CHART.selectionFill = cssVariable('--chart-selection-fill', CHART.selectionFill);
    CHART.selectionLine = cssVariable('--chart-selection-line', CHART.selectionLine);
    CHART.marker = cssVariable('--chart-marker', CHART.marker);
    attitudeView.setPalette({
      axisX: COLORS.x, axisY: COLORS.y, axisZ: COLORS.z,
      pcbTop: cssVariable('--model-pcb', '#2a4f40'),
      pcbBottom: cssVariable('--model-pcb-bottom', '#1b352b'),
      edge: cssVariable('--model-edge', '#3f6d5a'),
      chip: cssVariable('--model-chip', '#141d19'),
      chipEdge: cssVariable('--model-chip-edge', '#33463e'),
      connector: cssVariable('--model-connector', '#8c9a92'),
      grid: cssVariable('--model-grid', '#2c3b35'),
      shadow: cssVariable('--model-shadow', '#000000'),
      accent: cssVariable('--accent', '#b5ebc7'),
      panel: cssVariable('--panel', '#171e23'),
      text: cssVariable('--text', '#e7edf0'),
      muted: cssVariable('--muted', '#86959e'),
    });
  }

  function applyTheme(theme, persist = true) {
    const next = theme === 'light' ? 'light' : 'dark';
    document.documentElement.dataset.theme = next;
    if (persist) {
      themeLocked = true;
      try { localStorage.setItem('bmi-theme', next); } catch (_) { /* 本地文件仍可正常切换 */ }
    }
    const button = $('themeButton');
    const label = next === 'light' ? '切换到深色主题' : '切换到浅色主题';
    button.setAttribute('aria-pressed', String(next === 'light'));
    button.setAttribute('aria-label', label);
    button.title = label;
    const meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.content = next === 'light' ? '#f3f6f4' : '#101519';
    syncThemeColors();
    dirty = true;
    waveDirty = true;
  }

  function streamNode(session = chosenSession()) {
    const node = session && streamNodes.get(session.nodeId);
    return node && node.bootId === session.bootId ? node : null;
  }

  function renderSleepDialog(node, session) {
    if (sleepDialog.open && (node !== sleepDialogNode || !node?.motionSleeping || mode !== 'live')) {
      sleepDialog.close();
    }
    if (mode !== 'live' || !node?.motionSleeping || !node.sleepNoticePending || $('helpDialog').open) return;
    text('sleepDialogTitle', `节点 ${session.nodeId} 已休眠`);
    sleepDialogNode = node;
    node.sleepNoticePending = false;
    sleepDialog.showModal();
  }

  function streamRequestFor(node) {
    return [...streamRequests].reverse().find(request => request.node === node) || null;
  }

  function renderStreamControls(now = performance.now()) {
    const session = chosenSession();
    const node = streamNode(session);
    const request = streamRequestFor(node);
    const online = mode === 'live' && node?.status && now - node.statusAt <= STATUS_FRESH_MS;
    const running = node?.runningAt !== null && now - node?.runningAt <= STATUS_FRESH_MS;
    const available = online && !node.motionSleeping && !request?.blocking && !streamReplyRequest;
    $('stopStreamButton').disabled = !available || node.stoppedAt !== null;
    $('startStreamButton').disabled = !available || running;
    let message = '连接网关后可控制采集';
    let style = '';
    if (mode === 'live') {
      if (node?.motionSleeping) message = `节点 ${session.nodeId} 已休眠，晃动设备恢复采集`;
      else if (streamReplyRequest && streamReplyRequest.node !== node) {
        message = `节点 ${streamReplyRequest.nodeId} · 正在等待网关应答…`;
      }
      else if (request && (request.blocking || !node.feedback)) {
        message = `节点 ${request.nodeId} · ${request.message}`;
        style = request.style || '';
      }
      else if (streamReplyRequest) {
        message = '未收到网关应答，可重连网关后重试';
        style = 'warn';
      }
      else if (session && !node && streamNodes.has(session.nodeId)) message = '请选择该节点当前的采集会话';
      else if (node?.feedback && (online || node.feedback.style === 'warn')) ({ message, style } = node.feedback);
      else if (online) message = `节点 ${session.nodeId} 在线，可控制采集`;
      else message = '等待所选节点的状态回报';
    } else if (mode === 'disconnected') {
      if (request) {
        message = `节点 ${request.nodeId} · ${request.message}`;
        style = 'warn';
      } else if (node?.feedback?.style === 'warn') ({ message, style } = node.feedback);
    }
    text('streamStatus', message);
    $('streamStatus').className = `stream-status ${style}`;
  }

  function finishStreamRequest(request, message, style) {
    if (!streamRequests.has(request)) return;
    clearTimeout(request.timer);
    streamRequests.delete(request);
    request.node.feedback = { message: `节点 ${request.nodeId} · ${message}`, style };
    renderStreamControls();
  }

  function retireEarlierStreamRequests(request) {
    // 新命令被接纳或确认，说明该节点更早的命令已不占网关待执行槽。
    for (const earlier of streamRequests) {
      if (earlier === request) break;
      if (earlier.node !== request.node) continue;
      clearTimeout(earlier.timer);
      streamRequests.delete(earlier);
    }
  }

  function confirmStreamRequest(request) {
    if (!streamRequests.has(request)) return;
    if (request.queued && request.confirmed) {
      retireEarlierStreamRequests(request);
      request.node.stoppedAt = request.start ? null : request.boundary;
      request.node.runningAt = request.start ? performance.now() : null;
      finishStreamRequest(request, request.start ? '已恢复采集' : '已停止采集', 'good');
    } else if (request.timedOut && request.start && request.nodeAck && request.boundary !== null) {
      finishStreamRequest(request, '恢复未确认，仍未收到新样本，可重试恢复', 'warn');
    }
  }

  function watchStreamRequest(request) {
    clearTimeout(request.timer);
    request.timedOut = false;
    request.style = '';
    request.timer = setTimeout(() => {
      if (!streamRequests.has(request)) return;
      // v1.4 的命令槽没有过期或取消；超时后仍需跟踪迟到的节点确认。
      request.timedOut = true;
      request.style = 'warn';
      request.message = '等待超时，命令未取消，继续等待节点回报';
      confirmStreamRequest(request);
      renderStreamControls();
    }, STREAM_TIMEOUT_MS);
  }

  async function sendStreamRequest(request) {
    request.message = request.start ? '正在发送恢复命令…' : '正在发送停采命令…';
    // CMD_RESP 没有命令 ID，等这帧回包后才能发送下一条命令。
    streamReplyRequest = request;
    watchStreamRequest(request);
    renderStreamControls();
    try {
      await send(P.HOST.QUEUE_DOWNLINK, Uint8Array.of(request.nodeId, request.id, STREAM_CTRL, 1, Number(request.start)));
    } catch (error) {
      await disconnect(`串口发送中断：${error.message || error.name}。`, true);
    }
  }

  function handleStreamResponse(response) {
    if (!streamReplyRequest || response.reqType !== P.HOST.QUEUE_DOWNLINK) return;
    const request = streamReplyRequest;
    streamReplyRequest = null;
    if (!streamRequests.has(request)) { renderStreamControls(); return; }
    if (response.result !== 0) {
      const message = response.result === 3 ? '网关还有待执行命令，请等节点回报后重试' : '网关未接纳本次命令';
      finishStreamRequest(request, message, 'warn');
      return;
    }
    retireEarlierStreamRequests(request);
    request.queued = true;
    if (!request.nodeAck && !request.timedOut) request.message = '网关已排队，等待节点确认…';
    confirmStreamRequest(request);
    renderStreamControls();
  }

  function observeStreamPacket(packet, event, now) {
    if (mode !== 'live') return;
    let node = streamNodes.get(packet.nodeId);
    if (!node || node.bootId !== packet.bootId) {
      // boot_id 递增，迟到的旧会话不能重新成为控制目标。
      if (node && ((packet.bootId - node.bootId) & 0xffff) >= 0x8000) return;
      const previous = node;
      if (previous && streamNode() === previous) data.selected = event.session.key;
      node = { bootId: packet.bootId, status: null, statusAt: 0, dataAck: 0, stoppedAt: null,
        runningAt: null, feedback: null, motionSleeping: false, sleepNoticePending: false,
        previousBoot: previous?.bootId };
      streamNodes.set(packet.nodeId, node);
      for (const request of streamRequests) if (request.nodeId === packet.nodeId) {
        finishStreamRequest(request, '节点已重启，旧命令仍可能执行', 'warn');
        node.feedback = request.node.feedback;
      }
    }

    const freshStatus = packet.kind === 'status' && event.session.latestStatus === packet;
    if (freshStatus) {
      node.status = packet;
      node.statusAt = now;
      // A delayed sleep report may arrive after the first DATA from the next segment.
      if (node.motionSleeping !== packet.motionSleeping && !(packet.motionSleeping && event.outOfOrder)) {
        node.motionSleeping = packet.motionSleeping;
        node.sleepNoticePending = packet.motionSleeping;
        node.runningAt = node.motionSleeping ? null : now;
        node.feedback = null;
      }
    }
    if (packet.kind === 'data') {
      node.dataAck = packet.cmdAck;
      if (node.motionSleeping && event.added &&
          P.unwrap32(packet.firstSampleIdx, node.status.sampleIdxNow) >= node.status.sampleIdxNow) {
        node.motionSleeping = false;
        node.sleepNoticePending = false;
        node.runningAt = now;
      }
      if (event.added && node.stoppedAt !== null && P.unwrap32(packet.firstSampleIdx, node.stoppedAt) >= node.stoppedAt) {
        node.stoppedAt = null;
        node.runningAt = now;
        node.feedback = null;
      }
      if (node.runningAt !== null && event.added && !event.outOfOrder) node.runningAt = now;
    }

    for (const request of [...streamRequests]) {
      if (request.node !== node) continue;
      if (freshStatus && packet.lastCmdId === request.id) {
        request.nodeAck = request.queued = true;
        if (request.boundary === null) request.boundary = packet.sampleIdxNow;
        if (!request.start) request.confirmed = true;
        else request.message = '节点已应答，等待新样本…';
      }
      if (packet.kind === 'data' && packet.cmdAck === request.id) {
        request.nodeAck = request.queued = true;
        if (request.start && event.added && request.boundary !== null &&
            P.unwrap32(packet.firstSampleIdx, request.boundary) >= request.boundary) request.confirmed = true;
      }
      confirmStreamRequest(request);
    }
  }

  async function controlStream(start) {
    const session = chosenSession();
    const node = streamNode(session);
    if (mode !== 'live' || !writer || streamRequestFor(node)?.blocking || streamReplyRequest || !node?.status || node.motionSleeping ||
        performance.now() - node.statusAt > STATUS_FRESH_MS || (!start && node.stoppedAt !== null) ||
        (start && node.runningAt !== null && performance.now() - node.runningAt <= STATUS_FRESH_MS)) return;
    // 避开节点最近一次已执行的命令 ID；0 是无命令。
    do { streamCommandId = streamCommandId % 255 + 1; }
    while (streamCommandId === node.status.lastCmdId || streamCommandId === node.dataAck ||
      [...streamRequests].some(request => request.node === node && request.id === streamCommandId));
    const request = {
      node, nodeId: session.nodeId, id: streamCommandId, start,
      queued: false, nodeAck: false, confirmed: false, blocking: true, boundary: start ? node.stoppedAt : null,
    };
    streamRequests.add(request);
    node.feedback = null;
    await sendStreamRequest(request);
  }

  function handleFrame(frame) {
    data.lastFrameAt = performance.now();
    let decoded;
    try { decoded = P.decodeFrame(frame); }
    catch (_) { data.formatErrors++; return; }
    if (decoded.kind === 'hello') data.hello = decoded;
    else if (decoded.kind === 'gw_status') data.gatewayStatus = decoded;
    else if (decoded.kind === 'time_resp') {
      const sent = data.pendingRequests.get(decoded.reqId);
      if (sent !== undefined) {
        data.rttMs = performance.now() - sent;
        data.pendingRequests.delete(decoded.reqId);
      }
    } else if (decoded.kind === 'cmd_resp') handleStreamResponse(decoded);
    else if (decoded.kind === 'rf_rx') {
      const event = data.store.ingest(decoded);
      if (!event) return;
      observeStreamPacket(decoded.rf, event, data.lastFrameAt);
      if (event.added || (!event.duplicate && !event.stale && decoded.rf === event.session.latestStatus)) {
        const session = event.session;
        let quality = data.qualities.get(session.key);
        if (!quality) { quality = new A.ReceptionQuality(); data.qualities.set(session.key, quality); }
        quality.observe({ received: session.receivedSamples, indexMissing: session.missingSamples,
          fifoMissing: session.statusDelta?.fifoGapSamples || 0,
          ringMissing: session.statusDelta?.ringDropSamples || 0 }, data.lastFrameAt / 1000);
      }
      if (event.added) {
        let history = data.histories.get(event.session.key);
        if (!history) { history = new T.History(); data.histories.set(event.session.key, history); }
        if (event.outOfOrder) {
          const latest = event.session.getSamples(1)[0];
          if (latest && history.latestIndex === latest.index) {
            const shift = latest.nodeTime - history.latestTime;
            history.rebase(shift);
            if (event.session === waveSession && shift) {
              viewport.rebase(shift);
              if (waveMarker !== null) waveMarker += shift;
              finishWaveDrag();
              waveChart = null;
              waveDirty = true;
            }
          }
        }
        const samples = event.outOfOrder
          ? event.session.getSamples().filter(sample => sample.seq === decoded.rf.seq)
          : event.session.getSamples(event.added);
        history.ingest(samples, Date.now());
        data.lastDataAt.set(event.session.key, performance.now());
      }
      if (!chosenSession()) data.selected = event.session.key;
      while (data.store.sessions.size > MAX_SESSIONS) {
        const key = [...data.store.sessions.keys()].find(value => value !== data.selected && value !== event.session.key);
        if (!key) break;
        data.store.sessions.delete(key);
        data.lastDataAt.delete(key);
        data.histories.delete(key);
        data.qualities.delete(key);
        data.evicted++;
      }
    }
    dirty = true;
  }

  async function send(type, payload = new Uint8Array()) {
    if (mode !== 'live' || !writer) return;
    const currentWriter = writer;
    const frame = P.makeHostFrame(type, hostSeq++ & 255, payload);
    writeChain = writeChain.then(() => currentWriter.write(frame));
    return writeChain;
  }
  async function requestTime() {
    if (mode !== 'live' || data.pendingRequests.size >= 4) return;
    const id = ++requestId >>> 0;
    const payload = new Uint8Array(4);
    new DataView(payload.buffer).setUint32(0, id, true);
    data.pendingRequests.set(id, performance.now());
    await send(P.HOST.TIME_REQ, payload);
  }
  async function receive() {
    let failure = '';
    const activeReader = reader;
    try {
      while (mode === 'live') {
        const { value, done } = await activeReader.read();
        if (done) break;
        if (value) {
          data.bytes += value.length;
          data.parser.push(value);
        }
      }
    } catch (error) {
      failure = `串口读取中断：${error.message || error.name}。检查 USB 连接后可重新连接。`;
    } finally {
      activeReader.releaseLock();
      if (reader === activeReader) reader = null;
      if (mode === 'live') queueMicrotask(() => disconnect(failure || '串口数据流已结束，可重新连接。', true));
    }
  }
  async function connect() {
    if (mode === 'live') return disconnect();
    if (mode !== 'disconnected') return;
    setMode('connecting');
    notice('请在浏览器的串口列表中选择 BMI323 Gateway。');
    let selectedPort = null;
    let opened = false;
    try {
      const authorized = await navigator.serial.getPorts();
      const gateways = authorized.filter(candidate => {
        const info = candidate.getInfo();
        return info.usbVendorId === 0x1a86 && info.usbProductId === 0x8040;
      });
      selectedPort = gateways.length === 1 ? gateways[0] : await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x1a86, usbProductId: 0x8040 }] });
      await selectedPort.open({ baudRate: 115200, bufferSize: 65536 });
      opened = true;
      port = selectedPort;
      // 只丢弃上一次断开的半帧，保留本页已接收的数据与诊断计数。
      data.parser.used = 0;
      data.pendingRequests.clear();
      data.rttMs = null;
      if (streamPort && streamPort !== selectedPort) {
        for (const request of streamRequests) finishStreamRequest(request, '已切换网关，原网关命令仍可能执行', 'warn');
        streamNodes.clear();
      }
      streamPort = selectedPort;
      for (const node of streamNodes.values()) { node.statusAt = -Infinity; node.runningAt = null; }
      streamReplyRequest = null;
      for (const request of streamRequests) {
        request.blocking = false;
        request.message = '已重连，旧命令结果待确认，可手动操作';
      }
      writer = port.writable.getWriter();
      reader = port.readable.getReader();
      writeChain = Promise.resolve();
      connectedAt = performance.now();
      setMode('live');
      readTask = receive();
      await port.setSignals({ dataTerminalReady: true });
      await send(P.HOST.GET_INFO);
      await requestTime();
      notice('网关已连接，正在接收。节点上电后会自动出现在采集会话中。');
      timer = setInterval(() => {
        const now = performance.now();
        for (const [id, sent] of data.pendingRequests) if (now - sent > 5000) data.pendingRequests.delete(id);
        requestTime().catch(error => disconnect(`串口发送中断：${error.message || error.name}。`, true));
      }, 1000);
    } catch (error) {
      if (opened) await disconnect(`连接未完成：${error.message || error.name}。请重新连接。`, true);
      else {
        port = null;
        setMode('disconnected');
        if (error.name === 'NotFoundError') notice('尚未选择串口。点击“连接网关”可再次选择。');
        else notice(`串口无法打开：${error.message || error.name}。请关闭其他串口程序后重试。`, 'error');
      }
    }
  }
  async function disconnect(message = '已断开网关。本页缓存仍可分析和导出，点击“连接网关”可继续接收。', error = false) {
    if (closing) return closing;
    if (!port && mode === 'disconnected') return;
    setMode('disconnecting');
    for (const request of streamRequests) {
      clearTimeout(request.timer);
      request.message = '连接已断开，命令未取消，可重连确认结果';
      request.style = 'warn';
    }
    clearInterval(timer);
    timer = null;
    closing = (async () => {
      if (reader) { try { await reader.cancel(); } catch (_) { /* 拔线后的流可能已经结束。 */ } }
      if (readTask) await readTask;
      readTask = null;
      try { await writeChain; } catch (_) { /* 写失败仍需释放锁并关闭端口。 */ }
      if (writer) { writer.releaseLock(); writer = null; }
      if (port) {
        try { await port.setSignals({ dataTerminalReady: false }); } catch (_) { /* 设备可能已移除。 */ }
        try { await port.close(); } catch (_) { /* 拔线会使 close 拒绝，浏览器已释放设备。 */ }
      }
      port = null;
      data.pendingRequests.clear();
      setMode('disconnected');
      notice(message, error ? 'error' : '');
    })();
    try { await closing; } finally { closing = null; }
  }

  function updateSessionOptions() {
    const keys = [...data.store.sessions.keys()];
    const signature = keys.join(',');
    if (signature !== sessionOptions) {
      sessionOptions = signature;
      $('sessionSelect').replaceChildren();
      if (!keys.length) $('sessionSelect').add(new Option('等待节点数据', ''));
      for (const key of keys) {
        const session = data.store.sessions.get(key);
        $('sessionSelect').add(new Option(`节点 ${session.nodeId} / boot ${session.bootId}`, key));
      }
      $('sessionSelect').disabled = !keys.length;
    }
    $('sessionSelect').value = data.selected;
  }
  function updateMetrics(now) {
    updateSessionOptions();
    const session = chosenSession();
    renderStreamControls(now);
    $('exportButton').disabled = !session || !session.sampleCount;
    text('configLabel', session ? `${session.odr || '—'} Hz · ±${session.rangeG} g` : '— Hz · ±— g');
    text('rateValue', session && session.odr ? count(session.odr) : '—');
    const lastData = session && data.lastDataAt.get(session.key);
    const age = lastData ? (now - lastData) / 1000 : Infinity;
    text('rateDetail', !session ? '等待采样配置' : !lastData ? '等待首批样本' : age > 3 ? `距上次样本 ${age.toFixed(1)} s` : '按节点配置换算采样时间');
    text('rssiValue', session && session.lastRssi !== null ? session.lastRssi : '—');
    const signal = session && session.lastRssi !== null ? session.lastRssi : -200;
    const bars = signal > -55 ? 4 : signal > -70 ? 3 : signal > -85 ? 2 : signal > -105 ? 1 : 0;
    $('signalBars').querySelectorAll('i').forEach((el, i) => el.classList.toggle('on', i < bars));
    text('rssiDetail', session && session.gatewayId !== null ? `经网关 ${session.gatewayId} 接收` : '等待无线数据');
    text('samplesValue', session ? count(session.receivedSamples) : '0');
    text('samplesDetail', session ? `当前缓存 ${count(session.sampleCount)} 点 · ${count(session.receivedPackets)} 唯一包` : '按节点 / boot 独立统计');
    text('missingValue', session ? count(session.missingSamples) : '0');
    $('missingValue').style.color = session && session.missingSamples ? 'var(--x)' : '';
    text('missingDetail', session ? `${count(session.segments)} 个采样段 · 迟到补包可填补缺口` : '首末已收编号之间的缺口');
    text('bufferLabel', `原始 ${count(CAPACITY)} 点 / 会话 · 趋势 1 小时`);
    const status = session && session.latestStatus;
    const node = mode === 'live' && streamNode(session);
    const motionSleeping = node?.motionSleeping;
    renderSleepDialog(node, session);
    const stopped = node && node.stoppedAt !== null && now - node.statusAt <= STATUS_FRESH_MS;
    const delta = session && session.statusDelta;
    const firmware = info => info ? `v${info.fwMajor}.${info.fwMinor}` : '—';
    text('firmwareValue', `${firmware(data.hello)} / ${firmware(status)}`);
    text('temperatureValue', status?.temperatureC != null ? `${status.temperatureC.toFixed(1)} °C` : '— °C');
    text('crcValue', `${count(data.parser.stats.crcErrors)} / ${count(data.formatErrors + data.parser.stats.invalidLengths)}`);
    text('duplicatesValue', session ? `${count(session.duplicatePackets)} / ${count(session.outOfOrderPackets)}` : '0 / 0');
    text('fifoValue', delta ? `+${count(delta.fifoGapSamples)}` : '—');
    text('ringValue', delta ? `+${count(delta.ringDropSamples)}` : '—');
    const unhealthy = status && (status.bmiErrReg || delta.fifoGapSamples || delta.ringDropSamples || delta.fifoFullCnt || delta.bmiReinitCnt);
    text('healthBadge', !status ? '等待 STATUS' : motionSleeping ? '已休眠 · 等待晃动' : stopped ? '采集已停止' : age > 3 ? '样本已停更' : unhealthy ? '有异常计数' : '本次未见新增丢样');
    $('healthBadge').className = `health-badge ${status ? (unhealthy || (!motionSleeping && !stopped && age > 3) ? 'warn' : 'good') : ''}`;
    text('statusBaseline', status ? `节点累计：FIFO 丢样 ${count(status.fifoGapSamples)} · 缓存丢样 ${count(status.ringDropSamples)}。上方“+”值从本页首个 STATUS 起算。` : '收到首个 STATUS 后，以其累计计数作为本次观测基线。');

    const gw = data.gatewayStatus;
    const diagnostics = [
      ['主机已收帧', count(data.parser.stats.frames)], ['主机丢弃字节', count(data.parser.stats.discardedBytes)],
      ['TIME 往返', data.rttMs === null ? '—' : `${data.rttMs.toFixed(2)} ms`], ['IMU 错误寄存器', status ? `0x${status.bmiErrReg.toString(16).padStart(4, '0')}` : '—'],
      ['节点 FIFO 满 / 重初始化', status ? `${count(status.fifoFullCnt)} / ${count(status.bmiReinitCnt)}` : '—'], ['节点重传次数', status ? count(status.txRetry) : '—'],
      ['网关 RF CRC / 类型错误', gw ? `${count(gw.crcErr)} / ${count(gw.typeErr)}` : '—'], ['网关队满 / 转发丢包', gw ? `${count(gw.queueFull)} / ${count(gw.netDrop)}` : '—'],
      ['连续缓存范围', session ? `${(session.sampleCount / (session.odr || 1)).toFixed(2)} s（点数等效）` : '—'], ['过旧 / 冲突包', session ? `${count(session.stalePackets)} / ${count(session.conflictingPackets)}` : '—'],
    ];
    $('diagnosticsList').replaceChildren(...diagnostics.map(([label, value]) => {
      const row = document.createElement('div');
      const dt = document.createElement('dt'); dt.textContent = label;
      const dd = document.createElement('dd'); dd.textContent = value;
      row.append(dt, dd); return row;
    }));
    if (now - lastByteCheck.at > 1000) {
      lastByteCheck.rate = Math.max(0, (data.bytes - lastByteCheck.bytes) * 1000 / (now - lastByteCheck.at));
      lastByteCheck.at = now;
      lastByteCheck.bytes = data.bytes;
    }
    text('footerStatus', mode === 'live' ? `USB CDC · ${(lastByteCheck.rate / 1024).toFixed(1)} KiB/s · ${count(data.parser.stats.frames)} 帧` : session ? '串口已断开 · 当前缓存可分析和导出' : '串口未连接');
    if (mode === 'live') {
      if (now - connectedAt > 5000 && (!data.lastFrameAt || data.lastFrameAt < connectedAt)) notice('串口已打开，但还没有收到有效帧。请确认选择的是网关 USB 数据口，并检查网关固件。', 'error');
      else if (motionSleeping) notice(`节点 ${session.nodeId} 已休眠。请晃动设备恢复采集。`, 'sleeping');
      else if (stopped) notice(`节点 ${session.nodeId} 已停止采集，仍可通过网关恢复。`);
      else if (session && !node && streamNodes.has(session.nodeId)) notice(`正在查看节点 ${session.nodeId} 的历史采集会话。`);
      else if (node?.previousBoot !== undefined && !lastData) notice(`节点 ${session.nodeId} 已重启（boot ${node.previousBoot} → ${session.bootId}），等待新样本。`);
      else if (noticeKey.startsWith('sleeping:')) notice('等待所选节点的新样本。');
      else if (now - connectedAt > 6000 && (!lastData || lastData < connectedAt || age > 4)) notice('已连接网关，当前未收到所选节点的新样本。请检查节点供电、固件和无线参数。', 'error');
      else if (lastData && lastData >= connectedAt && age < 3 && data.lastFrameAt >= connectedAt) notice(`正在接收节点 ${session.nodeId} / boot ${session.bootId}。导出 CSV 可保存当前缓存。`);
    }
    if (data.evicted) text('bufferLabel', `最近 ${MAX_SESSIONS} 个会话 · 原始 ${count(CAPACITY)} 点 · 趋势 1 小时`);
    updateAttitudeMetrics(now);
  }

  function prepareCanvas(canvas) {
    const rect = canvas.getBoundingClientRect();
    const ratio = Math.min(window.devicePixelRatio || 1, 2);
    const width = Math.max(100, rect.width);
    const height = Math.max(100, rect.height);
    if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
      canvas.width = Math.round(width * ratio);
      canvas.height = Math.round(height * ratio);
    }
    const ctx = canvas.getContext('2d');
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, width, height);
    return { ctx, width, height, left: 53, top: 15, right: width - 16, bottom: height - 28 };
  }
  function decimal(value) { return Math.abs(value) < .01 && value !== 0 ? value.toExponential(1) : Number(value.toPrecision(3)).toString(); }
  function grid(plot, xMax, yMin, yMax, formatX) {
    const { ctx, left, top, right, bottom } = plot;
    ctx.font = '9px ui-monospace, SFMono-Regular, Menlo, monospace';
    const labels = Array.from({ length: 5 }, (_, i) => formatX(xMax * i / 4));
    // 首尾刻度向图内对齐，给它们与居中的刻度留足间距。
    const labelSpacing = Math.max(...labels.map(label => ctx.measureText(label).width)) * 1.5 + 8;
    const labelStep = (right - left) / 4 >= labelSpacing ? 1 : (right - left) / 2 >= labelSpacing ? 2 : 4;
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
      const y = top + (bottom - top) * i / 4;
      ctx.strokeStyle = i === 2 && yMin < 0 ? CHART.gridStrong : CHART.grid;
      ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(right, y); ctx.stroke();
      ctx.fillStyle = CHART.label; ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
      ctx.fillText(decimal(yMax - (yMax - yMin) * i / 4), left - 10, y);
      const x = left + (right - left) * i / 4;
      ctx.strokeStyle = CHART.gridMinor; ctx.setLineDash([2, 5]);
      ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, bottom); ctx.stroke(); ctx.setLineDash([]);
      if (i % labelStep === 0) {
        ctx.textAlign = i === 0 ? 'left' : i === 4 ? 'right' : 'center'; ctx.textBaseline = 'top';
        ctx.fillText(labels[i], x, bottom + 10);
      }
    }
    ctx.fillStyle = CHART.label; ctx.textAlign = 'right'; ctx.textBaseline = 'top';
    ctx.fillText('g', left - 10, 1);
  }
  function niceScale(value) {
    if (!value || !Number.isFinite(value)) return 2;
    const base = 10 ** Math.floor(Math.log10(value));
    return ([1, 1.25, 2, 2.5, 4, 5, 8, 10].find(n => n * base >= value) || 10) * base;
  }
  function drawWave(session) {
    if (session !== waveSession) {
      waveSession = session;
      viewport.follow();
      finishWaveDrag();
      waveMarker = null;
    }
    const history = data.histories.get(data.selected);
    const rawCount = session?.sampleCount || 0;
    const latest = session?.nodeTime ?? 0;
    const rawStart = session?.firstSampleTime ?? latest;
    const first = Math.min(rawStart, history?.firstTime ?? rawStart);
    const bounds = { first, latest };
    // 拖选时以按下鼠标时的视图为准，实时到达的样本不会移动选区。
    const view = waveDrag?.selecting ? waveDrag.chart.view : viewport.range(bounds);
    const { start, end, seconds } = view;
    const visible = session?.getSamples(session.capacity, view) || [];
    const buckets = history && start < rawStart
      ? history.getBuckets(start, Math.min(end, rawStart))
      : [];
    const older = buckets.filter(bucket => bucket.start < rawStart && bucket.start <= end && bucket.end >= start);
    const selectedAxes = AXES.filter(axis => document.querySelector(`[data-axis="${axis}"]`).checked);
    const offsetMs = history?.offsetMs ?? null;
    const epochMs = value => offsetMs === null ? null : value * 1000 + offsetMs;
    const basis = $('timeBasis').value;
    let amplitude = Number($('amplitudeScale').value);
    if (!amplitude) {
      let maximum = 0;
      for (const sample of visible) for (const axis of selectedAxes) maximum = Math.max(maximum, Math.abs(sample[`g${axis}`]));
      for (const bucket of older) for (const axis of selectedAxes) maximum = Math.max(maximum, Math.abs(bucket[axis].min), Math.abs(bucket[axis].max));
      amplitude = visible.length || older.length ? niceScale(Math.max(.01, maximum * 1.12)) : 2;
    }
    const plot = prepareCanvas(waveCanvas);
    const { ctx, left, top, right, bottom } = plot;
    waveChart = rawCount ? { bounds, view, left, right } : null;
    waveCanvas.classList.toggle('timeline-ready', !!waveChart);
    $('followWaveButton').disabled = !rawCount || viewport.following;
    const relativeDigits = seconds < 1 ? 3 : 2;
    grid(plot, seconds, -amplitude, amplitude, value => basis === 'beijing'
      ? T.beijingTime(epochMs(start + value), seconds < 4)
      : basis === 'node' ? T.nodeTime(start + value, seconds < 4) : `${(value - seconds + (end - latest)).toFixed(relativeDigits)} s`);
    const hasSignal = visible.length || older.length;
    $('waveEmpty').hidden = !!hasSignal;
    text('waveEmptyTitle', rawCount ? '这段时间没有样本' : '等待第一组样本');
    text('waveEmptyHint', rawCount ? '拖动时间轴继续查看' : '连接网关并开始节点采集');
    const viewState = viewport.following ? '跟随最新' : '浏览历史';
    text('waveState', rawCount ? `${viewState}${visible.length ? ` · ${count(visible.length)} 点` : older.length ? ' · 每秒极值' : ''}` : '等待信号');
    text('waveWindowLabel', `${T.span(seconds)} 范围${older.length ? ' · 含每秒极值' : ''}`);
    text('waveAxisLabel', basis === 'beijing' ? '北京时间 UTC+8 · 电脑对齐估算' : basis === 'node' ? '节点时钟 · 连接前完整周期未知' : '距最新样本 · 最新为 0 s');
    text('latestTimeLabel', rawCount ? (basis === 'beijing' ? T.beijingTime(epochMs(latest), true, true) : basis === 'node' ? `节点 ${T.nodeTime(latest, true)}` : '最新样本 0 s') : '等待采样时间');
    if (waveMarker !== null && waveMarker < first) waveMarker = null;
    const markerInView = waveMarker !== null && waveMarker >= start && waveMarker <= end;
    $('waveMarkerLabel').hidden = waveMarker === null;
    if (waveMarker !== null) {
      const time = basis === 'beijing' ? T.beijingTime(epochMs(waveMarker), true, true)
        : basis === 'node' ? T.nodeTime(waveMarker, true) : `${(waveMarker - latest).toFixed(3)} s`;
      text('waveMarkerLabel', `定位 ${time}${markerInView ? '' : ' · 视图外'}`);
    }
    waveCanvas.setAttribute('aria-label', `三轴加速度波形，横轴为${basis === 'beijing' ? '北京时间UTC+8估算' : basis === 'node' ? '节点时钟' : '距最新样本时间'}，纵轴单位g`);
    text('historyLabel', rawCount ? `已保留趋势 ${T.span(latest - first)} · 原始缓存 ${count(rawCount)} 点。${older.length ? '较早波形显示每秒最小/最大值。' : ''}` : '原始波形保留 96,000 点，运行趋势保留最近 1 小时。');
    const sx = value => left + (value - start) / seconds * (right - left);
    const sy = value => top + (amplitude - value) / (amplitude * 2) * (bottom - top);
    ctx.save(); ctx.beginPath(); ctx.rect(left, top, right - left, bottom - top); ctx.clip();
    for (const axis of selectedAxes) {
      ctx.beginPath(); ctx.strokeStyle = COLORS[axis]; ctx.lineWidth = 1.2;
      let bucket = null;
      let started = false;
      function flush() {
        if (!bucket) return;
        if (started && bucket.continuous) ctx.lineTo(bucket.x, sy(bucket.first));
        else ctx.moveTo(bucket.x, sy(bucket.first));
        ctx.lineTo(bucket.x, sy(bucket.min));
        ctx.lineTo(bucket.x, sy(bucket.max));
        ctx.lineTo(bucket.x, sy(bucket.last));
        started = true;
      }
      for (const sample of visible) {
        const x = Math.floor(sx(sample.nodeTime));
        const value = sample[`g${axis}`];
        if (bucket && x === bucket.x && sample.continuous) {
          bucket.min = Math.min(bucket.min, value); bucket.max = Math.max(bucket.max, value); bucket.last = value;
        } else {
          flush();
          bucket = { x, min: value, max: value, first: value, last: value, continuous: sample.continuous };
        }
      }
      flush(); ctx.stroke();
      // 旧数据画极值包络；每个桶独立绘制，不跨缺口或分段补连线。
      ctx.beginPath(); ctx.lineWidth = 1.2;
      for (const bucket of older) {
        const x1 = sx(Math.max(start, bucket.start));
        const x2 = sx(Math.min(end, rawStart, bucket.end));
        if (x2 < x1) continue;
        ctx.moveTo(x1, sy(bucket[axis].min)); ctx.lineTo(x2, sy(bucket[axis].min));
        ctx.moveTo(x1, sy(bucket[axis].max)); ctx.lineTo(x2, sy(bucket[axis].max));
        ctx.moveTo((x1 + x2) / 2, sy(bucket[axis].min)); ctx.lineTo((x1 + x2) / 2, sy(bucket[axis].max));
      }
      ctx.stroke();
    }
    ctx.restore();
    if (waveDrag?.selecting && waveDrag.moved) {
      const x1 = sx(Math.min(waveDrag.start, waveDrag.end));
      const x2 = sx(Math.max(waveDrag.start, waveDrag.end));
      ctx.fillStyle = CHART.selectionFill;
      ctx.fillRect(x1, top, x2 - x1, bottom - top);
      ctx.strokeStyle = CHART.selectionLine;
      ctx.lineWidth = 1;
      ctx.strokeRect(x1, top, x2 - x1, bottom - top);
    }
    if (markerInView) {
      const x = sx(waveMarker);
      ctx.strokeStyle = ctx.fillStyle = CHART.marker;
      ctx.lineWidth = 1.5;
      ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, bottom + 4); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x - 5, top - 7); ctx.lineTo(x + 5, top - 7); ctx.lineTo(x, top + 1); ctx.fill();
    }
  }
  function drawSpectrum(spectra, rate) {
    const plot = prepareCanvas($('spectrumCanvas'));
    const { ctx, left, top, right, bottom } = plot;
    const selected = AXES.filter(axis => spectrumToggles[axis].getAttribute('aria-pressed') === 'true');
    const reference = spectra.x;
    const maximum = Math.max(0, ...selected.map(axis => spectra[axis]?.peakAmplitude || 0));
    const yMax = reference && selected.length ? niceScale(Math.max(.001, maximum * 1.2)) : .2;
    const xMax = (rate || 3200) / 2;
    grid(plot, xMax, 0, yMax, value => `${Math.round(value)}${value === xMax ? ' Hz' : ''}`);
    $('spectrumEmpty').hidden = !!reference || !selected.length;
    $('spectrumHidden').hidden = !!selected.length;
    for (const axis of AXES) {
      const result = spectra[axis];
      text(`spectrumFrequency-${axis}`, result?.peakAmplitude > 1e-9 ? result.peakHz.toFixed(2) : '—');
      text(`spectrumAmplitude-${axis}`, result ? result.peakAmplitude.toFixed(4) : '—');
    }
    text('fftDetail', reference ? `FFT ${count(reference.size)} 点 · Δf ${reference.binHz.toFixed(2)} Hz` : 'FFT — 点');
    if (!reference || !selected.length) return;
    const sx = hz => left + hz / xMax * (right - left);
    const sy = g => bottom - g / yMax * (bottom - top);
    ctx.save(); ctx.beginPath(); ctx.rect(left, top, right - left, bottom - top); ctx.clip();
    for (const axis of selected) {
      const { frequencies, amplitudes } = spectra[axis];
      ctx.beginPath();
      ctx.strokeStyle = COLORS[axis];
      ctx.lineWidth = 1.3;
      // 多个频点落到同一像素列时保留峰值，缩窄图表也不会丢掉窄峰。
      let x = Math.floor(sx(frequencies[1]));
      let peak = amplitudes[1];
      ctx.moveTo(left, bottom);
      for (let k = 2; k < frequencies.length; k++) {
        const column = Math.floor(sx(frequencies[k]));
        if (column === x) peak = Math.max(peak, amplitudes[k]);
        else {
          ctx.lineTo(x, sy(peak));
          x = column;
          peak = amplitudes[k];
        }
      }
      ctx.lineTo(x, sy(peak));
      ctx.stroke();
    }
    for (const axis of selected) {
      const { peakHz, peakAmplitude } = spectra[axis];
      if (peakAmplitude <= 1e-9) continue;
      ctx.beginPath(); ctx.arc(sx(peakHz), sy(peakAmplitude), 3, 0, Math.PI * 2);
      ctx.fillStyle = COLORS[axis]; ctx.fill();
    }
    ctx.restore();
  }
  function drawStats(stats, samples, rate, quality) {
    for (const axis of AXES) {
      const cells = document.querySelector(`[data-stats-axis="${axis}"]`).querySelectorAll('td');
      const values = stats ? [A.formatAcceleration(stats[axis].acRms, true),
        stats[axis].cf === null ? '—' : stats[axis].cf.toFixed(1),
        A.formatAcceleration(stats[axis].acPeak), A.formatAcceleration(stats[axis].peakToPeak)] : Array(4).fill('—');
      cells.forEach((cell, i) => { if (cell.textContent !== values[i]) cell.textContent = values[i]; });
      text(`dc-${axis}`, A.formatAcceleration(stats?.[axis].mean));
    }
    const requested = Number($('statisticsWindow').value);
    const duration = rate ? samples.length / rate : 0;
    const window = `${duration.toFixed(2)} s${duration < requested ? `（不足 ${requested} s）` : ''}`;
    const missingRate = quality.missingRate === null ? '—' : `${(quality.missingRate * 100).toFixed(1)}%`;
    text('statsValidity', `${window} · 缺样 ${missingRate}${quality.isDegraded ? ' ⚠' : ''}`);
    text('statsWindow', `最近连续段 · ${count(samples.length)} 点`);
    text('statsQuality', `缺样率按近 60 s · 已收 ${count(quality.received)} 点 · 缺样 ${count(quality.missing)} 点`);
    document.querySelector('.stats-panel').classList.toggle('degraded', quality.isDegraded);
  }
  function latestGravity(session) {
    if (!session || !session.sampleCount) return null;
    const rate = session.odr || 3200;
    // 约 20 ms 的窗口抑制振动噪声；低采样率时至少取 8 点。
    const wanted = Math.min(96, Math.max(8, Math.round(rate * 0.02)));
    const samples = session.getSamples(wanted);
    if (!samples.length) return null;
    let x = 0, y = 0, z = 0, count = 0;
    for (let i = samples.length - 1; i >= 0; i--) {
      const sample = samples[i];
      x += sample.gx; y += sample.gy; z += sample.gz; count++;
      if (!sample.continuous) break;
    }
    const gx = x / count, gy = y / count, gz = z / count;
    const magnitude = Math.hypot(gx, gy, gz);
    if (!(magnitude > 1e-6)) return null;
    return { x: gx, y: gy, z: gz, magnitude };
  }

  function drawAttitude(now) {
    const session = chosenSession();
    const gravity = latestGravity(session);
    if (gravity) {
      attitudeTarget = gravity;
      attitudeFilter.update(gravity.x, gravity.y, gravity.z, now);
    }
    const lastData = session && data.lastDataAt.get(session.key);
    const stale = !gravity || !lastData || now - lastData > 1500;
    attitudeView.draw(attitudeFilter.quaternion, { stale });
    $('attitudeEmpty').hidden = !!attitudeFilter.quaternion;
  }

  function updateAttitudeMetrics(now) {
    const session = chosenSession();
    const gravity = attitudeTarget;
    const lastData = session && data.lastDataAt.get(session.key);
    const fresh = !!(mode === 'live' && gravity && lastData && now - lastData <= 1500);
    const quaternion = attitudeFilter.quaternion;
    const tilt = quaternion ? AT.tiltFromQuaternion(quaternion) : null;
    text('attitudePitch', tilt ? `${tilt.xElevation.toFixed(1)}°` : '—');
    text('attitudeRoll', tilt ? `${tilt.yElevation.toFixed(1)}°` : '—');
    text('attitudeTilt', tilt ? `${tilt.zTilt.toFixed(1)}°` : '—');
    text('attitudeMagnitude', gravity ? `${gravity.magnitude.toFixed(3)} g` : '—');
    text('attitudeX', gravity ? `${gravity.x.toFixed(3)} g` : '—');
    text('attitudeY', gravity ? `${gravity.y.toFixed(3)} g` : '—');
    text('attitudeZ', gravity ? `${gravity.z.toFixed(3)} g` : '—');
    let badge = '等待样本';
    let style = '';
    if (gravity) {
      if (mode !== 'live') badge = '已断开 · 最后姿态';
      else if (!fresh) { badge = '样本已停更'; style = 'warn'; }
      else if (Math.abs(gravity.magnitude - 1) > 0.12) { badge = '运动干扰 · 姿态含加速度'; style = 'warn'; }
      else badge = '重力解算';
    }
    text('attitudeBadge', badge);
    $('attitudeBadge').className = `health-badge ${style}`;
  }

  function drawCharts() {
    const session = chosenSession();
    const requested = Number($('statisticsWindow').value);
    const limit = Math.min(CAPACITY, Math.max(4096, Math.ceil(requested * (session?.odr || 3200))));
    const samples = session?.getSamples(limit) || [];
    const continuous = A.statisticsSamples(samples, requested);
    const rate = continuous.length ? continuous[continuous.length - 1].odr : session && session.odr;
    const key = `${data.selected}:${session?.receivedSamples ?? 0}:${requested}`;
    if (key !== cachedAnalysis.key) {
      const gSamples = continuous.map(sample => ({ x: sample.gx, y: sample.gy, z: sample.gz }));
      const fftSamples = session?.getContinuousSamples(4096) || [];
      const fftRate = fftSamples.at(-1)?.odr;
      const input = fftSamples.map(sample => ({ x: sample.gx, y: sample.gy, z: sample.gz }));
      const spectra = fftRate ? Object.fromEntries(AXES.map(axis => [axis, A.spectrum(input, fftRate, axis)])) : {};
      cachedAnalysis = { key, stats: A.stats(gSamples), spectra };
    }
    drawWave(session);
    drawSpectrum(cachedAnalysis.spectra, rate);
    const quality = (data.qualities.get(data.selected) || new A.ReceptionQuality()).snapshot(performance.now() / 1000);
    drawStats(cachedAnalysis.stats, continuous, rate, quality);
  }
  function exportCsv() {
    const session = chosenSession();
    if (!session) return;
    const samples = session.getSamples();
    if (!samples.length) return;
    const history = data.histories.get(session.key);
    const columns = ['source', 'node_id', 'boot_id', 'segment', 'segment_raw', 'sample_index', 'sample_index_raw', 'relative_time_s', 'odr_hz', 'range_g', 'cfg', 'packet_seq', 'continuous', 'flags', 'x_lsb', 'y_lsb', 'z_lsb', 'x_g', 'y_g', 'z_g', 'node_time_s', 'beijing_time_estimate', 'node_clock_raw', 'node_clock_unit'];
    const lines = [columns.join(',')];
    for (const sample of samples) lines.push([
      'gateway', sample.nodeId, sample.bootId, sample.segment, sample.rawSegment,
      sample.index, sample.rawIndex, sample.relativeTime.toFixed(9), sample.odr, sample.rangeG, sample.cfg,
      sample.seq, sample.continuous ? 1 : 0, sample.flags, sample.x, sample.y, sample.z, sample.gx, sample.gy, sample.gz,
      sample.nodeTime.toFixed(9), history ? T.beijingIso(history.epochMs(sample.nodeTime)) : '', sample.nodeClockRaw, sample.nodeClockUnit,
    ].join(','));
    const blob = new Blob(['\ufeff', lines.join('\r\n'), '\r\n'], { type: 'text/csv;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = `bmi323-node${session.nodeId}-boot${session.bootId}-${new Date().toISOString().replace(/[:.]/g, '-')}.csv`;
    document.body.append(anchor); anchor.click(); anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
    text('footerStatus', `已导出 ${count(samples.length)} 点 · 节点 ${session.nodeId} / boot ${session.bootId}`);
  }
  function render(now) {
    if (now - lastMetrics > 300) { updateMetrics(now); lastMetrics = now; }
    if ((dirty || mode === 'live') && now - lastDraw > 125 && !document.hidden) {
      drawCharts(); lastDraw = now; dirty = false; waveDirty = false;
    } else if (waveDirty && !document.hidden) {
      drawWave(chosenSession()); waveDirty = false;
    }
    if (!document.hidden && now - lastAttitudeDraw > 33) {
      drawAttitude(now);
      lastAttitudeDraw = now;
    }
    requestAnimationFrame(render);
  }

  $('connectButton').addEventListener('click', connect);
  $('stopStreamButton').addEventListener('click', () => controlStream(false));
  $('startStreamButton').addEventListener('click', () => controlStream(true));
  $('exportButton').addEventListener('click', exportCsv);
  $('themeButton').addEventListener('click', () => {
    applyTheme(document.documentElement.dataset.theme === 'light' ? 'dark' : 'light');
  });
  if (window.matchMedia) {
    const scheme = matchMedia('(prefers-color-scheme: light)');
    scheme.addEventListener('change', event => {
      if (!themeLocked) applyTheme(event.matches ? 'light' : 'dark', false);
    });
  }
  attitudeCanvas.addEventListener('pointerdown', event => {
    if (!event.isPrimary || event.button !== 0) return;
    attitudeOrbit = { id: event.pointerId, x: event.clientX, y: event.clientY };
    attitudeCanvas.setPointerCapture(event.pointerId);
  });
  attitudeCanvas.addEventListener('pointermove', event => {
    if (!attitudeOrbit || attitudeOrbit.id !== event.pointerId) return;
    const dx = event.clientX - attitudeOrbit.x;
    const dy = event.clientY - attitudeOrbit.y;
    attitudeOrbit.x = event.clientX;
    attitudeOrbit.y = event.clientY;
    attitudeView.orbit(dx, dy);
  });
  function finishAttitudeOrbit(event) {
    if (!attitudeOrbit || attitudeOrbit.id !== event.pointerId) return;
    const id = attitudeOrbit.id;
    attitudeOrbit = null;
    if (attitudeCanvas.hasPointerCapture(id)) attitudeCanvas.releasePointerCapture(id);
  }
  for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    attitudeCanvas.addEventListener(type, finishAttitudeOrbit);
  }
  attitudeCanvas.addEventListener('dblclick', () => attitudeView.resetView());
  $('sessionSelect').addEventListener('change', event => {
    data.selected = event.target.value; cachedAnalysis.key = ''; dirty = true;
    attitudeFilter.reset(); attitudeTarget = null;
    renderStreamControls();
  });
  function followLatestWave() {
    finishWaveDrag();
    viewport.follow();
    waveDirty = true;
  }

  function showZoomedRange() {
    $('timeWindow').value = 'zoom';
    $('customWindowLabel').hidden = true;
    waveDirty = true;
  }

  function zoomWave(factor, fraction) {
    if (viewport.zoom(waveChart.bounds, factor, fraction)) showZoomedRange();
  }

  function finishWaveDrag() {
    if (!waveDrag) return;
    const id = waveDrag.id;
    waveDrag = null;
    waveCanvas.classList.remove('timeline-dragging', 'timeline-selecting');
    if (waveCanvas.hasPointerCapture(id)) waveCanvas.releasePointerCapture(id);
    waveDirty = true;
  }

  function waveFraction(chart, clientX) {
    const x = clientX - waveCanvas.getBoundingClientRect().left;
    return (x - chart.left) / (chart.right - chart.left);
  }

  function waveTime(chart, fraction) {
    if (fraction <= 0) return chart.view.start;
    if (fraction >= 1) return chart.view.end;
    return chart.view.start + fraction * chart.view.seconds;
  }

  function moveWaveDrag(event) {
    const drag = waveDrag;
    const dx = event.clientX - drag.x;
    if (!drag.moved && Math.abs(dx) < 3) return;
    drag.moved = true;
    const { chart } = drag;
    if (drag.selecting) drag.end = waveTime(chart, waveFraction(chart, event.clientX));
    else {
      waveCanvas.classList.add('timeline-dragging');
      viewport.pan(waveChart.bounds, chart.view.end, -dx * chart.view.seconds / (chart.right - chart.left));
    }
    waveDirty = true;
  }

  waveCanvas.addEventListener('wheel', event => {
    if (!waveChart || waveSession !== chosenSession() || waveDrag || event.ctrlKey || !event.deltaY) return;
    event.preventDefault();
    const fraction = waveFraction(waveChart, event.clientX);
    const unit = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? waveCanvas.clientHeight : 1;
    const delta = Math.max(-240, Math.min(240, event.deltaY * unit));
    zoomWave(Math.exp(delta * .003), fraction);
  }, { passive: false });
  waveCanvas.addEventListener('pointerdown', event => {
    if (!waveChart || waveSession !== chosenSession() || waveDrag || !event.isPrimary || event.button !== 0) return;
    const fraction = waveFraction(waveChart, event.clientX);
    if (fraction < 0 || fraction > 1) return;
    const start = waveTime(waveChart, fraction);
    waveDrag = { id: event.pointerId, x: event.clientX, chart: waveChart, start, end: start,
      selecting: event.shiftKey, moved: false };
    if (waveDrag.selecting) waveCanvas.classList.add('timeline-selecting');
    waveCanvas.focus({ preventScroll: true });
    waveCanvas.setPointerCapture(event.pointerId);
  });
  waveCanvas.addEventListener('pointermove', event => {
    if (!waveChart || waveSession !== chosenSession() || waveDrag?.id !== event.pointerId) return;
    moveWaveDrag(event);
  });
  waveCanvas.addEventListener('pointerup', event => {
    if (waveDrag?.id !== event.pointerId) return;
    if (waveChart && waveSession === chosenSession()) {
      moveWaveDrag(event);
      const { selecting, moved, start, end, chart } = waveDrag;
      const { first, latest } = waveChart.bounds;
      if (selecting) {
        if (moved && viewport.fit(waveChart.bounds, start, end)) showZoomedRange();
      } else if (!moved && start >= first && start <= latest) {
        waveMarker = start;
        viewport.end = chart.view.end;
      }
    }
    finishWaveDrag();
  });
  for (const type of ['pointercancel', 'lostpointercapture']) {
    waveCanvas.addEventListener(type, event => { if (waveDrag?.id === event.pointerId) finishWaveDrag(); });
  }
  waveCanvas.addEventListener('keydown', event => {
    if (!waveChart || waveSession !== chosenSession() || event.metaKey || event.ctrlKey || event.altKey) return;
    if (waveDrag) {
      if (event.key === 'Escape') { finishWaveDrag(); event.preventDefault(); }
      return;
    }
    const { bounds, view } = waveChart;
    if (event.key === 'Home') followLatestWave();
    else if (event.key === '+' || event.key === '=') zoomWave(.8, .5);
    else if (event.key === '-') zoomWave(1.25, .5);
    else if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      viewport.pan(bounds, view.end, view.seconds * (event.key === 'ArrowLeft' ? -.25 : .25));
      waveDirty = true;
    } else return;
    event.preventDefault();
  });
  $('followWaveButton').addEventListener('click', followLatestWave);
  $('timeWindow').addEventListener('change', () => {
    finishWaveDrag();
    const value = $('timeWindow').value;
    $('customWindowLabel').hidden = value !== 'custom';
    viewport.seconds = value === 'custom' ? customWindowSeconds : value === 'all' ? null : Number(value);
    if (value === 'all') viewport.follow();
    waveDirty = true;
  });
  $('statisticsWindow').addEventListener('change', () => { cachedAnalysis.key = ''; dirty = true; });
  $('customTimeWindow').addEventListener('change', () => {
    finishWaveDrag();
    if ($('customTimeWindow').checkValidity() && $('customTimeWindow').value !== '') {
      customWindowSeconds = Number($('customTimeWindow').value);
      viewport.seconds = customWindowSeconds;
    } else {
      $('customTimeWindow').reportValidity();
      $('customTimeWindow').value = customWindowSeconds;
    }
    waveDirty = true;
  });
  for (const id of ['timeBasis', 'amplitudeScale']) $(id).addEventListener('change', () => { dirty = true; });
  document.querySelectorAll('[data-axis]').forEach(input => input.addEventListener('change', () => { dirty = true; }));
  Object.values(spectrumToggles).forEach(button => button.addEventListener('click', () => {
    const visible = button.getAttribute('aria-pressed') === 'true';
    button.setAttribute('aria-pressed', String(!visible));
    dirty = true;
  }));
  $('helpButton').addEventListener('click', () => $('helpDialog').showModal());
  $('closeSleepButton').addEventListener('click', () => sleepDialog.close());
  $('closeHelpButton').addEventListener('click', () => $('helpDialog').close());
  $('helpDialog').addEventListener('click', event => { if (event.target === $('helpDialog')) { const r = $('helpDialog').getBoundingClientRect(); if (event.clientX < r.left || event.clientX > r.right || event.clientY < r.top || event.clientY > r.bottom) $('helpDialog').close(); } });
  window.addEventListener('resize', () => { dirty = true; });
  document.addEventListener('visibilitychange', () => { dirty = true; });
  window.addEventListener('beforeunload', event => {
    if (mode === 'live' || data.store.sessions.size) { event.preventDefault(); event.returnValue = ''; }
  });
  if ('serial' in navigator) navigator.serial.addEventListener('disconnect', event => {
    if (event.target === port || event.port === port) disconnect('网关 USB 已断开。缓存仍可导出，重新插入后可再次连接。', true);
  });
  applyTheme(document.documentElement.dataset.theme === 'light' ? 'light' : 'dark', false);
  setMode('disconnected');
  if (!('serial' in navigator) || !isSecureContext) notice('当前浏览器不能连接串口。请用桌面版 Chrome 或 Edge 打开此 HTML。', 'error');
  drawAttitude(performance.now());
  updateMetrics(performance.now());
  drawCharts();
  requestAnimationFrame(render);
})();
