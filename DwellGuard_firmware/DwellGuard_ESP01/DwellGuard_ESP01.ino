/*
 * ======================================================================
 *  DwellGuard - ESP-01 (ESP8266) firmware
 *  Bridge between the Arduino Pro Mini and the website (dwellguard.casa)
 * ======================================================================
 *
 *  How it works
 *    Pro Mini --serial frames--> ESP-01 --HTTPS--> website (through Cloudflare)
 *    Pro Mini <--serial frames-- ESP-01 <--HTTPS-- website (the ESP asks every 2 s)
 *
 *  The ESP always CALLS the website (outbound only), so nothing has to be
 *  opened on your router and it works through the Cloudflare tunnel.
 *
 *  Pro Mini -> website
 *    DATA  frame  ->  POST /api/device/telemetry   live reading (every 5 s in AUTO)
 *    LOG   frame  ->  POST /api/device/log         database row (every 3 min)
 *    MODE / SLEEP / WOKE / CHK / ACK frames -> POST /api/device/event
 *    LOG rows and events are kept in RAM and re-sent if the website is
 *    unreachable (offline buffer), with their age so the website can
 *    put the correct time on them.
 *
 *  website -> Pro Mini
 *    GET /api/device/poll   answers with one text line:
 *        NONE | MODE AUTO|MAINT|SLEEP | WAKE | SET buzzer 1 | SET servo 90 | READ temp|hum|dist|gas|all
 *    WAKE: the Pro Mini sleeps in power-down and can only be woken by a LOW level
 *          on D2 (INT0). D2 is wired to this ESP's GPIO2 (a 10K pull-up to 3.3 V keeps
 *          it HIGH), so the ESP pulls GPIO2 LOW for WAKE_PULSE_MS. The Pro Mini then
 *          answers with a WOKE frame.
 *
 *  Libraries: ESP8266 core (Boards Manager) + DwellComm (included)
 *  Board:     Generic ESP8266 Module
 * ======================================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#if USE_HTTPS
  #include <WiFiClientSecure.h>
#endif
#include <stdarg.h>
#include <DwellComm.h>
#include "config.h"

/* ---------------- objects ---------------- */
DwellComm comm(Serial);                 // serial link to the Pro Mini

#if USE_HTTPS
  BearSSL::WiFiClientSecure netClient;
#else
  WiFiClient netClient;
#endif
HTTPClient http;

/* ---------------- state ---------------- */
char curMode[8] = "AUTO";               // what the Pro Mini last told us: AUTO | MAINT | SLEEP
bool arduinoAsleep = false;
bool wakePending = false, wakeSynced = false;
uint8_t wakeTries = 0;
unsigned long wakeSentAt = 0;

unsigned long lastPollMs = 0, nextHttpAt = 0, lastStatusMs = 0;
int  lastHttpCode = 0;
bool wasConnected = false;

struct Pending {                        // offline buffer entry
  uint8_t       kind;                   // 0 = log row, 1 = event
  unsigned long t;                      // when it happened (millis)
  char          body[150];              // JSON without the closing '}' (age is added when sending)
};
Pending pend[QUEUE_MAX];
uint8_t pendCount = 0;

/* ---------------- prototypes ---------------- */
void dbg(const char *fmt, ...);
bool httpRequest(bool post, const char *path, const char *body, char *resp, size_t respCap);
void onArduinoMessage(const DwellMessage &m);
void toArduino(const char *type, const char *a = NULL, const char *b = NULL, const char *c = NULL);
void wakeArduino();
void pulseWake();
void serviceWake(unsigned long now);

/* ====================================================================== */
/*  small helpers                                                          */
/* ====================================================================== */
void dbg(const char *fmt, ...) {
#if ESP_DEBUG
  if (arduinoAsleep) return;            // nobody is listening while the Pro Mini sleeps
  char buf[110];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  Serial.print("# "); Serial.println(buf);
#else
  (void)fmt;
#endif
}

// Copies a value into the JSON only if it looks like a number, otherwise "null" ("na" = sensor failed).
void putNum(char *out, size_t cap, const char *v) {
  bool ok = (v != NULL && *v != '\0');
  for (const char *p = v; ok && *p; p++) if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) ok = false;
  snprintf(out, cap, "%s", ok ? v : "null");
}

// Keeps only letters, digits, '.', '-', '_' so nothing odd can end up inside the JSON.
void putTok(char *out, size_t cap, const char *v) {
  size_t n = 0;
  for (const char *p = v; p && *p && n + 1 < cap; p++) {
    char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') out[n++] = c;
  }
  out[n] = '\0';
}

