// 仅使用 Node 标准库，把开发源文件内联为可直接打开的独立 HTML。
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const files = {
  STYLE: 'src/style.css',
  PROTOCOL: 'js/protocol.js',
  ANALYSIS: 'src/analysis.js',
  TIMELINE: 'src/timeline.js',
  APP: 'src/app.js',
};
let html = fs.readFileSync(path.join(__dirname, 'client.template.html'), 'utf8');
for (const [token, file] of Object.entries(files)) {
  // 编码自包含资源，让生成的 HTML 保持模板行结构，源码仍可直接编辑。
  const source = fs.readFileSync(path.join(__dirname, file));
  html = html.replace(`/*__${token}__*/`, source.toString('base64'));
}
const target = path.join(__dirname, 'index.html');
fs.writeFileSync(target, html);
process.stdout.write(`已生成 ${target}（${Buffer.byteLength(html).toLocaleString('en-US')} B）\n`);
