'use strict';
/*
 * fake-device.js  -  pretends to be the DwellGuard device (ESP-01 + Pro Mini).
 * Use it to test the website without any hardware.
 *
 *   PowerShell:  $env:DEVICE_TOKEN="paste-token"; node tools/fake-device.js
 *   Optional:    $env:SERVER="https://dwellguard.casa"   (default http://localhost:3000)
 *                $env:LOG_EVERY_S="20"                    (default 20 s; the real device uses 180)
 */
const SERVER = (process.env.SERVER || 'http://localhost:3000').replace(/\/$/, '');
const TOKEN  = process.env.DEVICE_TOKEN;
const LOG_EVERY_MS = Number(process.env.LOG_EVERY_S || 20) * 1000;
if (!TOKEN) { console.error('Set DEVICE_TOKEN first (the server prints it the first time it starts).'); process.exit(1); }

let mode = 'AUTO', asleep = false, buzzer = 0, servo = 0;
let temp = 28.4, hum = 55, dist = 86, gas = 300;
let nextLog = 0;

async function call(path, body) {
  const res = await fetch(SERVER + path, {
    method: body ? 'POST' : 'GET',
    headers: { 'X-Device-Token': TOKEN, 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined
  });
  if (res.status === 401) { console.error('Server says UNAUTHORIZED: wrong DEVICE_TOKEN.'); process.exit(1); }
  return res.ok ? res.text() : null;
}
const event = body => call('/api/device/event', body);

function walk() {
  temp = Math.min(38, Math.max(22, temp + (Math.random() - 0.5) * 0.8));
  hum  = Math.min(80, Math.max(35, hum + (Math.random() - 0.5) * 2));
  dist = Math.round(60 + Math.random() * 80);
  gas  = Math.round(280 + Math.random() * 60);
}
const row = () => ({ temp: +temp.toFixed(1), hum: +hum.toFixed(1), dist, presence: dist <= 100 ? 1 : 0, gas, gasAlert: gas >= 400 ? 1 : 0 });

async function handle(line) {
  const [cmd, a, b] = line.split(' ');
  console.log('command:', line);
  if (cmd === 'WAKE') {
    if (asleep) { asleep = false; mode = 'AUTO'; await event({ type: 'wake' }); await event({ type: 'mode', value: 'AUTO' }); }
  } else if (cmd === 'MODE') {
    if (a === 'SLEEP') { await event({ type: 'ack', cmd: 'MODE', arg: a, result: 'OK' }); mode = 'SLEEP'; asleep = true; await event({ type: 'mode', value: 'SLEEP' }); await event({ type: 'sleep' }); }
    else { mode = a; await event({ type: 'mode', value: a }); await event({ type: 'ack', cmd: 'MODE', arg: a, result: 'OK' }); }
  } else if (cmd === 'SET') {
    const ok = mode === 'MAINT';
    if (ok && a === 'buzzer') buzzer = Number(b) ? 1 : 0;
    if (ok && a === 'servo') servo = Number(b);
    await event({ type: 'ack', cmd: 'SET', arg: a, result: ok ? 'OK' : 'ERR' });
  } else if (cmd === 'READ') {
    walk();
    const all = a === 'all';
    if (all || a === 'temp' || a === 'hum') { await event({ type: 'check', sensor: 'temp', value: temp.toFixed(1) }); await event({ type: 'check', sensor: 'hum', value: hum.toFixed(1) }); }
    if (all || a === 'dist') await event({ type: 'check', sensor: 'dist', value: String(dist) });
    if (all || a === 'gas')  await event({ type: 'check', sensor: 'gas', value: String(gas), extra: gas >= 400 ? '1' : '0' });
    await event({ type: 'ack', cmd: 'READ', arg: a, result: 'OK' });
  }
}

async function tick() {
  try {
    const line = await call('/api/device/poll?mode=' + mode);
    if (line && line !== 'NONE') await handle(line.trim());
    if (!asleep && mode === 'AUTO') {
      walk();
      const r = row();
      await call('/api/device/telemetry', { mode, ...r, motion: 0, buzzer, servo });
      if (Date.now() >= nextLog) { nextLog = Date.now() + LOG_EVERY_MS; await call('/api/device/log', { ...r, age: 0 }); console.log('log row sent'); }
    }
  } catch (e) { console.error('server unreachable:', e.message); }
}

console.log(`fake device talking to ${SERVER}  (Ctrl+C to stop)`);
event({ type: 'boot' }).then(() => { tick(); setInterval(tick, 2000); });