bool isNumeric(const char *s) {
  if (!s || !*s) return false;
  for (; *s; s++) if (*s < '0' || *s > '9') return false;
  return true;
}
bool validMode(const char *s)   { return !strcmp(s, "AUTO") || !strcmp(s, "MAINT") || !strcmp(s, "SLEEP"); }
bool validSensor(const char *s) { return !strcmp(s, "temp") || !strcmp(s, "hum") || !strcmp(s, "dist") || !strcmp(s, "gas") || !strcmp(s, "all"); }

/* ====================================================================== */
/*  HTTP to the website                                                    */
/* ====================================================================== */
bool httpRequest(bool post, const char *path, const char *body, char *resp, size_t respCap) {
  if (WiFi.status() != WL_CONNECTED) return false;

  http.setReuse(true);                              // keep the TCP/TLS connection open between requests
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(netClient, SERVER_HOST, SERVER_PORT, String(path), USE_HTTPS)) return false;

  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  int code;
  if (post) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST((uint8_t *)body, strlen(body));
  } else {
    code = http.GET();
  }

  if (code >= 200 && code < 300 && resp != NULL) {
    String s = http.getString();
    strncpy(resp, s.c_str(), respCap - 1);
    resp[respCap - 1] = '\0';
  }
  http.end();
  lastHttpCode = code;
  if (code <= 0) netClient.stop();                  // broken connection: start fresh next time
  return code >= 200 && code < 300;
}

void enqueue(uint8_t kind, const char *openJson) {
  if (pendCount == QUEUE_MAX) {                     // full: drop the oldest
    memmove(&pend[0], &pend[1], sizeof(Pending) * (QUEUE_MAX - 1));
    pendCount--;
  }
  Pending &p = pend[pendCount++];
  p.kind = kind;
  p.t = millis();
  strncpy(p.body, openJson, sizeof(p.body) - 1);
  p.body[sizeof(p.body) - 1] = '\0';
}

void queueEvent(const char *fmt, ...) {             // builds {"type":...  (the closing part is added later)
  char buf[150];
  buf[0] = '{';
  va_list ap; va_start(ap, fmt); vsnprintf(buf + 1, sizeof(buf) - 1, fmt, ap); va_end(ap);
  enqueue(1, buf);
}

void flushQueue(unsigned long now) {
  if (pendCount == 0 || now < nextHttpAt || WiFi.status() != WL_CONNECTED) return;

  char body[200];
  snprintf(body, sizeof(body), "%s,\"age\":%lu}", pend[0].body, (now - pend[0].t) / 1000UL);
  const char *path = (pend[0].kind == 0) ? "/api/device/log" : "/api/device/event";

  if (httpRequest(true, path, body, NULL, 0)) {
    memmove(&pend[0], &pend[1], sizeof(Pending) * (QUEUE_MAX - 1));
    pendCount--;
  } else {
    nextHttpAt = now + RETRY_MS;
    dbg("send failed (http %d), %u item(s) waiting", lastHttpCode, pendCount);
  }
}

