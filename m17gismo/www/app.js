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
    case 'hello':
      checkStamp(m.stamp);
      addLog('info', 'connected to ' + (m.app || 'm17web') + ' (protocol ' + m.proto + ')');
      break;
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
  // In client mode there is no radio: what is heard came off the reflector, so
  // say who it is rather than talking about frames.
  const client = (s.mode || '').trim() === 'client';
  fi.textContent = s.in_frame
    ? (rx ? ((client ? 'hearing ' : 'in frame: ') + rx) : (client ? 'hearing' : 'in frame'))
    : 'no signal';
  fi.className = 'badge' + (s.in_frame ? ' sig' : '');

  $('signalbar').value = s.signal || 0;
  $('powerbar').value = s.power || 0;
  dialLevels(s.mic_level, s.spk_level);

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

  renderAudio(s);
  renderTxHealth(s);
  renderPosition(s);
  renderCompass(s);
  renderHwPtt(s);
  renderMode(s);
  renderMixer(s);
  renderBt(s);
  renderFargan(s);
  renderRecord(s);
  $('simrow').style.display = (s.radio === 'sim') ? 'flex' : 'none';
}

function dbLabel(g) {
  g = Number(g);
  if (!(g > 0.001)) return '-inf dB';
  return (20 * Math.log10(g)).toFixed(1) + ' dB';
}

function routeLabel(r) {
  if (r === 'soundcard') return 'sound card';
  if (r === 'phone') return 'phone';
  return r;
}

// The configuration box's radios: they follow the station's own choice, except
// while the box is open and a different one is being picked.  Nothing is
// applied until Configure Audio is pressed.
let cfgOpen = false, cfgReopen = false;

function setRoutes(spk, mic) {
  document.querySelectorAll('input[name=spkroute]').forEach((r) => { r.checked = (r.value === spk); });
  document.querySelectorAll('input[name=microute]').forEach((r) => { r.checked = (r.value === mic); });
}

function radioValue(name) {
  const el = document.querySelector('input[name=' + name + ']:checked');
  return el ? el.value : '';
}

