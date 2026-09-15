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
let resolved = null;     // {capture: {id, name, how, moved, reason}, playback: {...}} or null
let dirty = false;       // the form differs from `settings`
let busy = false;
let applyPending = false; // settings were saved while the stream ran; a restart applies them

function markDirty() { dirty = true; refreshButtons(); }

// --- tabs (issue #106) --------------------------------------------------------------------
// Setup and Stream. The page opens on Setup until the radio's capture device is chosen, so the
// first thing a new operator sees is the choice they have to make; once it is chosen — or when
// a stream is already running, which is worth seeing whatever the setup says — it opens on
// Stream. After that the operator's clicks decide. Nothing here is remembered across loads.
const TABS = { setup: ['tabSetup', 'panelSetup'], stream: ['tabStream', 'panelStream'] };
let initialTabChosen = false;

function showTab(name) {
  for (const [tab, [btn, panel]] of Object.entries(TABS)) {
    const on = tab === name;
    $(btn).setAttribute('aria-selected', on ? 'true' : 'false');
    $(panel).hidden = !on;
  }
}

// "Set up" means the capture device is chosen — by id from the picker or by a name pattern in
// the file. The automatic pick (first USB audio device) still works for the daemon's own
// autostart, but the page does not let Start rest on it: a device the operator never chose is
// the one the operator cannot see is wrong.
function setupComplete() {
  const v = (k) => (settings[k] ? settings[k].value : '');
  return !!(v('capture-id') || v('capture'));
}

function chooseInitialTab() {
  if (initialTabChosen) return;
  initialTabChosen = true;
  const running = lastState === 'running' || lastState === 'starting';
  showTab(running || setupComplete() ? 'stream' : 'setup');
}

