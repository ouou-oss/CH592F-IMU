// 页面初始化与章节导航；普通脚本保证 file:// 下无需模块服务器。
'use strict';

renderPacketTabs();
renderFlags();
renderTable('#cmd-table', PROTO.cmds);
renderTable('#host-table', PROTO.hostTypes);
initDecoder();

const navLinks = Array.from(document.querySelectorAll('.sidebar nav a'));
const sections = Array.from(document.querySelectorAll('main > section'));
let scrollPending = false;

function updateNavigation() {
  const active = sections.reduce((last, section) => section.getBoundingClientRect().top <= 140 ? section.id : last, sections[0].id);
  navLinks.forEach((link) => {
    const on = link.hash === '#' + active;
    link.classList.toggle('active', on);
    if (on) link.setAttribute('aria-current', 'location');
    else link.removeAttribute('aria-current');
  });
  scrollPending = false;
}

window.addEventListener('scroll', () => {
  if (scrollPending) return;
  scrollPending = true;
  window.requestAnimationFrame(updateNavigation);
}, { passive: true });
updateNavigation();