// The speaker and the microphone, and the two level controls beside the meters.
// Both levels are applied on the Pi - the gain between the microphone and the
// modulator, the volume between the decoder and the speaker - so the meters
// show the audio that is really going out.
function renderAudio(s) {
  const a = s.alsa || {};
  const spk = s.spk_route || 'mute', mic = s.mic_route || 'mute';
  const badge = $('audiosummary');
  if (badge) badge.textContent = 'out ' + routeLabel(spk) + ' / in ' + routeLabel(mic);

  const mg = (s.mic_gain === undefined) ? 1 : s.mic_gain;
  const sv = (s.spk_vol === undefined) ? 0.8 : s.spk_vol;
  if (document.activeElement !== $('micgain')) {
    $('micgain').value = Math.round(mg * 100);
    $('micgainout').textContent = dbLabel(mg);
  }
  if (document.activeElement !== $('spkvol')) {
    $('spkvol').value = Math.round(sv * 100);
    $('spkvolout').textContent = dbLabel(sv);
  }

  // What the station is actually receiving from the network microphone.  The VU
  // meter below shows the audio that reached the modulator, which cannot tell a
  // silent client from a station that is ignoring it; this can.
  const mi = s.mic_in || {};
  const micin = $('micin');
  if (micin) {
    const msgs = mi.msgs || 0, peak = mi.peak || 0, age = (mi.age_ms === undefined) ? -1 : mi.age_ms;
    if (msgs === 0 && age < 0) {
      micin.textContent = 'phone mic: nothing received yet';
    } else if (age >= 0 && age > 1500) {
      micin.textContent = 'phone mic: silent for ' + Math.round(age / 1000) + ' s (no frames arriving)';
    } else if (peak < 0.002) {
      micin.textContent = 'phone mic: ' + msgs + ' frames, all silence - the phone is not capturing';
    } else {
      micin.textContent = 'phone mic: ' + msgs + ' frames, peak ' + peak.toFixed(2) + ' - arriving';
    }
  }
  const el = $('audiostatus');
  const local = (r) => r === 'soundcard' || r === 'bluetooth';
  let txt, col = 'var(--green)';
  if (spk === 'mute' && mic === 'mute') {
    txt = 'muted: nothing is played, and silence is transmitted';
    col = 'var(--dim)';
  } else {
    txt = 'speaker: ' + routeLabel(spk) + ' \u00b7 microphone: ' + routeLabel(mic);
    if (local(spk) || local(mic)) {
      if (!a.compiled) {
        txt += ' - this build has no sound card support';
        col = 'var(--red)';
      } else {
        if (a.device) txt += ' (' + a.device + ')';
        if (s.spk_open || s.mic_open) {
          const open = [];
          if (s.spk_open) open.push('speaker open');
          if (s.mic_open) open.push('mic open');
          txt += ' - ' + open.join(', ');
        } else {
          txt += ' - attaching';
          col = 'var(--amber)';
        }
        if (a.error) { txt += ' - ' + a.error; col = 'var(--red)'; }
      }
    }
    if (spk === 'browser' || mic === 'browser') {
      if (!audioReady) { txt += ' - browser audio not enabled yet'; col = 'var(--amber)'; }
    }
  }
  if (s.audio_pending) txt += ' (applying\u2026)';
  // Speaker underruns are the one number that says whether a dropout is the
  // card running dry or nothing arriving to play.
  if (s.spk_xruns > 0) {
    txt += ' \u00b7 ' + s.spk_xruns + ' speaker underrun' + (s.spk_xruns === 1 ? '' : 's');
    if (col === 'var(--green)') col = 'var(--amber)';
  }
  el.textContent = txt;
  el.style.color = col;

  if (!cfgOpen) setRoutes(spk, mic);
  const en = $('audioenable');
  if (en) en.disabled = audioReady;
}

// The sound card's own hardware controls.  The names come from the card, so the
// panel shows whatever is really there.  The controls are only rebuilt when the
// set of them changes, and a value is not written back while its own slider has
// focus - the state stream runs continuously and would fight a drag.
let mixerSig = '';
// The neural receiver is receive-only, so this checkbox only changes what this
// station hears.  Hidden entirely when the build left it out.
//
// The synthesis runs on its own thread, so the useful thing to show is whether
// that thread is keeping up: frames still queued, frames it has finished, and
// whether anything was dropped or the audio path ever found the queue empty.
function renderFargan(s) {
  const box = $('fargan');
  if (!box) return;
  const f = (s && s.fargan) || {};
  const row = box.closest('.row');
  if (!f.compiled) { if (row) row.style.display = 'none'; return; }
  if (row) row.style.display = '';
  if (document.activeElement !== box) box.checked = !!f.on;

  const st = $('farganstat');
  if (!st) return;
  if (!f.on) { st.textContent = ''; return; }
  const r = (s && s.fargan_rx) || {};
  const decoded = r.decoded || 0, queued = r.queued || 0;
  const dropped = r.dropped || 0, dry = r.dry || 0;
  st.textContent = 'decoded ' + decoded + ' frames, ' + Math.max(0, queued - decoded) +
                   ' waiting, ' + dropped + ' dropped, ' + dry + ' starved';
  st.style.color = (dropped || dry) ? 'var(--amber)' : 'var(--dim)';
}

// Recording is deliberately not a remembered setting: it is off after every
// restart, so this always shows the recorder's real state.  The checkbox is
// wired straight to "record on"/"record off".
function renderRecord(s) {
  const box = $('recbox');
  if (!box) return;
  const r = (s && s.record) || {};
  if (document.activeElement !== box) box.checked = !!r.on;
  const st = $('recstat');
  if (!st) return;
  if (r.on) {
    st.textContent = r.file + '  ' + (r.seconds || 0) + ' s at ' +
      ((r.rate || 8000) / 1000) + ' kHz';
    st.style.color = 'var(--green)';
  } else if (r.error) {
    st.textContent = r.error;
    st.style.color = 'var(--amber)';
  } else if (r.file) {
    st.textContent = 'saved ' + r.file;
    st.style.color = 'var(--dim)';
  } else {
    st.textContent = '';
  }
}

