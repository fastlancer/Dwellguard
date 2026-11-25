'use strict';

/* =====================================================================
   DwellGuard device API

   ESP-01  ->  server   (header  X-Device-Token: <token>)
     GET  /api/device/poll        next command for the device, as one text line
     POST /api/device/telemetry   live reading (AUTO mode, every ~5 s)
     POST /api/device/log         one row for the database (every 3 min)
     POST /api/device/event       mode / sleep / wake / check / ack / boot

   Browser  ->  server  (logged-in session cookie)
     GET  /api/state              everything the dashboard shows
     POST /api/command            mode, wake, set (buzzer / servo), read (sensor check)

   The ESP-01 always calls the server (outbound only), so nothing has to
   be opened on your network or router: it works through the Cloudflare tunnel.
   ===================================================================== */

const crypto = require('crypto');

const DEVICE_ID   = process.env.DEVICE_ID   || 'DEV-4F2A-9B7C';
const DEVICE_NAME = process.env.DEVICE_NAME || 'Living room sensor';

const ONLINE_MS     = 10 * 1000;       // device counts as online if it called in during the last 10 s
const COMMAND_TTL   = 60 * 1000;       // a command nobody picked up is dropped after 60 s
const MAX_QUEUE     = 20;
const LOG_LIMIT     = 200;
const SLEEP_LIMIT   = 50;

const MODES   = ['AUTO', 'MAINT', 'SLEEP'];
const SENSORS = ['temp', 'hum', 'dist', 'gas', 'all'];

const sha256 = s => crypto.createHash('sha256').update(String(s)).digest('hex');

