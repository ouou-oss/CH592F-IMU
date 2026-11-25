'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const T = require('../src/timeline.js');

function sample(index, time, x = 0, extra = {}) {
  return { index, nodeTime: time, segment: 1, cfg: 0xbd, gx: x, gy: 0, gz: 1, ...extra };
}

test('北京时间始终使用UTC+8，正确跨年和显示毫秒', () => {
  const epoch = Date.parse('2026-12-31T16:00:00.125Z');
  assert.equal(T.beijingTime(epoch, true, true), '2027-01-01 00:00:00.125');
  assert.equal(T.beijingIso(epoch), '2027-01-01T00:00:00.125+08:00');
  assert.equal(T.beijingTime(epoch), '00:00:00');
});

test('节点时钟按累计小时显示，时窗长度独立格式化', () => {
  assert.equal(T.nodeTime(7200.125, true), '02:00:00.125');
  assert.equal(T.nodeTime(90000), '25:00:00');
  assert.equal(T.nodeTime(59.9996, true), '00:01:00.000');
  assert.equal(T.nodeTime(-.00001, true), '00:00:00.000');
  assert.equal(T.nodeTime(-.001, true), '−00:00:00.001');
  assert.equal(T.span(.25), '250 ms');
  assert.equal(T.span(20), '20 s');
  assert.equal(T.span(300), '5 min');
});

test('电脑对齐时间以节点样本时间推进，串口成批到达也保留采样间隔', () => {
  const h = new T.History();
  h.ingest([sample(0, 7200), sample(1, 7200.0003125)], 100000);
  assert.ok(Math.abs(h.epochMs(7200.0003125) - 100000) < 1e-6);
  assert.ok(Math.abs(h.epochMs(7200.0003125) - h.epochMs(7200) - .3125) < 1e-6);
});

test('积压的旧包由新鲜包修正现场时间，不把旧样本标作当前时刻', () => {
  const h = new T.History();
  h.ingest([sample(0, 100)], 1000000);
  h.ingest([sample(320000, 200)], 1000010);
  assert.equal(h.epochMs(200), 1000010);
  assert.equal(h.epochMs(100), 900010);
  h.ingest([sample(10, 101)], 1000030);
  assert.equal(h.epochMs(200), 1000010);
});

test('每秒极值保留短冲击和迟到样本，节点分段与配置不混合', () => {
  const h = new T.History();
  h.ingest([sample(0, 1.1, .01), sample(2, 1.3, -.7)], 5000);
  h.ingest([sample(1, 1.2, 1.8)], 5010);
  h.ingest([sample(3, 1.4, .2, { segment: 2 }), sample(4, 1.5, .3, { segment: 2, cfg: 0xbc })], 5020);
  const b = h.getBuckets();
  assert.equal(b.length, 3);
  for (const [key, expected] of Object.entries({ min: -.7, max: 1.8, first: .01, last: -.7 })) assert.equal(b[0].x[key], expected);
  assert.equal(b[0].count, 3);
  assert.equal(b[0].lastIndex - b[0].firstIndex + 1, b[0].count);
});

test('Welford 每秒桶与直接统计一致，乱序入桶保留 DC/AC RMS/CF', () => {
  const A = require('../src/analysis.js');
  const samples = Array.from({ length: 6400 }, (_, i) => sample(i, i / 3200, 1 + .16 * Math.sin(2 * Math.PI * 100 * i / 3200)));
  const h = new T.History();
  h.ingest(samples.slice(1000), 10000);
  h.ingest(samples.slice(0, 1000), 10001);
  assert.equal(h.getBuckets().length, 2);
  for (const bucket of h.getBuckets()) {
    const direct = A.stats(samples.filter(s => Math.floor(s.nodeTime) === Math.floor(bucket.start)).map(s => ({ x: s.gx, y: s.gy, z: s.gz })));
    for (const axis of ['x', 'y', 'z']) {
      const actual = bucket[axis];
      assert.equal(actual.count, 3200);
      for (const [key, value] of Object.entries({ dc: direct[axis].mean, acRms: direct[axis].acRms, acPeak: direct[axis].acPeak })) assert.ok(Math.abs(actual[key] - value) < 1e-6);
      if (direct[axis].cf === null) assert.equal(actual.cf, null);
      else assert.ok(Math.abs(actual.cf - direct[axis].cf) < 1e-6);
    }
  }
});