// Bluetooth audio.  The state carries the list the "bt" command cached, so the
// dialog only ever renders: opening it asks the station for a refresh, and the
// next state message fills it in.  Nothing here runs per state message.
//
// Output and microphone are separate selections.  A device row offers "speaker"
// and "mic" independently, so a speaker can carry the sound while a headset
// carries the microphone; pressing a button that is already lit puts that one
// direction back on the sound card.
let btSig = '';
function renderBt(s) {
  const b = (s && s.bt) || {};
  const st = $('cfgbtstate');
  const open = $('cfgbt');
  if (!st || !open) return;
  if (!b.available) {
    open.disabled = true;
    st.textContent = 'Bluetooth ' + (b.why ? ('unavailable: ' + b.why) : 'unavailable') +
      ' - use the sound card or the browser instead';
    st.style.color = 'var(--dim)';
    return;
  }
  open.disabled = false;
  const a = (s && s.alsa) || {};
  const o = b.out || {}, i = b.in || {};
  const onBt = (mac) => !!(mac && a.device && String(a.device).indexOf('bluealsa:DEV=' + mac) === 0);
  let sp = o.mac
    ? ('speaker: ' + (o.name || o.mac) + (o.profile === 'sco' ? ' (HFP)' : ' (A2DP)'))
    : 'speaker: sound card';
  if (o.mac && !o.connected) sp += ' (not connected)';
  else if (o.mac && !onBt(o.mac)) sp += ' (audio on the card)';
  let mi = i.mac ? ('mic: ' + (i.name || i.mac)) : 'mic: sound card';
  if (i.mac && !i.connected) mi += ' (not connected)';
  st.textContent = sp + ' \u00b7 ' + mi;
  st.style.color = ((!o.mac || o.connected) && (!i.mac || i.connected))
    ? 'var(--green)' : 'var(--amber)';

  const devs = b.devices || [];
  const sig = devs.map(d => d.mac + (d.connected ? '1' : '0') + (d.paired ? 'p' : 'u') +
      (d.out ? 'O' : '-') + (d.in ? 'I' : '-') + (d.a2dp ? 'A' : '-') + (d.hfp ? 'H' : '-')).join('|') +
    '#' + (o.mac || '') + '#' + (i.mac || '');
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

  // A row of its own for the sound card, so either direction can be put back
  // without hunting for which device currently owns it.
  const card = document.createElement('div');
  card.className = 'btdev';
  const cardName = document.createElement('span');
  cardName.textContent = 'sound card';
  const cardSpk = document.createElement('button');
  cardSpk.className = 'mini'; cardSpk.textContent = 'speaker';
  cardSpk.disabled = !o.mac;
  cardSpk.onclick = () => cmd('bt sel spk off');
  const cardMic = document.createElement('button');
  cardMic.className = 'mini'; cardMic.textContent = 'mic';
  cardMic.disabled = !i.mac;
  cardMic.onclick = () => cmd('bt sel mic off');
  card.append(cardName, cardSpk, cardMic);
  box.append(card);

  for (const d of devs) {
    const row = document.createElement('div');
    row.className = 'btdev' + ((d.out || d.in) ? ' active' : '');
    const name = document.createElement('span');
    name.textContent = d.name + (d.connected ? ' (connected)' : '') +
      (d.paired ? '' : ' (not paired)');
    const mac = document.createElement('code');
    mac.textContent = d.mac;
    row.append(name, mac);
    if (!d.paired) {
      // A device a scan has just found is only *discovered*: BlueZ will not
      // connect it until it is paired.  Offer that step first, then re-read the
      // list once the pairing has had time to finish.
      const pr = document.createElement('button');
      pr.className = 'mini'; pr.textContent = 'pair';
      pr.onclick = () => {
        pr.disabled = true; pr.textContent = 'pairing...';
        cmd('bt pair ' + d.mac);
        setTimeout(() => cmd('bt'), 9000);
        setTimeout(() => cmd('bt'), 22000);
      };
      row.append(pr);
    } else {
      const spk = document.createElement('button');
      spk.className = 'mini';
      // A headset with no A2DP is driven as HFP instead (8 kHz, M17's own rate),
      // so say so rather than offering an A2DP role the device cannot fill.
      const hfpOnly = (d.a2dp === false && d.hfp);
      spk.textContent = hfpOnly ? 'speaker (HFP)' : 'speaker';
      if (hfpOnly) spk.title = 'This device has no A2DP: the speaker output uses HFP at 8 kHz.';
      spk.style.fontWeight = d.out ? '700' : '400';
      spk.style.outline = d.out ? '1px solid var(--green)' : 'none';
      spk.onclick = () => cmd(d.out ? 'bt sel spk off' : 'bt sel spk ' + d.mac);
      const mic = document.createElement('button');
      mic.className = 'mini'; mic.textContent = 'mic';
      mic.style.fontWeight = d.in ? '700' : '400';
      mic.style.outline = d.in ? '1px solid var(--green)' : 'none';
      mic.onclick = () => cmd(d.in ? 'bt sel mic off' : 'bt sel mic ' + d.mac);
      row.append(spk, mic);
    }
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
  const down = (e) => {
    e.preventDefault();
    if (!txHeld) { txHeld = true; btn.classList.add('live'); cmd('tx'); setSpkMute(); }
  };
  const up = (e) => {
    if (txHeld) { txHeld = false; btn.classList.remove('live'); cmd('rx'); flushMic(); setSpkMute(); }
  };
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
    if (txHeld) { txHeld = false; btn.classList.remove('live'); cmd('rx'); setSpkMute(); }
  });

  // ---- The audio configuration box --------------------------------------
  const cfgDlg = $('audiodlg');
  const btDlg = $('btdlg');

  $('audiocfg').onclick = () => {
    cfgOpen = true;
    setRoutes(st.spk_route || 'mute', st.mic_route || 'mute');
    $('cfgstatus').textContent = '';
    btSig = '';                    // rebuild the device list when it arrives
    cmd('bt');                     // ask the station to refresh its cache
    if (!cfgDlg.open) cfgDlg.showModal();
  };
  $('cfgclose').onclick = () => { cfgOpen = false; cfgDlg.close(); };
  cfgDlg.addEventListener('close', () => { cfgOpen = false; });

  $('cfgaudio').onclick = async () => {
    const spk = radioValue('spkroute'), mic = radioValue('microute');
    const note = $('cfgstatus');
    note.style.color = 'var(--dim)';
    if (!spk || !mic) {
      note.textContent = 'choose one for the speaker and one for the microphone';
      note.style.color = 'var(--amber)';
      return;
    }
    // Apply first.  The station must end up configured even if this browser
    // cannot open a microphone (no device, permission refused, or an audio
    // context that never starts) - waiting on it would leave the request
    // unsent, which looks exactly like Configure Audio doing nothing.
    note.textContent = 'configuring\u2026';
    cmd('audio apply spk ' + spk + ' mic ' + mic);
    setTimeout(() => {
      if (note.textContent.indexOf('configuring') === 0)
        $('cfgstatus').textContent = 'sent - the station\'s answer is in the console';
    }, 2000);

    // A "browser" direction needs this page's own audio, and this click is the
    // gesture browsers insist on before they will open a microphone.
    if ((spk === 'browser' || mic === 'browser') && !audioReady) {
      const ok = await Promise.race([startAudio(), sleep(6000).then(() => 'timeout')]);
      if (ok === 'timeout') {
        note.textContent = 'browser audio is taking too long - press "enable browser audio"';
        note.style.color = 'var(--amber)';
      } else if (!ok) {
        note.textContent = 'browser audio could not start (see the console)';
        note.style.color = 'var(--amber)';
      }
    }
  };

  $('cfgbt').onclick = () => {
    cfgReopen = cfgDlg.open;
    if (cfgDlg.open) cfgDlg.close();     // one modal at a time
    btSig = '';
    cmd('bt');
    if (!btDlg.open) btDlg.showModal();
  };
  if (btDlg) {
    $('btclose').onclick = () => btDlg.close();
    btDlg.addEventListener('close', () => {
      if (cfgReopen) { cfgReopen = false; cfgOpen = true; cfgDlg.showModal(); }
    });
    $('btscan').onclick = () => {
      cmd('bt scan');
      $('btnote').textContent = 'scanning for 20 seconds...';
      setTimeout(() => cmd('bt'), 22000);
    };
  }

  $('breakbtn').onclick = () => {
    txHeld = false; $('pttbtn').classList.remove('live'); cmd('b'); setSpkMute();
  };
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

  $('micgain').oninput = (e) => { $('micgainout').textContent = dbLabel(e.target.value / 100); };
  $('micgain').onchange = (e) => cmd('micgain ' + e.target.value);
  $('spkvol').oninput = (e) => { $('spkvolout').textContent = dbLabel(e.target.value / 100); };
  $('spkvol').onchange = (e) => cmd('spkvol ' + e.target.value);
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

  cmd('refs');
}

