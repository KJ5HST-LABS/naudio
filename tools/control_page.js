// naudio — control page behaviour (issue #96 item 4).
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2025-2026 Terrell Deppe
//
// Served from the daemon at /app.js so the page's Content-Security-Policy can say
// `script-src 'self'` and forbid inline script outright. No framework, no build step, no
// network fetch of any kind: every request below goes to the daemon that served this file.
'use strict';

// Every mutating request is application/json. That is not decoration — it is the third of the
// three browser defences the daemon enforces (Host, Origin, Content-Type). An HTML form cannot
// produce this content type, so a cross-site form post cannot reach these endpoints.
async function api(path, body) {
  const opts = body === undefined
    ? { method: 'GET' }
    : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) };
  const res = await fetch(path, opts);
  let payload = null;
  try { payload = await res.json(); } catch (e) { /* a non-JSON body is reported as the status */ }
  if (!res.ok) throw new Error((payload && payload.error) || (res.status + ' ' + res.statusText));
  return payload;
}

const $ = (id) => document.getElementById(id);

function notice(el, kind, text) {
  if (!text) { el.hidden = true; return; }
  el.hidden = false;
  el.className = 'notice ' + kind;
  el.textContent = text;
}

// --- formatting -------------------------------------------------------------------------
// -1 means "this connection does not measure it" and NEVER "none happened" (the daemon's
// ClientStats contract). Rendering it as 0 would tell a TCP operator their link is perfect
// when nothing counted at all, so it renders as an em dash and says so on hover.
function counter(v) { return v < 0 ? '—' : String(v); }
function counterTitle(v) { return v < 0 ? 'not measured on this transport' : ''; }

function bytes(n) {
  if (n < 1024) return n + ' B';
  const u = ['KB', 'MB', 'GB', 'TB'];
  let i = -1, v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return v.toFixed(v < 10 ? 1 : 0) + ' ' + u[i];
}

function duration(ms) {
  if (ms <= 0) return '—';
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  const p = (n) => String(n).padStart(2, '0');
  return h > 0 ? `${h}:${p(m)}:${p(sec)}` : `${m}:${p(sec)}`;
}

// dBFS spans roughly -120 (the daemon's floor) to 0. Map to a bar, clamped.
function dbBar(db) { return Math.max(0, Math.min(100, ((db + 60) / 60) * 100)); }

// --- state ------------------------------------------------------------------------------
let settings = {};       // key -> {value, kind, lo, hi} as the daemon reported it
let devices = null;      // {capture: [...], playback: [...]} or null before the first scan
let devicesStale = true;
let dirty = false;       // the form differs from `settings`
let busy = false;

function markDirty() { dirty = true; refreshButtons(); }

// --- devices ----------------------------------------------------------------------------
// The picker offers BOTH ways the daemon can name a device, because both exist in the config
// file for a reason: an id is exact but moves when devices are re-enumerated, a name pattern
// survives that but can match nothing. Selecting a device writes the id; "match by name"
// keeps whatever pattern is already configured.
function fillDeviceSelect(sel, list, dir, idKey, patKey) {
  const currentId = settings[idKey] ? settings[idKey].value : '';
  const currentPat = settings[patKey] ? settings[patKey].value : '';
  sel.replaceChildren();

  const auto = document.createElement('option');
  auto.value = '';
  auto.textContent = currentPat
    ? `Match by name: “${currentPat}”`
    : (dir === 'capture' ? 'Automatic (first USB audio device)' : 'None — discard received audio');
  sel.appendChild(auto);

  if (!list) {
    const none = document.createElement('option');
    none.value = '';
    none.disabled = true;
    none.textContent = 'No device list yet — press “Rescan devices”';
    sel.appendChild(none);
    sel.value = '';
    return;
  }
  for (const d of list) {
    const o = document.createElement('option');
    o.value = String(d.id);
    const ch = dir === 'capture' ? d.inputs : d.outputs;
    o.textContent = `[${d.id}] ${d.name} — ${ch} ch, ${Math.round(d.defaultRate)} Hz, ${d.hostApi}` +
                    (d.virtual ? ' (virtual)' : '');
    sel.appendChild(o);
  }
  sel.value = list.some((d) => String(d.id) === currentId) ? currentId : '';
}