module.exports = function mountDeviceApi(app, db, { requireApi, wrap }) {

  /* ---------- tables ---------- */
  db.exec(`
    CREATE TABLE IF NOT EXISTS device (
      id         TEXT PRIMARY KEY,
      name       TEXT NOT NULL,
      token_hash TEXT NOT NULL,
      created_at TEXT NOT NULL DEFAULT (datetime('now'))
    );
    CREATE TABLE IF NOT EXISTS readings_log (
      id         INTEGER PRIMARY KEY AUTOINCREMENT,
      device_id  TEXT    NOT NULL,
      ts         INTEGER NOT NULL,
      temp       REAL,
      hum        REAL,
      dist       INTEGER,
      presence   INTEGER,
      gas        INTEGER,
      gas_alert  INTEGER
    );
    CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings_log (device_id, ts);
    CREATE TABLE IF NOT EXISTS sleep_log (
      id        INTEGER PRIMARY KEY AUTOINCREMENT,
      device_id TEXT    NOT NULL,
      start_ts  INTEGER NOT NULL,
      wake_ts   INTEGER
    );
  `);

  const q = {
    device:       db.prepare('SELECT id, name, token_hash FROM device WHERE id = ?'),
    insertDevice: db.prepare('INSERT INTO device (id, name, token_hash) VALUES (?, ?, ?)'),
    setToken:     db.prepare('UPDATE device SET token_hash = ? WHERE id = ?'),
    insertLog:    db.prepare('INSERT INTO readings_log (device_id, ts, temp, hum, dist, presence, gas, gas_alert) VALUES (?, ?, ?, ?, ?, ?, ?, ?)'),
    logs:         db.prepare('SELECT ts, temp, hum, dist, presence, gas, gas_alert FROM readings_log WHERE device_id = ? ORDER BY ts DESC LIMIT ?'),
    openSleep:    db.prepare('SELECT id FROM sleep_log WHERE device_id = ? AND wake_ts IS NULL ORDER BY id DESC LIMIT 1'),
    insertSleep:  db.prepare('INSERT INTO sleep_log (device_id, start_ts) VALUES (?, ?)'),
    closeSleep:   db.prepare('UPDATE sleep_log SET wake_ts = ? WHERE id = ?'),
    sleeps:       db.prepare('SELECT start_ts, wake_ts FROM sleep_log WHERE device_id = ? ORDER BY start_ts DESC LIMIT ?')
  };

  /* ---------- device record + token ---------- */
  function newToken() { return crypto.randomBytes(32).toString('hex'); }

  function announceToken(token) {
    console.log('');
    console.log('==================== DEVICE TOKEN (shown once) ====================');
    console.log('  DEVICE_ID:    ' + DEVICE_ID);
    console.log('  DEVICE_TOKEN: ' + token);
    console.log('  Copy both into the ESP-01 config.h. If you lose the token, run:');
    console.log('      node server.js --reset-device-token');
    console.log('===================================================================');
    console.log('');
  }

  let dev = q.device.get(DEVICE_ID);
  if (!dev) {
    const token = newToken();
    q.insertDevice.run(DEVICE_ID, DEVICE_NAME, sha256(token));
    announceToken(token);
  } else if (process.argv.includes('--reset-device-token')) {
    const token = newToken();
    q.setToken.run(sha256(token), DEVICE_ID);
    announceToken(token);
  }

  function requireDevice(req, res, next) {
    const given = req.headers['x-device-token'];
    const row = q.device.get(DEVICE_ID);
    if (!given || !row) return res.status(401).type('text/plain').send('UNAUTHORIZED');
    const a = Buffer.from(sha256(given), 'hex');
    const b = Buffer.from(row.token_hash, 'hex');
    if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) {
      return res.status(401).type('text/plain').send('UNAUTHORIZED');
    }
    state.lastSeen = Date.now();               // any valid call proves the device is alive
    next();
  }

  /* ---------- live state (memory) ---------- */
  const state = {
    lastSeen: 0,
    mode: 'AUTO',
    asleep: false,
    lastSleepChange: null,
    live: null,
    checks: {},
    acks: {},
    logVersion: 1,
    sleepVersion: 1
  };
  const queue = [];                            // commands waiting for the device
  let cmdId = 0;

  const num = v => (v === null || v === undefined || v === '' || !Number.isFinite(Number(v))) ? null : Number(v);
  const flag = v => (Number(v) ? 1 : 0);
  const ageMs = v => {
    const s = Number(v);
    return (Number.isFinite(s) && s >= 0 && s <= 86400) ? Math.round(s * 1000) : 0;
  };

  function applyMode(mode, ts) {
    if (MODES.indexOf(mode) < 0) return;
    if (mode === 'SLEEP') {
      if (!state.asleep) {
        state.asleep = true;
        state.lastSleepChange = ts;
        if (!q.openSleep.get(DEVICE_ID)) q.insertSleep.run(DEVICE_ID, ts);
        state.sleepVersion++;
      }
    } else if (state.asleep) {
      state.asleep = false;
      state.lastSleepChange = ts;
      const open = q.openSleep.get(DEVICE_ID);
      if (open) q.closeSleep.run(ts, open.id);
      state.sleepVersion++;
    }
    state.mode = mode;
  }

  /* =================== device -> server =================== */

  app.get('/api/device/poll', requireDevice, (req, res) => {
    // The ESP also tells us what mode it believes it is in, so the website recovers after a server restart.
    const m = String(req.query.mode || '');
    if (MODES.indexOf(m) >= 0 && m !== state.mode) applyMode(m, Date.now());

    const now = Date.now();
    while (queue.length && now - queue[0].at > COMMAND_TTL) queue.shift();
    const next = queue.shift();
    res.type('text/plain').send(next ? next.line : 'NONE');
  });

  app.post('/api/device/telemetry', requireDevice, (req, res) => {
    const b = req.body || {};
    const ts = Date.now();
    state.live = {
      ts,
      temp: num(b.temp), hum: num(b.hum), dist: num(b.dist),
      presence: flag(b.presence), motion: flag(b.motion),
      gas: num(b.gas), gasAlert: flag(b.gasAlert),
      buzzer: flag(b.buzzer), servo: num(b.servo)
    };
    if (MODES.indexOf(b.mode) >= 0) applyMode(b.mode, ts);
    res.type('text/plain').send('OK');
  });

  app.post('/api/device/log', requireDevice, (req, res) => {
    const b = req.body || {};
    const ts = Date.now() - ageMs(b.age);          // the ESP may have held the row back while offline
    q.insertLog.run(DEVICE_ID, ts, num(b.temp), num(b.hum), num(b.dist), flag(b.presence), num(b.gas), flag(b.gasAlert));
    state.logVersion++;
    res.type('text/plain').send('OK');
  });

  app.post('/api/device/event', requireDevice, (req, res) => {
    const b = req.body || {};
    const ts = Date.now() - ageMs(b.age);

    switch (b.type) {
      case 'mode':  applyMode(String(b.value), ts); break;
      case 'sleep': applyMode('SLEEP', ts);         break;
      case 'wake':  applyMode('AUTO', ts);          break;
      case 'check':
        if (SENSORS.indexOf(String(b.sensor)) >= 0) {
          state.checks[b.sensor] = { value: String(b.value).slice(0, 20), extra: String(b.extra || '').slice(0, 20), ts };
        }
        break;
      case 'ack': {
        const key = String(b.cmd).slice(0, 10) + ':' + String(b.arg).slice(0, 10);
        state.acks[key] = { ok: String(b.result) === 'OK', ts };
        break;
      }
      case 'boot':
        break;                                       // only counts as "seen" (done in requireDevice)
      default:
        return res.status(400).type('text/plain').send('BAD EVENT');
    }
    res.type('text/plain').send('OK');
  });

  /* =================== browser -> server =================== */

  app.get('/api/state', requireApi, (req, res) => {
    const now = Date.now();
    const out = {
      device: { id: DEVICE_ID, name: (q.device.get(DEVICE_ID) || {}).name || DEVICE_NAME },
      serverTime: now,
      online: now - state.lastSeen < ONLINE_MS,
      lastSeen: state.lastSeen || null,
      mode: state.mode,
      asleep: state.asleep,
      lastSleepChange: state.lastSleepChange,
      live: state.live,
      checks: state.checks,
      acks: state.acks,
      logVersion: state.logVersion,
      sleepVersion: state.sleepVersion
    };
    if (String(req.query.lv) !== String(state.logVersion)) {
      out.logs = q.logs.all(DEVICE_ID, LOG_LIMIT).map(r => ({
        ts: r.ts, temp: r.temp, hum: r.hum, dist: r.dist,
        presence: r.presence, gas: r.gas, gasAlert: r.gas_alert
      }));
    }
    if (String(req.query.sv) !== String(state.sleepVersion)) {
      out.sleepLogs = q.sleeps.all(DEVICE_ID, SLEEP_LIMIT).map(r => ({ start: r.start_ts, wake: r.wake_ts }));
    }
    res.json(out);
  });

  app.post('/api/command', requireApi, (req, res) => {
    const b = req.body || {};
    let line = null;

    switch (b.cmd) {
      case 'mode':
        if (MODES.indexOf(b.value) >= 0) line = 'MODE ' + b.value;
        break;
      case 'wake':
        line = 'WAKE';
        break;
      case 'set':
        if (b.name === 'buzzer') line = 'SET buzzer ' + (Number(b.value) ? 1 : 0);
        else if (b.name === 'servo') {
          const a = Math.round(Number(b.value));
          if (Number.isFinite(a) && a >= 0 && a <= 180) line = 'SET servo ' + a;
        }
        break;
      case 'read':
        if (SENSORS.indexOf(b.sensor) >= 0) line = 'READ ' + b.sensor;
        break;
    }
    if (!line) return res.status(400).json({ error: 'Unknown or invalid command.' });

    if (state.asleep && line !== 'WAKE') {
      return res.status(409).json({ error: 'The microcontroller is asleep. Wake it first.' });
    }
    if (queue.length >= MAX_QUEUE) return res.status(429).json({ error: 'Too many commands waiting.' });

    const now = Date.now();
    queue.push({ id: ++cmdId, line, at: now });
    res.status(202).json({ ok: true, id: cmdId, ts: now });
  });
};
