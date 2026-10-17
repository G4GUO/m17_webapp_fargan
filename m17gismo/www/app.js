// m17web browser front end
//
// Talks to the m17web binary over one WebSocket:
//   text frames   JSON state / commands / log lines
//   binary frames [0x02][s16le PCM]  receive audio (server -> browser)
//                 [0x01][s16le PCM]  microphone audio (browser -> server)
//
'use strict';

const $ = (id) => document.getElementById(id);
const RATE = 8000;                 // voice sample rate of the M17 codec path
const TAG_SPK = 2, TAG_MIC = 1;

let ws = null;
let reconnectDelay = 500;
let st = {};                       // last state message
let audioReady = false;
let txHeld = false;

// ---------------------------------------------------------------------------
// WebSocket plumbing
// ---------------------------------------------------------------------------
function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/ws`);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    reconnectDelay = 500;
    $('link').textContent = 'online';
    $('link').className = 'link on';
    $('pttbtn').disabled = false;
  };
  ws.onclose = () => {
    $('link').textContent = 'offline';
    $('link').className = 'link off';
    $('pttbtn').disabled = true;
    setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 8000);
  };
  ws.onerror = () => { /* onclose follows */ };
  ws.onmessage = (ev) => {
    if (typeof ev.data === 'string') handleJson(ev.data);
    else handleBinary(new Uint8Array(ev.data));
  };
}

function send(text) { if (ws && ws.readyState === WebSocket.OPEN) ws.send(text); }
function cmd(text) { send(text); }

function handleJson(txt) {
  let m;
  try { m = JSON.parse(txt); } catch (e) { return; }
  switch (m.t) {
    case 'state': applyState(m); break;
    case 'log': addLog(m.kind, m.line); break;
    case 'resp': addLog('resp', m.line); break;
    case 'help': $('help').textContent = (m.lines || []).join('\n'); break;
    case 'clear': $('log').innerHTML = ''; break;
    case 'refs': fillRefs(m.names); break;
    case 'hello': checkStamp(m.stamp); break;
    case 'hello': addLog('info', 'connected to ' + (m.app || 'm17web') + ' (protocol ' + m.proto + ')'); break;
  }
}

// ---------------------------------------------------------------------------
// State rendering
// ---------------------------------------------------------------------------
function setIfIdle(el, value) {
  if (document.activeElement === el) return;
  el.value = value;
}

function applyState(s) {
  st = s;
  $('radio').textContent = s.radio || '-';
  $('mode').textContent = s.mode || '-';
  $('audiobackend').textContent = s.audio || '-';
  $('clients').textContent = s.clients ?? 0;

  const led = $('pttled');
  led.textContent = s.ptt || 'RX';
  led.className = 'led' + (s.ptt === 'TX' ? ' tx' : s.ptt === 'DX' ? ' dx' : '');

  // The station being received belongs here, not in the callsign box
  const fi = $('frameind');
  const rx = (s.rx_src || '').trim();
  fi.textContent = s.in_frame ? (rx ? 'in frame: ' + rx : 'in frame') : 'no signal';
  fi.className = 'badge' + (s.in_frame ? ' sig' : '');

  $('signalbar').value = s.signal || 0;
  $('powerbar').value = s.power || 0;
  $('micbar').value = s.mic_level || 0;
  $('spkbar').value = s.spk_level || 0;

  setIfIdle($('src'), (s.src || '').trim());
  setIfIdle($('dst'), (s.dst || '').trim());
  setIfIdle($('gate'), (s.gate || '').trim());
  $('listenall').checked = !!s.listen_all;

  setIfIdle($('rxfreq'), s.rx_freq || 0);
  setIfIdle($('txfreq'), s.tx_freq || 0);
  if (document.activeElement !== $('rggain')) {
    $('rggain').value = s.rx_gain;
    $('rggainout').textContent = Number(s.rx_gain).toFixed(2);
  }
  if (document.activeElement !== $('tggain')) {
    $('tggain').value = s.tx_gain;
    $('tggainout').textContent = Number(s.tx_gain).toFixed(2);
  }
  setIfIdle($('fc'), s.fc || 1);
  $('afc').checked = !!s.afc;

  if (s.ref) {
    const rs = $('refstatus');
    rs.textContent = s.ref.connected ? `connected to ${s.ref.name}-${s.ref.module}` : 'not connected';
    rs.className = 'badge' + (s.ref.connected ? ' ok' : '');
  }

  if (document.activeElement !== $('audioselect')) $('audioselect').value = s.audio || 'browser';
  renderAudioStatus(s);
  renderTxHealth(s);
  renderPosition(s);
  renderCompass(s);
  renderHwPtt(s);
  renderMode(s);
  renderMixer(s);
  renderBt(s);
  renderFargan(s);
  $('simrow').style.display = (s.radio === 'sim') ? 'flex' : 'none';
}

function renderAudioStatus(s) {
  const p = s.pulse || {};
  const a = s.alsa || {};
  const el = $('audiostatus');
  const pend = $('audiopending');

  if (s.audio === 'alsa' || s.audio === 'pulse') {
    const isAlsa = (s.audio === 'alsa');
    const info = isAlsa ? a : p;
    const which = isAlsa ? 'ALSA' : 'PulseAudio';
    if (!info.compiled) {
      el.textContent = 'this build has no ' + which + ' support (rebuild with ' +
        (isAlsa ? 'ALSA' : 'PULSE') + '=1)';
      el.style.color = 'var(--amber)';
    } else if (info.error) {
      el.textContent = which + ' problem: ' + info.error;
      el.style.color = 'var(--red)';
    } else if (s.spk_open || s.mic_open || info.ready) {
      // Report the streams that are really open: the microphone is closed
      // while receiving, by design, and a failed open can leave an error
      // string behind even though the speaker came up fine.
      const dev = isAlsa ? (' (' + (a.device || 'default') + ')') : ' (PulseAudio)';
      const what = s.spk_open && s.mic_open ? 'speaker and microphone open'
                 : s.spk_open ? 'speaker open, microphone opens when you transmit'
                 : s.mic_open ? 'microphone open, speaker not open'
                 : 'streams opening';
      el.textContent = 'Pi sound card in use' + dev + ' - ' + what;
      el.style.color = s.spk_open || s.mic_open ? 'var(--green)' : 'var(--amber)';
      if (info.error && !s.spk_open) {
        el.textContent = 'Pi sound card: speaker not open - ' + info.error;
        el.style.color = 'var(--red)';
      }
    } else {
      el.textContent = 'Pi sound card (' + which + ') selected - opens at the next ' +
        'transmit/receive cycle';
      el.style.color = 'var(--amber)';
    }
    pend.textContent = s.audio_pending ? '(applying...)' : '';
  } else if (s.audio === 'both') {
    el.textContent = 'browser audio + Pi sound card';
    el.style.color = 'var(--green)';
    pend.textContent = s.audio_pending ? '(applying...)' : '';
  } else if (s.audio === 'none') {
    el.textContent = 'audio muted';
    el.style.color = 'var(--dim)';
    pend.textContent = '';
  } else {
    el.textContent = 'browser audio' + (audioReady ? ' enabled' : ' not enabled yet');
    el.style.color = audioReady ? 'var(--green)' : 'var(--dim)';
    pend.textContent = '';
  }
}

// The sound card's own hardware controls.  The names come from the card, so the
// panel shows whatever is really there.  The controls are only rebuilt when the
// set of them changes, and a value is not written back while its own slider has
// focus - the state stream runs continuously and would fight a drag.
let mixerSig = '';
// The neural receiver is receive-only, so this checkbox only changes what this
// station hears.  Hidden entirely when the build left it out.
function renderFargan(s) {
  const box = $('fargan');
  if (!box) return;
  const f = (s && s.fargan) || {};
  const row = box.closest('.row');
  if (!f.compiled) { if (row) row.style.display = 'none'; return; }
  if (row) row.style.display = '';
  if (document.activeElement !== box) box.checked = !!f.on;
}

// Bluetooth audio.  The state carries the list the "bt" command cached, so the
// dialog only ever renders: opening it asks the station for a refresh, and the
// next state message fills it in.  Nothing here runs per state message.
let btSig = '';
function renderBt(s) {
  const b = (s && s.bt) || {};
  const st = $('btstatus');
  const open = $('btopen');
  if (!st || !open) return;
  if (!b.available) {
    open.disabled = true;
    st.textContent = b.why ? ('Bluetooth unavailable: ' + b.why) : 'Bluetooth unavailable';
    st.style.color = 'var(--dim)';
    return;
  }
  open.disabled = false;
  if (b.active) {
    st.textContent = b.role + ': ' + (b.active_name || b.active) +
      (b.connected ? ' (connected)' : ' (not connected)');
    st.style.color = b.connected ? 'var(--green)' : 'var(--amber)';
  } else {
    st.textContent = 'no Bluetooth device selected';
    st.style.color = 'var(--dim)';
  }
  const devs = b.devices || [];
  const sig = devs.map(d => d.mac + (d.connected ? '1' : '0')).join('|') + '#' + (b.active || '');
  if (sig === btSig) return;
  btSig = sig;
  const note = $('btnote');
  if (note) note.textContent = b.adapter
    ? ('adapter ' + b.adapter + (b.powered ? '' : ' (powered off)') +
       (devs.length ? '' : ' - nothing known yet, try scan'))
    : 'no Bluetooth adapter found';
  const box = $('btlist');
  if (!box) return;
  box.innerHTML = '';
  for (const d of devs) {
    const row = document.createElement('div');
    row.className = 'btdev' + (d.mac === b.active ? ' active' : '');
    const name = document.createElement('span');
    name.textContent = d.name + (d.connected ? ' (connected)' : '');
    const mac = document.createElement('code');
    mac.textContent = d.mac;
    const spk = document.createElement('button');
    spk.className = 'mini'; spk.textContent = 'speaker';
    spk.onclick = () => cmd('bt use ' + d.mac + ' speaker');
    const hs = document.createElement('button');
    hs.className = 'mini'; hs.textContent = 'headset';
    hs.onclick = () => cmd('bt use ' + d.mac + ' headset');
    row.append(name, mac, spk, hs);
    box.append(row);
  }
}

function renderMixer(s) {
  const ctls = s.mixer || [];
  const box = $('mixerctls');
  const sig = ctls.map(c => c.name + (c.mic ? 'm' : 'p') + (c.sw ? 's' : '')).join('|');
  if (sig !== mixerSig) {
    mixerSig = sig;
    box.innerHTML = '';
    ctls.forEach((c, i) => {
      const row = document.createElement('div');
      row.className = 'row';
      const lab = document.createElement('label');
      lab.appendChild(document.createTextNode(c.name + (c.mic ? ' (mic)' : '') + ' '));
      const sl = document.createElement('input');
      sl.type = 'range'; sl.min = '0'; sl.max = '100'; sl.step = '1'; sl.value = c.pct;
      sl.dataset.i = i;
      const out = document.createElement('output');
      out.textContent = c.pct;
      sl.oninput = () => { out.textContent = sl.value; };
      sl.onchange = () => cmd('mixer ' + i + ' ' + sl.value);
      lab.appendChild(sl);
      lab.appendChild(out);
      row.appendChild(lab);
      if (c.sw) {
        const sw = document.createElement('label');
        sw.className = 'chk';
        const cb = document.createElement('input');
        cb.type = 'checkbox'; cb.checked = c.on; cb.dataset.i = i;
        cb.onchange = () => cmd('mixer ' + i + ' ' + (cb.checked ? '100' : '0'));
        sw.appendChild(cb);
        sw.appendChild(document.createTextNode(' on'));
        row.appendChild(sw);
      }
      box.appendChild(row);
    });
  } else {
    box.querySelectorAll('input[type=range]').forEach(el => {
      const c = ctls[Number(el.dataset.i)];
      if (!c || document.activeElement === el) return;
      el.value = c.pct;
      const o = el.parentNode.querySelector('output');
      if (o) o.textContent = c.pct;
    });
    box.querySelectorAll('input[type=checkbox]').forEach(el => {
      const c = ctls[Number(el.dataset.i)];
      if (c && document.activeElement !== el) el.checked = c.on;
    });
  }
  $('mixerbadge').textContent = ctls.length ? (ctls.length + (ctls.length === 1 ? ' control' : ' controls')) : 'no card';
  $('mixernote').textContent = ctls.length
    ? "The card's own levels, saved in config.txt and re-applied whenever the audio opens."
    : 'No sound card controls: no card attached, or the audio is set to the browser.';
}

// Transmit health: blank frames sent before the voice, and how much microphone
// audio the transmitter lost this session (card overruns, or padded silence).
// Position: the GPS fix, and the Maidenhead locator computed from it
function renderPosition(s) {
  const g = s.gps || {};
  const grid = (g.grid || '').trim();
  $('grid').textContent = (grid && grid !== '--') ? grid : '------';
  $('latlon').textContent = g.valid
    ? Number(g.lat).toFixed(5) + ', ' + Number(g.lon).toFixed(5)
    : 'no fix';
  const bits = [];
  if (g.valid) {
    if (g.sats) bits.push(g.sats + ' satellites');
    if (g.hdop) bits.push('HDOP ' + Number(g.hdop).toFixed(2));
    if (g.alt_m !== undefined) bits.push(g.alt_m + ' m');
    if (g.age_s !== undefined) bits.push('updated ' + Number(g.age_s).toFixed(1) + 's ago');
  }
  if (g.status) bits.push(g.status);
  $('gpsdetail').textContent = bits.join(' \u00b7 ') || 'waiting for the GPS...';
  // The same information in the top bar
  const hg = $('hdrgrid');
  hg.textContent = (grid && grid !== '--') ? grid : '------';
  hg.className = 'val grid' + (g.fresh ? ' live' : '');
  $('hdrlatlon').textContent = g.valid
    ? Number(g.lat).toFixed(5) + ', ' + Number(g.lon).toFixed(5)
    : 'no fix';
  const hgn = $('hdrgnss');
  hgn.textContent = g.tx ? 'sending' : (g.valid ? 'ready' : 'none');
  hgn.className = 'val' + (g.tx ? ' live' : '');

  // A position heard from another station
  const hd = s.heard, hel = $('heard');
  if (hd && hd.grid) {
    hel.style.display = '';
    hel.textContent = 'heard ' + hd.src + ' at ' + hd.grid +
      ' (' + Number(hd.lat).toFixed(5) + ', ' + Number(hd.lon).toFixed(5) + ', ' +
      Number(hd.age_s).toFixed(0) + 's ago)';
  } else {
    hel.style.display = 'none';
  }

  // The position log (CSV) tick box and its state
  const csv = g.csv || {};
  const mt = $('metatick');
  if (mt) mt.checked = !!g.meta;
  const cb = $('csvtick');
  if (document.activeElement !== cb) cb.checked = !!csv.on;
  $('csvstate').textContent = csv.path
    ? (csv.on
        ? 'logging every ' + csv.interval + 's to ' + csv.path + ' (' + csv.rows + ' rows)'
        : 'CSV log off' + (csv.rows ? ' (' + csv.rows + ' rows so far in ' + csv.path + ')' : ''))
    : '';

  const flag = $('gnssflag');
  if (g.meta === false) {
    // The switch is off: say that, rather than implying anything is being sent.
    flag.textContent = 'position off air';
    flag.className = 'badge';
  } else {
    flag.textContent = g.tx ? 'position sent' : (g.valid ? 'position ready' : 'no fix');
    flag.className = 'badge' + (g.tx ? ' ok' : (g.valid ? ' sig' : ''));
  }
}

// The compass: a needle pointing at the bearing to the station being heard, and a
// second small marker for the course that station reports.
// Hardware PTT: the switch on the Pi's header, if the station has one
// The mode: how the station is wired up.  Changing it takes effect at once -
// the running mode's thread stands down and the new one starts.
const MODE_NOTE = {
  'radio':  'RF in, local audio out \u2014 the normal voice mode.',
  'loop':   'audio is looped back inside the program, for practice with no radio.',
  'gate':   'the radio is bridged to a reflector \u2014 connect to one in the Reflector panel.',
  'client': 'local audio is bridged to a reflector \u2014 connect to one in the Reflector panel.',
};

function renderMode(s) {
  const want = (s.mode || '').trim();
  let known = false;
  document.querySelectorAll('input[name=mode]').forEach((r) => {
    if (r.value === want) {
      known = true;
      if (document.activeElement !== r) r.checked = true;
    }
  });
  const note = $('modenote');
  note.textContent = MODE_NOTE[want] || ('unknown mode: ' + want);
  note.style.color = known ? 'var(--dim)' : 'var(--amber)';
}

function renderHwPtt(s) {
    const g = s.gpio || {}, el = $('hwinfo');
    if (!g.ok) {
        el.textContent = g.built ? ('hardware PTT: ' + (g.error || 'unavailable')) : 'hardware PTT not built in';
        el.style.color = 'var(--dim)';
        return;
    }
    el.textContent = 'hardware PTT: GPIO ' + g.in + (g.pressed ? ' pressed' : ' ready') +
        (g.hw_tx ? ' (keyed)' : '') + ' \u00b7 keys GPIO ' + g.out;
    el.style.color = g.pressed ? 'var(--red)' : 'var(--green)';
}

function renderCompass(s) {
  const h = s.heard;
  const needle = $('cmp-needle'), course = $('cmp-course');
  const deg = $('cmp-deg'), dist = $('cmp-dist');
  if (!h || h.bearing === undefined) {
    needle.setAttribute('transform', 'rotate(0)');
    needle.style.opacity = 0.3;
    course.setAttribute('opacity', 0);
    deg.textContent = '--';
    dist.textContent = h
      ? ((h.src || '').trim() + ': no fix of our own to bear on')
      : 'no station heard';
    return;
  }
  needle.style.opacity = 1;
  needle.setAttribute('transform', 'rotate(' + Number(h.bearing).toFixed(0) + ')');
  deg.textContent = Number(h.bearing).toFixed(0) + '\u00b0';
  if (h.course !== undefined) {
    course.setAttribute('opacity', 0.85);
    course.setAttribute('transform', 'rotate(' + Number(h.course).toFixed(0) + ')');
  } else {
    course.setAttribute('opacity', 0);
  }
  dist.textContent = (h.src || '').trim() + ' ' + Number(h.distance_km).toFixed(1) + ' km ' +
    (h.grid || '') + (h.speed_kmh ? ' ' + Number(h.speed_kmh).toFixed(0) + ' km/h' : '');
}

function renderTxHealth(s) {
  const el = $('txdrops');
  if (!el) return;
  const drops = s.tx_drops || 0, over = s.tx_overruns || 0;
  const lead = (s.tx_lead === undefined) ? 4 : s.tx_lead;
  const lf = $('leadframes');
  if (lf && document.activeElement !== lf) lf.value = lead;
  const ms = (lead * 20) + ' ms of silence';
  if (drops === 0 && over === 0) {
    el.textContent = 'transmit clean (lead-in ' + ms + ')';
    el.style.color = 'var(--green)';
  } else {
    el.textContent = 'dropped ' + drops + ' samples, ' + over + ' card overruns';
    el.style.color = 'var(--amber)';
  }
}

// The reflector list is a datalist, so the field can be typed into as well as
// picked from. Anything may be entered: m17_net.cpp matches the designator in
// M17Hosts.txt with or without the leading "M17-".
// Fills both controls, tolerating a browser that has cached one asset and not
// the other: if the picker is missing (an older index.html), the typing field is
// still filled, and vice versa.
// The station stamps its greeting with the build it is running.  If a later
// greeting carries a different stamp, this tab is running an older page: say so
// rather than letting the two disagree quietly (an old page once kept copying a
// received callsign into the callsign box and transmitting with it).
let seenStamp = null;

function checkStamp(stamp) {
  if (!stamp) return;
  if (seenStamp && stamp !== seenStamp) {
    const el = $('stale');
    if (el) el.style.display = '';
  }
  seenStamp = stamp;
}

function fillRefs(names) {
  names = names || [];
  const dl = $('reflist'), sel = $('refsel');
  if (dl) dl.innerHTML = '';
  const keep = sel ? sel.value : '';
  if (sel) sel.innerHTML = '<option value="">&mdash; pick a reflector &mdash;</option>';
  for (const n of names) {
    if (dl) {
      const o = document.createElement('option');     // for typing
      o.value = n;
      dl.appendChild(o);
    }
    if (sel) {
      const s = document.createElement('option');     // for clicking
      s.value = n; s.textContent = n;
      sel.appendChild(s);
    }
  }
  if (sel && keep) sel.value = keep;
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------
const MAXLOG = 500;
function addLog(kind, line) {
  const log = $('log');
  const d = document.createElement('div');
  d.className = 'k-' + (kind || 'info');
  const t = new Date().toLocaleTimeString();
  d.textContent = `[${t}] ${line}`;
  log.appendChild(d);
  while (log.childElementCount > MAXLOG) log.removeChild(log.firstChild);
  log.scrollTop = log.scrollHeight;
  $('logcount').textContent = log.childElementCount + ' lines';
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------
function wire() {
  // PTT button + space bar
  const btn = $('pttbtn');
  const down = (e) => { e.preventDefault(); if (!txHeld) { txHeld = true; btn.classList.add('live'); cmd('tx'); } };
  const up = (e) => { if (txHeld) { txHeld = false; btn.classList.remove('live'); cmd('rx'); flushMic(); } };
  btn.addEventListener('pointerdown', down);
  btn.addEventListener('pointerup', up);
  btn.addEventListener('pointerleave', up);
  btn.addEventListener('pointercancel', up);
  window.addEventListener('keydown', (e) => {
    if (e.code === 'Space' && !e.repeat && !isTyping(e.target)) down(e);
  });
  // The release is deliberately NOT guarded by isTyping(): pressing space to
  // transmit and then clicking into a text field (the callsign box) used to leave
  // txHeld stuck true, and with txHeld true every pointerdown is ignored, so the
  // button stopped working until the page was reloaded.  A release must always be
  // honoured.
  window.addEventListener('keyup', (e) => { if (e.code === 'Space') up(e); });
  // ...and if the page loses focus mid-transmission, unkey rather than leaving
  // the button latched.
  window.addEventListener('blur', () => {
    if (txHeld) { txHeld = false; btn.classList.remove('live'); cmd('rx'); }
  });

  const btDlg = $('btdlg');
  if (btDlg) {
    $('btopen').onclick = () => {
      btSig = '';                 // rebuild the list when the answer arrives
      cmd('bt');                  // ask the station to refresh its cache
      if (!btDlg.open) btDlg.showModal();
    };
    $('btclose').onclick = () => btDlg.close();
    $('btoff').onclick = () => cmd('bt off');
    $('btscan').onclick = () => {
      cmd('bt scan');
      $('btnote').textContent = 'scanning for 20 seconds...';
      setTimeout(() => cmd('bt'), 22000);
    };
  }

  $('breakbtn').onclick = () => { txHeld = false; $('pttbtn').classList.remove('live'); cmd('b'); };
  $('carrierbtn').onclick = () => cmd('tc');
  $('duplexbtn').onclick = () => cmd('td');

  $('src').onchange = (e) => cmd('sa ' + e.target.value);
  $('dst').onchange = (e) => cmd('da ' + e.target.value);
  $('gate').onchange = (e) => cmd('ga ' + e.target.value);
  $('listenall').onchange = (e) => cmd('la ' + (e.target.checked ? 'on' : 'off'));

  $('freqapply').onclick = () => {
    cmd('rf ' + Math.round($('rxfreq').value));
    cmd('tf ' + Math.round($('txfreq').value));
    cmd('fc ' + $('fc').value);
  };
  $('rggain').oninput = (e) => { $('rggainout').textContent = Number(e.target.value).toFixed(2); };
  $('rggain').onchange = (e) => cmd('rg ' + e.target.value);
  $('tggain').oninput = (e) => { $('tggainout').textContent = Number(e.target.value).toFixed(2); };
  $('tggain').onchange = (e) => cmd('tg ' + e.target.value);
  $('afc').onchange = (e) => cmd('afc ' + (e.target.checked ? 'on' : 'off'));

  $('simloop').onchange = (e) => cmd('sim loop ' + (e.target.checked ? 'on' : 'off'));
  $('simrt').onchange = (e) => cmd('sim realtime ' + (e.target.checked ? 'on' : 'off'));
  $('simnoise').onchange = (e) => cmd('sim noise ' + e.target.value);

  $('connbtn').onclick = () => {
    const name = $('refname').value || 'M17';
    const mod = ($('refmod').value || 'C').toUpperCase().slice(0, 1);
    cmd(`conn ${name} ${mod}`);
  };
  $('discbtn').onclick = () => cmd('disc');

  $('cmdform').onsubmit = (e) => {
    e.preventDefault();
    const v = $('cmdinput').value.trim();
    if (v) { cmd(v); $('cmdinput').value = ''; }
  };

  document.querySelectorAll('button[data-cmd]').forEach((b) => {
    b.onclick = () => cmd(b.dataset.cmd);
  });

  $('audioselect').onchange = (e) => cmd('audio ' + e.target.value);
  $('csvtick').onchange = (e) => cmd('csv ' + (e.target.checked ? 'on' : 'off'));
  if ($('metatick'))
    $('metatick').onchange = (e) => cmd('gps meta ' + (e.target.checked ? 'on' : 'off'));

  document.querySelectorAll('input[name=mode]').forEach((r) => {
    r.onchange = () => { if (r.checked) cmd('mode ' + r.value); };
  });

  // Picking from the list fills the name field, which is what connect sends
  if ($('refsel')) {
    $('refsel').onchange = () => {
      if ($('refsel').value) $('refname').value = $('refsel').value;
    };
  }

  $('copygrid').onclick = () => {
    const t = $('grid').textContent.trim();
    if (navigator.clipboard && t.indexOf('-') === -1) navigator.clipboard.writeText(t);
  };

  $('leadframes').onchange = (e) => {
    const v = Math.max(0, Math.min(50, Number(e.target.value) || 0));
    e.target.value = v - (v % 2);          // whole sub frames only
    cmd('lead ' + e.target.value);
  };
  $('audioenable').onclick = () => startAudio();
  $('volume').oninput = (e) => { if (audio.outGain) audio.outGain.gain.value = e.target.value; };
  $('mute').onchange = (e) => { if (audio.outGain) audio.outGain.gain.value = e.target.checked ? 0 : $('volume').value; };

  cmd('refs');
}

function isTyping(el) {
  return el && (el.tagName === 'INPUT' || el.tagName === 'SELECT' || el.tagName === 'TEXTAREA');
}

// ---------------------------------------------------------------------------
// Audio (WebAudio <-> 8 kHz PCM over the WebSocket)
// ---------------------------------------------------------------------------
const audio = {
  ctx: null, outGain: null, micNode: null, spkNode: null,
  stream: null, queue: [], queued: 0, playing: false, lastMicLevel: 0,
};

class Downsampler {
  // box-filter decimation from ctxRate to 8 kHz
  constructor(inRate, outRate) {
    this.step = inRate / outRate;
    this.acc = 0; this.sum = 0; this.n = 0; this.out = [];
  }
  process(float32) {
    for (let i = 0; i < float32.length; i++) {
      this.sum += float32[i]; this.n++;
      this.acc += 1;
      if (this.acc >= this.step) {
        const v = Math.max(-1, Math.min(1, this.sum / Math.max(1, this.n)));
        this.out.push(v < 0 ? v * 0x8000 : v * 0x7FFF);
        this.acc -= this.step; this.sum = 0; this.n = 0;
      }
    }
    const a = this.out; this.out = [];
    return a;
  }
}

class Upsampler {
  // linear interpolation from 8 kHz to the context rate
  constructor(inRate, outRate) {
    this.step = inRate / outRate;
    this.pos = 0; this.buf = [];
  }
  push(int16) {
    for (let i = 0; i < int16.length; i++) this.buf.push(int16[i] / 32768);
  }
  pull(out) {
    const n = out.length;
    for (let i = 0; i < n; i++) {
      if (this.buf.length < 2) { out[i] = 0; continue; }
      const idx = Math.floor(this.pos);
      if (idx >= this.buf.length - 1) {
        this.buf = [this.buf[this.buf.length - 1]];
        this.pos = 0;
        out[i] = 0;
        continue;
      }
      const f = this.pos - idx;
      out[i] = this.buf[idx] * (1 - f) + this.buf[idx + 1] * f;
      this.pos += this.step;
      const drop = Math.floor(this.pos);
      if (drop > 0) { this.buf.splice(0, drop); this.pos -= drop; }
    }
  }
}

let down = null, up = null;

async function startAudio() {
  if (audioReady) { await audio.ctx.resume(); return; }
  try {
    audio.ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
    await audio.ctx.resume();
    const rate = audio.ctx.sampleRate;
    down = new Downsampler(rate, RATE);
    up = new Upsampler(RATE, rate);

    audio.stream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: false, noiseSuppression: false, autoGainControl: false }
    });
    const src = audio.ctx.createMediaStreamSource(audio.stream);
    audio.outGain = audio.ctx.createGain();
    audio.outGain.gain.value = $('mute').checked ? 0 : $('volume').value;
    audio.outGain.connect(audio.ctx.destination);

    // microphone -> 8 kHz -> websocket
    audio.micNode = audio.ctx.createScriptProcessor(1024, 1, 1);
    const pending = [];
    audio.micNode.onaudioprocess = (ev) => {
      const s = down.process(ev.inputBuffer.getChannelData(0));
      // The passthrough buffer must be silent: we never monitor ourselves
      for (let i = 0; i < ev.outputBuffer.numberOfChannels; i++)
        ev.outputBuffer.getChannelData(i).fill(0);
      if ($('mute').checked) return;
      let rms = 0;
      for (let i = 0; i < s.length; i++) rms += s[i] * s[i];
      audio.lastMicLevel = Math.min(1, Math.sqrt(rms / Math.max(1, s.length)) / 32768 * 3);
      // Send from the moment PTT is pressed: waiting for the state to say TX
      // costs a round trip, which left a gap at the start of every over.
      if (!txHeld || !ws || ws.readyState !== WebSocket.OPEN) return;
      for (let i = 0; i < s.length; i++) pending.push(s[i]);
      while (pending.length >= 160) {
        const frame = new Int16Array(161);
        frame[0] = TAG_MIC;
        for (let i = 0; i < 160; i++) frame[i + 1] = pending.shift();
        ws.send(frame.buffer);
      }
    };
    src.connect(audio.micNode);
    audio.micNode.connect(audio.ctx.destination); // sink so the node runs; buffer stays silent

    // 8 kHz -> speaker
    audio.spkNode = audio.ctx.createScriptProcessor(2048, 0, 1);
    const outBuf = new Float32Array(2048);
    audio.spkNode.onaudioprocess = (ev) => {
      up.pull(outBuf);
      ev.outputBuffer.getChannelData(0).set(outBuf);
    };
    audio.spkNode.connect(audio.outGain);

    audioReady = true;
    $('audioenable').disabled = true;
    $('audionote').textContent = `browser audio running at ${rate} Hz (device) / ${RATE} Hz (M17)`;
    addLog('info', 'browser audio enabled');
  } catch (err) {
    addLog('err', 'audio failed: ' + err.message +
      ' (needs https or localhost, and microphone permission)');
  }
}

let micFlush = [];
function flushMic() { micFlush = []; }

function handleBinary(u8) {
  if (u8.length < 3 || u8[0] !== TAG_SPK) return;
  const n = (u8.length - 1) >> 1;
  const pcm = new Int16Array(n);
  for (let i = 0; i < n; i++) pcm[i] = (u8[1 + i * 2] | (u8[2 + i * 2] << 8)) << 16 >> 16;
  if (up) up.push(pcm);
}

// ---------------------------------------------------------------------------
connect();
wire();
