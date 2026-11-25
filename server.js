'use strict';

/* =====================================================================
   DwellGuard server — login / register with a local SQLite database
   Run:  npm install   then   npm start   (needs Node.js 22.13 or newer)
   ===================================================================== */

// DwellGuard uses the SQLite that is built into Node.js (no extra database package to compile).
const [nodeMajor, nodeMinor] = process.versions.node.split('.').map(Number);
if (nodeMajor < 22 || (nodeMajor === 22 && nodeMinor < 13)) {
  console.error(`DwellGuard needs Node.js 22.13 or newer. You have ${process.versions.node}.`);
  console.error('Install the latest LTS version from https://nodejs.org and try again.');
  process.exit(1);
}

const path = require('path');
const fs = require('fs');
const crypto = require('crypto');
const express = require('express');
const cookieParser = require('cookie-parser');
const rateLimit = require('express-rate-limit');
const bcrypt = require('bcryptjs');
const { DatabaseSync } = require('node:sqlite');

const PORT = Number(process.env.PORT) || 3000;
// Listen on this computer only, so the site can be reached ONLY through the
// Cloudflare tunnel. Change to '0.0.0.0' later if the ESP32 must reach the
// server directly over your local network.
const HOST = process.env.HOST || '127.0.0.1';
const ALLOW_REGISTRATION = process.env.ALLOW_REGISTRATION !== 'false';
const SESSION_DAYS = 7;
const COOKIE_NAME = 'dg_session';
const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

/* ---------- database ---------- */

fs.mkdirSync(path.join(__dirname, 'data'), { recursive: true });
const db = new DatabaseSync(path.join(__dirname, 'data', 'dwellguard.db'));
db.exec('PRAGMA journal_mode = WAL;');
db.exec('PRAGMA foreign_keys = ON;');

db.exec(`
  CREATE TABLE IF NOT EXISTS users (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    name          TEXT    NOT NULL,
    email         TEXT    NOT NULL UNIQUE,
    password_hash TEXT    NOT NULL,
    created_at    TEXT    NOT NULL DEFAULT (datetime('now'))
  );

  CREATE TABLE IF NOT EXISTS sessions (
    token_hash TEXT    PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires_at INTEGER NOT NULL
  );
`);

const q = {
  insertUser:    db.prepare('INSERT INTO users (name, email, password_hash) VALUES (?, ?, ?)'),
  userByEmail:   db.prepare('SELECT id, name, email, password_hash FROM users WHERE email = ?'),
  insertSession: db.prepare('INSERT INTO sessions (token_hash, user_id, expires_at) VALUES (?, ?, ?)'),
  sessionUser:   db.prepare(`
    SELECT u.id, u.name, u.email, s.expires_at
    FROM sessions s JOIN users u ON u.id = s.user_id
    WHERE s.token_hash = ?`),
  deleteSession: db.prepare('DELETE FROM sessions WHERE token_hash = ?'),
  purgeExpired:  db.prepare('DELETE FROM sessions WHERE expires_at < ?')
};

q.purgeExpired.run(Date.now());
setInterval(() => q.purgeExpired.run(Date.now()), 60 * 60 * 1000).unref();

/* ---------- sessions ---------- */
// The browser gets a random token in an httpOnly cookie.
// Only a SHA-256 hash of it is stored, so a leaked database cannot be used to log in.

const sha256 = s => crypto.createHash('sha256').update(s).digest('hex');

function startSession(req, res, userId){
  const token = crypto.randomBytes(32).toString('hex');
  const maxAge = SESSION_DAYS * 24 * 60 * 60 * 1000;
  q.insertSession.run(sha256(token), userId, Date.now() + maxAge);
  res.cookie(COOKIE_NAME, token, {
    httpOnly: true,
    sameSite: 'lax',
    secure: req.secure,      // true when the request came in over https (Cloudflare)
    maxAge,
    path: '/'
  });
}

function currentUser(req){
  const token = req.cookies[COOKIE_NAME];
  if (!token) return null;
  const row = q.sessionUser.get(sha256(token));
  if (!row) return null;
  if (row.expires_at < Date.now()) { q.deleteSession.run(sha256(token)); return null; }
  return row;
}

const requirePage = (req, res, next) => {
  const user = currentUser(req);
  if (!user) return res.redirect('/login.html');
  req.user = user;
  next();
};