function renderDevices() {
  fillDeviceSelect($('captureSel'), devices && devices.capture, 'capture', 'capture-id', 'capture');
  fillDeviceSelect($('playbackSel'), devices && devices.playback, 'playback', 'playback-id', 'playback');
  $('captureHint').textContent = devicesStale
    ? 'This list was captured earlier — stop the stream and rescan to see a device plugged in since.'
    : 'Pick the radio’s USB audio interface.';
}

async function rescan() {
  try {
    const r = await api('/api/devices');
    devices = r.devices;
    devicesStale = !!r.stale;
    renderDevices();
    notice($('configNotice'), 'info', devicesStale
      ? 'Device list unchanged: PortAudio cannot be re-scanned while the stream is running. Stop it first.'
      : `Found ${devices.capture.length} capture and ${devices.playback.length} playback devices.`);
  } catch (e) {
    notice($('configNotice'), 'err', 'Could not list devices: ' + e.message);
  }
}

// --- settings form ----------------------------------------------------------------------
function applySettingsToForm() {
  const v = (k) => (settings[k] ? settings[k].value : '');
  $('transportSel').replaceChildren();
  for (const t of ['tcp', 'udp', 'dual']) {
    const o = document.createElement('option');
    o.value = t;
    o.textContent = t.toUpperCase();
    $('transportSel').appendChild(o);
  }
  $('transportSel').value = v('transport') || 'tcp';
  $('portInp').value = v('port');
  $('rateInp').value = v('rate');
  $('channelsSel').value = v('channels') || '2';
  $('controlPortInp').value = v('control-port');
  $('autostartChk').checked = v('autostart') === 'true';
  for (const k of ['port', 'rate', 'control-port']) {
    const el = { 'port': $('portInp'), 'rate': $('rateInp'), 'control-port': $('controlPortInp') }[k];
    if (settings[k] && settings[k].kind === 'int') { el.min = settings[k].lo; el.max = settings[k].hi; }
  }
  renderDevices();
  dirty = false;
  refreshButtons();
}

// Only the keys the form actually owns are sent. Anything else the daemon knows about
// (duration-ms, mode) is left exactly as it is rather than being rewritten from a widget that
// does not exist — a page that posts every key would silently freeze settings it cannot show.
function formToSettings() {
  const out = {
    transport: $('transportSel').value,
    port: $('portInp').value.trim(),
    rate: $('rateInp').value.trim(),
    channels: $('channelsSel').value,
    'control-port': $('controlPortInp').value.trim(),
    autostart: $('autostartChk').checked ? 'true' : 'false',
  };
  // A chosen device writes its id and clears the name pattern; leaving the selector on the
  // first entry keeps whichever pattern is configured and clears the id.
  const cap = $('captureSel').value;
  const play = $('playbackSel').value;
  if (cap) { out['capture-id'] = cap; out['capture'] = ''; } else { out['capture-id'] = ''; }
  if (play) { out['playback-id'] = play; out['playback'] = ''; } else { out['playback-id'] = ''; }
  return out;
}

async function save() {
  busy = true; refreshButtons();
  try {
    const r = await api('/api/config', formToSettings());
    notice($('configNotice'), 'good',
      'Saved to ' + r.configPath + (r.restartNeeded ? ' — press Restart to apply it to the running stream.' : '.'));
    await load();
  } catch (e) {
    notice($('configNotice'), 'err', e.message);
  } finally {
    busy = false; refreshButtons();
  }
}

// --- stream actions ---------------------------------------------------------------------
async function stream(action) {
  busy = true; refreshButtons();
  notice($('streamNotice'), 'info', action === 'stop' ? 'Stopping…' : 'Starting…');
  try {
    const r = await api('/api/stream', { action: action });
    renderStream(r.stream);
    notice($('streamNotice'), null);
  } catch (e) {
    notice($('streamNotice'), 'err', e.message);
  } finally {
    busy = false; refreshButtons();
  }
}

// Stopping the daemon is not the same as stopping the stream, so it asks first — and it is the
// only action here that cannot be undone from this page, since the page goes away with it.
async function quitDaemon() {
  if (!window.confirm('Stop the naudio daemon? The stream ends and this page stops working. '
                    + 'Start it again from the naudio shortcut, or by logging in if you enabled '
                    + 'the service.')) return;
  try {
    await api('/api/quit', {});
    notice($('streamNotice'), 'info', 'The daemon is stopping. This page will stop responding.');
  } catch (e) {
    notice($('streamNotice'), 'err', e.message);
  }
}

