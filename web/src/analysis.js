// 实时分析：输入单位为 g；调用者负责提供同一连续段内、同一采样率的数据。
(function (root, factory) {
  'use strict';
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.BmiAnalysis = api;
})(typeof globalThis === 'object' ? globalThis : this, function () {
  'use strict';

  const AXES = ['x', 'y', 'z'];

  function finiteValue(sample, axis) {
    const value = sample[axis];
    if (!Number.isFinite(value)) throw new TypeError('样本必须是有限数值，单位为 g');
    return value;
  }

  // Welford 方差避免“大直流、小振动”时 RMS 去直流后的精度损失。
  function stats(samples) {
    if (samples.length === 0) return null;
    const result = { count: samples.length };
    for (const axis of AXES) {
      let mean = 0;
      let m2 = 0;
      let min = Infinity;
      let max = -Infinity;
      let peak = 0;
      for (let i = 0; i < samples.length; i++) {
        const value = finiteValue(samples[i], axis);
        const delta = value - mean;
        mean += delta / (i + 1);
        m2 += delta * (value - mean);
        min = Math.min(min, value);
        max = Math.max(max, value);
        peak = Math.max(peak, Math.abs(value));
      }
      const variance = Math.max(0, m2 / samples.length);
      const acRms = Math.sqrt(variance);
      const acPeak = Math.max(Math.abs(min - mean), Math.abs(max - mean));
      result[axis] = {
        mean,
        rms: Math.hypot(mean, Math.sqrt(variance)),
        acRms,
        peakRaw: peak,
        acPeak,
        cf: acRms > 0 ? acPeak / acRms : null,
        peakToPeak: max - min,
      };
    }
    return result;
  }

  function statisticsSamples(samples, window = 2) {
    const last = samples.at(-1);
    if (!last || !(last.odr > 0) || !Number.isFinite(window) || window <= 0) return [];
    const count = Math.max(1, Math.floor(Math.min(samples.length, window * last.odr)));
    const lower = samples.length - count;
    let start = samples.length - 1;
    while (start > lower && samples[start].continuous && samples[start - 1].cfg === last.cfg
      && samples[start - 1].segment === last.segment && samples[start - 1].odr === last.odr) start--;
    return samples.slice(start);
  }

  function formatAcceleration(value, rms = false) {
    if (!Number.isFinite(value) || (rms && value === 0)) return '—';
    if (rms && value > 0 && value < 0.0001) return '<0.1 mg';
    return `${value.toFixed(4)} g`;
  }

  // 按主机观测时间统计最近 60 s，DATA 缺口与 STATUS 丢样依方案相加。
  class ReceptionQuality {
    constructor() {
      this.observations = [];
      this.previous = { received: 0, indexMissing: 0, fifoMissing: 0, ringMissing: 0 };
    }
    observe(current, time) {
      const previous = this.previous;
      const modulus = 2 ** 32;
      this.observations.push({ time, received: current.received - previous.received,
        indexMissing: current.indexMissing - previous.indexMissing,
        fifoMissing: (current.fifoMissing + modulus - previous.fifoMissing) % modulus,
        ringMissing: (current.ringMissing + modulus - previous.ringMissing) % modulus });
      this.previous = { ...current };
      this.prune(time);
    }
    prune(time) {
      let expired = 0;
      while (expired < this.observations.length && this.observations[expired].time <= time - 60) expired++;
      if (expired) this.observations.splice(0, expired);
    }
    snapshot(time) {
      this.prune(time);
      const result = { received: 0, indexMissing: 0, fifoMissing: 0, ringMissing: 0 };
      for (const observation of this.observations) {
        for (const key of Object.keys(result)) result[key] += observation[key];
      }
      result.indexMissing = Math.max(0, result.indexMissing);
      result.missing = Math.max(result.indexMissing, result.fifoMissing + result.ringMissing);
      const total = result.received + result.missing;
      result.missingRate = total > 0 ? result.missing / total : null;
      result.isDegraded = result.missingRate > 0.05;
      return result;
    }
  }

  // 原位 radix-2 FFT；虚部初值为 0。
  function fft(real, imag) {
    const size = real.length;
    for (let i = 1, j = 0; i < size; i++) {
      let bit = size / 2;
      while (j >= bit) {
        j -= bit;
        bit /= 2;
      }
      j += bit;
      if (i < j) {
        [real[i], real[j]] = [real[j], real[i]];
        [imag[i], imag[j]] = [imag[j], imag[i]];
      }
    }
    for (let width = 2; width <= size; width *= 2) {
      const half = width / 2;
      const angle = -2 * Math.PI / width;
      const stepR = Math.cos(angle);
      const stepI = Math.sin(angle);
      for (let offset = 0; offset < size; offset += width) {
        let wr = 1;
        let wi = 0;
        for (let j = 0; j < half; j++) {
          const even = offset + j;
          const odd = even + half;
          const tr = wr * real[odd] - wi * imag[odd];
          const ti = wr * imag[odd] + wi * real[odd];
          real[odd] = real[even] - tr;
          imag[odd] = imag[even] - ti;
          real[even] += tr;
          imag[even] += ti;
          const nextR = wr * stepR - wi * stepI;
          wi = wr * stepI + wi * stepR;
          wr = nextR;
        }
      }
    }
  }

  function spectrum(samples, sampleRate, axis = 'x', maxSize = 4096) {
    if (!Number.isFinite(sampleRate) || sampleRate <= 0) {
      throw new RangeError('采样率必须为正数');
    }
    if (!AXES.includes(axis)) throw new RangeError('分析轴必须为 x、y 或 z');
    if (!Number.isSafeInteger(maxSize) || maxSize < 1) {
      throw new RangeError('FFT 上限必须为正整数');
    }
    const limit = Math.min(samples.length, maxSize);
    if (limit < 32) return null;
    let size = 1;
    while (size * 2 <= limit) size *= 2;
    const start = samples.length - size;
    const real = new Float64Array(size);
    const imag = new Float64Array(size);
    let mean = 0;
    for (let i = 0; i < size; i++) {
      real[i] = finiteValue(samples[start + i], axis);
      mean += (real[i] - mean) / (i + 1);
    }
    let windowSum = 0;
    for (let i = 0; i < size; i++) {
      const window = 0.5 * (1 - Math.cos(2 * Math.PI * i / (size - 1)));
      real[i] = (real[i] - mean) * window;
      windowSum += window;
    }
    fft(real, imag);
    const bins = size / 2 + 1;
    const frequencies = new Float64Array(bins);
    const amplitudes = new Float64Array(bins);
    const binHz = sampleRate / size;
    let peakBin = 0;
    let peakAmplitude = 0;
    for (let k = 0; k < bins; k++) {
      // 单边峰值幅度：DC 与 Nyquist 不能乘 2；Hann 相干增益由实际窗和校正。
      const factor = k === 0 || k === size / 2 ? 1 : 2;
      frequencies[k] = k * binHz;
      amplitudes[k] = Math.hypot(real[k], imag[k]) * factor / windowSum;
      if (k > 0 && amplitudes[k] > peakAmplitude) {
        peakBin = k;
        peakAmplitude = amplitudes[k];
      }
    }
    return {
      size,
      frequencies,
      amplitudes,
      peakHz: peakBin * binHz,
      peakAmplitude,
      binHz,
    };
  }

  return { stats, statisticsSamples, spectrum, formatAcceleration, ReceptionQuality };
});