test('长时间趋势按时长和桶数双重限制，原始样本裁剪不影响旧趋势', () => {
  const h = new T.History({ maxSeconds: 3600, maxBuckets: 4096 });
  for (let i = 0; i <= 7200; i++) h.ingest([sample(i, i, i === 4000 ? 2 : 0)], i * 1000);
  assert.equal(h.firstTime, 3600);
  assert.equal(h.getBuckets().length, 3601);
  assert.equal(h.getBuckets(3999, 4001)[1].x.max, 2);
  const limited = new T.History({ maxSeconds: 3600, maxBuckets: 3 });
  for (let i = 0; i < 5; i++) limited.ingest([sample(i, i)], 10000);
  assert.deepEqual(limited.getBuckets().map(b => b.start), [2, 3, 4]);
});

test('迟到的回绕前数据补充时钟epoch后，北京时间保持不跳变', () => {
  const h = new T.History();
  h.ingest([sample(10, .25)], 10000);
  const previous = h.epochMs(.25);
  h.rebase(86400);
  assert.equal(h.epochMs(86400.25), previous);
  assert.equal(h.firstTime, 86400.25);
  h.ingest([sample(11, 86400.5)], 10250);
  assert.equal(h.getBuckets().length, 1);
  assert.equal(h.latestTime, 86400.5);
  const us = new T.History();
  us.ingest([sample(1, .1, 2), sample(2, 1.01, -3)], 20000);
  us.rebase(4294.967296);
  assert.equal(us.getBuckets().length, 2);
  assert.equal(us.getBuckets()[0].x.max, 2);
  assert.equal(us.getBuckets()[1].x.min, -3);
});

test('波形缩放保留鼠标下的时刻，之后收流不移动视窗', () => {
  const view = new T.WaveViewport();
  const bounds = { first: 60, latest: 100 };
  const fraction = .25;
  const before = view.range(bounds);
  assert.equal(view.zoom(bounds, .5, fraction), true);
  const after = view.range(bounds);
  assert.equal(after.seconds, .5);
  assert.equal(after.start + fraction * after.seconds, before.start + fraction * before.seconds);
  assert.deepEqual(view.range({ ...bounds, latest: 102 }), after);
  view.follow();
  assert.deepEqual(view.range({ ...bounds, latest: 102 }), { start: 101.5, end: 102, seconds: .5 });
});

test('波形拖动贴合可用历史边界，并跟随时钟重定位', () => {
  const view = new T.WaveViewport();
  const bounds = { first: 80, latest: 100 };
  view.seconds = 5;
  view.pan(bounds, 100, -4);
  assert.deepEqual(view.range(bounds), { start: 91, end: 96, seconds: 5 });
  view.rebase(1000);
  assert.equal(view.range({ first: 1080, latest: 1100 }).end, 1096);
  view.pan(bounds, 96, -100);
  assert.deepEqual(view.range(bounds), { start: 80, end: 85, seconds: 5 });
  assert.equal(view.range({ first: 82, latest: 101 }).start, 82);
  view.pan(bounds, 85, 100);
  assert.equal(view.range(bounds).end, 100);
});

test('波形缩放和全部趋势保留显示范围限制', () => {
  const view = new T.WaveViewport();
  const bounds = { first: 100, latest: 102 };
  view.zoom(bounds, .001, .5);
  assert.equal(view.range(bounds).seconds, .05);
  view.zoom(bounds, 1000000, .5);
  assert.deepEqual(view.range(bounds), { start: -3498, end: 102, seconds: 3600 });
  view.seconds = null;
  view.follow();
  assert.deepEqual(view.range(bounds), { start: 100, end: 102, seconds: 2 });
});