function isTyping(el) {
  return el && (el.tagName === 'INPUT' || el.tagName === 'SELECT' || el.tagName === 'TEXTAREA');
}

// ---------------------------------------------------------------------------
// Analogue level meters
//
// One VU-style dial per direction.  The scale is dB relative to full scale, from
// -20 to +3, with the last three dB in red; the needle has meter ballistics (fast
// up, slow back), a peak marker falls slowly behind it, and the clip lamp latches
// until it is clicked.  The number under each dial is the one to quote; the
// needle is for glancing at while you talk.
const DIAL_MIN = -40, DIAL_MAX = 3;
// Few, big numbers beat a forest of small ones at this size: the dial is 116 px
// wide, and the figure underneath is the precise reading.  The unlabelled ticks
// are there so the needle has something to sit against.
const DIAL_LABELS = [[-40, '-40'], [-30, '-30'], [-20, '-20'], [-10, '-10'], [0, '0'], [3, '+3']];
const DIAL_MINORS = [-35, -25, -15, -6, -3];
const DIAL_NS = 'http://www.w3.org/2000/svg';

function levelDb(lin) {
  return (lin > 0) ? 20 * Math.log10(lin) : -60;
}

function makeDial(host) {
  const W = 116, H = 86, PX = W / 2, PY = 74, R = 48, SPAN = 110;
  const rad = (deg) => (deg - 90) * Math.PI / 180;          // 0 deg = straight up
  const pt = (deg, r) => [PX + r * Math.cos(rad(deg)), PY + r * Math.sin(rad(deg))];
  const angOf = (db) => -SPAN / 2 + SPAN * Math.max(0, Math.min(1, (db - DIAL_MIN) / (DIAL_MAX - DIAL_MIN)));

  const el = (name, attrs, parent) => {
    const e = document.createElementNS(DIAL_NS, name);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  };

  const svg = el('svg', { viewBox: `0 0 ${W} ${H}`, class: 'dialsvg' }, host);
  // face, scale arc, and the red zone over the top of it
  el('rect', { x: .6, y: .6, width: W - 1.2, height: H - 1.2, rx: 9, class: 'dface' }, svg);
  const arc = (a0, a1, r) => {
    const p0 = pt(a0, r), p1 = pt(a1, r);
    const large = Math.abs(a1 - a0) > 180 ? 1 : 0;
    return `M ${p0[0].toFixed(2)} ${p0[1].toFixed(2)} A ${r} ${r} 0 ${large} 1 ${p1[0].toFixed(2)} ${p1[1].toFixed(2)}`;
  };
  el('path', { d: arc(-SPAN / 2, SPAN / 2, R), class: 'darc' }, svg);
  el('path', { d: arc(angOf(0), angOf(DIAL_MAX), R), class: 'darc-red' }, svg);

  // minor ticks first, so the labelled ones draw over them
  for (const db of DIAL_MINORS) {
    const a = angOf(db);
    const p0 = pt(a, R - 2), p1 = pt(a, R + 2);
    el('line', { x1: p0[0], y1: p0[1], x2: p1[0], y2: p1[1], class: 'dtick minor' }, svg);
  }
  // labelled ticks, with room to read them
  for (const [db, text] of DIAL_LABELS) {
    const a = angOf(db);
    const p0 = pt(a, R - 5), p1 = pt(a, R + 3);
    el('line', { x1: p0[0], y1: p0[1], x2: p1[0], y2: p1[1], class: db >= 0 ? 'dtick red' : 'dtick' }, svg);
    const lp = pt(a, R + 12);
    const t = el('text', { x: lp[0], y: lp[1] + 3.2, class: 'dnum' + (db >= 0 ? ' red' : '') }, svg);
    t.textContent = text;
  }

  // peak marker and needle, both drawn from the pivot
  const peak = el('line', { x1: 0, y1: 0, x2: 0, y2: 0, class: 'dpeak' }, svg);
  const needleBack = el('line', { x1: 0, y1: 0, x2: 0, y2: 0, class: 'dneedleback' }, svg);
  const needle = el('line', { x1: 0, y1: 0, x2: 0, y2: 0, class: 'dneedle' }, svg);
  el('circle', { cx: PX, cy: PY, r: 3, class: 'dhub' }, svg);
  const clip = el('circle', { cx: 10, cy: 10, r: 3.2, class: 'dclip' }, svg);
  const clipTitle = el('title', {}, clip);
  clipTitle.textContent = 'clip: lights when the level reaches full scale (click to reset)';

  const setLine = (ln, a, r0, r1) => {
    const a0 = pt(a, r0), a1 = pt(a, r1);
    ln.setAttribute('x1', a0[0].toFixed(2)); ln.setAttribute('y1', a0[1].toFixed(2));
    ln.setAttribute('x2', a1[0].toFixed(2)); ln.setAttribute('y2', a1[1].toFixed(2));
  };
  setLine(needle, angOf(DIAL_MIN), 5, R - 9);
  setLine(needleBack, angOf(DIAL_MIN), 5, R - 9);
  setLine(peak, angOf(DIAL_MIN), R - 19, R - 10);

  const read = document.createElement('span');
  read.className = 'dbread';
  read.textContent = '-\u221e dB';
  host.appendChild(read);

  clip.style.cursor = 'pointer';
  clip.addEventListener('click', () => { d.clipUntil = 0; clip.classList.remove('on'); });

  const d = {
    cur: DIAL_MIN, target: DIAL_MIN, peak: DIAL_MIN, peakAt: 0,
    clipUntil: 0, read, needle, needleBack, peakEl: peak, clipEl: clip,
    set(level) {
      const db = Math.max(-60, Math.min(6, levelDb(level)));
      d.target = db;
      if (level >= 0.995) { d.clipUntil = performance.now() + 1500; clip.classList.add('on'); }
    },
    frame(now, dt) {
      // Meter ballistics: 300 ms to get most of the way up, about a second to
      // fall back, which is what makes a dial readable rather than twitchy.
      const up = 1 - Math.pow(0.001, dt / 0.30);
      const down = 1 - Math.pow(0.001, dt / 1.10);
      d.cur += (d.target - d.cur) * (d.target > d.cur ? up : down);
      if (d.cur > d.peak) { d.peak = d.cur; d.peakAt = now; }
      else if (now - d.peakAt > 700) d.peak = Math.max(d.cur, d.peak - dt * 12);
      setLine(d.needle, angOf(d.cur), 5, R - 9);
      setLine(d.needleBack, angOf(d.cur), 5, R - 9);
      setLine(d.peakEl, angOf(d.peak), R - 19, R - 10);
      d.peakEl.style.opacity = (d.peak > DIAL_MIN + 0.5) ? .95 : 0;
      d.read.textContent = (d.target <= -59 ? '-\u221e' : d.cur.toFixed(1) + ' dB');
      // the figure takes the colour of the zone the needle is in
      d.read.style.color = (d.cur >= 0) ? 'var(--red)' : (d.cur > -6 ? 'var(--amber)' : 'var(--fg)');
      if (d.clipUntil && now > d.clipUntil) { d.clipUntil = 0; clip.classList.remove('on'); }
    },
  };
  return d;
}

