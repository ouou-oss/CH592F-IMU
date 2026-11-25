// 节点时钟与北京时间展示；每秒桶保留极值和 AC RMS，不代替原始样本或 FFT 输入。
(function (root, factory) {
  'use strict';
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.BmiTimeline = api;
})(typeof globalThis === 'object' ? globalThis : this, function () {
  'use strict';
  const AXES = ['x', 'y', 'z'];
  const pad = (value, length = 2) => String(value).padStart(length, '0');

  function beijingTime(epochMs, milliseconds = false, date = false) {
    if (!Number.isFinite(epochMs)) return '—';
    // 使用固定 UTC+8，不能继承浏览器/操作系统时区。
    const d = new Date(Math.round(epochMs) + 8 * 3600000);
    const prefix = date ? `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())} ` : '';
    return `${prefix}${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())}${milliseconds ? `.${pad(d.getUTCMilliseconds(), 3)}` : ''}`;
  }
  function beijingIso(epochMs) {
    return Number.isFinite(epochMs) ? `${beijingTime(epochMs, true, true).replace(' ', 'T')}+08:00` : '';
  }
  function nodeTime(seconds, milliseconds = false) {
    const ticks = Math.round(Math.abs(seconds) * 1000);
    const whole = Math.floor(ticks / 1000);
    const negative = seconds < 0 && (milliseconds ? ticks : whole) ? '−' : '';
    return `${negative}${pad(Math.floor(whole / 3600))}:${pad(Math.floor(whole / 60) % 60)}:${pad(whole % 60)}${milliseconds ? `.${pad(ticks % 1000, 3)}` : ''}`;
  }
  function span(seconds) {
    if (seconds < 1) return `${Number((seconds * 1000).toFixed(2))} ms`;
    if (seconds < 60) return `${Number(seconds.toFixed(2))} s`;
    return `${Number((seconds / 60).toFixed(2))} min`;
  }

  const MIN_VIEW_SECONDS = .05;
  const MAX_VIEW_SECONDS = 3600;
  const clamp = (value, min, max) => Math.min(max, Math.max(min, value));

  class WaveViewport {
    constructor() {
      this.seconds = 1;
      this.end = null;
    }

    get following() { return this.end === null; }
    follow() { this.end = null; }
    rebase(shift) { if (!this.following) this.end += shift; }

    range({ first, latest }) {
      const seconds = clamp(this.seconds ?? (latest - first), MIN_VIEW_SECONDS, MAX_VIEW_SECONDS);
      const end = clamp(this.end ?? latest, Math.min(latest, first + seconds), latest);
      return { start: end - seconds, end, seconds };
    }

    zoom(bounds, factor, fraction) {
      const view = this.range(bounds);
      const seconds = clamp(view.seconds * factor, MIN_VIEW_SECONDS, MAX_VIEW_SECONDS);
      if (seconds === view.seconds) return false;
      const afterCursor = 1 - clamp(fraction, 0, 1);
      this.seconds = seconds;
      this.end = view.end + (seconds - view.seconds) * afterCursor;
      this.end = this.range(bounds).end;
      return true;
    }

    fit(bounds, a, b) {
      const start = Math.max(bounds.first, Math.min(a, b));
      const end = Math.min(bounds.latest, Math.max(a, b));
      if (end <= start) return false;
      this.seconds = clamp(end - start, MIN_VIEW_SECONDS, MAX_VIEW_SECONDS);
      this.end = start + (end - start + this.seconds) / 2;
      this.end = this.range(bounds).end;
      return true;
    }

    pan(bounds, originEnd, seconds) {
      const previous = this.range(bounds).end;
      const wasFollowing = this.following;
      this.end = originEnd + seconds;
      const next = this.range(bounds).end;
      this.end = wasFollowing && previous === next ? null : next;
    }
  }

  class History {
    constructor({ maxSeconds = 3600, maxBuckets = 4096 } = {}) {
      this.maxSeconds = maxSeconds;
      this.maxBuckets = maxBuckets;
      this.buckets = new Map();
      this.offsetMs = null;
      this.latestTime = null;
      this.latestIndex = null;
      this._lastPrunedSecond = null;
    }

    ingest(samples, receivedMs) {
      if (!samples.length) return;
      let newest = -Infinity;
      for (const sample of samples) {
        const time = sample.nodeTime;
        if (!Number.isFinite(time)) continue;
        newest = Math.max(newest, time);
        this.latestIndex = this.latestIndex === null ? sample.index : Math.max(this.latestIndex, sample.index);
        const second = Math.floor(time);
        const key = `${second}:${sample.segment}:${sample.cfg}`;
        let bucket = this.buckets.get(key);
        if (!bucket) {
          bucket = { start: time, end: time, firstIndex: sample.index, lastIndex: sample.index, segment: sample.segment, cfg: sample.cfg, count: 0 };
          for (const axis of AXES) bucket[axis] = { min: Infinity, max: -Infinity, first: 0, last: 0,
            count: 0, dc: 0, sumSq: 0, acRms: 0, acPeak: 0, cf: null };
          this.buckets.set(key, bucket);
        }
        const isFirst = sample.index <= bucket.firstIndex;
        const isLast = sample.index >= bucket.lastIndex;
        bucket.start = Math.min(bucket.start, time);
        bucket.end = Math.max(bucket.end, time);
        bucket.firstIndex = Math.min(bucket.firstIndex, sample.index);
        bucket.lastIndex = Math.max(bucket.lastIndex, sample.index);
        bucket.count++;
        for (const axis of AXES) {
          const value = sample[`g${axis}`];
          const values = bucket[axis];
          values.count++;
          const delta = value - values.dc;
          values.dc += delta / values.count;
          // sumSq 为 Welford 中心平方和，避免大直流下的消减误差。
          values.sumSq += delta * (value - values.dc);
          values.min = Math.min(values.min, value);
          values.max = Math.max(values.max, value);
          values.acRms = Math.sqrt(Math.max(0, values.sumSq / values.count));
          values.acPeak = Math.max(Math.abs(values.min - values.dc), Math.abs(values.max - values.dc));
          values.cf = values.acRms > 0 ? values.acPeak / values.acRms : null;
          if (isFirst) values.first = value;
          if (isLast) values.last = value;
        }
      }
      if (!Number.isFinite(newest)) return;
      // 最小接收延迟对齐：旧积压包不会在随后新鲜帧到达后被当作当前时刻。
      // 这是电脑时钟上的现场时间估算，未校准 RF/USB 的固定延迟。
      const candidate = receivedMs - newest * 1000;
      if (Number.isFinite(candidate)) this.offsetMs = this.offsetMs === null ? candidate : Math.min(this.offsetMs, candidate);
      this.latestTime = this.latestTime === null ? newest : Math.max(this.latestTime, newest);
      const second = Math.floor(this.latestTime);
      if (this._lastPrunedSecond !== second || this.buckets.size > this.maxBuckets) {
        this._lastPrunedSecond = second;
        const cutoff = this.latestTime - this.maxSeconds;
        for (const [key, bucket] of this.buckets) if (bucket.end < cutoff) this.buckets.delete(key);
        if (this.buckets.size > this.maxBuckets) {
          const sorted = [...this.buckets.entries()].sort((a, b) => a[1].start - b[1].start);
          for (let i = 0; i < sorted.length - this.maxBuckets; i++) this.buckets.delete(sorted[i][0]);
        }
      }
    }

    epochMs(time) { return this.offsetMs === null ? null : time * 1000 + this.offsetMs; }
    rebase(seconds) {
      if (!seconds) return;
      for (const bucket of this.buckets.values()) { bucket.start += seconds; bucket.end += seconds; }
      const rebased = new Map();
      for (const bucket of this.buckets.values()) {
        const key = `${Math.floor(bucket.start)}:${bucket.segment}:${bucket.cfg}`;
        // µs 计数周期不是整数秒，重定位后相邻旧桶可能落到同一整数秒内。
        rebased.set(rebased.has(key) ? `${key}:${rebased.size}` : key, bucket);
      }
      this.buckets = rebased;
      if (this.latestTime !== null) this.latestTime += seconds;
      if (this.offsetMs !== null) this.offsetMs -= seconds * 1000;
    }
    getBuckets(start = -Infinity, end = Infinity) {
      return [...this.buckets.values()].filter(bucket => bucket.end >= start && bucket.start <= end).sort((a, b) => a.start - b.start);
    }
    get firstTime() {
      let first = Infinity;
      for (const bucket of this.buckets.values()) first = Math.min(first, bucket.start);
      return Number.isFinite(first) ? first : null;
    }
  }
  return { History, WaveViewport, beijingTime, beijingIso, nodeTime, span };
});
