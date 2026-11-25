'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { stats, statisticsSamples, spectrum, formatAcceleration, ReceptionQuality } = require('../src/analysis.js');

function near(actual, expected, tolerance = 1e-10) {
  assert.ok(Math.abs(actual - expected) <= tolerance,
    `${actual} 应在 ${expected} ± ${tolerance} 内`);
}

function signal(count, rate, frequency, amplitude = 1, dc = 0) {
  return Array.from({ length: count }, (_, i) => ({
    x: dc + amplitude * Math.sin(2 * Math.PI * frequency * i / rate),
    y: -2,
    z: 1,
  }));
}

test('统计量区分总 RMS、去直流 RMS、绝对峰值与峰峰值', () => {
  const result = stats([
    { x: 1, y: -2, z: 0 },
    { x: 2, y: -2, z: -3 },
    { x: 3, y: -2, z: 3 },
  ]);
  assert.equal(result.count, 3);
  near(result.x.mean, 2);
  near(result.x.rms, Math.sqrt(14 / 3));
  near(result.x.acRms, Math.sqrt(2 / 3));
  assert.equal(result.x.peakRaw, 3);
  assert.equal(result.x.acPeak, 1);
  near(result.x.cf, 1 / Math.sqrt(2 / 3));
  assert.equal(result.x.peakToPeak, 2);
  assert.deepEqual(result.y, { mean: -2, rms: 2, acRms: 0, peakRaw: 2, acPeak: 0, cf: null, peakToPeak: 0 });
  near(result.z.rms, Math.sqrt(6));
  assert.equal(result.z.peakToPeak, 6);
});

test('已知正弦的均值与 RMS 保留 g 单位', () => {
  const result = stats(signal(3200, 3200, 100, 0.5, 1));
  near(result.x.mean, 1);
  near(result.x.acRms, 0.5 / Math.sqrt(2));
  near(result.x.rms, Math.sqrt(1 + 0.125));
  near(result.x.peakRaw, 1.5);
  near(result.x.acPeak, 0.5);
  near(result.x.cf, Math.sqrt(2));
  near(result.x.peakToPeak, 1);
});

test('大直流上的微小振动不会因方差相减丢失', () => {
  const result = stats(signal(4096, 4096, 64, 0.01, 1000000));
  near(result.x.acRms, 0.01 / Math.sqrt(2), 1e-9);
});

test('空统计、单样本与输入有限性', () => {
  assert.equal(stats([]), null);
  assert.deepEqual(stats([{ x: -1, y: 0, z: 2 }]).x,
    { mean: -1, rms: 1, acRms: 0, peakRaw: 1, acPeak: 0, cf: null, peakToPeak: 0 });
  assert.throws(() => stats([{ x: NaN, y: 0, z: 0 }]), TypeError);
});

test('Hann 单边幅值校正恢复已知频率与峰值幅度，消除直流', () => {
  const result = spectrum(signal(4096, 3200, 100, 0.75, 1.25), 3200);
  assert.equal(result.size, 4096);
  near(result.binHz, 3200 / 4096);
  near(result.peakHz, 100);
  near(result.peakAmplitude, 0.75, 2e-7);
  assert.ok(result.amplitudes[0] < 1e-8);
  assert.equal(result.frequencies.length, 2049);
  near(result.frequencies.at(-1), 1600);
});

test('分析轴选择与混合频率的主峰选择', () => {
  const samples = signal(2048, 2048, 64, 0.2);
  for (let i = 0; i < samples.length; i++) {
    samples[i].x += 0.8 * Math.sin(2 * Math.PI * 200 * i / 2048);
    samples[i].y = 0.4 * Math.cos(2 * Math.PI * 128 * i / 2048);
  }
  const sx = spectrum(samples, 2048);
  const sy = spectrum(samples, 2048, 'y');
  near(sx.peakHz, 200);
  near(sx.peakAmplitude, 0.8, 1e-6);
  near(sy.peakHz, 128);
  near(sy.peakAmplitude, 0.4, 1e-6);
});

test('只使用不超过上限的最大 2 次幂后缀', () => {
  const samples = Array.from({ length: 476 }, () => ({ x: NaN, y: 0, z: 0 }))
    .concat(signal(1024, 1024, 32, 0.6));
  const result = spectrum(samples, 1024, 'x', 1200);
  assert.equal(result.size, 1024);
  near(result.peakHz, 32);
  near(result.peakAmplitude, 0.6, 1e-6);
});

test('常量和短窗口不产生虚假频率', () => {
  const constant = Array.from({ length: 1024 }, () => ({ x: 1.25, y: 0, z: 0 }));
  const result = spectrum(constant, 3200);
  assert.equal(result.peakHz, 0);
  assert.equal(result.peakAmplitude, 0);
  assert.ok(result.amplitudes.every((value) => value === 0));
  assert.equal(spectrum(constant.slice(0, 31), 3200), null);
  assert.equal(spectrum(constant, 3200, 'x', 31), null);
  assert.equal(spectrum(constant.slice(0, 32), 3200).size, 32);
});

test('Nyquist 只有单个频率分量，不重复计算单边幅值', () => {
  const samples = Array.from({ length: 1024 }, (_, i) => ({ x: i % 2 ? -0.5 : 0.5, y: 0, z: 0 }));
  const result = spectrum(samples, 3200);
  near(result.amplitudes.at(-1), 0.5, 1e-10);
});

