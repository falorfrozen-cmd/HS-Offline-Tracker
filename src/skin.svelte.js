import backdrop from './assets/brand/obsidian-arcane-bg.png';
import appMark from './assets/brand/app-mark.png';

let skin = $state('obsidian');

export function wearSkin(name) {
  skin = name === 'ember' ? 'ember' : name === 'void' ? 'void' : 'obsidian';
  if (typeof document !== 'undefined') document.documentElement.dataset.theme = skin;
}

const svg = (body, view = '0 0 64 64') =>
  `data:image/svg+xml,${encodeURIComponent(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="${view}">${body}</svg>`)}`;

const panel = svg(`
  <defs><linearGradient id="p" x2="0" y2="1"><stop stop-color="#1a2635"/><stop offset="1" stop-color="#080d15"/></linearGradient></defs>
  <rect x="2" y="2" width="60" height="60" rx="10" fill="url(#p)" stroke="#36506b" stroke-width="2"/>
  <path d="M3 18V8a5 5 0 0 1 5-5h10M46 3h10a5 5 0 0 1 5 5v10M61 46v10a5 5 0 0 1-5 5H46M18 61H8a5 5 0 0 1-5-5V46" fill="none" stroke="#d6a64c" stroke-width="1.4"/>
`);

const chip = svg(`
  <defs><linearGradient id="c" x2="1" y2="1"><stop stop-color="#142334"/><stop offset=".55" stop-color="#0b121d"/><stop offset="1" stop-color="#171323"/></linearGradient></defs>
  <rect x="1.5" y="1.5" width="61" height="61" rx="9" fill="url(#c)" stroke="#294157" stroke-width="2"/>
`);

const button = (hover = false, down = false) => svg(`
  <defs><linearGradient id="b" x2="0" y2="1"><stop stop-color="${down ? '#172231' : hover ? '#263e52' : '#1b2b3c'}"/><stop offset="1" stop-color="${down ? '#0b111b' : hover ? '#122235' : '#0d1623'}"/></linearGradient></defs>
  <rect x="2" y="7" width="60" height="50" rx="9" fill="url(#b)" stroke="${hover ? '#62d9c0' : '#36526c'}" stroke-width="2"/>
  <path d="M12 11h40M12 53h40" stroke="${hover ? '#d6a64c' : '#806735'}" opacity=".75"/>
`);

const icon = (path, hover = false) => svg(`
  <circle cx="32" cy="32" r="27" fill="${hover ? '#20394b' : '#101c29'}" stroke="${hover ? '#63ddc4' : '#40566c'}" stroke-width="2"/>
  <path d="${path}" fill="none" stroke="${hover ? '#f4d38a' : '#d9e3ed'}" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>
`);

const star = svg(`
  <defs><radialGradient id="g"><stop stop-color="#fff2b0"/><stop offset=".3" stop-color="#e2ad48"/><stop offset="1" stop-color="#8f3156" stop-opacity="0"/></radialGradient></defs>
  <circle cx="32" cy="32" r="30" fill="url(#g)"/><path d="M32 9l6.4 16.6L56 32l-17.6 6.4L32 55l-6.4-16.6L8 32l17.6-6.4z" fill="#f7d77f" stroke="#fff1bd"/>
`);

const transparent = svg('');
const assets = {
  app_mark: appMark,
  backdrop,
  panel,
  chip_dark: chip,
  button: button(),
  button_hover: button(true),
  button_down: button(false, true),
  header: svg('<rect width="64" height="64" rx="12" fill="#0b1522"/><path d="M7 50V22L20 9h24l13 13v28" fill="none" stroke="#d6a64c" stroke-width="2" opacity=".7"/>'),
  minimize: icon('M19 34h26'),
  minimize_hover: icon('M19 34h26', true),
  close: icon('M21 21l22 22M43 21L21 43'),
  close_hover: icon('M21 21l22 22M43 21L21 43', true),
  check_on: icon('M18 32l9 9 20-21', true),
  check_off: icon('M21 21l22 22M43 21L21 43'),
  dashboard: icon('M19 19h10v10H19zM35 19h10v10H35zM19 35h10v10H19zM35 35h10v10H35z'),
  dashboard_hover: icon('M19 19h10v10H19zM35 19h10v10H35zM19 35h10v10H19zM35 35h10v10H35z', true),
  lock: icon('M22 30h20v17H22zM26 30v-6a6 6 0 0 1 12 0v6'),
  lock_gold: icon('M22 30h20v17H22zM26 30v-6a6 6 0 0 1 12 0v6', true),
  lock_pale: icon('M22 30h20v17H22zM26 30v-6a6 6 0 0 1 12 0v6'),
  reset: icon('M44 25a15 15 0 1 0 1 14M44 25v-9m0 9h-9'),
  reset_hover: icon('M44 25a15 15 0 1 0 1 14M44 25v-9m0 9h-9', true),
  satanic_star: star,
  frozen_icon: icon('M32 15v34M18 23l28 18M46 23L18 41'),
  frozen: chip,
  coin_strip: svg('<circle cx="22" cy="32" r="13" fill="#d6a64c" stroke="#ffe3a0" stroke-width="2"/><circle cx="42" cy="32" r="13" fill="#8b6730" stroke="#d6a64c" stroke-width="2"/><path d="M18 32h8M38 32h8" stroke="#fff0bd" stroke-width="2"/>'),
  fx_glow: star,
  fx_sparks: svg('<g fill="#f7d77f"><circle cx="10" cy="18" r="2"/><circle cx="52" cy="13" r="1.5"/><circle cx="48" cy="49" r="2"/><circle cx="15" cy="52" r="1.5"/><circle cx="33" cy="8" r="1"/></g>'),
};

export function art(name) {
  void skin;
  return assets[name] ?? transparent;
}
