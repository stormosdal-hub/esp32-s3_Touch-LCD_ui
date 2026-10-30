/*
 * LCD UI Designer - browser editor for the ESP32-S3-Touch-LCD-7 firmware.
 * Edits the layout JSON (tabs, widgets, inputs, rules, settings) and deploys
 * it to the device with POST /api/layout. No external dependencies.
 */
'use strict';

/* ============================== Helpers ============================== */

const $ = (s, r = document) => r.querySelector(s);
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const clone = o => JSON.parse(JSON.stringify(o));
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  let value, checked;
  for (const [k, v] of Object.entries(props || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
    else if (k === 'html') el.innerHTML = v;
    else if (k === 'value') value = v;
    else if (k === 'checked') checked = v;
    else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat(Infinity)) {
    if (c == null || c === false) continue;
    el.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  if (value !== undefined) el.value = value;
  if (checked !== undefined) el.checked = !!checked;
  return el;
}

function toast(msg, kind = '') {
  const t = h('div', { class: 'toast ' + kind }, msg);
  $('#toasts').append(t);
  setTimeout(() => t.remove(), kind === 'err' ? 7000 : 3500);
}

function modal(content) {
  const body = $('#modalBody');
  body.replaceChildren(content);
  $('#modal').hidden = false;
}

const api = {
  async get(path) {
    const r = await fetch(path, { cache: 'no-store' });
    if (!r.ok) throw new Error(`${path}: ${r.status} ${r.statusText}`);
    return r.json();
  },
  async post(path, body, type = 'application/json') {
    const r = await fetch(path, { method: 'POST', headers: { 'Content-Type': type }, body });
    let j = {};
    try { j = await r.json(); } catch (e) { /* empty body */ }
    if (!r.ok || j.ok === false) throw new Error(j.error || `${r.status} ${r.statusText}`);
    return j;
  },
};

function numToStr(v) {
  if (typeof v !== 'number') return String(v ?? '');
  return Number.isInteger(v) ? String(v) : String(Number(v.toPrecision(6)));
}

/* Same rules as util_format_value() in the firmware. */
function fmtValue(fmt, num, text) {
  if (text == null) text = numToStr(num);
  if (!fmt) return text;
  return fmt.replace(/%%|%([-+ 0#]*)(\d{0,3})(?:\.(\d{0,2}))?([diuxXfFeEgGs])/g, (m, flags, width, prec, conv) => {
    if (m === '%%') return '%';
    const p = prec === undefined ? 6 : (prec === '' ? 0 : +prec);
    const n = Number(num) || 0;
    let s;
    switch (conv) {
      case 'd': case 'i': case 'u': s = String(Math.round(n)); break;
      case 'x': s = (Math.round(n) >>> 0).toString(16); break;
      case 'X': s = (Math.round(n) >>> 0).toString(16).toUpperCase(); break;
      case 'f': case 'F': s = n.toFixed(p); break;
      case 'e': case 'E': s = n.toExponential(p); if (conv === 'E') s = s.toUpperCase(); break;
      case 'g': case 'G': s = String(Number(n.toPrecision(p || 1))); break;
      default: s = String(text);
    }
    if (flags.includes('+') && conv !== 's' && n >= 0) s = '+' + s;
    if (width && s.length < +width) {
      if (flags.includes('-')) s = s.padEnd(+width);
      else if (flags.includes('0') && conv !== 's') s = s.replace(/^([+-]?)/, (_, sg) => sg + '0'.repeat(+width - s.length));
      else s = s.padStart(+width);
    }
    return s;
  });
}

/* ============================== Definitions ============================== */

const FONT_SIZES = [12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 48];
const EVENT_NAMES = {
  clicked: 'Clicked', pressed: 'Pressed', released: 'Released', long_pressed: 'Long press',
  changed: 'Value changed', submit: 'Text submitted',
};

const F = {
  text: { label: 'Text', type: 'text' },
  options: { label: 'Options', type: 'textarea', help: 'One option per line. Value = index (0, 1, ...), {text} = option text.' },
  signal: { label: 'Signal', type: 'signal', help: 'Shows this signal; user changes write back to it.' },
  format: { label: 'Format', type: 'text', placeholder: 'e.g. %.1f °C or Mode: %s', help: 'printf style: %d %.1f %s %x. Empty = plain value.' },
  font: { label: 'Font size', type: 'font' },
  fg: { label: 'Text color', type: 'color' },
  bg: { label: 'Background', type: 'color' },
  accent: { label: 'Accent color', type: 'color' },
  track: { label: 'Track color', type: 'color' },
  border_color: { label: 'Border color', type: 'color' },
  border_width: { label: 'Border width', type: 'number', placeholder: 'theme' },
  radius: { label: 'Corner radius', type: 'number', placeholder: 'theme' },
  align: { label: 'Text align', type: 'select', options: [['', 'left'], ['center', 'center'], ['right', 'right']] },
  center_v: { label: 'Center vertically', type: 'checkbox' },
  toggle: { label: 'Toggle button', type: 'checkbox', help: 'Stays on/off. Otherwise the signal is 1 while pressed.' },
  min: { label: 'Min', type: 'number', def: 0 },
  max: { label: 'Max', type: 'number', def: 100 },
  decimals: { label: 'Decimals', type: 'select', options: [['', '0'], [1, '1'], [2, '2'], [3, '3']], numeric: true },
  show_value: { label: 'Show value', type: 'checkbox' },
  thickness: { label: 'Thickness', type: 'number', placeholder: 'theme' },
  numeric: { label: 'Numeric keypad', type: 'checkbox' },
  points: { label: 'Points', type: 'number', def: 60 },
  sample_ms: { label: 'Sample every', type: 'number', def: 1000, help: 'ms. 0 = add a point on every change.' },
  chart_type: { label: 'Chart type', type: 'select', options: [['', 'line'], ['bar', 'bar']] },
  legend: { label: 'Legend', type: 'checkbox', def: true },
  signals: { label: 'Log signals', type: 'text', placeholder: '* or name1, name2', help: '* = all changes (except sys.*). Log actions always appear.' },
  max_lines: { label: 'Max lines', type: 'number', def: 50 },
  newest_first: { label: 'Newest first', type: 'checkbox' },
  hidden: { label: 'Hidden', type: 'checkbox' },
};

const COMMON_STYLE = ['bg', 'border_color', 'border_width', 'radius'];
const WIDGETS = {
  label: { name: 'Label', def: { w: 200, h: 40, text: 'Label', font: 20 }, content: ['text', 'font', 'align', 'center_v'], data: ['signal', 'format'], style: ['fg', ...COMMON_STYLE], events: ['clicked', 'long_pressed'] },
  button: { name: 'Button', def: { w: 180, h: 64, text: 'Button', font: 20 }, content: ['text', 'font', 'toggle'], data: ['signal'], style: ['fg', ...COMMON_STYLE], events: ['clicked', 'pressed', 'released', 'long_pressed', 'changed'] },
  switch: { name: 'Switch', def: { w: 76, h: 38 }, content: [], data: ['signal'], style: ['accent'], events: ['changed'] },
  checkbox: { name: 'Checkbox', def: { w: 220, h: 36, text: 'Checkbox' }, content: ['text', 'font'], data: ['signal'], style: ['fg', 'accent'], events: ['changed'] },
  slider: { name: 'Slider', def: { w: 300, h: 16, min: 0, max: 100 }, content: ['min', 'max', 'decimals'], data: ['signal'], style: ['accent', 'bg'], events: ['changed', 'released'] },
  arc: { name: 'Arc knob', def: { w: 200, h: 200, min: 0, max: 100, thickness: 16, font: 28 }, content: ['min', 'max', 'decimals', 'show_value', 'font', 'thickness'], data: ['signal', 'format'], style: ['fg', 'accent', 'track'], events: ['changed', 'released'] },
  gauge: { name: 'Gauge', def: { w: 200, h: 200, min: 0, max: 100, thickness: 14, font: 28 }, content: ['min', 'max', 'decimals', 'font', 'thickness'], data: ['signal', 'format'], style: ['fg', 'accent', 'track'], events: ['clicked'] },
  bar: { name: 'Bar', def: { w: 300, h: 26, min: 0, max: 100 }, content: ['min', 'max', 'decimals', 'show_value', 'font'], data: ['signal', 'format'], style: ['fg', 'accent', 'bg'], events: ['clicked'] },
  led: { name: 'LED', def: { w: 40, h: 40, accent: '#4CAF50' }, content: [], data: ['signal'], style: ['accent'], events: ['clicked'] },
  dropdown: { name: 'Dropdown', def: { w: 220, h: 46, options: 'Option 1\nOption 2\nOption 3' }, content: ['options', 'font'], data: ['signal'], style: ['fg', ...COMMON_STYLE], events: ['changed'] },
  chart: { name: 'Chart', def: { w: 420, h: 220, min: 0, max: 100, points: 60, sample_ms: 1000, series: [{ signal: '', color: '#2196F3' }] }, content: ['min', 'max', 'points', 'sample_ms', 'chart_type', 'legend', 'format'], data: [], style: COMMON_STYLE, events: ['clicked'] },
  log: { name: 'Event log', def: { w: 420, h: 200, signals: '*', max_lines: 50, font: 14 }, content: ['max_lines', 'newest_first', 'font'], data: ['signals'], style: ['fg', ...COMMON_STYLE], events: [] },
  panel: { name: 'Panel', def: { w: 300, h: 200, text: '', bg: '#263238', radius: 12, border_width: 0 }, content: ['text', 'font'], data: [], style: ['fg', ...COMMON_STYLE], events: ['clicked'] },
  input: { name: 'Text input', def: { w: 320, h: 50, text: 'Type here...' }, content: ['text', 'numeric', 'font'], data: ['signal'], style: ['fg', ...COMMON_STYLE], events: ['submit'] },
};

const DATA_FORMATS = ['u8', 's8', 'u16', 's16', 'u16le', 's16le', 'u32', 's32', 'u32le', 's32le', 'f32', 'f32le', 'bit0', 'bit1', 'bit2', 'bit3', 'bit4', 'bit5', 'bit6', 'bit7'];
const MODBUS_FORMATS = [['u16', 'u16'], ['s16', 's16'], ['u32', 'u32 (high word first)'], ['s32', 's32 (high word first)'], ['f32', 'float (high word first)'], ['u32r', 'u32 (low word first)'], ['s32r', 's32 (low word first)'], ['f32r', 'float (low word first)']];
const TEMPLATE_HELP = 'Placeholders: {value} {text} {id} {time} {date} {sig:NAME} {value:hex} {value:hex16} {value:int} {value:.2}';

const ACTIONS = {
  set_signal: { name: 'Set signal', fields: {
    signal: { label: 'Signal', type: 'signal' },
    mode: { label: 'Mode', type: 'select', options: [['', 'set'], ['toggle', 'toggle 0/1'], ['add', 'add']] },
    value: { label: 'Value', type: 'text', placeholder: '{value}', help: 'Text or number. ' + TEMPLATE_HELP } } },
  ws_send: { name: 'Send WebSocket message', fields: {
    payload: { label: 'Message', type: 'textarea', placeholder: '{"id":"{id}","value":{value}}', help: 'Sent to every client on ws://<device>/ws. ' + TEMPLATE_HELP } } },
  serial_send: { name: 'Send on RS-485', fields: {
    data: { label: 'Data', type: 'text', placeholder: 'PUMP ON or 01 06 00 01 00 {value:hex}', help: 'Text with \\r \\n \\xHH escapes, or hex bytes.' },
    hex: { label: 'Data is hex', type: 'checkbox' },
    append: { label: 'Append', type: 'select', options: [['', 'nothing'], ['cr', 'CR'], ['lf', 'LF'], ['crlf', 'CR LF'], ['modbus_crc', 'Modbus CRC']] } } },
  modbus_write: { name: 'Modbus write', fields: {
    slave: { label: 'Slave id', type: 'number', def: 1 },
    fn: { label: 'Function', type: 'select', numeric: true, options: [[6, '06 write register'], [5, '05 write coil'], [16, '16 write registers']] },
    reg: { label: 'Register', type: 'number', def: 0 },
    format: { label: 'Format (fn 16)', type: 'select', options: MODBUS_FORMATS },
    value: { label: 'Value', type: 'text', placeholder: '{value}' } } },
  gpio: { name: 'Set GPIO output', fields: {
    pin: { label: 'GPIO', type: 'gpio' },
    level: { label: 'Level', type: 'text', placeholder: '{value}', help: '0, 1, toggle or {value} (non-zero = high).' } } },
  i2c_write: { name: 'I2C write', fields: {
    addr: { label: 'Address', type: 'hex', placeholder: '0x20' },
    data: { label: 'Bytes (hex)', type: 'text', placeholder: '01 {value:hex}' } } },
  can_send: { name: 'CAN send', fields: {
    can_id: { label: 'CAN id', type: 'hex', placeholder: '0x123' },
    ext: { label: 'Extended id', type: 'checkbox' },
    data: { label: 'Bytes (hex)', type: 'text', placeholder: '01 02 {value:hex}' } } },
  mqtt_publish: { name: 'MQTT publish', fields: {
    topic: { label: 'Topic', type: 'text', placeholder: 'home/panel/{id}' },
    payload: { label: 'Payload', type: 'text', placeholder: '{value}' },
    qos: { label: 'QoS', type: 'select', numeric: true, options: [[0, '0'], [1, '1']] },
    retain: { label: 'Retain', type: 'checkbox' } } },
  http: { name: 'HTTP request', fields: {
    method: { label: 'Method', type: 'select', options: [['', 'GET'], ['POST', 'POST'], ['PUT', 'PUT']] },
    url: { label: 'URL', type: 'text', placeholder: 'http://192.168.1.20/relay?on={value}' },
    body: { label: 'Body', type: 'textarea' },
    content_type: { label: 'Content type', type: 'text', placeholder: 'application/json' } } },
  tab: { name: 'Switch tab', fields: { tab: { label: 'Tab', type: 'tab' } } },
  log: { name: 'Write to event log', fields: { text: { label: 'Text', type: 'text', placeholder: 'Pump started ({value})', help: TEMPLATE_HELP } } },
  backlight: { name: 'Backlight', fields: { state: { label: 'State', type: 'select', options: [['', 'toggle'], ['on', 'on'], ['off', 'off']] } } },
  delay: { name: 'Wait', fields: { ms: { label: 'Milliseconds', type: 'number', def: 500, help: 'Max 10000. Following actions run after the wait.' } } },
};

const SIGNAL_FIELD = { label: 'Signal', type: 'signal', help: 'Name of the signal that receives the value.' };
const SCALE_FIELDS = {
  scale: { label: 'Scale', type: 'number', def: 1 },
  offset: { label: 'Offset', type: 'number', def: 0, help: 'signal = raw × scale + offset' },
};
const INPUTS = {
  gpio: { name: 'GPIO input', fields: {
    pin: { label: 'GPIO', type: 'gpio' },
    pull: { label: 'Pull', type: 'select', options: [['', 'up'], ['down', 'down'], ['none', 'none']] },
    invert: { label: 'Invert', type: 'checkbox' },
    debounce_ms: { label: 'Debounce ms', type: 'number', def: 30 },
    signal: SIGNAL_FIELD } },
  adc: { name: 'Analog input (ADC)', fields: {
    pin: { label: 'GPIO', type: 'gpio', help: 'GPIO6 is the "Sensor" header.' },
    mode: { label: 'Unit', type: 'select', options: [['', 'millivolts'], ['raw', 'raw counts']] },
    period_ms: { label: 'Every ms', type: 'number', def: 1000 },
    ...SCALE_FIELDS, signal: SIGNAL_FIELD } },
  i2c: { name: 'I2C sensor poll', fields: {
    addr: { label: 'Address', type: 'hex', placeholder: '0x48' },
    reg: { label: 'Register', type: 'number', placeholder: 'none', help: 'Register byte written before reading. Empty = plain read.' },
    format: { label: 'Format', type: 'select', options: DATA_FORMATS.map(f => [f === 'u16' ? '' : f, f]) },
    len: { label: 'Read bytes', type: 'number', placeholder: 'auto' },
    period_ms: { label: 'Every ms', type: 'number', def: 1000 },
    ...SCALE_FIELDS, signal: SIGNAL_FIELD } },
  serial: { name: 'RS-485 text', fields: {
    mode: { label: 'Parse as', type: 'select', options: [['', 'whole line → signal'], ['kv', 'name=value lines'], ['json', 'JSON objects']] },
    signal: { label: 'Signal / prefix', type: 'signal', help: 'Line mode: target signal. kv/json: optional name prefix, e.g. "rs."' } } },
  modbus: { name: 'Modbus RTU poll', fields: {
    slave: { label: 'Slave id', type: 'number', def: 1 },
    fn: { label: 'Function', type: 'select', numeric: true, options: [[3, '03 holding registers'], [4, '04 input registers'], [1, '01 coils'], [2, '02 discrete inputs']] },
    reg: { label: 'Register', type: 'number', def: 0 },
    format: { label: 'Format', type: 'select', options: MODBUS_FORMATS },
    period_ms: { label: 'Every ms', type: 'number', def: 1000 },
    ...SCALE_FIELDS, signal: SIGNAL_FIELD } },
  can: { name: 'CAN frame', fields: {
    can_id: { label: 'CAN id', type: 'hex', placeholder: '0x123' },
    ext: { label: 'Extended id', type: 'checkbox' },
    start: { label: 'First byte', type: 'number', def: 0 },
    format: { label: 'Format', type: 'select', options: [['', 'u8'], ...DATA_FORMATS.filter(f => f !== 'u8').map(f => [f, f]), ['hex', 'hex text']] },
    ...SCALE_FIELDS, signal: SIGNAL_FIELD } },
  mqtt: { name: 'MQTT topic', fields: {
    topic: { label: 'Topic', type: 'text', placeholder: 'home/livingroom/temp (+ and # allowed)' },
    json_key: { label: 'JSON key', type: 'text', placeholder: 'optional, e.g. temperature' },
    ...SCALE_FIELDS, signal: SIGNAL_FIELD } },
};

const BUILTIN_SIGNALS = ['sys.time', 'sys.date', 'sys.ip', 'sys.wifi', 'sys.rssi', 'sys.heap', 'sys.psram', 'sys.uptime',
  'ui.tab', 'ui.backlight', 'rs485.rx', 'can.rx', 'ws.rx', 'mqtt.connected'];
const TIMEZONES = [
  ['UTC0', 'UTC'], ['CET-1CEST,M3.5.0,M10.5.0/3', 'Central Europe (Oslo, Berlin, Paris)'],
  ['GMT0BST,M3.5.0/1,M10.5.0', 'UK / Ireland'], ['EET-2EEST,M3.5.0/3,M10.5.0/4', 'Eastern Europe (Helsinki)'],
  ['EST5EDT,M3.2.0,M11.1.0', 'US Eastern'], ['CST6CDT,M3.2.0,M11.1.0', 'US Central'], ['PST8PDT,M3.2.0,M11.1.0', 'US Pacific'],
  ['JST-9', 'Japan'], ['AEST-10AEDT,M10.1.0,M4.1.0/3', 'Australia Eastern'],
];

/* ============================== State ============================== */

const state = {
  layout: null,
  tab: 0,
  sel: null,              // selected widget id
  mode: 'props',          // right panel
  undo: [], redo: [],
  snapshot: '',
  lastKey: '', lastTime: 0,
  dirty: false,
  info: null,
  signals: {},
  history: {},
  wsLog: [],
  ws: null, wsOk: false,
  clipboard: null,
  zoom: 1,
  drag: null,
};

const L = () => state.layout;
const settings = () => (L().settings ||= {});
const curTab = () => L().tabs[state.tab];

function emptyLayout() {
  return { version: 1, name: 'New layout', settings: { theme: 'dark', tabbar: 'top', tabbar_size: 50 }, ports: {}, signals: [], tabs: [{ id: 'tab1', title: 'Main', widgets: [] }], inputs: [], rules: [] };
}

function normalize(l) {
  l.settings ||= {};
  l.ports ||= {};
  l.signals ||= [];
  l.tabs ||= [];
  l.inputs ||= [];
  l.rules ||= [];
  if (!l.tabs.length) l.tabs.push({ id: 'tab1', title: 'Main', widgets: [] });
  for (const t of l.tabs) t.widgets ||= [];
  return l;
}

function allWidgets() {
  return L().tabs.flatMap((t, ti) => t.widgets.map(w => ({ w, t, ti })));
}

function findWidget(id) {
  for (const t of L().tabs) {
    const i = t.widgets.findIndex(w => w.id === id);
    if (i >= 0) return { w: t.widgets[i], tab: t, idx: i };
  }
  return null;
}

function uniqueId(prefix, taken) {
  const used = taken || new Set([...allWidgets().map(x => x.w.id), ...L().tabs.map(t => t.id), ...L().inputs.map(i => i.id), ...L().rules.map(r => r.id)]);
  for (let n = 1; ; n++) if (!used.has(prefix + n)) return prefix + n;
}

function contentRect() {
  const s = settings();
  const pos = s.tabbar || 'top';
  const size = pos === 'none' ? 0 : (s.tabbar_size ?? 50);
  const W = 800, H = 480;
  switch (pos) {
    case 'bottom': return { x: 0, y: 0, w: W, h: H - size, bar: { x: 0, y: H - size, w: W, h: size } };
    case 'left': return { x: size, y: 0, w: W - size, h: H, bar: { x: 0, y: 0, w: size, h: H }, vertical: true };
    case 'right': return { x: 0, y: 0, w: W - size, h: H, bar: { x: W - size, y: 0, w: size, h: H }, vertical: true };
    case 'none': return { x: 0, y: 0, w: W, h: H, bar: null };
    default: return { x: 0, y: size, w: W, h: H - size, bar: { x: 0, y: 0, w: W, h: size } };
  }
}

function knownSignals() {
  const s = new Set(BUILTIN_SIGNALS);
  const add = n => { if (n && typeof n === 'string' && !n.includes('*')) s.add(n); };
  const l = L();
  l.signals.forEach(x => add(x.name));
  allWidgets().forEach(({ w }) => { add(w.signal); (w.series || []).forEach(x => add(x.signal)); });
  l.inputs.forEach(i => add(i.signal));
  const scanActions = list => (list || []).forEach(a => a.type === 'set_signal' && add(a.signal));
  l.rules.forEach(r => { add(r.signal); scanActions(r.actions); });
  allWidgets().forEach(({ w }) => (w.events || []).forEach(e => scanActions(e.actions)));
  Object.keys(state.signals).forEach(add);
  return [...s].sort();
}

/* ============================== Undo / change tracking ============================== */

let draftTimer = 0;

function changed(key) {
  const snap = JSON.stringify(L());
  if (snap === state.snapshot) return;
  const now = Date.now();
  const coalesce = key && key === state.lastKey && now - state.lastTime < 1200;
  if (!coalesce) {
    state.undo.push(state.snapshot);
    if (state.undo.length > 100) state.undo.shift();
  }
  state.redo = [];
  state.snapshot = snap;
  state.lastKey = key || '';
  state.lastTime = now;
  setDirty(true);
  clearTimeout(draftTimer);
  draftTimer = setTimeout(saveDraft, 800);
  updateUndoButtons();
}

function resetHistory() {
  state.undo = [];
  state.redo = [];
  state.snapshot = JSON.stringify(L());
  updateUndoButtons();
}

function undo() {
  if (!state.undo.length) return;
  state.redo.push(state.snapshot);
  state.snapshot = state.undo.pop();
  state.layout = JSON.parse(state.snapshot);
  state.lastKey = '';
  afterLoad();
  setDirty(true);
}

function redo() {
  if (!state.redo.length) return;
  state.undo.push(state.snapshot);
  state.snapshot = state.redo.pop();
  state.layout = JSON.parse(state.snapshot);
  state.lastKey = '';
  afterLoad();
  setDirty(true);
}

function updateUndoButtons() {
  $('#btnUndo').disabled = !state.undo.length;
  $('#btnRedo').disabled = !state.redo.length;
}

function setDirty(d) {
  state.dirty = d;
  document.body.classList.toggle('dirty', d);
  $('#docname').textContent = (L()?.name || 'Layout') + (d ? ' (not deployed)' : '');
}

function afterLoad() {
  normalize(L());
  state.tab = clamp(state.tab, 0, L().tabs.length - 1);
  if (state.sel && !findWidget(state.sel)) state.sel = null;
  updateUndoButtons();
  renderAll();
}

function saveDraft() {
  try { localStorage.setItem('lcdui.draft', JSON.stringify({ t: Date.now(), layout: L() })); } catch (e) { /* storage unavailable */ }
}

function loadDraft() {
  try { return JSON.parse(localStorage.getItem('lcdui.draft') || 'null'); } catch (e) { return null; }
}

function clearDraft() {
  try { localStorage.removeItem('lcdui.draft'); } catch (e) { /* storage unavailable */ }
}

/* ============================== Form fields ============================== */

function colorField(obj, key, onchange) {
  const val = obj[key] || '';
  const text = h('input', { type: 'text', value: val, placeholder: 'theme' });
  const pick = h('input', { type: 'color', value: /^#[0-9a-f]{6}$/i.test(val) ? val : '#808080' });
  const apply = v => {
    if (v) obj[key] = v; else delete obj[key];
    onchange(key);
  };
  pick.addEventListener('input', () => { text.value = pick.value.toUpperCase(); apply(text.value); });
  text.addEventListener('input', () => {
    const v = text.value.trim();
    if (/^#?[0-9a-f]{6}$/i.test(v)) pick.value = v.startsWith('#') ? v : '#' + v;
    apply(v);
  });
  return h('div', { class: 'color' }, pick, text);
}

function fieldEl(obj, key, def, onchange) {
  const val = obj[key];
  const set = v => {
    if (v === '' || v === undefined || (typeof v === 'number' && Number.isNaN(v))) delete obj[key];
    else obj[key] = v;
    onchange(key);
  };
  let input;
  switch (def.type) {
    case 'textarea':
      input = h('textarea', { rows: def.rows || 3, value: val ?? '', placeholder: def.placeholder || '', oninput: e => set(e.target.value) });
      break;
    case 'number':
      input = h('input', { type: 'number', step: 'any', value: val ?? '', placeholder: def.placeholder ?? (def.def ?? ''),
        oninput: e => set(e.target.value === '' ? undefined : Number(e.target.value)) });
      break;
    case 'hex':
      input = h('input', { type: 'text', value: typeof val === 'number' ? '0x' + val.toString(16).toUpperCase() : (val ?? ''), placeholder: def.placeholder || '',
        oninput: e => { const v = e.target.value.trim(); set(v === '' ? undefined : (Number.isNaN(Number(v)) ? v : Number(v))); } });
      break;
    case 'checkbox':
      input = h('input', { type: 'checkbox', checked: val ?? def.def ?? false, onchange: e => { obj[key] = e.target.checked; onchange(key); } });
      input = h('div', null, input);
      break;
    case 'select':
    case 'font':
    case 'gpio':
    case 'tab': {
      let opts = def.options;
      if (def.type === 'font') opts = [['', 'default'], ...FONT_SIZES.map(s => [s, s + ' px'])];
      if (def.type === 'gpio') opts = (state.info?.user_gpios || [6, 15, 16, 19, 20, 43, 44]).map(g => [g, 'GPIO' + g]);
      if (def.type === 'tab') opts = L().tabs.map((t, i) => [t.id || String(i), t.title || t.id]);
      const numeric = def.numeric || def.type === 'font' || def.type === 'gpio';
      let cur = String(val ?? def.def ?? opts[0]?.[0] ?? '');
      if (!opts.some(o => String(o[0]) === cur)) {
        // e.g. "dark" stored explicitly while the option list uses '' for the default
        const byLabel = opts.find(o => String(o[1]) === cur);
        cur = String(byLabel ? byLabel[0] : opts[0]?.[0] ?? '');
      }
      input = h('select', { value: String(cur), onchange: e => { const v = e.target.value; set(v === '' ? undefined : (numeric ? Number(v) : v)); } },
        opts.map(o => h('option', { value: String(o[0]) }, o[1])));
      input.value = String(cur);
      break;
    }
    case 'color':
      input = colorField(obj, key, onchange);
      break;
    case 'signal':
      input = h('input', { type: 'text', value: val ?? '', list: 'sigList', placeholder: def.placeholder || 'signal name', spellcheck: 'false',
        oninput: e => set(e.target.value.trim()), onfocus: refreshSignalList });
      break;
    default:
      input = h('input', { type: 'text', value: val ?? '', placeholder: def.placeholder || '', oninput: e => set(e.target.value) });
  }
  return h('div', { class: 'field' + (def.type === 'textarea' ? ' wide' : '') },
    h('label', null, def.label || key), input, def.help ? h('div', { class: 'help' }, def.help) : null);
}

function refreshSignalList() {
  let dl = $('#sigList');
  if (!dl) { dl = h('datalist', { id: 'sigList' }); document.body.append(dl); }
  dl.replaceChildren(...knownSignals().map(s => h('option', { value: s })));
}

function fieldsFor(obj, defs, onchange) {
  return Object.entries(defs).map(([k, d]) => fieldEl(obj, k, d, onchange));
}

/* ============================== Actions editor ============================== */

function actionsEditor(list, keyPrefix) {
  const box = h('div');
  const rerender = () => { changed(); renderPanel(); };
  list.forEach((a, i) => {
    const def = ACTIONS[a.type] || { name: a.type, fields: {} };
    const typeSel = h('select', { value: a.type, onchange: e => { list[i] = { type: e.target.value }; rerender(); } },
      Object.entries(ACTIONS).map(([k, d]) => h('option', { value: k }, d.name)));
    box.append(h('div', { class: 'card inner' },
      h('div', { class: 'card-head' }, h('span', { class: 'muted' }, i + 1 + '.'), typeSel,
        h('button', { title: 'Move up', disabled: i === 0, onclick: () => { [list[i - 1], list[i]] = [list[i], list[i - 1]]; rerender(); } }, '↑'),
        h('button', { title: 'Move down', disabled: i === list.length - 1, onclick: () => { [list[i + 1], list[i]] = [list[i], list[i + 1]]; rerender(); } }, '↓'),
        h('button', { class: 'danger', title: 'Remove action', onclick: () => { list.splice(i, 1); rerender(); } }, '✕')),
      fieldsFor(a, def.fields, k => { changed(keyPrefix + i + k); renderStage(); })));
  });
  box.append(h('button', { onclick: () => { list.push({ type: 'log', text: '' }); rerender(); } }, '+ Add action'));
  return box;
}

function eventsEditor(w) {
  const allowed = WIDGETS[w.type]?.events || [];
  if (!allowed.length) return h('div', { class: 'hint' }, 'This widget has no events.');
  w.events ||= [];
  const box = h('div');
  const rerender = () => { changed(); renderPanel(); };
  w.events.forEach((ev, i) => {
    ev.actions ||= [];
    box.append(h('div', { class: 'card' },
      h('div', { class: 'card-head' }, h('span', { class: 'title' }, 'When'),
        h('select', { value: ev.on, onchange: e => { ev.on = e.target.value; changed(); } },
          allowed.map(n => h('option', { value: n }, EVENT_NAMES[n] || n))),
        h('button', { class: 'danger', title: 'Remove event', onclick: () => { w.events.splice(i, 1); rerender(); } }, '✕')),
      actionsEditor(ev.actions, `ev:${w.id}:${i}:`)));
  });
  box.append(h('button', { onclick: () => {
    const used = new Set(w.events.map(e => e.on));
    w.events.push({ on: allowed.find(n => !used.has(n)) || allowed[0], actions: [] });
    rerender();
  } }, '+ Add event'));
  return box;
}

/* ============================== Stage (device preview) ============================== */

function themeColors() {
  const s = settings();
  const dark = s.theme !== 'light';
  return {
    dark,
    scr: s.bg || (dark ? '#15171A' : '#F2F3F5'),
    card: dark ? '#282B30' : '#FFFFFF',
    text: dark ? '#EEEEEE' : '#212121',
    muted: dark ? '#3A3F47' : '#D0D4DA',
    primary: s.primary || '#2196F3',
    secondary: s.secondary || '#FF9800',
  };
}

function sigValue(name) {
  if (!name || !$('#livePreview').checked) return undefined;
  return state.signals[name];
}

function arcPath(cx, cy, r, a0, a1) {
  const rad = a => a * Math.PI / 180;
  const p = a => [cx + r * Math.cos(rad(a)), cy + r * Math.sin(rad(a))];
  let sweep = a1 - a0;
  if (sweep <= 0) sweep += 360;
  if (sweep >= 359.99) sweep = 359.99;
  const [x0, y0] = p(a0), [x1, y1] = p(a0 + sweep);
  return `M${x0.toFixed(1)} ${y0.toFixed(1)} A${r} ${r} 0 ${sweep > 180 ? 1 : 0} 1 ${x1.toFixed(1)} ${y1.toFixed(1)}`;
}

function widgetPreview(w, c) {
  const fs = w.font || 16;
  const fg = w.fg || c.text;
  const accent = w.accent || c.primary;
  const live = sigValue(w.signal);
  const min = w.min ?? 0, max = w.max ?? 100;
  const num = typeof live === 'number' ? live : (typeof live === 'string' ? (parseFloat(live) || 0) : null);
  const sample = num ?? (min + (max - min) * 0.4);
  const frac = clamp((sample - min) / ((max - min) || 1), 0, 1);
  const dec = w.decimals || 0;
  const valFmt = w.format || `%.${dec}f`;
  const base = `color:${fg};font-size:${fs}px;`;
  switch (w.type) {
    case 'label': {
      const text = live !== undefined ? fmtValue(w.format, num ?? 0, typeof live === 'string' ? live : null) : (w.text || '');
      const pad = w.center_v ? Math.max(0, (w.h - fs * 1.2) / 2) : 0;
      return `<div class="lv-label" style="${base}text-align:${w.align || 'left'};padding-top:${pad}px;height:100%;background:${w.bg || 'transparent'};border-radius:${w.radius ?? 0}px;${w.border_width ? `border:${w.border_width}px solid ${w.border_color || c.primary};` : ''}">${esc(text)}</div>`;
    }
    case 'button': {
      const on = w.toggle && num;
      const bg = on ? c.secondary : (w.bg || c.primary);
      return `<div class="lv-btn" style="width:100%;height:100%;background:${bg};color:${w.fg || '#fff'};font-size:${fs}px;border-radius:${w.radius ?? 8}px;${w.border_width ? `border:${w.border_width}px solid ${w.border_color || '#fff'};` : ''}">${esc(w.text || '')}</div>`;
    }
    case 'switch': {
      const on = num != null ? !!num : false;
      const d = w.h - 8;
      return `<div style="width:100%;height:100%;border-radius:${w.h}px;background:${on ? accent : c.muted};position:relative">
        <div style="position:absolute;top:4px;left:${on ? w.w - d - 4 : 4}px;width:${d}px;height:${d}px;border-radius:50%;background:#fff"></div></div>`;
    }
    case 'checkbox': {
      const on = num != null ? !!num : true;
      const b = Math.round(fs * 1.25);
      return `<div style="display:flex;align-items:center;gap:8px;height:100%;${base}">
        <span style="width:${b}px;height:${b}px;border-radius:4px;border:2px solid ${accent};background:${on ? accent : 'transparent'};color:#fff;font-size:${fs * 0.8}px;display:flex;align-items:center;justify-content:center">${on ? '✓' : ''}</span>${esc(w.text || '')}</div>`;
    }
    case 'slider': {
      const vert = w.h > w.w;
      const k = Math.min(w.w, w.h) + 12;
      const track = `background:${w.bg || c.muted};border-radius:${Math.min(w.w, w.h)}px;position:absolute;inset:0`;
      const ind = vert ? `bottom:0;left:0;right:0;height:${frac * 100}%` : `top:0;bottom:0;left:0;width:${frac * 100}%`;
      const knob = vert ? `left:${(w.w - k) / 2}px;bottom:calc(${frac * 100}% - ${k / 2}px)` : `top:${(w.h - k) / 2}px;left:calc(${frac * 100}% - ${k / 2}px)`;
      return `<div style="${track}"><div style="position:absolute;${ind};background:${accent};border-radius:inherit"></div></div>
        <div style="position:absolute;${knob};width:${k}px;height:${k}px;border-radius:50%;background:${accent};box-shadow:0 0 0 3px rgba(255,255,255,.15)"></div>`;
    }
    case 'arc':
    case 'gauge': {
      const t = w.thickness || 14;
      const sz = Math.min(w.w, w.h);
      const r = sz / 2 - t / 2 - (w.type === 'arc' ? 6 : 0);
      const cx = w.w / 2, cy = w.h / 2;
      const end = 135 + 270 * frac;
      const showVal = w.type === 'gauge' || w.show_value !== false;
      const kx = cx + r * Math.cos(end * Math.PI / 180), ky = cy + r * Math.sin(end * Math.PI / 180);
      return `<svg width="${w.w}" height="${w.h}"><path d="${arcPath(cx, cy, r, 135, 45)}" stroke="${w.track || c.muted}" stroke-width="${t}" fill="none" stroke-linecap="round"/>
        ${frac > 0.001 ? `<path d="${arcPath(cx, cy, r, 135, end)}" stroke="${accent}" stroke-width="${t}" fill="none" stroke-linecap="round"/>` : ''}
        ${w.type === 'arc' ? `<circle cx="${kx}" cy="${ky}" r="${t / 2 + 5}" fill="${accent}"/>` : ''}</svg>
        ${showVal ? `<div style="position:absolute;inset:0;display:flex;align-items:center;justify-content:center;${base}">${esc(fmtValue(valFmt, sample, null))}</div>` : ''}`;
    }
    case 'bar':
      return `<div style="position:absolute;inset:0;background:${w.bg || c.muted};border-radius:${w.h}px;overflow:hidden">
        <div style="height:100%;width:${frac * 100}%;background:${accent}"></div></div>
        ${w.show_value ? `<div style="position:absolute;inset:0;display:flex;align-items:center;justify-content:center;${base}">${esc(fmtValue(valFmt, sample, null))}</div>` : ''}`;
    case 'led': {
      const on = num != null ? !!num : true;
      return `<div style="width:100%;height:100%;border-radius:50%;opacity:${on ? 1 : 0.35};background:radial-gradient(circle at 35% 35%, rgba(255,255,255,${on ? 0.6 : 0.2}), ${accent} 55%)"></div>`;
    }
    case 'dropdown': {
      const opts = (w.options || '').split('\n');
      let idx = num != null ? Math.round(num) : 0;
      if (typeof live === 'string' && opts.includes(live)) idx = opts.indexOf(live);
      return `<div class="lv-card" style="height:100%;background:${w.bg || c.card};border:${w.border_width ?? 2}px solid ${w.border_color || c.muted};display:flex;align-items:center;justify-content:space-between;padding:0 12px;${base}">
        <span>${esc(opts[idx] ?? '')}</span><span style="opacity:.7">▾</span></div>`;
    }
    case 'chart': {
      const series = w.series || [];
      const pts = clamp(w.points || 60, 2, 500);
      let lines = '';
      for (let i = 1; i < 5; i++) lines += `<line x1="0" x2="${w.w}" y1="${w.h * i / 5}" y2="${w.h * i / 5}" stroke="${c.muted}" stroke-width="1"/>`;
      for (let i = 1; i < 8; i++) lines += `<line y1="0" y2="${w.h}" x1="${w.w * i / 8}" x2="${w.w * i / 8}" stroke="${c.muted}" stroke-width="1"/>`;
      let poly = '';
      series.forEach((s, si) => {
        const hist = $('#livePreview').checked ? (state.history[s.signal] || []) : [];
        const vals = hist.length > 1 ? hist.slice(-pts) : Array.from({ length: pts }, (_, i) => min + (max - min) * (0.5 + 0.35 * Math.sin(i / 6 + si * 1.7)));
        const xs = i => (i * w.w / (pts - 1)).toFixed(1);
        const ys = v => (w.h - clamp((v - min) / ((max - min) || 1), 0, 1) * w.h).toFixed(1);
        const off = pts - vals.length;
        if (w.chart_type === 'bar') {
          const bw = Math.max(1, w.w / pts / Math.max(1, series.length) - 1);
          vals.forEach((v, i) => { poly += `<rect x="${(i + off) * w.w / pts + si * bw}" y="${ys(v)}" width="${bw}" height="${w.h - ys(v)}" fill="${s.color || c.primary}"/>`; });
        } else {
          poly += `<polyline fill="none" stroke="${s.color || c.primary}" stroke-width="2" points="${vals.map((v, i) => xs(i + off) + ',' + ys(v)).join(' ')}"/>`;
        }
      });
      const legend = w.legend !== false ? series.filter(s => s.signal).map(s => esc(s.signal) + ': ' + esc(fmtValue(w.format || '%.1f', Number(sigValue(s.signal) ?? 0), null))).join('<br>') : '';
      return `<div class="lv-card" style="position:absolute;inset:0;background:${w.bg || c.card};padding:8px;${w.border_width ? `border:${w.border_width}px solid ${w.border_color};` : ''}">
        <svg width="100%" height="100%" viewBox="0 0 ${w.w} ${w.h}" preserveAspectRatio="none">${lines}${poly}</svg>
        <div style="position:absolute;left:10px;top:8px;font-size:12px;color:${c.text}">${legend}</div></div>`;
    }
    case 'log': {
      const lines = ['12:00:01 demo.level = 40', '12:00:02 Hello button pressed', '12:00:05 rs485.rx = OK'].join('\n');
      return `<div class="lv-card lv-log" style="position:absolute;inset:0;background:${w.bg || c.card};${base}">${esc(lines)}</div>`;
    }
    case 'panel':
      return `<div class="lv-card" style="position:absolute;inset:0;background:${w.bg || c.card};border-radius:${w.radius ?? 8}px;${w.border_width ? `border:${w.border_width}px solid ${w.border_color || c.muted};` : ''}padding:16px;${base}">${esc(w.text || '')}</div>`;
    case 'input':
      return `<div class="lv-card" style="position:absolute;inset:0;background:${w.bg || c.card};border:2px solid ${w.border_color || c.muted};padding:0 12px;display:flex;align-items:center;${base}">
        <span style="opacity:.5">${esc(typeof live === 'string' && live ? live : (w.text || ''))}</span></div>`;
    default:
      return `<div style="border:1px dashed red;height:100%">${esc(w.type)}</div>`;
  }
}

const HANDLES = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w'];
const HANDLE_POS = { nw: [0, 0], n: [50, 0], ne: [100, 0], e: [100, 50], se: [100, 100], s: [50, 100], sw: [0, 100], w: [0, 50] };

function computeZoom() {
  const z = $('#zoom').value;
  if (z !== 'fit') return Number(z);
  const wrap = $('#stageWrap');
  return clamp(Math.min((wrap.clientWidth - 60) / 800, (wrap.clientHeight - 60) / 480), 0.3, 2);
}

function renderStage() {
  const stage = $('#stage');
  if (!L()) return;
  const c = themeColors();
  const z = state.zoom = computeZoom();
  const r = contentRect();
  const s = settings();
  stage.style.width = 800 * z + 'px';
  stage.style.height = 480 * z + 'px';
  stage.style.setProperty('--lv-primary', c.primary);

  const dev = h('div', { class: 'device', style: { width: '800px', height: '480px', background: c.scr, color: c.text, transform: `scale(${z})`, transformOrigin: '0 0' } });
  if (r.bar) {
    const bar = h('div', { class: 'tabbar' + (r.vertical ? ' vertical' : ''), style: { left: r.bar.x + 'px', top: r.bar.y + 'px', width: r.bar.w + 'px', height: r.bar.h + 'px', background: c.card, fontSize: (s.tab_font || 16) + 'px' } });
    L().tabs.forEach((t, i) => bar.append(h('div', { class: 't' + (i === state.tab ? ' on' : ''), onpointerdown: e => { e.stopPropagation(); selectTab(i); } }, t.title || t.id)));
    dev.append(bar);
  }
  const tab = curTab();
  const content = h('div', { class: 'content' + ($('#showGrid').checked && gridSize() > 1 ? ' grid' : ''), id: 'content',
    style: { left: r.x + 'px', top: r.y + 'px', width: r.w + 'px', height: r.h + 'px', background: tab.bg || 'transparent', backgroundSize: `${gridSize() * 2}px ${gridSize() * 2}px` } });
  content.addEventListener('pointerdown', e => { if (e.target === content) { select(null); } });
  content.addEventListener('pointermove', e => {
    const p = toContent(e);
    $('#cursorPos').textContent = `x ${Math.round(p.x)}  y ${Math.round(p.y)}`;
  });
  for (const w of tab.widgets) {
    const el = h('div', { class: 'w' + (w.id === state.sel ? ' sel' : '') + (w.hidden ? ' hiddenw' : ''), 'data-id': w.id,
      style: { left: (w.x || 0) + 'px', top: (w.y || 0) + 'px', width: (w.w || 100) + 'px', height: (w.h || 40) + 'px' } });
    el.append(h('div', { class: 'inner', html: widgetPreview({ ...WIDGETS[w.type]?.def, ...w, w: w.w || 100, h: w.h || 40 }, c) }));
    if (w.id === state.sel) {
      el.append(h('div', { class: 'idtag' }, w.id));
      for (const hd of HANDLES) {
        const [px, py] = HANDLE_POS[hd];
        el.append(h('div', { class: 'handle', 'data-h': hd, style: { left: `calc(${px}% - 5px)`, top: `calc(${py}% - 5px)`, cursor: hd + '-resize' } }));
      }
    }
    el.addEventListener('pointerdown', e => startDrag(e, w, e.target.dataset.h || null));
    content.append(el);
  }
  dev.append(content);
  stage.replaceChildren(dev);
}

function gridSize() { return Number($('#grid').value) || 1; }
function snap(v) { const g = gridSize(); return Math.round(v / g) * g; }

function toContent(e) {
  const rect = $('#content').getBoundingClientRect();
  return { x: (e.clientX - rect.left) / state.zoom, y: (e.clientY - rect.top) / state.zoom };
}

function startDrag(e, w, handle) {
  if (e.button !== 0) return;
  e.stopPropagation();
  e.preventDefault();
  if (state.sel !== w.id) select(w.id);
  const p = toContent(e);
  state.drag = { id: w.id, handle, sx: p.x, sy: p.y, x: w.x || 0, y: w.y || 0, w: w.w || 100, h: w.h || 40, moved: false };
  window.addEventListener('pointermove', onDrag);
  window.addEventListener('pointerup', endDrag, { once: true });
}

function onDrag(e) {
  const d = state.drag;
  if (!d) return;
  const f = findWidget(d.id);
  if (!f) return;
  const w = f.w;
  const p = toContent(e);
  const dx = p.x - d.sx, dy = p.y - d.sy;
  if (!d.moved && Math.abs(dx) < 2 && Math.abs(dy) < 2) return;
  d.moved = true;
  if (!d.handle) {
    w.x = snap(d.x + dx);
    w.y = snap(d.y + dy);
  } else {
    const hd = d.handle;
    let { x, y, w: ww, h: hh } = d;
    if (hd.includes('e')) ww = snap(d.w + dx);
    if (hd.includes('s')) hh = snap(d.h + dy);
    if (hd.includes('w')) { x = snap(d.x + dx); ww = d.w + (d.x - x); }
    if (hd.includes('n')) { y = snap(d.y + dy); hh = d.h + (d.y - y); }
    w.x = x; w.y = y; w.w = Math.max(8, ww); w.h = Math.max(8, hh);
  }
  const el = document.querySelector(`.w[data-id="${CSS.escape(d.id)}"]`);
  if (el) {
    Object.assign(el.style, { left: w.x + 'px', top: w.y + 'px', width: w.w + 'px', height: w.h + 'px' });
    if (d.handle) el.querySelector('.inner').innerHTML = widgetPreview({ ...WIDGETS[w.type]?.def, ...w }, themeColors());
  }
  $('#cursorPos').textContent = `x ${w.x}  y ${w.y}  w ${w.w}  h ${w.h}`;
}

function endDrag() {
  window.removeEventListener('pointermove', onDrag);
  const d = state.drag;
  state.drag = null;
  if (d && d.moved) {
    changed();
    renderStage();
    if (state.mode === 'props') renderPanel();
  }
}

/* ============================== Left sidebar ============================== */

function renderPalette() {
  $('#palette').replaceChildren(...Object.entries(WIDGETS).map(([type, d]) =>
    h('button', { title: 'Add ' + d.name, onclick: () => addWidget(type) }, d.name)));
}

function renderTabList() {
  const list = $('#tabList');
  list.replaceChildren(...L().tabs.map((t, i) => h('li', { class: i === state.tab ? 'active' : '', onclick: () => selectTab(i) },
    h('span', { class: 'name' }, t.title || t.id),
    h('button', { title: 'Move up', disabled: i === 0, onclick: e => { e.stopPropagation(); moveTab(i, -1); } }, '↑'),
    h('button', { title: 'Move down', disabled: i === L().tabs.length - 1, onclick: e => { e.stopPropagation(); moveTab(i, 1); } }, '↓'),
    h('button', { class: 'danger', title: 'Delete tab', onclick: e => { e.stopPropagation(); deleteTab(i); } }, '✕'))));
}

function renderWidgetList() {
  const list = $('#widgetList');
  const ws = curTab().widgets;
  if (!ws.length) { list.replaceChildren(h('li', { class: 'muted' }, 'none yet')); return; }
  list.replaceChildren(...ws.map(w => h('li', { class: w.id === state.sel ? 'active' : '', onclick: () => select(w.id) },
    h('span', { class: 'name' }, w.id), h('span', { class: 'type' }, WIDGETS[w.type]?.name || w.type))));
}

/* ============================== Operations ============================== */

function selectTab(i) {
  state.tab = i;
  state.sel = null;
  renderAll();
}

function select(id) {
  state.sel = id;
  if (id && state.mode !== 'props' && state.mode !== 'json') state.mode = 'props';
  renderStage();
  renderWidgetList();
  renderModes();
  renderPanel();
}

function addWidget(type) {
  const d = WIDGETS[type];
  const r = contentRect();
  const w = { id: uniqueId(type), type, x: 0, y: 0, ...clone(d.def) };
  // place it free-ish: cascade from the top-left
  const n = curTab().widgets.length;
  w.x = snap(Math.min(20 + (n % 8) * 20, Math.max(0, r.w - w.w)));
  w.y = snap(Math.min(20 + (n % 8) * 20, Math.max(0, r.h - w.h)));
  curTab().widgets.push(w);
  changed();
  select(w.id);
}

function deleteSelected() {
  const f = state.sel && findWidget(state.sel);
  if (!f) return;
  f.tab.widgets.splice(f.idx, 1);
  state.sel = null;
  changed();
  renderAll();
}

function duplicateSelected(target) {
  const f = state.sel && findWidget(state.sel);
  if (!f) return;
  pasteWidget(clone(f.w), target);
}

function pasteWidget(src, target) {
  const w = clone(src);
  w.id = uniqueId(w.type);
  const g = Math.max(10, gridSize());
  w.x = (w.x || 0) + g;
  w.y = (w.y || 0) + g;
  (target || curTab()).widgets.push(w);
  changed();
  select(w.id);
}

function reorder(delta) {
  const f = state.sel && findWidget(state.sel);
  if (!f) return;
  const list = f.tab.widgets;
  list.splice(f.idx, 1);
  list.splice(delta > 0 ? list.length : 0, 0, f.w);
  changed();
  renderAll();
}

function moveToTab(tabIndex) {
  const f = state.sel && findWidget(state.sel);
  if (!f) return;
  f.tab.widgets.splice(f.idx, 1);
  L().tabs[tabIndex].widgets.push(f.w);
  state.tab = tabIndex;
  changed();
  renderAll();
}

function addTab() {
  const id = uniqueId('tab');
  L().tabs.push({ id, title: 'Tab ' + (L().tabs.length + 1), widgets: [] });
  state.tab = L().tabs.length - 1;
  state.sel = null;
  changed();
  renderAll();
}

function moveTab(i, d) {
  const t = L().tabs;
  const j = i + d;
  if (j < 0 || j >= t.length) return;
  [t[i], t[j]] = [t[j], t[i]];
  state.tab = j;
  changed();
  renderAll();
}

function deleteTab(i) {
  const t = L().tabs[i];
  if (L().tabs.length === 1) { toast('A layout needs at least one tab.', 'err'); return; }
  if (t.widgets.length && !confirm(`Delete tab "${t.title}" and its ${t.widgets.length} widgets?`)) return;
  L().tabs.splice(i, 1);
  state.tab = clamp(state.tab, 0, L().tabs.length - 1);
  state.sel = null;
  changed();
  renderAll();
}

/* ============================== Right panel ============================== */

const MODES = [['props', 'Properties'], ['inputs', 'Inputs'], ['rules', 'Rules'], ['settings', 'Settings'], ['monitor', 'Monitor'], ['device', 'Device'], ['json', 'JSON']];

function renderModes() {
  $('#modes').replaceChildren(...MODES.map(([k, n]) => h('button', { class: state.mode === k ? 'active' : '', onclick: () => { state.mode = k; renderModes(); renderPanel(); } }, n)));
}

function renderPanel() {
  const p = $('#panel');
  const scroll = p.scrollTop;
  const fn = { props: panelProps, inputs: panelInputs, rules: panelRules, settings: panelSettings, monitor: panelMonitor, device: panelDevice, json: panelJson }[state.mode];
  p.replaceChildren(...[fn()].flat(Infinity).filter(x => x != null && x !== false));
  p.scrollTop = scroll;
}

function onProp(w) {
  return key => {
    changed(`w:${w.id}:${key}`);
    renderStage();
    if (key === 'id') renderWidgetList();
  };
}

function panelProps() {
  const f = state.sel && findWidget(state.sel);
  if (!f) return panelTab();
  const w = f.w;
  const d = WIDGETS[w.type] || { name: w.type, content: [], data: [], style: [], events: [] };
  const on = onProp(w);
  const idInput = h('input', { type: 'text', value: w.id, spellcheck: 'false', onchange: e => {
    const v = e.target.value.trim();
    if (!v || (v !== w.id && findWidget(v))) { toast('Widget ids must be unique and not empty.', 'err'); e.target.value = w.id; return; }
    w.id = v; state.sel = v; changed(); renderAll();
  } });
  const geo = ['x', 'y', 'w', 'h'].map(k => h('label', { class: 'row' }, h('span', { class: 'fixed muted' }, k),
    h('input', { type: 'number', value: w[k] ?? 0, oninput: e => { w[k] = Number(e.target.value) || 0; on(k); } })));
  const out = [
    h('div', { class: 'card-head' }, h('span', { class: 'title' }, d.name),
      h('button', { title: 'Duplicate (Ctrl+D)', onclick: () => duplicateSelected() }, 'Duplicate'),
      h('button', { title: 'Bring to front', onclick: () => reorder(1) }, 'Front'),
      h('button', { title: 'Send to back', onclick: () => reorder(-1) }, 'Back'),
      h('button', { class: 'danger', title: 'Delete (Del)', onclick: deleteSelected }, 'Delete')),
    h('div', { class: 'field' }, h('label', null, 'Id'), idInput),
    h('div', { class: 'field' }, h('label', null, 'Position / size'), h('div', { style: { display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '4px' } }, geo)),
    L().tabs.length > 1 ? h('div', { class: 'field' }, h('label', null, 'Move to tab'),
      h('select', { value: String(state.tab), onchange: e => moveToTab(Number(e.target.value)) }, L().tabs.map((t, i) => h('option', { value: String(i) }, t.title || t.id)))) : null,
    fieldEl(w, 'hidden', F.hidden, on),
  ];
  if (d.content.length) out.push(h('h4', null, 'Content'), d.content.map(k => fieldEl(w, k, F[k], on)));
  if (w.type === 'chart') out.push(h('h4', null, 'Series'), seriesEditor(w));
  if (d.data.length) out.push(h('h4', null, 'Data'), d.data.map(k => fieldEl(w, k, F[k], on)));
  if (d.style.length) out.push(h('h4', null, 'Style'), d.style.map(k => fieldEl(w, k, F[k], on)));
  out.push(h('h4', null, 'Events'), eventsEditor(w));
  return out;
}

function seriesEditor(w) {
  w.series ||= [];
  const box = h('div');
  const rerender = () => { changed(); renderPanel(); renderStage(); };
  w.series.forEach((s, i) => box.append(h('div', { class: 'card inner' },
    h('div', { class: 'card-head' }, h('span', { class: 'title' }, 'Series ' + (i + 1)),
      h('button', { class: 'danger', onclick: () => { w.series.splice(i, 1); rerender(); } }, '✕')),
    fieldEl(s, 'signal', F.signal, k => { changed(`series:${w.id}:${i}`); renderStage(); }),
    fieldEl(s, 'color', { label: 'Color', type: 'color' }, () => { changed(`series:${w.id}:${i}c`); renderStage(); }))));
  if (w.series.length < 4) box.append(h('button', { onclick: () => { w.series.push({ signal: '', color: ['#2196F3', '#FF9800', '#4CAF50', '#E91E63'][w.series.length] }); rerender(); } }, '+ Add series'));
  return box;
}

function panelTab() {
  const t = curTab();
  const on = key => { changed('tab:' + key); renderStage(); if (key === 'title') renderTabList(); };
  return [
    h('h4', null, 'Tab'),
    fieldEl(t, 'title', { label: 'Title', type: 'text' }, on),
    fieldEl(t, 'id', { label: 'Id', type: 'text', help: 'Used by "Switch tab" actions and WebSocket {"tab": "..."}.' }, on),
    fieldEl(t, 'bg', { label: 'Background', type: 'color' }, on),
    h('h4', null, 'Getting started'),
    h('div', { class: 'hint', html: `
      <p>Add widgets from the left, drag them into place and resize with the handles. Select a widget to edit it here.</p>
      <p><b>Signals</b> are named values that connect everything: inputs (GPIO, RS-485, Modbus, I2C, CAN, MQTT, WebSocket) write signals, widgets show them, and user changes write back.</p>
      <p><b>Events</b> run <b>actions</b>: send WebSocket/RS-485/MQTT/HTTP messages, set GPIOs or signals, switch tabs, log.</p>
      <p>Press <b>Deploy</b> to show the result on the device. Shortcuts: Del, Ctrl+D, Ctrl+C/V, Ctrl+Z/Y, arrows (Shift = grid), Ctrl+S.</p>` }),
  ];
}

function panelInputs() {
  const list = L().inputs;
  const rerender = () => { changed(); renderPanel(); };
  const out = [h('div', { class: 'hint' }, 'Inputs read hardware and store the result in a signal. Bind widgets to the signal to show it. Built-in signals (sys.*, rs485.rx, ws.rx ...) need no input.')];
  list.forEach((inp, i) => {
    const d = INPUTS[inp.type] || { name: inp.type, fields: {} };
    out.push(h('div', { class: 'card' },
      h('div', { class: 'card-head' }, h('span', { class: 'title' }, d.name),
        h('label', { class: 'row fixed', title: 'Enabled' }, h('input', { type: 'checkbox', checked: inp.enabled !== false, onchange: e => { inp.enabled = e.target.checked; changed(); } }), 'on'),
        h('button', { class: 'danger', onclick: () => { list.splice(i, 1); rerender(); } }, '✕')),
      fieldEl(inp, 'id', { label: 'Id', type: 'text' }, k => changed(`in:${i}:${k}`)),
      fieldsFor(inp, d.fields, k => changed(`in:${i}:${k}`))));
  });
  const sel = h('select', null, Object.entries(INPUTS).map(([k, d]) => h('option', { value: k }, d.name)));
  out.push(h('div', { class: 'row' }, sel, h('button', { class: 'fixed', onclick: () => {
    const type = sel.value;
    list.push({ id: uniqueId(type), type, signal: '' });
    rerender();
  } }, '+ Add input')));
  return out;
}

function panelRules() {
  const list = L().rules;
  const rerender = () => { changed(); renderPanel(); };
  const out = [h('div', { class: 'hint' }, 'Rules watch a signal and run actions when a condition becomes true (or on every change). Example: when temp > 30, send an MQTT alarm.')];
  list.forEach((r, i) => {
    r.actions ||= [];
    out.push(h('div', { class: 'card' },
      h('div', { class: 'card-head' }, h('span', { class: 'title' }, r.id || 'rule'),
        h('button', { class: 'danger', onclick: () => { list.splice(i, 1); rerender(); } }, '✕')),
      fieldEl(r, 'id', { label: 'Id', type: 'text' }, k => changed(`rule:${i}:${k}`)),
      fieldEl(r, 'signal', { label: 'When signal', type: 'signal' }, k => changed(`rule:${i}:${k}`)),
      fieldEl(r, 'op', { label: 'Condition', type: 'select', options: [['', 'changes'], ['>', '>'], ['<', '<'], ['>=', '>='], ['<=', '<='], ['==', '=='], ['!=', '!=']] }, k => changed(`rule:${i}:${k}`)),
      fieldEl(r, 'value', { label: 'Value', type: 'text', help: 'Number, or text for == / !=' }, k => {
        const v = r.value;
        if (v !== undefined && v !== '' && !Number.isNaN(Number(v))) r.value = Number(v);
        changed(`rule:${i}:${k}`);
      }),
      h('div', { class: 'muted', style: { margin: '6px 0' } }, 'Then'),
      actionsEditor(r.actions, `rule:${i}:a`)));
  });
  out.push(h('button', { onclick: () => { list.push({ id: uniqueId('rule'), signal: '', op: '>', value: 0, actions: [] }); rerender(); } }, '+ Add rule'));
  return out;
}

function panelSettings() {
  const s = settings();
  const ports = L().ports;
  const rs = (ports.rs485 ||= { enabled: true, baud: 9600 });
  const can = (ports.can ||= { enabled: false, bitrate: 500000 });
  const mq = (ports.mqtt ||= { enabled: false });
  const on = key => { changed('set:' + key); renderStage(); };
  const tzSel = h('select', { onchange: e => { if (e.target.value) { s.timezone = e.target.value; changed(); renderPanel(); } } },
    h('option', { value: '' }, 'Presets...'), TIMEZONES.map(([v, n]) => h('option', { value: v }, n)));
  return [
    fieldEl(L(), 'name', { label: 'Layout name', type: 'text' }, k => { changed('name'); setDirty(true); }),
    h('h4', null, 'Display'),
    fieldEl(s, 'theme', { label: 'Theme', type: 'select', options: [['', 'dark'], ['light', 'light']] }, on),
    fieldEl(s, 'primary', { label: 'Primary color', type: 'color' }, on),
    fieldEl(s, 'secondary', { label: 'Secondary', type: 'color' }, on),
    fieldEl(s, 'bg', { label: 'Screen color', type: 'color' }, on),
    fieldEl(s, 'font', { label: 'Default font', type: 'font' }, on),
    fieldEl(s, 'tabbar', { label: 'Tab bar', type: 'select', options: [['', 'top'], ['bottom', 'bottom'], ['left', 'left'], ['right', 'right'], ['none', 'hidden']] }, k => { on(k); }),
    fieldEl(s, 'tabbar_size', { label: 'Tab bar size', type: 'number', def: 50 }, on),
    fieldEl(s, 'tab_font', { label: 'Tab font', type: 'font' }, on),
    fieldEl(s, 'swipe', { label: 'Swipe tabs', type: 'checkbox', help: 'Change tabs by swiping (can interfere with sliders).' }, on),
    fieldEl(s, 'screensaver_s', { label: 'Screen off after', type: 'number', placeholder: 'never', help: 'Seconds without touch. The first touch only wakes the screen.' }, on),
    h('h4', null, 'Network'),
    fieldEl(s, 'ws_signals', { label: 'WS: signals', type: 'checkbox', def: true, help: 'Push every signal change to WebSocket clients.' }, on),
    fieldEl(s, 'ws_events', { label: 'WS: events', type: 'checkbox', def: true, help: 'Push widget events, e.g. {"event":"clicked","widget":"btn1","value":1}.' }, on),
    fieldEl(s, 'timezone', { label: 'Time zone', type: 'text', placeholder: 'UTC0', help: 'POSIX TZ string, used for sys.time / sys.date.' }, on),
    h('div', { class: 'field' }, h('label'), tzSel),
    h('h4', null, 'Initial signal values'),
    signalsTable(),
    h('h4', null, 'RS-485'),
    fieldEl(rs, 'enabled', { label: 'Enabled', type: 'checkbox', def: true }, on),
    fieldEl(rs, 'mode', { label: 'Mode', type: 'select', options: [['', 'raw text lines'], ['modbus', 'Modbus RTU master']] }, on),
    fieldEl(rs, 'baud', { label: 'Baud rate', type: 'select', numeric: true, def: 9600, options: [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200].map(b => [b, b]) }, on),
    fieldEl(rs, 'parity', { label: 'Parity', type: 'select', options: [['', 'none'], ['even', 'even'], ['odd', 'odd']] }, on),
    fieldEl(rs, 'stop_bits', { label: 'Stop bits', type: 'select', numeric: true, options: [[1, '1'], [2, '2']] }, on),
    fieldEl(rs, 'timeout_ms', { label: 'Reply timeout', type: 'number', def: 200, help: 'ms, Modbus mode.' }, on),
    fieldEl(rs, 'idle_ms', { label: 'Line timeout', type: 'number', def: 100, help: 'ms of silence that ends a line without newline (raw mode, 0 = off).' }, on),
    h('h4', null, 'CAN bus'),
    h('div', { class: 'hint' }, 'CAN shares GPIO19/20 with the native USB port; enabling it switches them to the CAN transceiver.'),
    fieldEl(can, 'enabled', { label: 'Enabled', type: 'checkbox' }, on),
    fieldEl(can, 'bitrate', { label: 'Bitrate', type: 'select', numeric: true, def: 500000, options: [125000, 250000, 500000, 800000, 1000000].map(b => [b, b / 1000 + ' kbit/s']) }, on),
    fieldEl(can, 'log_raw', { label: 'Raw to can.rx', type: 'checkbox', help: 'Put every frame as text in signal can.rx.' }, on),
    h('h4', null, 'MQTT'),
    fieldEl(mq, 'enabled', { label: 'Enabled', type: 'checkbox' }, on),
    fieldEl(mq, 'uri', { label: 'Broker URI', type: 'text', placeholder: 'mqtt://192.168.1.10:1883' }, on),
    fieldEl(mq, 'username', { label: 'Username', type: 'text' }, on),
    fieldEl(mq, 'password', { label: 'Password', type: 'text' }, on),
    fieldEl(mq, 'client_id', { label: 'Client id', type: 'text', placeholder: 'automatic' }, on),
    fieldEl(mq, 'publish_prefix', { label: 'Publish signals', type: 'text', placeholder: 'e.g. lcdpanel', help: 'If set, every signal change is published (retained) to <prefix>/<signal>.' }, on),
  ];
}

function signalsTable() {
  const list = L().signals;
  const rerender = () => { changed(); renderPanel(); };
  const rows = list.map((sg, i) => h('tr', null,
    h('td', null, h('input', { type: 'text', value: sg.name || '', list: 'sigList', onfocus: refreshSignalList, oninput: e => { sg.name = e.target.value.trim(); changed('sig' + i); } })),
    h('td', null, h('input', { type: 'text', value: sg.value ?? '', oninput: e => {
      const v = e.target.value;
      sg.value = v !== '' && !Number.isNaN(Number(v)) ? Number(v) : v;
      changed('sigv' + i);
    } })),
    h('td', null, h('button', { class: 'danger', onclick: () => { list.splice(i, 1); rerender(); } }, '✕'))));
  return h('div', null,
    h('div', { class: 'hint' }, 'Values a signal starts with after boot (until an input or user changes it).'),
    h('table', { class: 'sig' }, rows),
    h('button', { onclick: () => { list.push({ name: '', value: 0 }); rerender(); } }, '+ Add signal'));
}

/* ---------- Monitor ---------- */

let monitorTimer = 0;

function panelMonitor() {
  const names = Object.keys(state.signals).sort();
  const table = h('table', { class: 'sig', id: 'monTable' }, names.map(n => monitorRow(n)));
  const logBox = h('div', { class: 'wslog', id: 'wsLog' });
  fillWsLog(logBox);
  const send = h('input', { type: 'text', placeholder: '{"set":{"demo.level":75}}  or  name=value' });
  const doSend = () => {
    if (!send.value.trim()) return;
    if (!wsSend(send.value)) toast('WebSocket not connected', 'err');
    send.value = '';
  };
  send.addEventListener('keydown', e => { if (e.key === 'Enter') doSend(); });
  return [
    h('div', { class: 'hint' }, state.wsOk ? `Connected to ws://${location.host}/ws` : 'WebSocket not connected (retrying)...'),
    h('h4', null, 'Signals'),
    names.length ? table : h('div', { class: 'hint' }, 'No signal values received yet.'),
    h('h4', null, 'Messages'),
    logBox,
    h('div', { class: 'row', style: { marginTop: '6px' } }, send, h('button', { class: 'fixed', onclick: doSend }, 'Send'),
      h('button', { class: 'fixed', onclick: () => { state.wsLog = []; fillWsLog($('#wsLog')); } }, 'Clear')),
    h('div', { class: 'hint' }, 'Other programs can connect to the same WebSocket. Send {"set":{"name":value}} to set signals, {"log":"text"} to write to log widgets, {"tab":"id"} to switch tab, {"get":1} for all values.'),
  ];
}

function monitorRow(n) {
  const v = state.signals[n];
  const input = h('input', { type: 'text', placeholder: 'set...', onkeydown: e => {
    if (e.key !== 'Enter') return;
    const raw = e.target.value;
    const val = raw !== '' && !Number.isNaN(Number(raw)) ? Number(raw) : raw;
    wsSend(JSON.stringify({ set: { [n]: val } })) || api.post('/api/signals', JSON.stringify({ [n]: val })).catch(err => toast(err.message, 'err'));
    e.target.value = '';
  } });
  return h('tr', { 'data-sig': n }, h('td', null, n), h('td', { class: 'v' }, typeof v === 'number' ? numToStr(v) : String(v)), h('td', { style: { width: '90px' } }, input));
}

function fillWsLog(box) {
  if (!box) return;
  box.innerHTML = state.wsLog.map(m => `<div class="${m.dir}">${m.dir === 'in' ? '←' : '→'} ${esc(m.text)}</div>`).join('');
  box.scrollTop = box.scrollHeight;
}

function refreshMonitor() {
  if (state.mode !== 'monitor') return;
  clearTimeout(monitorTimer);
  monitorTimer = setTimeout(() => {
    const table = $('#monTable');
    if (!table) { renderPanel(); return; }
    for (const n of Object.keys(state.signals)) {
      let row = table.querySelector(`tr[data-sig="${CSS.escape(n)}"]`);
      if (!row) { renderPanel(); return; }
      const v = state.signals[n];
      row.querySelector('.v').textContent = typeof v === 'number' ? numToStr(v) : String(v);
    }
    fillWsLog($('#wsLog'));
  }, 200);
}

/* ---------- Device ---------- */

function panelDevice() {
  const i = state.info;
  const kv = (k, v) => [h('div', null, k), h('div', null, v ?? '-')];
  const ssid = h('input', { type: 'text', value: i?.wifi?.ssid || '', placeholder: 'network name' });
  const pass = h('input', { type: 'password', placeholder: 'password' });
  const host = h('input', { type: 'text', value: i?.wifi?.hostname || '', placeholder: 'lcd-ui' });
  const nets = h('div');
  const otaFile = h('input', { type: 'file', accept: '.bin' });
  const bar = h('div');
  const out = [
    h('div', { class: 'row' }, h('button', { onclick: async () => { await loadInfo(); renderPanel(); } }, 'Refresh')),
    h('h4', null, 'Device'),
    i ? h('div', { class: 'kv' },
      kv('Firmware', `${i.project} ${i.version}`), kv('ESP-IDF', i.idf), kv('Built', i.built),
      kv('Uptime', fmtDuration(i.uptime_s)), kv('Free RAM', `${(i.heap_internal / 1024).toFixed(0)} kB internal, ${(i.heap_psram / 1024 / 1024).toFixed(1)} MB PSRAM`),
      kv('Storage', i.storage ? `${(i.storage.used / 1024).toFixed(0)} of ${(i.storage.total / 1024).toFixed(0)} kB` : '-'),
      kv('Partition', i.partition), kv('Inputs active', i.inputs_active),
      kv('RS-485', i.ports?.rs485 ? `${i.ports.rs485.config}, ${i.ports.rs485.rx_lines} lines in, Modbus ${i.ports.rs485.modbus_ok} ok / ${i.ports.rs485.modbus_errors} err` : '-'),
      kv('CAN', i.ports?.can?.running ? `${i.ports.can.bitrate / 1000} kbit/s, rx ${i.ports.can.rx}, tx ${i.ports.can.tx}` : 'off'),
      kv('MQTT', i.ports?.mqtt?.running ? (i.ports.mqtt.connected ? 'connected' : 'connecting...') : 'off'))
      : h('div', { class: 'hint' }, 'No device info (offline?).'),
    h('h4', null, 'WiFi'),
    i ? h('div', { class: 'hint' }, i.wifi.connected ? `Connected to ${i.wifi.ssid} as ${i.wifi.ip} (http://${i.wifi.hostname}.local)` : `Not connected.${i.wifi.ap_active ? ` Setup access point "${i.wifi.ap_ssid}" is active.` : ''}`) : null,
    h('div', { class: 'field' }, h('label', null, 'Network'), ssid),
    h('div', { class: 'field' }, h('label', null, 'Password'), pass),
    h('div', { class: 'field' }, h('label', null, 'Hostname'), host),
    h('div', { class: 'row' },
      h('button', { onclick: async e => {
        e.target.disabled = true;
        try {
          const list = await api.get('/api/wifi/scan');
          nets.replaceChildren(...list.sort((a, b) => b.rssi - a.rssi).map(n => h('div', { class: 'list' }, h('li', { onclick: () => { ssid.value = n.ssid; pass.focus(); } },
            h('span', { class: 'name' }, n.ssid), h('span', { class: 'type' }, `${n.rssi} dBm${n.secure ? ' 🔒' : ''}`)))));
        } catch (err) { toast(err.message, 'err'); }
        e.target.disabled = false;
      } }, 'Scan'),
      h('button', { class: 'primary', onclick: async () => {
        try {
          await api.post('/api/wifi', JSON.stringify({ ssid: ssid.value, password: pass.value, hostname: host.value }));
          toast(`Connecting to ${ssid.value}... If you are connected through the setup access point, join your network and open http://${host.value || 'lcd-ui'}.local`, 'ok');
        } catch (err) { toast(err.message, 'err'); }
      } }, 'Save & connect')),
    nets,
    h('h4', null, 'Firmware update'),
    h('div', { class: 'hint' }, 'Upload build/lcd_ui_designer.bin. The device restarts into the new firmware and rolls back automatically if it fails to start.'),
    otaFile, bar,
    h('button', { onclick: () => uploadFirmware(otaFile.files[0], bar) }, 'Upload firmware'),
    h('h4', null, 'Layout'),
    h('div', { class: 'row' },
      h('button', { onclick: async () => {
        if (!confirm('Restore the previously deployed layout on the device?')) return;
        try { await api.post('/api/layout/rollback', ''); toast('Previous layout restored', 'ok'); await loadFromDevice(); } catch (err) { toast(err.message, 'err'); }
      } }, 'Restore previous'),
      h('button', { onclick: async () => {
        if (state.dirty && !confirm('Replace the layout in the editor with the built-in example? (Not deployed until you press Deploy.)')) return;
        try { state.layout = normalize(await api.get('/api/layout/default')); state.tab = 0; state.sel = null; changed(); afterLoad(); } catch (err) { toast(err.message, 'err'); }
      } }, 'Load example'),
      h('button', { onclick: () => {
        if (!confirm('Start a new empty layout in the editor?')) return;
        state.layout = emptyLayout(); state.tab = 0; state.sel = null; changed(); afterLoad();
      } }, 'New empty')),
    h('h4', null, 'System'),
    h('button', { class: 'danger', onclick: async () => {
      if (!confirm('Restart the device?')) return;
      try { await api.post('/api/reboot', ''); toast('Restarting...', 'ok'); } catch (err) { toast(err.message, 'err'); }
    } }, 'Restart device'),
  ];
  return out;
}

function fmtDuration(s) {
  if (s == null) return '-';
  const d = Math.floor(s / 86400), hh = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + 'd ' : '') + (hh ? hh + 'h ' : '') + m + 'm';
}

function uploadFirmware(file, bar) {
  if (!file) { toast('Choose a .bin file first', 'err'); return; }
  if (!confirm(`Flash ${file.name} (${(file.size / 1024).toFixed(0)} kB) and restart the device?`)) return;
  const prog = h('div');
  bar.replaceChildren(h('div', { class: 'progress' }, prog));
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota');
  xhr.upload.onprogress = e => { if (e.lengthComputable) prog.style.width = (100 * e.loaded / e.total) + '%'; };
  xhr.onload = () => {
    let r = {};
    try { r = JSON.parse(xhr.responseText); } catch (e) { /* not json */ }
    if (xhr.status === 200 && r.ok) toast('Firmware uploaded, device is restarting...', 'ok');
    else toast('Update failed: ' + (r.error || xhr.status), 'err');
  };
  xhr.onerror = () => toast('Upload failed (connection lost)', 'err');
  xhr.send(file);
}

/* ---------- JSON ---------- */

function panelJson() {
  const ta = h('textarea', { rows: 30, spellcheck: 'false', style: { height: 'calc(100vh - 220px)' }, value: JSON.stringify(L(), null, 2) });
  return [
    h('div', { class: 'hint' }, 'The complete layout. Edit and press Apply, or copy it somewhere safe.'),
    ta,
    h('div', { class: 'row', style: { marginTop: '6px' } },
      h('button', { class: 'primary', onclick: () => {
        try {
          const l = JSON.parse(ta.value);
          if (!Array.isArray(l.tabs)) throw new Error('"tabs" must be an array');
          state.layout = normalize(l);
          changed();
          afterLoad();
          toast('Applied to editor', 'ok');
        } catch (err) { toast('Invalid JSON: ' + err.message, 'err'); }
      } }, 'Apply'),
      h('button', { onclick: () => { ta.value = JSON.stringify(L(), null, 2); } }, 'Revert'),
      h('button', { onclick: () => { navigator.clipboard?.writeText(ta.value).then(() => toast('Copied', 'ok')); } }, 'Copy')),
  ];
}

/* ============================== Device communication ============================== */

async function loadInfo() {
  try {
    state.info = await api.get('/api/info');
    setStatus(true);
  } catch (e) {
    state.info = null;
    setStatus(false);
  }
}

function setStatus(on) {
  const s = $('#status');
  const ok = on || state.wsOk;
  s.className = 'status ' + (ok ? 'on' : 'off');
  s.textContent = ok ? (state.info?.wifi?.ip && state.info.wifi.ip !== '0.0.0.0' ? state.info.wifi.ip : 'online') : 'offline';
}

async function loadFromDevice(askDraft) {
  let layout;
  try {
    layout = await api.get('/api/layout');
  } catch (e) {
    toast('Could not load the layout from the device: ' + e.message, 'err');
    layout = state.layout || emptyLayout();
  }
  state.layout = normalize(layout);
  state.tab = 0;
  state.sel = null;
  resetHistory();
  setDirty(false);
  if (askDraft) {
    const draft = loadDraft();
    if (draft?.layout && JSON.stringify(draft.layout) !== JSON.stringify(state.layout) &&
        confirm(`You have undeployed changes from ${new Date(draft.t).toLocaleString()}. Restore them?`)) {
      state.layout = normalize(draft.layout);
      changed();
    }
  }
  afterLoad();
}

async function deploy() {
  const problems = validate();
  if (problems.length && !confirm('Deploy anyway?\n\n' + problems.slice(0, 12).join('\n'))) return;
  const btn = $('#btnDeploy');
  btn.disabled = true;
  try {
    await api.post('/api/layout', JSON.stringify(L()));
    setDirty(false);
    clearDraft();
    toast('Deployed to the device', 'ok');
  } catch (e) {
    toast('Deploy failed: ' + e.message, 'err');
  }
  btn.disabled = false;
}

function validate() {
  const out = [];
  const ids = new Map();
  for (const { w, t } of allWidgets()) {
    if (ids.has(w.id)) out.push(`Duplicate widget id "${w.id}"`);
    ids.set(w.id, true);
    const r = contentRect();
    if ((w.x || 0) < 0 || (w.y || 0) < 0 || (w.x || 0) + (w.w || 0) > r.w || (w.y || 0) + (w.h || 0) > r.h) out.push(`"${w.id}" on tab "${t.title}" is partly outside the screen`);
    for (const e of w.events || []) for (const a of e.actions || []) checkAction(a, `"${w.id}"`, out);
  }
  L().inputs.forEach((i, n) => { if (i.enabled !== false && !i.signal && i.type !== 'serial') out.push(`Input ${i.id || n + 1} has no signal`); });
  L().rules.forEach(r => { if (!r.signal) out.push(`Rule ${r.id} has no signal`); (r.actions || []).forEach(a => checkAction(a, `rule ${r.id}`, out)); });
  return out;
}

function checkAction(a, where, out) {
  if (a.type === 'set_signal' && !a.signal) out.push(`${where}: "Set signal" without a signal name`);
  if (a.type === 'tab' && !a.tab) out.push(`${where}: "Switch tab" without a tab`);
  if (a.type === 'gpio' && a.pin == null) out.push(`${where}: "Set GPIO" without a pin`);
}

function wsConnect() {
  let delay = 1000;
  const open = () => {
    let ws;
    try { ws = new WebSocket(`ws://${location.host}/ws`); } catch (e) { return; }
    state.ws = ws;
    ws.onopen = () => { state.wsOk = true; delay = 1000; setStatus(true); if (state.mode === 'monitor') renderPanel(); };
    ws.onclose = () => {
      state.wsOk = false;
      setStatus(!!state.info);
      if (state.mode === 'monitor') renderPanel();
      setTimeout(open, delay);
      delay = Math.min(delay * 2, 10000);
    };
    ws.onmessage = ev => onWsMessage(ev.data);
  };
  open();
}

function wsSend(text) {
  if (!state.ws || state.ws.readyState !== 1) return false;
  state.ws.send(text);
  logWs('out', text);
  return true;
}

function logWs(dir, text) {
  state.wsLog.push({ dir, text: text.length > 400 ? text.slice(0, 400) + '…' : text });
  if (state.wsLog.length > 300) state.wsLog.splice(0, state.wsLog.length - 300);
}

let stageTimer = 0;

function onWsMessage(text) {
  let m = null;
  try { m = JSON.parse(text); } catch (e) { /* plain text */ }
  const sigs = m && typeof m === 'object' ? m.signals : null;
  const onlySys = sigs && Object.keys(sigs).every(k => k.startsWith('sys.'));
  if (!onlySys) logWs('in', text);
  if (sigs) {
    for (const [k, v] of Object.entries(sigs)) {
      state.signals[k] = v;
      if (typeof v === 'number') {
        const hist = (state.history[k] ||= []);
        hist.push(v);
        if (hist.length > 500) hist.shift();
      }
    }
    if ($('#livePreview').checked && !state.drag && !stageTimer) {
      stageTimer = setTimeout(() => { stageTimer = 0; if (!state.drag) renderStage(); }, 250);
    }
  }
  refreshMonitor();
}

/* ============================== Top bar / keyboard ============================== */

function exportLayout() {
  const blob = new Blob([JSON.stringify(L(), null, 2)], { type: 'application/json' });
  const a = h('a', { href: URL.createObjectURL(blob), download: (L().name || 'layout').replace(/[^\w.-]+/g, '_') + '.json' });
  document.body.append(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}

function importLayout(file) {
  if (!file) return;
  file.text().then(t => {
    const l = JSON.parse(t);
    if (!Array.isArray(l.tabs)) throw new Error('not a layout (no "tabs" array)');
    state.layout = normalize(l);
    state.tab = 0;
    state.sel = null;
    changed();
    afterLoad();
    toast(`Imported ${file.name}. Press Deploy to send it to the device.`, 'ok');
  }).catch(e => toast('Import failed: ' + e.message, 'err'));
}

function screenshot() {
  const url = '/api/screenshot.bmp?t=' + Date.now();
  const img = h('img', { src: url, alt: 'Screenshot', width: 800, height: 480 });
  img.onerror = () => toast('Screenshot failed', 'err');
  modal(h('div', null, img, h('div', { class: 'row', style: { marginTop: '8px' } },
    h('a', { href: url, download: 'screenshot.bmp', class: 'fixed' }, 'Download'))));
}

function onKey(e) {
  const tag = (e.target.tagName || '').toLowerCase();
  const typing = tag === 'input' || tag === 'textarea' || tag === 'select';
  const mod = e.ctrlKey || e.metaKey;
  if (mod && e.key.toLowerCase() === 's') { e.preventDefault(); deploy(); return; }
  if (typing) return;
  if (mod && e.key.toLowerCase() === 'z') { e.preventDefault(); e.shiftKey ? redo() : undo(); return; }
  if (mod && e.key.toLowerCase() === 'y') { e.preventDefault(); redo(); return; }
  const f = state.sel && findWidget(state.sel);
  if (mod && e.key.toLowerCase() === 'c' && f) { state.clipboard = clone(f.w); toast('Copied ' + f.w.id); return; }
  if (mod && e.key.toLowerCase() === 'v' && state.clipboard) { e.preventDefault(); pasteWidget(state.clipboard); return; }
  if (mod && e.key.toLowerCase() === 'd' && f) { e.preventDefault(); duplicateSelected(); return; }
  if (!f) return;
  if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); deleteSelected(); return; }
  if (e.key === 'Escape') { select(null); return; }
  const step = e.shiftKey ? Math.max(gridSize(), 10) : 1;
  const moves = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] };
  if (moves[e.key]) {
    e.preventDefault();
    f.w.x = (f.w.x || 0) + moves[e.key][0];
    f.w.y = (f.w.y || 0) + moves[e.key][1];
    changed('nudge:' + f.w.id);
    renderStage();
    if (state.mode === 'props') renderPanel();
  }
}

function renderAll() {
  renderTabList();
  renderWidgetList();
  renderStage();
  renderModes();
  renderPanel();
  setDirty(state.dirty);
}

async function init() {
  renderPalette();
  $('#btnUndo').onclick = undo;
  $('#btnRedo').onclick = redo;
  $('#btnDeploy').onclick = deploy;
  $('#btnExport').onclick = exportLayout;
  $('#btnImport').onclick = () => $('#fileImport').click();
  $('#fileImport').onchange = e => { importLayout(e.target.files[0]); e.target.value = ''; };
  $('#btnReload').onclick = () => { if (!state.dirty || confirm('Discard your changes and load the layout from the device?')) { clearDraft(); loadFromDevice(false); } };
  $('#btnShot').onclick = screenshot;
  $('#btnAddTab').onclick = addTab;
  $('#modalClose').onclick = () => { $('#modal').hidden = true; };
  $('#modal').addEventListener('click', e => { if (e.target.id === 'modal') $('#modal').hidden = true; });
  for (const id of ['#zoom', '#grid', '#showGrid', '#livePreview']) $(id).addEventListener('change', renderStage);
  window.addEventListener('resize', () => { if ($('#zoom').value === 'fit') renderStage(); });
  window.addEventListener('keydown', onKey);
  window.addEventListener('beforeunload', e => { if (state.dirty) { e.preventDefault(); e.returnValue = ''; } });

  await loadInfo();
  await loadFromDevice(true);
  refreshSignalList();
  wsConnect();
  setInterval(loadInfo, 15000);
}

init();