void postTelemetry(const DwellMessage &m) {
  char f[9][12];
  for (uint8_t i = 0; i < 9; i++) putNum(f[i], sizeof(f[i]), m.get(i));
  char body[220];
  snprintf(body, sizeof(body),
    "{\"mode\":\"%s\",\"temp\":%s,\"hum\":%s,\"dist\":%s,\"presence\":%s,\"motion\":%s,"
    "\"gas\":%s,\"gasAlert\":%s,\"buzzer\":%s,\"servo\":%s}",
    curMode, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
  if (millis() < nextHttpAt) return;                // live data is not worth queueing: skip if the link is down
  if (!httpRequest(true, "/api/device/telemetry", body, NULL, 0)) {
    nextHttpAt = millis() + RETRY_MS;
    dbg("telemetry failed (http %d)", lastHttpCode);
  }
}

void queueLog(const DwellMessage &m) {
  char f[6][12];
  for (uint8_t i = 0; i < 6; i++) putNum(f[i], sizeof(f[i]), m.get(i));
  char body[150];
  snprintf(body, sizeof(body),
    "{\"temp\":%s,\"hum\":%s,\"dist\":%s,\"presence\":%s,\"gas\":%s,\"gasAlert\":%s",
    f[0], f[1], f[2], f[3], f[4], f[5]);
  enqueue(0, body);
  dbg("log row queued (%u waiting)", pendCount);
}

/* ====================================================================== */
/*  Pro Mini  ->  website                                                  */
/* ====================================================================== */
void onArduinoMessage(const DwellMessage &m) {
  char a[12], b[12], c[12], d[12];

  if (m.is("DATA")) {
    postTelemetry(m);

  } else if (m.is("LOG")) {
    queueLog(m);

  } else if (m.is("MODE")) {
    putTok(a, sizeof(a), m.get(0));
    strncpy(curMode, a, sizeof(curMode) - 1);
    arduinoAsleep = !strcmp(a, "SLEEP");            // silence debug text before it falls asleep
    if (!arduinoAsleep) wakePending = false;
    queueEvent("\"type\":\"mode\",\"value\":\"%s\"", a);

  } else if (m.is("SLEEP")) {
    arduinoAsleep = true;
    queueEvent("\"type\":\"sleep\"");

  } else if (m.is("WOKE")) {
    arduinoAsleep = false;
    wakePending = false;
    queueEvent("\"type\":\"wake\"");

  } else if (m.is("CHK")) {
    putTok(a, sizeof(a), m.get(0)); putTok(b, sizeof(b), m.get(1)); putTok(c, sizeof(c), m.get(2));
    queueEvent("\"type\":\"check\",\"sensor\":\"%s\",\"value\":\"%s\",\"extra\":\"%s\"", a, b, c);

  } else if (m.is("ACK")) {
    putTok(a, sizeof(a), m.get(0)); putTok(b, sizeof(b), m.get(1)); putTok(c, sizeof(c), m.get(2));
    queueEvent("\"type\":\"ack\",\"cmd\":\"%s\",\"arg\":\"%s\",\"result\":\"%s\"", a, b, c);

  } else if (m.is("HELLO")) {
    queueEvent("\"type\":\"boot\"");
  }
  (void)d;
}

/* ====================================================================== */
/*  website  ->  Pro Mini                                                  */
/* ====================================================================== */
#if SIMULATE_ARDUINO
void simCommand(const char *c, const char *a, const char *b);
#endif

void toArduino(const char *type, const char *a, const char *b, const char *c) {
#if SIMULATE_ARDUINO
  if (!strcmp(type, "CMD")) simCommand(a, b, c);
#else
  comm.send(type, a, b, c);
#endif
}

// GPIO2 is wired to INT0 (D2) of the Pro Mini. A LOW pulse wakes it from power-down.
// Between pulses the pin is left floating (input) and the 10K pull-up keeps the line HIGH,
// which also keeps GPIO2 HIGH during the ESP-01's boot (required).
void pulseWake() {
  pinMode(WAKE_PIN, OUTPUT);
  digitalWrite(WAKE_PIN, LOW);
  delay(WAKE_PULSE_MS);                             // short one-off wait (below 50 ms)
  digitalWrite(WAKE_PIN, HIGH);
  pinMode(WAKE_PIN, INPUT);
}

void wakeArduino() {
  if (!arduinoAsleep) {                             // already awake: just make sure it is in AUTO
    toArduino("CMD", "MODE", "AUTO");
    return;
  }
#if SIMULATE_ARDUINO
  toArduino("CMD", "WAKE");
#else
  pulseWake();
  wakePending = true;
  wakeSynced = false;
  wakeTries = 1;
  wakeSentAt = millis();
#endif
}

void serviceWake(unsigned long now) {
  if (!wakePending) return;
  if (!wakeSynced && now - wakeSentAt >= 250UL) {   // give the Pro Mini time to start its clock, then sync
    toArduino("CMD", "MODE", "AUTO");
    wakeSynced = true;
  }
  if (now - wakeSentAt >= WAKE_WAIT_MS) {           // no WOKE frame yet: pulse again
    if (wakeTries < WAKE_TRIES) {
      pulseWake();
      wakeTries++;
      wakeSentAt = now;
      wakeSynced = false;
    } else {
      wakePending = false;
      dbg("Pro Mini did not wake up");
    }
  }
}

void handleCommandLine(char *line) {
  if (!*line || !strcmp(line, "NONE")) return;
  dbg("command from website: %s", line);

  char *cmd = strtok(line, " ");
  char *a   = strtok(NULL, " ");
  char *b   = strtok(NULL, " ");
  if (!cmd) return;

  if (!strcmp(cmd, "WAKE")) { wakeArduino(); return; }
  if (arduinoAsleep)        { dbg("ignored (Pro Mini is asleep)"); return; }

  if (!strcmp(cmd, "MODE") && a && validMode(a)) {
    toArduino("CMD", "MODE", a);
  } else if (!strcmp(cmd, "SET") && a && b && isNumeric(b) && (!strcmp(a, "buzzer") || !strcmp(a, "servo"))) {
    toArduino("CMD", "SET", a, b);
  } else if (!strcmp(cmd, "READ") && a && validSensor(a)) {
    toArduino("CMD", "READ", a);
  } else {
    dbg("unknown command ignored");
  }
}

void pollWebsite(unsigned long now) {
  if (now - lastPollMs < POLL_MS || now < nextHttpAt) return;
  lastPollMs = now;

  char path[40], line[48] = "";
  snprintf(path, sizeof(path), "/api/device/poll?mode=%s", curMode);   // also lets the website resync its state

  if (httpRequest(false, path, NULL, line, sizeof(line))) {
    handleCommandLine(line);
  } else {
    nextHttpAt = now + RETRY_MS;
    if (lastHttpCode == 401) dbg("website says UNAUTHORIZED: check DEVICE_TOKEN in config.h");
    else                     dbg("poll failed (http %d)", lastHttpCode);
  }
}

/* ====================================================================== */
/*  Simulated Arduino (SIMULATE_ARDUINO 1): test the website without a Pro Mini */
/* ====================================================================== */
#if SIMULATE_ARDUINO
struct Sim {
  char  mode[8];
  bool  asleep, buzzer;
  float temp, hum;
  int   dist, gas, servo;
  unsigned long nextData, nextLog;
} sim = { "AUTO", false, false, 28.4f, 55.0f, 86, 300, 0, 0, 0 };

void fake(const char *type, const char *f0 = NULL, const char *f1 = NULL, const char *f2 = NULL) {
  DwellMessage m;
  memset(&m, 0, sizeof(m));
  strncpy(m.type, type, DC_TYPE_LEN - 1);
  const char *f[3] = { f0, f1, f2 };
  for (uint8_t i = 0; i < 3 && f[i]; i++) { strncpy(m.field[i], f[i], DC_FIELD_LEN - 1); m.count = i + 1; }
  onArduinoMessage(m);
}

void simCommand(const char *c, const char *a, const char *b) {
  if (!strcmp(c, "WAKE")) {
    if (sim.asleep) { sim.asleep = false; strcpy(sim.mode, "AUTO"); fake("WOKE"); fake("MODE", "AUTO"); }

  } else if (!strcmp(c, "MODE")) {
    if (!strcmp(a, "SLEEP")) {
      fake("ACK", "MODE", "SLEEP", "OK");
      strcpy(sim.mode, "SLEEP"); sim.asleep = true;
      fake("MODE", "SLEEP"); fake("SLEEP");
    } else {
      strcpy(sim.mode, a);
      fake("MODE", a); fake("ACK", "MODE", a, "OK");
    }

  } else if (!strcmp(c, "SET")) {
    bool ok = !strcmp(sim.mode, "MAINT");
    if (ok && !strcmp(a, "buzzer")) sim.buzzer = atoi(b) != 0;
    if (ok && !strcmp(a, "servo"))  sim.servo = atoi(b);
    fake("ACK", "SET", a, ok ? "OK" : "ERR");

  } else if (!strcmp(c, "READ")) {
    char v[12];
    bool all = !strcmp(a, "all");
    if (all || !strcmp(a, "temp") || !strcmp(a, "hum")) { snprintf(v, sizeof(v), "%d.%d", (int)sim.temp, (int)(sim.temp * 10) % 10); fake("CHK", "temp", v); snprintf(v, sizeof(v), "%d.%d", (int)sim.hum, (int)(sim.hum * 10) % 10); fake("CHK", "hum", v); }
    if (all || !strcmp(a, "dist")) { snprintf(v, sizeof(v), "%d", sim.dist); fake("CHK", "dist", v); }
    if (all || !strcmp(a, "gas"))  { snprintf(v, sizeof(v), "%d", sim.gas);  fake("CHK", "gas", v, sim.gas >= 400 ? "1" : "0"); }
    fake("ACK", "READ", a, "OK");
  }
}

void simTick(unsigned long now) {
  if (sim.asleep || strcmp(sim.mode, "AUTO") != 0) return;

  if (now >= sim.nextData) {
    sim.nextData = now + SIM_DATA_MS;
    sim.temp += (random(-4, 5)) / 10.0f;  if (sim.temp < 22) sim.temp = 22; if (sim.temp > 38) sim.temp = 38;
    sim.hum  += (random(-10, 11)) / 10.0f; if (sim.hum < 35) sim.hum = 35;  if (sim.hum > 80) sim.hum = 80;
    sim.dist  = 60 + random(0, 80);
    sim.gas   = 280 + random(0, 60);
    char t[10], h[10], d[10], g[10], sv[10];
    snprintf(t, sizeof(t), "%d.%d", (int)sim.temp, (int)(sim.temp * 10) % 10);
    snprintf(h, sizeof(h), "%d.%d", (int)sim.hum,  (int)(sim.hum * 10) % 10);
    snprintf(d, sizeof(d), "%d", sim.dist);
    snprintf(g, sizeof(g), "%d", sim.gas);
    snprintf(sv, sizeof(sv), "%d", sim.servo);
    DwellMessage m; memset(&m, 0, sizeof(m)); strcpy(m.type, "DATA");
    const char *f[9] = { t, h, d, sim.dist <= 100 ? "1" : "0", "0", g, "0", sim.buzzer ? "1" : "0", sv };
    for (uint8_t i = 0; i < 9; i++) strncpy(m.field[i], f[i], DC_FIELD_LEN - 1);
    m.count = 9;
    onArduinoMessage(m);
  }

  if (now >= sim.nextLog) {
    sim.nextLog = now + SIM_LOG_MS;
    char t[10], h[10], d[10], g[10];
    snprintf(t, sizeof(t), "%d.%d", (int)sim.temp, (int)(sim.temp * 10) % 10);
    snprintf(h, sizeof(h), "%d.%d", (int)sim.hum,  (int)(sim.hum * 10) % 10);
    snprintf(d, sizeof(d), "%d", sim.dist);
    snprintf(g, sizeof(g), "%d", sim.gas);
    DwellMessage m; memset(&m, 0, sizeof(m)); strcpy(m.type, "LOG");
    const char *f[6] = { t, h, d, sim.dist <= 100 ? "1" : "0", g, "0" };
    for (uint8_t i = 0; i < 6; i++) strncpy(m.field[i], f[i], DC_FIELD_LEN - 1);
    m.count = 6;
    onArduinoMessage(m);
  }
}
#endif

/* ====================================================================== */
/*  setup / loop                                                           */
/* ====================================================================== */
void setup() {
  pinMode(WAKE_PIN, INPUT);                         // wake line idle: pulled HIGH by the 10K on the board
  Serial.setRxBufferSize(512);                      // the Pro Mini keeps talking while an HTTPS request is running
  Serial.begin(SERIAL_BAUD);
  Serial.setDebugOutput(false);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

#if USE_HTTPS
  netClient.setInsecure();                          // see the guide: no certificate check (token still protects the API)
  netClient.setBufferSizes(4096, 1024);             // Cloudflare's certificate message needs a large receive buffer
#endif

  dbg("DwellGuard ESP-01 starting (%s, %s://%s:%d)",
      SIMULATE_ARDUINO ? "SIMULATED Arduino" : "Pro Mini on serial",
      USE_HTTPS ? "https" : "http", SERVER_HOST, SERVER_PORT);
  queueEvent("\"type\":\"boot\"");
}

void loop() {
  unsigned long now = millis();

  // Wi-Fi status messages
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected != wasConnected) {
    wasConnected = connected;
    if (connected) dbg("Wi-Fi connected, IP %s, signal %d dBm", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    else           dbg("Wi-Fi lost, reconnecting...");
  }

  // 1) Pro Mini -> website
#if SIMULATE_ARDUINO
  simTick(now);
#else
  if (comm.update()) onArduinoMessage(comm.message());
  while (comm.readRaw() >= 0) {}                    // text lines from the Pro Mini ("# ...") are ignored
#endif

  serviceWake(now);
  flushQueue(now);                                  // at most one HTTP request per pass
  pollWebsite(now);                                 // website -> Pro Mini

  if (now - lastStatusMs >= 30000UL) {
    lastStatusMs = now;
    dbg("status: wifi=%s mode=%s queue=%u heap=%u", connected ? "ok" : "down", curMode, pendCount, (unsigned)ESP.getFreeHeap());
  }
  yield();
}