// --- devices ----------------------------------------------------------------------------
// A device id is a position in the enumeration, and it moves when a device comes or goes; the
// device's NAME survives that. So the picker lists devices by id, as the daemon enumerates
// them, but a choice is SAVED as name + id (issue #107): the daemon lets the name decide and
// uses the id only to tell two identically named devices apart. What the picker shows selected
// is what the saved choice resolves to NOW — read off `resolved`, which the daemon computes
// with the same rule the stream applies at start — and the hint beside it says when the device
// has moved to a different id, or is not connected at all.
//
// The option's value is the device id, '' for the automatic/none choice, or 'saved' for a
// choice that cannot be shown as a device right now (not connected, or no list yet); saving
// with 'saved' selected leaves the file's choice exactly as it is.
function fillDeviceSelect(sel, list, dir, idKey, patKey, res) {
  const currentId = settings[idKey] ? settings[idKey].value : '';
  const currentPat = settings[patKey] ? settings[patKey].value : '';
  const savedLabel = currentPat ? `“${currentPat}”` : (currentId ? `device #${currentId}` : '');
  sel.replaceChildren();

  const auto = document.createElement('option');
  auto.value = '';
  auto.textContent = dir === 'capture' ? 'Automatic (first USB audio device)'
                                       : 'None — discard received audio';
  sel.appendChild(auto);

  const keep = (text) => {
    const o = document.createElement('option');
    o.value = 'saved';
    o.textContent = text;
    sel.appendChild(o);
    sel.value = 'saved';
  };

  if (!list) {
    if (savedLabel) keep(`Saved: ${savedLabel} — press “Rescan devices” to see it`);
    else {
      const none = document.createElement('option');
      none.value = '';
      none.disabled = true;
      none.textContent = 'No device list yet — press “Rescan devices”';
      sel.appendChild(none);
      sel.value = '';
    }
    sel.dataset.filled = sel.value;
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
  const r = res && res[dir];
  if (r && r.id >= 0 && (r.how === 'name' || r.how === 'id') &&
      list.some((d) => d.id === r.id)) {
    sel.value = String(r.id);
  } else if (savedLabel) {
    // Saved, but resolving to nothing in this list — the device is not connected. Shown as
    // such rather than as "Automatic", which would be a different choice and, on Save, would
    // silently replace the one the operator made.
    keep(`Saved: ${savedLabel} — not connected`);
  } else {
    sel.value = '';
  }
  sel.dataset.filled = sel.value;
}

// One line under each picker about the saved choice, when there is something to say.
function deviceHint(dir, res) {
  const r = res && res[dir];
  const idKey = dir + '-id';
  const currentId = settings[idKey] ? settings[idKey].value : '';
  const currentPat = settings[dir] ? settings[dir].value : '';
  if (!r || (!currentPat && !currentId)) return '';
  if (r.how === 'none') {
    return `Not connected: ${r.reason}. Plug it in and rescan, or choose another device.`;
  }
  if (r.how === 'name' && r.moved) {
    return `“${r.name}” is now device ${r.id} (it was saved as ${currentId}); the name is what ` +
           'is followed, so nothing needs changing.';
  }
  if (r.how === 'id') {
    // An id the current list does not carry is handed to the backend as given (the flag's
    // meaning), and the open fails; there is no name to pin, so say "not connected" here
    // rather than promising the next Save something it cannot do.
    const present = devices && devices[dir].some((d) => d.id === r.id);
    if (!present) {
      return `Not connected: there is no device ${currentId} in the current list. Plug it in ` +
             'and rescan, or choose another device.';
    }
    return `Saved by number only (device ${currentId}). Numbers change when a device is added or ` +
           'removed; the next Save pins it by name as well.';
  }
  return '';
}

function renderDevices() {
  fillDeviceSelect($('captureSel'), devices && devices.capture, 'capture', 'capture-id', 'capture', resolved);
  fillDeviceSelect($('playbackSel'), devices && devices.playback, 'playback', 'playback-id', 'playback', resolved);
  $('captureHint').textContent = deviceHint('capture', resolved) || (devicesStale
    ? 'This list was captured earlier — stop the stream and rescan to see a device plugged in since.'
    : 'Pick the radio’s USB audio interface. It is saved by name, so it is still the radio if its number changes.');
  $('playbackHint').textContent = deviceHint('playback', resolved) ||
    'A virtual sink (BlackHole, VB-Cable, Loopback) is what a digital-mode app reads.';
}

async function rescan() {
  try {
    const r = await api('/api/devices');
    devices = r.devices;
    devicesStale = !!r.stale;
    if (r.resolved !== undefined) resolved = r.resolved;
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
  // A chosen device is written as its NAME and its id (issue #107): the daemon follows the name
  // and keeps the id only to tell identically named devices apart. The first entry clears both
  // (automatic / none). 'saved' — a choice that could not be shown as a device — sends nothing,
  // so the file keeps the choice the operator made. And a picker the operator did not touch,
  // whose saved pattern already resolves by name, sends nothing either: a pattern typed into
  // the file by hand ("USB Audio") is not rewritten to the full device name by a save that was
  // about the port. A choice saved by id alone IS upgraded to name + id on any save — that is
  // the file this issue was found in, and the name is what its author meant.
  const device = (selId, dir, idKey, patKey) => {
    const sel = $(selId);
    const v = sel.value;
    if (v === 'saved') return;
    if (v === '') { out[idKey] = ''; out[patKey] = ''; return; }
    const r = resolved && resolved[dir];
    if (v === sel.dataset.filled && r && r.how === 'name') return;
    const d = devices && devices[dir].find((x) => String(x.id) === v);
    out[idKey] = v;
    out[patKey] = d ? d.name : '';
  };
  device('captureSel', 'capture', 'capture-id', 'capture');
  device('playbackSel', 'playback', 'playback-id', 'playback');
  return out;
}

async function save() {
  busy = true; refreshButtons();
  try {
    const r = await api('/api/config', formToSettings());
    // A save while the stream runs is not the stream's settings until it restarts. That is
    // said on the Stream tab, beside Restart (issue #106); here the notice points there.
    if (r.restartNeeded) applyPending = true;
    notice($('configNotice'), 'good',
      'Saved to ' + r.configPath + (r.restartNeeded
        ? ' — the running stream still has the old settings; Restart it from the Stream tab.'
        : '. Streaming is on the Stream tab.'));
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

// --- the login service's switch (issue #102) ----------------------------------------------
// The daemon reports whether it is registered to run at login and whether that switch is on.
// This row MIRRORS the switch; it does not offer one. On macOS the switch is Login Items, whose
// state nothing outside System Settings can move (measured), so the honest control is a link
// to the pane; on Linux and Windows the row names the command INSTALL.md gives. `service` is
// null on a build with no service manager to ask, and the row stays hidden.
let serviceManager = null;

function renderService(svc) {
  const row = $('serviceRow');
  if (!svc) { row.hidden = true; return; }
  row.hidden = false;
  serviceManager = svc.manager;
  const badge = $('serviceBadge');
  if (!svc.installed) { badge.className = 'badge'; badge.textContent = 'Not installed'; }
  else if (svc.enabled) { badge.className = 'badge running'; badge.textContent = 'On'; }
  else { badge.className = 'badge'; badge.textContent = 'Off'; }
  $('btnLoginItems').hidden = svc.manager !== 'launchd';
  const hints = {
    'launchd': 'The switch is in System Settings › General › Login Items & Extensions › '
             + 'Allow in the Background, as Network Audio Service. Turning it off there stops '
             + 'the service; turning it on starts it.',
    'systemd': svc.enabled
             ? 'Turn it off with: systemctl --user disable --now naudio-daemon'
             : 'Turn it on with: systemctl --user enable --now naudio-daemon',
    'task-scheduler': 'The switch is the naudio-daemon task in Task Scheduler (Enable / Disable).',
  };
  $('serviceHint').textContent = svc.installed
    ? (hints[svc.manager] || '')
    : 'No login service is registered on this machine; the installers register one.';
}

async function openLoginItems() {
  try {
    await api('/api/open-login-items', {});
  } catch (e) {
    notice($('configNotice'), 'err', e.message);
  }
}

// Stopping the daemon is not the same as stopping the stream, so it asks first — and it is the
// only action here that cannot be undone from this page, since the page goes away with it.
async function quitDaemon() {
  const back = serviceManager === 'launchd'
    ? 'Start it again from Network Audio Service in Applications, or at your next login if it '
      + 'is on in Login Items.'
    : 'Start it again from the naudio Control shortcut, or at your next login if the service '
      + 'is enabled.';
  if (!window.confirm('Stop the naudio daemon? The stream ends and this page stops working. '
                    + back)) return;
  try {
    await api('/api/quit', {});
    notice($('streamNotice'), 'info', 'The daemon is stopping. This page will stop responding.');
  } catch (e) {
    notice($('streamNotice'), 'err', e.message);
  }
}

// Why Start is off, in one line, or null when it is not. Unsaved changes block it too: a stream
// started on a form that differs from the file is the exact failure #106 reports.
function startBlockedReason() {
  if (!setupComplete()) return 'Choose the radio’s capture device in Setup and save it.';
  if (dirty) return 'Setup has unsaved changes — save or discard them first.';
  // The saved device is not in the current list (issue #107). Start would refuse anyway — the
  // daemon never opens whatever now holds the saved number — but the reason belongs here, before
  // the click, in the device's own name.
  const r = resolved && resolved.capture;
  if (r && r.how === 'none') return `The saved capture device is not connected: ${r.reason}.`;
  return null;
}

function refreshButtons() {
  const running = lastState === 'running' || lastState === 'starting';
  const blocked = startBlockedReason();
  $('btnStart').disabled = busy || running || !!blocked;
  $('btnStop').disabled = busy || !running;
  $('btnRestart').disabled = busy || !running;
  $('btnSave').disabled = busy || !dirty;
  $('btnRevert').disabled = busy || !dirty;

  // The reason sits beside the buttons, with the way to Setup, and only while it applies.
  // Rebuilt only when the text changes — this runs on every poll.
  const reason = $('startReason');
  if (blocked && !running) {
    if (reason.dataset.text !== blocked) {
      reason.dataset.text = blocked;
      reason.replaceChildren();
      reason.append(blocked + ' ');
      const go = document.createElement('button');
      go.className = 'link';
      go.textContent = 'Open Setup';
      go.addEventListener('click', () => showTab('setup'));
      reason.appendChild(go);
    }
    reason.hidden = false;
  } else {
    reason.hidden = true;
  }

  $('setupIntro').hidden = setupComplete();

  // Restart is the primary action while saved settings wait to be applied (issue #106).
  const pending = applyPending && running;
  $('applyNotice').hidden = !pending;
  $('btnRestart').className = pending ? 'primary' : '';
  $('btnStart').className = pending ? '' : 'primary';

  // The tab markers: Setup carries a dot while the form has unsaved changes, Stream the
  // pipeline's state colour, so each tab says what the other one needs.
  $('markSetup').className = 'mark' + (dirty ? ' on dirty' : '');
  $('markStream').className = 'mark' + (lastState === 'idle' ? '' : ' on ' + lastState);
}

// --- live status ------------------------------------------------------------------------
let lastState = 'idle';
let lastUptimeMs = 0;

function renderStream(s) {
  lastState = s.state;
  // Saved-but-unapplied settings are applied by whatever starts the pipeline next: a stop, or
  // a restart, which shows as the uptime going backwards between two polls.
  if (s.state !== 'running' || s.uptimeMs < lastUptimeMs) applyPending = false;
  lastUptimeMs = s.state === 'running' ? s.uptimeMs : 0;
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
  resolved = st.resolved || null;
  applySettingsToForm();
  renderStream(st.stream);
  renderService(st.service);
  chooseInitialTab();
}

async function poll() {
  try {
    const st = await api('/api/state');
    // The form is left alone while the operator is editing it: overwriting a half-typed port
    // number once a second is the classic way a live-updating page becomes unusable. Taken
    // before the stream is rendered, since Start's reason reads `resolved`.
    if (!dirty) { settings = st.settings; resolved = st.resolved || null; }
    renderStream(st.stream);
    // Re-rendered every poll, so a switch flipped in System Settings shows here within a second.
    renderService(st.service);
  } catch (e) {
    lastState = 'error';
    $('stateBadge').className = 'badge error';
    $('stateBadge').textContent = 'unreachable';
    $('stateDetail').textContent = 'The daemon is not answering — it may have stopped.';
    refreshButtons();
  }
}

function init() {
  $('tabSetup').addEventListener('click', () => showTab('setup'));
  $('tabStream').addEventListener('click', () => showTab('stream'));
  $('btnStart').addEventListener('click', () => stream('start'));
  $('btnStop').addEventListener('click', () => stream('stop'));
  $('btnRestart').addEventListener('click', () => stream('restart'));
  $('btnQuit').addEventListener('click', quitDaemon);
  $('btnLoginItems').addEventListener('click', openLoginItems);
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
