'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const A = require('../src/attitude.js');

function near(actual, expected, tolerance = 1e-9) {
  assert.ok(Math.abs(actual - expected) <= tolerance, `${actual} 应在 ${expected} ± ${tolerance} 内`);
}

function apply(q, point) {
  return A.transformPoint(A.quatToMatrix(q), point);
}

function determinant(matrix) {
  return matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7])
    - matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6])
    + matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
}

test('静止时把机体 Z 轴转到世界上方', () => {
  const q = A.quatFromGravity(0, 0, 1);
  assert.ok(q);
  const z = apply(q, [0, 0, 1]);
  near(z[0], 0);
  near(z[1], 1);
  near(z[2], 0);
  const x = apply(q, [1, 0, 0]);
  near(x[0], 1);
  near(x[1], 0);
  near(x[2], 0);
});

test('任意重力方向都映射到世界上方且矩阵保持刚体性质', () => {
  for (const direction of [
    [1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, -1],
    [0.5, 0.3, Math.sqrt(1 - 0.25 - 0.09)], [-0.4, 0.7, -0.59],
  ]) {
    const size = Math.hypot(...direction);
    const unit = direction.map(value => value / size);
    const q = A.quatFromGravity(...direction);
    assert.ok(q, `方向 ${direction} 应得到四元数`);
    const mapped = apply(q, unit);
    near(mapped[0], 0, 1e-8);
    near(mapped[1], 1, 1e-8);
    near(mapped[2], 0, 1e-8);
    const matrix = A.quatToMatrix(q);
    near(determinant(matrix), 1, 1e-8);
  }
});

test('无效重力输入返回空，不产生 NaN 姿态', () => {
  assert.equal(A.quatFromGravity(0, 0, 0), null);
  assert.equal(A.quatFromGravity(NaN, 0, 1), null);
  assert.equal(A.quatFromGravity(Infinity, 0, 1), null);
});

test('倾角读数与机体轴仰角一致', () => {
  const angle = 30 * Math.PI / 180;
  const q = A.quatFromGravity(Math.sin(angle), 0, Math.cos(angle));
  const tilt = A.tiltFromQuaternion(q);
  near(tilt.xElevation, 30, 1e-6);
  near(tilt.yElevation, 0, 1e-6);
  near(tilt.zTilt, 30, 1e-6);

  const rest = A.tiltFromQuaternion(A.quatFromGravity(0, 0, 1));
  near(rest.xElevation, 0);
  near(rest.yElevation, 0);
  near(rest.zTilt, 0);
});

test('球面插值覆盖端点并保持单位长度', () => {
  const a = A.quatFromGravity(0, 0, 1);
  const b = A.quatFromGravity(1, 0, 0);
  near(A.quatSlerp(a, b, 0).w, a.w);
  near(A.quatSlerp(a, b, 1).x, b.x);
  const middle = A.quatSlerp(a, b, 0.5);
  near(Math.hypot(middle.w, middle.x, middle.y, middle.z), 1, 1e-9);
  assert.ok(apply(middle, [0, 0, 1])[1] > 0);
});

test('姿态滤波从首次样本起步并随时间收敛到目标', () => {
  const filter = new A.AttitudeFilter({ tau: 0.1 });
  const first = filter.update(0, 0, 1, 1000);
  assert.ok(first);
  near(first.w, A.quatFromGravity(0, 0, 1).w);
  filter.update(1, 0, 0, 1000);
  const target = A.quatFromGravity(1, 0, 0);
  for (let i = 1; i <= 60; i++) filter.update(1, 0, 0, 1000 + i * 16);
  const mapped = apply(filter.quaternion, [1, 0, 0]);
  near(mapped[1], 1, 1e-4);
  near(filter.quaternion.w, target.w, 1e-3);
  near(Math.hypot(filter.quaternion.w, filter.quaternion.x, filter.quaternion.y, filter.quaternion.z), 1, 1e-9);
  filter.reset();
  assert.equal(filter.quaternion, null);
});