const dials = { mic: null, spk: null };
let dialRaf = 0, dialLast = 0;

function dialFrame(now) {
  const dt = dialLast ? Math.min(0.25, (now - dialLast) / 1000) : 0.016;
  dialLast = now;
  if (dials.mic) dials.mic.frame(now, dt);
  if (dials.spk) dials.spk.frame(now, dt);
  dialRaf = requestAnimationFrame(dialFrame);
}

function initDials() {
  if (dials.mic || !$('micdial')) return;
  dials.mic = makeDial($('micdial'));
  dials.spk = makeDial($('spkdial'));
  dialRaf = requestAnimationFrame(dialFrame);
}

function dialLevels(mic, spk) {
  initDials();
  if (dials.mic) dials.mic.set(mic || 0);
  if (dials.spk) dials.spk.set(spk || 0);
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

// Half duplex: the page's own speaker is muted while transmitting, so the room
// does not feed the microphone.  The station's volume control is separate and
// lives on the Pi.
function setSpkMute() {
  if (audio.outGain) audio.outGain.gain.value = txHeld ? 0 : 1;
}

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

async function startAudio() {
  if (audioReady) { try { await audio.ctx.resume(); } catch (e) {} return true; }
  try {
    audio.ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
    // resume() never settles on a machine with no output device, so it must not
    // be able to stall the set-up: the nodes below work while suspended, and a
    // later gesture resumes the context.
    await Promise.race([audio.ctx.resume().catch(() => {}), sleep(1500)]);
    const rate = audio.ctx.sampleRate;
    down = new Downsampler(rate, RATE);
    up = new Upsampler(RATE, rate);

    audio.stream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: false, noiseSuppression: false, autoGainControl: false }
    });
    const src = audio.ctx.createMediaStreamSource(audio.stream);
    audio.outGain = audio.ctx.createGain();
    // Unity here by design: the received audio arrives already scaled by the
    // station's speaker volume, which is the control the meters reflect.
    audio.outGain.gain.value = txHeld ? 0 : 1;
    audio.outGain.connect(audio.ctx.destination);

    // microphone -> 8 kHz -> websocket
    audio.micNode = audio.ctx.createScriptProcessor(1024, 1, 1);
    const pending = [];
    audio.micNode.onaudioprocess = (ev) => {
      const s = down.process(ev.inputBuffer.getChannelData(0));
      // The passthrough buffer must be silent: we never monitor ourselves
      for (let i = 0; i < ev.outputBuffer.numberOfChannels; i++)
        ev.outputBuffer.getChannelData(i).fill(0);
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
    $('audioerr').textContent = '';
    // Name the device the browser actually opened: "far too loud at any trim"
    // is usually the laptop's own array rather than the one that was expected.
    const track = audio.stream.getAudioTracks ? audio.stream.getAudioTracks()[0] : null;
    addLog('info', 'browser audio running: ' + (track ? track.label : 'microphone') +
      ' at ' + rate + ' Hz, carried at ' + RATE + ' Hz');
    return true;
  } catch (err) {
    const msg = 'browser audio failed: ' + err.message +
      ' (needs https or localhost, and microphone permission)';
    $('audioerr').textContent = msg;
    addLog('err', msg);
    return false;
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