const requireApi = (req, res, next) => {
  const user = currentUser(req);
  if (!user) return res.status(401).json({ error: 'You are not logged in.' });
  req.user = user;
  next();
};

const wrap = fn => (req, res, next) => Promise.resolve(fn(req, res, next)).catch(next);

/* ---------- app ---------- */

const app = express();
app.disable('x-powered-by');
app.set('trust proxy', 1);                       // we sit behind the Cloudflare tunnel
app.use(express.json({ limit: '10kb' }));        // JSON only: plain cross-site form posts are ignored
app.use(cookieParser());

const authLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  limit: 10,                                      // 10 login/register attempts per 15 min per visitor
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: req => req.headers['cf-connecting-ip'] || req.ip,
  message: { error: 'Too many attempts. Please try again in a few minutes.' }
});

/* ----- API ----- */

app.post('/api/register', authLimiter, wrap(async (req, res) => {
  if (!ALLOW_REGISTRATION) return res.status(403).json({ error: 'Registration is closed.' });

  const name = String(req.body.name || '').trim();
  const email = String(req.body.email || '').trim().toLowerCase();
  const password = String(req.body.password || '');

  if (!name || name.length > 80)                       return res.status(400).json({ error: 'Enter your name.' });
  if (!EMAIL_RE.test(email) || email.length > 254)     return res.status(400).json({ error: 'Enter a valid email address.' });
  if (password.length < 8 || password.length > 72)     return res.status(400).json({ error: 'Password must be 8 to 72 characters.' });

  const hash = await bcrypt.hash(password, 12);
  try {
    q.insertUser.run(name, email, hash);
  } catch (err) {
    if (/UNIQUE constraint failed/i.test(String(err.message))) {
      return res.status(409).json({ error: 'An account with that email already exists.' });
    }
    throw err;
  }
  res.status(201).json({ ok: true });
}));

const DUMMY_HASH = bcrypt.hashSync('not-a-real-password', 12);   // keeps timing equal for unknown emails

app.post('/api/login', authLimiter, wrap(async (req, res) => {
  const email = String(req.body.email || '').trim().toLowerCase();
  const password = String(req.body.password || '');

  const user = q.userByEmail.get(email);
  const match = await bcrypt.compare(password, user ? user.password_hash : DUMMY_HASH);
  if (!user || !match) return res.status(401).json({ error: 'Invalid email or password.' });

  startSession(req, res, user.id);
  res.json({ ok: true });
}));

app.post('/api/logout', (req, res) => {
  const token = req.cookies[COOKIE_NAME];
  if (token) q.deleteSession.run(sha256(token));
  res.clearCookie(COOKIE_NAME, { path: '/' });
  res.json({ ok: true });
});

app.get('/api/me', requireApi, (req, res) => {
  res.json({ name: req.user.name, email: req.user.email });
});

/* ----- device + dashboard API (ESP-01 <-> website) ----- */
require('./deviceApi')(app, db, { requireApi, wrap });

/* ----- pages ----- */

app.get('/', (req, res) => res.redirect(currentUser(req) ? '/devices.html' : '/login.html'));

// Logged-in visitors skip the login page.
app.get('/login.html', (req, res, next) => (currentUser(req) ? res.redirect('/devices.html') : next()));

// The dashboard lives in /private and is only sent to logged-in users.
app.get('/devices.html', requirePage, (req, res) => {
  res.sendFile(path.join(__dirname, 'private', 'devices.html'));
});

// The old "Register device" page was removed: the device identifies itself with its token.
app.get('/index.html', (req, res) => res.redirect('/devices.html'));

// Everything in /public (login page, images, etc.) is open to everyone.
app.use(express.static(path.join(__dirname, 'public')));

app.use((err, req, res, next) => {     // eslint-disable-line no-unused-vars
  console.error(err);
  res.status(500).json({ error: 'Something went wrong on the server.' });
});

const server = app.listen(PORT, HOST, () => {
  console.log(`DwellGuard running at http://${HOST}:${PORT}`);
});
// The ESP-01 keeps one connection open between requests; don't let Node close it after 5 s.
server.keepAliveTimeout = 65 * 1000;
server.headersTimeout = 66 * 1000;