function refreshButtons() {
  const running = lastState === 'running' || lastState === 'starting';
  $('btnStart').disabled = busy || running;
  $('btnStop').disabled = busy || !running;
  $('btnRestart').disabled = busy || !running;
  $('btnSave').disabled = busy || !dirty;
  $('btnRevert').disabled = busy || !dirty;
}

// --- live status ------------------------------------------------------------------------
let lastState = 'idle';

function renderStream(s) {
  lastState = s.state;
  const badge = $('stateBadge');
  badge.className = 'badge ' + s.state;
  badge.textContent = s.state;

  const bits = [];
  if (s.state === 'running') {
    bits.push(`${s.transport.toUpperCase()} on port ${s.port}`);
    if (s.captureName) bits.push(`capturing “${s.captureName}”`);
    bits.push(s.realSink ? `feeding “${s.sinkName}”` : 'no virtual sink — received audio is discarded');
  }
  $('stateDetail').textContent = bits.join(' · ');

  if (s.state === 'error' && s.error) notice($('streamNotice'), 'err', s.error);
  else if (s.state !== 'error') notice($('streamNotice'), null);

  const live = s.state === 'running';
  $('sDelivery').innerHTML = live
    ? s.deliveryPct.toFixed(0) + '<small>%</small>'
    : '—';
  $('mDelivery').style.width = live ? Math.min(100, s.deliveryPct) + '%' : '0';
  $('sClients').textContent = live ? String(s.clients) : '—';
  $('sRx').innerHTML = live ? bytes(s.rxBytes) + ' <small>' + bytes(s.bps) + '/s</small>' : '—';
  $('sUptime').textContent = duration(s.uptimeMs);
  $('sRmsL').innerHTML = live ? s.rmsL.toFixed(0) + '<small> dBFS</small>' : '—';
  $('sRmsR').innerHTML = live ? s.rmsR.toFixed(0) + '<small> dBFS</small>' : '—';
  $('mRmsL').style.width = live ? dbBar(s.rmsL) + '%' : '0';
  $('mRmsR').style.width = live ? dbBar(s.rmsR) + '%' : '0';
  $('sGaps').textContent = counter(s.sequenceGaps);
  $('sGaps').title = counterTitle(s.sequenceGaps);
  $('sFec').textContent = counter(s.fecRecovered);
  $('sErrors').textContent = String(s.clientErrors + s.crcErrors + s.queueDrops);
  $('sErrors').title = `${s.clientErrors} client, ${s.crcErrors} CRC, ${s.queueDrops} queue drops`;
  refreshButtons();
}

// --- load / poll ------------------------------------------------------------------------
async function load() {
  const st = await api('/api/state');
  $('version').textContent = 'v' + st.version;
  $('configPath').textContent = st.configPath || 'no writable configuration file on this machine';
  settings = st.settings;
  if (st.devices) { devices = st.devices; devicesStale = st.devicesStale; }
  applySettingsToForm();
  renderStream(st.stream);
}

async function poll() {
  try {
    const st = await api('/api/state');
    renderStream(st.stream);
    // The form is left alone while the operator is editing it: overwriting a half-typed port
    // number once a second is the classic way a live-updating page becomes unusable.
    if (!dirty) { settings = st.settings; }
  } catch (e) {
    lastState = 'error';
    $('stateBadge').className = 'badge error';
    $('stateBadge').textContent = 'unreachable';
    $('stateDetail').textContent = 'The daemon is not answering — it may have stopped.';
    refreshButtons();
  }
}

function init() {
  $('btnStart').addEventListener('click', () => stream('start'));
  $('btnStop').addEventListener('click', () => stream('stop'));
  $('btnRestart').addEventListener('click', () => stream('restart'));
  $('btnQuit').addEventListener('click', quitDaemon);
  $('btnSave').addEventListener('click', save);
  $('btnRescan').addEventListener('click', rescan);
  $('btnRevert').addEventListener('click', () => { applySettingsToForm(); notice($('configNotice'), null); });
  for (const id of ['captureSel', 'playbackSel', 'transportSel', 'portInp', 'rateInp',
                    'channelsSel', 'controlPortInp', 'autostartChk']) {
    $(id).addEventListener('change', markDirty);
    $(id).addEventListener('input', markDirty);
  }
  load()
    .then(() => { if (!devices) return rescan(); })
    .catch((e) => notice($('streamNotice'), 'err', 'Could not reach the daemon: ' + e.message));
  setInterval(poll, 1000);
}

document.addEventListener('DOMContentLoaded', init);
