// 姿态解算与三维传感器视图：只利用加速度方向估计重力朝向，偏航角不可观测。
(function (root, factory) {
  'use strict';
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.BmiAttitude = api;
})(typeof globalThis === 'object' ? globalThis : this, function () {
  'use strict';

  const DEG = 180 / Math.PI;
  const WORLD_UP = [0, 1, 0];
  const LIGHT = normalize3([0.45, 0.92, 0.55]);

  function clamp(value, min, max) { return Math.min(max, Math.max(min, value)); }

  function sub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
  function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
  function cross(a, b) {
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  }
  function length(a) { return Math.hypot(a[0], a[1], a[2]); }
  function normalize3(a) {
    const size = length(a);
    return size > 1e-9 ? [a[0] / size, a[1] / size, a[2] / size] : null;
  }

  function normalizeQuaternion(q) {
    const size = Math.hypot(q.w, q.x, q.y, q.z);
    if (!(size > 1e-12)) return { w: 1, x: 0, y: 0, z: 0 };
    return { w: q.w / size, x: q.x / size, y: q.y / size, z: q.z / size };
  }

  // 求把重力方向 n（机体系）转到世界上方的四元数；最短弧保持视觉上的横滚/俯仰。
  function quatFromGravity(x, y, z) {
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) return null;
    const direction = normalize3([x, y, z]);
    if (!direction) return null;
    const [nx, ny, nz] = direction;
    const cosine = clamp(ny, -1, 1);
    if (cosine > 1 - 1e-12) return { w: 1, x: 0, y: 0, z: 0 };
    if (cosine < -1 + 1e-12) {
      const axis = normalize3([nz, 0, -nx]) || [1, 0, 0];
      return { w: 0, x: axis[0], y: axis[1], z: axis[2] };
    }
    const axis = normalize3([-nz, 0, nx]) || [1, 0, 0];
    const half = Math.acos(cosine) / 2;
    const sine = Math.sin(half);
    return { w: Math.cos(half), x: axis[0] * sine, y: axis[1] * sine, z: axis[2] * sine };
  }

  function quatSlerp(a, b, t) {
    if (!a) return b ? normalizeQuaternion({ ...b }) : null;
    if (!b) return normalizeQuaternion({ ...a });
    let { w: bw, x: bx, y: by, z: bz } = b;
    let cosine = a.w * bw + a.x * bx + a.y * by + a.z * bz;
    if (cosine < 0) {
      cosine = -cosine; bw = -bw; bx = -bx; by = -by; bz = -bz;
    }
    if (cosine > 0.9995) {
      return normalizeQuaternion({
        w: a.w + (bw - a.w) * t,
        x: a.x + (bx - a.x) * t,
        y: a.y + (by - a.y) * t,
        z: a.z + (bz - a.z) * t,
      });
    }
    const angle = Math.acos(clamp(cosine, -1, 1));
    const sine = Math.sin(angle);
    const first = Math.sin((1 - t) * angle) / sine;
    const second = Math.sin(t * angle) / sine;
    return {
      w: a.w * first + bw * second,
      x: a.x * first + bx * second,
      y: a.y * first + by * second,
      z: a.z * first + bz * second,
    };
  }

  // 行主序 3×3，列是机体系 X/Y/Z 轴在世界中的方向。
  function quatToMatrix(q) {
    const { w, x, y, z } = q;
    return [
      1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
      2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
      2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y),
    ];
  }

  function transformPoint(matrix, point) {
    return [
      matrix[0] * point[0] + matrix[1] * point[1] + matrix[2] * point[2],
      matrix[3] * point[0] + matrix[4] * point[1] + matrix[5] * point[2],
      matrix[6] * point[0] + matrix[7] * point[1] + matrix[8] * point[2],
    ];
  }

  // 机体 X/Y 轴相对水平面的仰角，以及 Z 轴相对竖直方向的倾角。
  function tiltFromQuaternion(q) {
    const matrix = quatToMatrix(q);
    return {
      xElevation: Math.asin(clamp(matrix[3], -1, 1)) * DEG,
      yElevation: Math.asin(clamp(matrix[4], -1, 1)) * DEG,
      zTilt: Math.acos(clamp(matrix[5], -1, 1)) * DEG,
    };
  }

  class AttitudeFilter {
    constructor({ tau = 0.12 } = {}) {
      this.tau = tau;
      this.quaternion = null;
      this.time = null;
    }

    reset() {
      this.quaternion = null;
      this.time = null;
    }

    update(x, y, z, now = Date.now()) {
      const target = quatFromGravity(x, y, z);
      if (!target) return this.quaternion;
      if (!this.quaternion || this.time === null) {
        this.quaternion = target;
        this.time = now;
        return this.quaternion;
      }
      const seconds = clamp((now - this.time) / 1000, 0, 0.5);
      this.time = now;
      const alpha = 1 - Math.exp(-seconds / Math.max(1e-3, this.tau));
      this.quaternion = quatSlerp(this.quaternion, target, alpha);
      return this.quaternion;
    }
  }

  function parseColor(value) {
    const text = String(value || '').trim();
    if (text[0] === '#') {
      let hex = text.slice(1);
      if (hex.length === 3) hex = hex.split('').map(part => part + part).join('');
      if (hex.length >= 6) return [
        parseInt(hex.slice(0, 2), 16), parseInt(hex.slice(2, 4), 16), parseInt(hex.slice(4, 6), 16),
      ];
    }
    const match = text.match(/rgba?\(([^)]+)\)/);
    if (match) {
      const parts = match[1].split(',').map(Number);
      return [parts[0] || 0, parts[1] || 0, parts[2] || 0];
    }
    return [128, 128, 128];
  }

  function shade(color, factor) {
    const [r, g, b] = parseColor(color);
    return `rgb(${Math.round(clamp(r * factor, 0, 255))}, ${Math.round(clamp(g * factor, 0, 255))}, ${Math.round(clamp(b * factor, 0, 255))})`;
  }

  function fade(color, alpha) {
    const [r, g, b] = parseColor(color);
    return `rgba(${Math.round(r)}, ${Math.round(g)}, ${Math.round(b)}, ${clamp(alpha, 0, 1)})`;
  }

  const FACE_INDICES = [
    [4, 5, 6, 7], [1, 0, 3, 2], [1, 2, 6, 5], [0, 4, 7, 3], [3, 7, 6, 2], [0, 1, 5, 4],
  ];

  function boxVertices(center, half) {
    const [cx, cy, cz] = center;
    const [hx, hy, hz] = half;
    return [
      [cx - hx, cy - hy, cz - hz], [cx + hx, cy - hy, cz - hz],
      [cx + hx, cy + hy, cz - hz], [cx - hx, cy + hy, cz - hz],
      [cx - hx, cy - hy, cz + hz], [cx + hx, cy - hy, cz + hz],
      [cx + hx, cy + hy, cz + hz], [cx - hx, cy + hy, cz + hz],
    ];
  }

  function projectPoint(camera, point) {
    const relative = sub(point, camera.eye);
    const depth = dot(relative, camera.forward);
    if (depth <= 0.08) return null;
    const scale = camera.focal / depth;
    return [
      camera.cx + dot(relative, camera.right) * scale,
      camera.cy - dot(relative, camera.up) * scale,
      depth,
    ];
  }

  class SensorRenderer {
    constructor(canvas) {
      this.canvas = canvas;
      this.ctx = canvas.getContext('2d');
      this.palette = null;
      this.azimuth = 0.68;
      this.elevation = 0.5;
      this.rest = quatFromGravity(0, 0, 1);
    }

    setPalette(palette) { this.palette = palette; }

    orbit(dx, dy) {
      this.azimuth += dx * 0.008;
      this.elevation = clamp(this.elevation + dy * 0.006, 0.1, 1.35);
    }

    resetView() {
      this.azimuth = 0.68;
      this.elevation = 0.5;
    }

    resize() {
      const width = this.canvas.clientWidth;
      const height = this.canvas.clientHeight;
      if (!width || !height) return null;
      const ratio = Math.min(window.devicePixelRatio || 1, 2);
      const pixelWidth = Math.round(width * ratio);
      const pixelHeight = Math.round(height * ratio);
      if (this.canvas.width !== pixelWidth || this.canvas.height !== pixelHeight) {
        this.canvas.width = pixelWidth;
        this.canvas.height = pixelHeight;
      }
      this.ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
      return { width, height };
    }

    camera(width, height) {
      const distance = 9.2;
      const eye = [
        distance * Math.cos(this.elevation) * Math.sin(this.azimuth),
        distance * Math.sin(this.elevation),
        distance * Math.cos(this.elevation) * Math.cos(this.azimuth),
      ];
      const target = [0, -0.05, 0];
      const forward = normalize3(sub(target, eye));
      const right = normalize3(cross(forward, WORLD_UP));
      const up = cross(right, forward);
      return {
        eye, forward, right, up,
        focal: Math.min(width, height) * 2.3,
        cx: width / 2,
        cy: height * 0.55,
      };
    }

    draw(quaternion, { stale = false } = {}) {
      const size = this.resize();
      if (!size || !this.palette) return;
      const { ctx } = this;
      const { width, height } = size;
      const p = this.palette;
      const q = quaternion || this.rest;
      const matrix = quatToMatrix(q);
      const camera = this.camera(width, height);

      ctx.save();
      ctx.beginPath();
      if (ctx.roundRect) ctx.roundRect(0, 0, width, height, 8);
      else ctx.rect(0, 0, width, height);
      ctx.clip();
      ctx.clearRect(0, 0, width, height);

      const glow = ctx.createRadialGradient(camera.cx, camera.cy * 0.9, 8, camera.cx, camera.cy, Math.max(width, height) * 0.7);
      glow.addColorStop(0, fade(p.accent, 0.055));
      glow.addColorStop(1, fade(p.accent, 0));
      ctx.fillStyle = glow;
      ctx.fillRect(0, 0, width, height);

      this.drawGround(camera, width, height);
      if (stale) ctx.globalAlpha = 0.5;

      this.drawModel(camera, matrix);
      this.drawBoardMarks(camera, matrix);
      this.drawAxes(camera, matrix);

      ctx.restore();
    }

    drawGround(camera, width, height) {
      const p = this.palette;
      const { ctx } = this;
      const level = -1.62;
      const extent = 3.6;
      ctx.save();
      ctx.strokeStyle = fade(p.grid, 0.55);
      ctx.lineWidth = 1;
      for (let offset = -extent; offset <= extent + 1e-6; offset += 0.45) {
        for (const points of [
          [[offset, level, -extent], [offset, level, extent]],
          [[-extent, level, offset], [extent, level, offset]],
        ]) {
          const first = projectPoint(camera, points[0]);
          const second = projectPoint(camera, points[1]);
          if (!first || !second) continue;
          ctx.beginPath();
          ctx.moveTo(first[0], first[1]);
          ctx.lineTo(second[0], second[1]);
          ctx.stroke();
        }
      }
      const center = projectPoint(camera, [0, level, 0]);
      if (center) {
        const radius = Math.min(width, height) * 0.24;
        const shadow = ctx.createRadialGradient(center[0], center[1], 0, center[0], center[1], radius);
        shadow.addColorStop(0, fade(p.shadow, 0.24));
        shadow.addColorStop(1, fade(p.shadow, 0));
        ctx.fillStyle = shadow;
        ctx.beginPath();
        ctx.ellipse(center[0], center[1], radius, radius * 0.55, 0, 0, Math.PI * 2);
        ctx.fill();
      }
      ctx.restore();
    }

    drawModel(camera, matrix) {
      const p = this.palette;
      const { ctx } = this;
      const boxes = [
        { center: [0, 0, 0], half: [1.0, 1.3, 0.2], top: p.pcbTop, bottom: p.pcbBottom, side: p.edge },
        { center: [0, -0.5, 0.275], half: [0.4, 0.36, 0.075], top: p.chip, bottom: p.chipEdge, side: p.chipEdge },
        { center: [0, 1.24, 0.06], half: [0.34, 0.14, 0.14], top: p.connector, bottom: p.connector, side: p.connector },
      ];
      const faces = [];
      for (const box of boxes) {
        const vertices = boxVertices(box.center, box.half).map(point => transformPoint(matrix, point));
        for (let index = 0; index < FACE_INDICES.length; index++) {
          const world = FACE_INDICES[index].map(vertex => vertices[vertex]);
          const normal = normalize3(cross(sub(world[1], world[0]), sub(world[2], world[0])));
          if (!normal) continue;
          const center = [
            (world[0][0] + world[1][0] + world[2][0] + world[3][0]) / 4,
            (world[0][1] + world[1][1] + world[2][1] + world[3][1]) / 4,
            (world[0][2] + world[1][2] + world[2][2] + world[3][2]) / 4,
          ];
          if (dot(normal, sub(camera.eye, center)) <= 0) continue;
          const projected = world.map(point => projectPoint(camera, point));
          if (projected.some(point => !point)) continue;
          const color = index === 0 ? box.top : index === 1 ? box.bottom : box.side;
          faces.push({
            points: projected,
            depth: projected.reduce((sum, point) => sum + point[2], 0) / 4,
            color: shade(color, 0.62 + 0.38 * Math.max(0, dot(normal, LIGHT))),
          });
        }
      }
      faces.sort((a, b) => b.depth - a.depth);
      for (const face of faces) {
        ctx.beginPath();
        ctx.moveTo(face.points[0][0], face.points[0][1]);
        for (let i = 1; i < face.points.length; i++) ctx.lineTo(face.points[i][0], face.points[i][1]);
        ctx.closePath();
        ctx.fillStyle = face.color;
        ctx.fill();
        ctx.strokeStyle = fade(p.edge, 0.5);
        ctx.lineWidth = 0.8;
        ctx.stroke();
      }
    }

    drawBoardMarks(camera, matrix) {
      const p = this.palette;
      const { ctx } = this;
      const top = transformPoint(matrix, [0, 0, 1]);
      const center = transformPoint(matrix, [0, 0, 0]);
      if (dot(top, sub(camera.eye, center)) <= 0) return;
      const z = 0.202;
      const corners = [[-0.84, -1.14], [0.84, -1.14], [0.84, 1.14], [-0.84, 1.14]]
        .map(([x, y]) => projectPoint(camera, transformPoint(matrix, [x, y, z])));
      if (corners.every(Boolean)) {
        ctx.beginPath();
        ctx.moveTo(corners[0][0], corners[0][1]);
        for (let i = 1; i < corners.length; i++) ctx.lineTo(corners[i][0], corners[i][1]);
        ctx.closePath();
        ctx.strokeStyle = fade(p.accent, 0.42);
        ctx.lineWidth = 1;
        ctx.stroke();
      }
      for (const [x, y] of [[-0.72, -1.02], [0.72, -1.02], [-0.72, 1.02], [0.72, 1.02]]) {
        const hole = projectPoint(camera, transformPoint(matrix, [x, y, z]));
        if (!hole) continue;
        ctx.beginPath();
        ctx.arc(hole[0], hole[1], 2, 0, Math.PI * 2);
        ctx.fillStyle = fade(p.shadow, 0.75);
        ctx.fill();
      }
      const pin = projectPoint(camera, transformPoint(matrix, [-0.7, -1.18, z]));
      if (pin) {
        ctx.beginPath();
        ctx.arc(pin[0], pin[1], 1.8, 0, Math.PI * 2);
        ctx.fillStyle = fade(p.accent, 0.8);
        ctx.fill();
      }
    }

    drawAxes(camera, matrix) {
      const p = this.palette;
      const { ctx } = this;
      const origin = [0, 0, 0.42];
      const stemTop = projectPoint(camera, transformPoint(matrix, origin));
      const stemBottom = projectPoint(camera, transformPoint(matrix, [0, 0, 0.205]));
      if (stemTop && stemBottom) {
        ctx.strokeStyle = fade(p.muted, 0.6);
        ctx.lineWidth = 1.2;
        ctx.beginPath();
        ctx.moveTo(stemBottom[0], stemBottom[1]);
        ctx.lineTo(stemTop[0], stemTop[1]);
        ctx.stroke();
      }
      this.drawAxis(camera, matrix, [1, 0, 0], 1.12, p.axisX, 'X');
      this.drawAxis(camera, matrix, [0, 1, 0], 1.5, p.axisY, 'Y');
      this.drawAxis(camera, matrix, [0, 0, 1], 1.05, p.axisZ, 'Z');
      if (stemTop) {
        ctx.beginPath();
        ctx.arc(stemTop[0], stemTop[1], 3, 0, Math.PI * 2);
        ctx.fillStyle = p.panel;
        ctx.fill();
        ctx.strokeStyle = fade(p.text, 0.8);
        ctx.lineWidth = 1;
        ctx.stroke();
      }
    }

    drawAxis(camera, matrix, direction, distance, color, label) {
      const p = this.palette;
      const { ctx } = this;
      const start = projectPoint(camera, transformPoint(matrix, [0, 0, 0.42]));
      const end = projectPoint(camera, transformPoint(matrix, [
        direction[0] * distance, direction[1] * distance, 0.42 + direction[2] * distance,
      ]));
      if (!start || !end) return;
      const angle = Math.atan2(end[1] - start[1], end[0] - start[0]);
      ctx.lineCap = 'round';
      ctx.beginPath();
      ctx.moveTo(start[0], start[1]);
      ctx.lineTo(end[0], end[1]);
      ctx.strokeStyle = fade(p.shadow, 0.5);
      ctx.lineWidth = 5;
      ctx.stroke();
      ctx.strokeStyle = color;
      ctx.lineWidth = 2.4;
      ctx.stroke();
      const head = 7;
      ctx.beginPath();
      ctx.moveTo(end[0], end[1]);
      ctx.lineTo(
        end[0] - head * Math.cos(angle - 0.42),
        end[1] - head * Math.sin(angle - 0.42),
      );
      ctx.lineTo(
        end[0] - head * Math.cos(angle + 0.42),
        end[1] - head * Math.sin(angle + 0.42),
      );
      ctx.closePath();
      ctx.fillStyle = color;
      ctx.fill();
      const labelX = end[0] + Math.cos(angle) * 13;
      const labelY = end[1] + Math.sin(angle) * 13;
      ctx.font = '600 11px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.strokeStyle = p.panel;
      ctx.lineWidth = 3;
      ctx.strokeText(label, labelX, labelY);
      ctx.fillStyle = color;
      ctx.fillText(label, labelX, labelY);
    }
  }

  return {
    quatFromGravity,
    quatSlerp,
    quatToMatrix,
    transformPoint,
    tiltFromQuaternion,
    AttitudeFilter,
    SensorRenderer,
  };
});