test('无效频谱参数被明确拒绝', () => {
  const samples = signal(32, 3200, 100);
  for (const rate of [0, -1, NaN, Infinity]) {
    assert.throws(() => spectrum(samples, rate), RangeError);
  }
  assert.throws(() => spectrum(samples, 3200, 'w'), RangeError);
  assert.throws(() => spectrum(samples, 3200, 'x', 32.5), RangeError);
  samples[0].x = Infinity;
  assert.throws(() => spectrum(samples, 3200), TypeError);
});

test('浏览器普通脚本暴露同一 BmiAnalysis API', () => {
  const vm = require('node:vm');
  const fs = require('node:fs');
  const path = require('node:path');
  const context = vm.createContext({});
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../src/analysis.js'), 'utf8'), context);
  assert.equal(typeof context.BmiAnalysis.stats, 'function');
  assert.equal(typeof context.BmiAnalysis.spectrum, 'function');
  assert.equal(context.BmiAnalysis.stats([]), null);
});

test('统计窗口独立选择 1/2/5/10 s，缺口和配置变化处截断', () => {
  const values = signal(32000, 3200, 100, .16, 1).map((s, i) => ({ ...s, index: i, cfg: 0xbd, segment: 1, odr: 3200, continuous: i > 0 }));
  for (const seconds of [1, 2, 5, 10]) assert.equal(statisticsSamples(values, seconds).length, seconds * 3200);
  assert.equal(statisticsSamples(values).length, 6400);
  const short = statisticsSamples(values.slice(-1000));
  assert.equal(short.length / 3200, .3125);
  assert.ok(Number.isFinite(stats(short).x.acRms));
  values[31500].continuous = false;
  assert.equal(statisticsSamples(values, 10).length, 500);
  values[31750].cfg = 0xbc;
  assert.equal(statisticsSamples(values, 10).length, 249);
  assert.equal(spectrum(values, 3200).size, 4096);
});

test('1 g 直流 + 0.16 g 正弦的振动幅值与 CF，零值及极小 RMS 格式', () => {
  const result = stats(signal(6400, 3200, 100, .16, 1));
  near(result.x.mean, 1, 1e-6);
  near(result.x.acPeak, .16, .0016);
  near(result.x.peakRaw, 1.16, .0016);
  near(result.x.acRms, .16 / Math.sqrt(2), .001131);
  near(result.x.cf, Math.sqrt(2), .01414);
  assert.equal(formatAcceleration(.0038, true), '0.0038 g');
  assert.equal(formatAcceleration(0, true), '—');
  assert.equal(formatAcceleration(.00003, true), '<0.1 mg');
  assert.equal(formatAcceleration(-.2214), '-0.2214 g');
});

test('60 s 缺样率取 DATA 缺口与 STATUS 丢样之较大值，迟到回减且过期', () => {
  const quality = new ReceptionQuality();
  quality.observe({ received: 450, indexMissing: 0, fifoMissing: 0, ringMissing: 0 }, 0);
  quality.observe({ received: 900, indexMissing: 50, fifoMissing: 25, ringMissing: 25 }, 1);
  const overlap = quality.snapshot(1);
  near(overlap.missingRate, 50 / 950);
  assert.deepEqual([overlap.indexMissing, overlap.fifoMissing, overlap.ringMissing], [50, 25, 25]);
  assert.equal(overlap.isDegraded, true);
  quality.observe({ received: 950, indexMissing: 0, fifoMissing: 25, ringMissing: 25 }, 2);
  near(quality.snapshot(2).missingRate, .05);
  assert.equal(quality.snapshot(2).isDegraded, false);
  assert.equal(quality.snapshot(62).missingRate, null);
  quality.observe({ received: 1050, indexMissing: 0, fifoMissing: 25, ringMissing: 25 }, 63);
  assert.equal(quality.snapshot(63).missingRate, 0);
  quality.observe({ received: 1050, indexMissing: 0, fifoMissing: 0xfffffff0, ringMissing: 25 }, 64);
  quality.observe({ received: 1150, indexMissing: 0, fifoMissing: 4, ringMissing: 25 }, 125);
  assert.equal(quality.snapshot(125).fifoMissing, 20);
});

test('近 3.3% 的重叠丢样不触发降级', () => {
  const quality = new ReceptionQuality();
  quality.observe({ received: 29, indexMissing: 1, fifoMissing: 0, ringMissing: 0 }, 0);
  quality.observe({ received: 29, indexMissing: 1, fifoMissing: 1, ringMissing: 0 }, 1);
  near(quality.snapshot(1).missingRate, 1 / 30);
  assert.equal(quality.snapshot(1).isDegraded, false);
});

test('固定种子白噪声的 AC RMS 与 CF', () => {
  let seed = 42;
  const random = () => { seed = (Math.imul(1664525, seed) + 1013904223) >>> 0; return (seed + .5) / 2 ** 32; };
  const values = Array.from({ length: 6400 }, () => ({ x: 1 + .0038 * Math.sqrt(-2 * Math.log(random())) * Math.cos(2 * Math.PI * random()), y: 0, z: 1 }));
  const result = stats(values).x;
  near(result.acRms, .0038, .0038 * .02);
  assert.ok(result.cf >= 3.2 && result.cf <= 4.2);
});
