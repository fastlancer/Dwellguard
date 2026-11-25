/*
 * ======================================================================
 *  DwellGuard - Arduino Pro Mini (5V / 16 MHz) firmware
 *  CpE 414 Embedded Systems final output
 * ======================================================================
 *
 *  Three modes (enum + switch-case):
 *    AUTO         sensors run on their own, outputs react to the readings,
 *                 a log row is produced every LOG_INTERVAL_MS (3 min).
 *    MAINTENANCE  automatic operation is paused; sensors can be read on
 *                 request and the buzzer / servo can be driven by hand.
 *    SLEEP        everything is switched off and the Arduino sleeps in
 *                 SLEEP_MODE_PWR_DOWN with the brown-out detector disabled.
 *                 It wakes through a hardware interrupt (INT0 on D2: the
 *                 ESP-01's GPIO2 pulling the line low, or the optional button on D3) and goes
 *                 back to AUTO.
 *
 *  Mode switching: hardware interrupt (optional button on D3 = INT1)
 *                  and commands from the website (through the ESP-01).
 *  Timing:         millis() only, no delay() longer than 50 ms (except sleep).
 *
 *  Libraries used:   DwellSensors  (all sensors, one library)
 *                    DwellComm     (communication protocol with the ESP-01)
 *  Local files:      config.h      (pins, thresholds, switches)
 *                    actuators.h   (buzzer, servo, LCD)
 *
 *  Serial Monitor: 115200 baud. Every debug line starts with "# ".
 *  Test keys (DEBUG_KEYS 1), type them in the Serial Monitor:
 *    !a AUTO   !m MAINTENANCE   !s SLEEP   !r read sensors   !b buzzer
 *    !v servo  !p pin/config report   !h help
 * ======================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <Servo.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <DwellSensors.h>
#include <DwellComm.h>
#include "config.h"
#include "actuators.h"

/* ---------------- debug print helpers ---------------- */
#if DEBUG_PRINT
  #define P(...)   Serial.print(__VA_ARGS__)
  #define PL(...)  Serial.println(__VA_ARGS__)
#else
  #define P(...)   ((void)0)
  #define PL(...)  ((void)0)
#endif

/* ---------------- modes ---------------- */
enum Mode : uint8_t { MODE_AUTO = 0, MODE_MAINTENANCE = 1, MODE_SLEEP = 2 };

/* ---------------- prototypes ---------------- */
SensorConfig buildSensorConfig();
void parkUnusedPins();
void setupModeButton();
void onModeButton();
void onWakeInt();
void printBootReport();
const __FlashStringHelper *modeName(Mode m);
const char *modeFrameName(Mode m);
bool parseMode(const char *s, Mode &out);
void enterMode(Mode m);
bool requestMode(Mode target, const __FlashStringHelper *source);
void resetAutoTimers();
void runAuto(unsigned long now);
void runMaintenance(unsigned long now);
void runSleep();
void goToSleep();
void applyAutoLogic();
void handleButton();
void handleIncoming();
void handleFrame(const DwellMessage &m);
void handleDebugKey(char k);
bool cmdSetBuzzer(bool on);
bool cmdSetServo(int angle);
bool cmdRead(const char *what);
bool checkDht();
bool checkDistance();
bool checkGas();
void printReadings(const __FlashStringHelper *tag);
void printAlerts();
void printWarmup();
void updateLcdAuto();
void lcdText(const char *row0, const char *row1);
void sendModeFrame();
void sendDataFrame();
void sendLogFrame();
void sendAck(const char *cmd, const char *arg, bool ok);
void fmtFloat(char *out, float v, bool valid);

/* ---------------- objects ---------------- */
SensorHub sensors(buildSensorConfig());
Buzzer    buzzer(PIN_BUZZER, BUZZER_ACTIVE);
ServoArm  servoArm(PIN_SERVO);
I2cLcd    lcd(LCD_ADDRESS);
DwellComm comm(Serial);

/* ---------------- state ---------------- */
Mode mode = MODE_AUTO;

volatile bool          buttonFlag   = false;   // set by the pin-change interrupt
volatile unsigned long lastButtonMs = 0;

unsigned long lastReadMs = 0, lastLiveMs = 0, lastLogMs = 0, lastWarmNoticeMs = 0;
unsigned long logCount = 0;
bool sensorsAnnounced = false;
bool prevPresence = false;
bool expectKey = false;

/* ====================================================================== */
/*  setup                                                                  */
/* ====================================================================== */
void setup() {
  parkUnusedPins();                       // low-power: no floating pins
  power_spi_disable();                    // low-power: SPI is not used
#if BUZZER_ACTIVE
  power_timer2_disable();                 // low-power: Timer2 only needed by tone()
#endif

  Serial.begin(SERIAL_BAUD);

  pinMode(PIN_WAKE_ESP, INPUT);           // ESP-01 TX line (3.3 V push-pull, no pull-up needed)
  pinMode(PIN_RELAY, INPUT_PULLUP);       // relay ignored for now; pull-up keeps an active-LOW board OFF

  sensors.begin();                        // all sensors start powered OFF
  buzzer.begin();

#if USE_LCD
  Wire.begin();
  bool lcdOk = lcd.begin();
#endif
  servoArm.begin(SERVO_REST_ANGLE);

#if USE_MODE_BUTTON
  setupModeButton();
#endif

  PL();
  PL(F("# ================================================"));
  PL(F("#  DwellGuard Pro Mini firmware  (CpE 414)"));
  PL(F("# ================================================"));
#if USE_LCD
  P(F("# LCD on I2C 0x")); P(LCD_ADDRESS, HEX); PL(lcdOk ? F(": found") : F(": NOT found (check wiring / address)"));
#endif
  printBootReport();

  if (SEND_FRAMES) comm.send("HELLO", "DWG1");

  PL(F("# Starting in AUTO mode"));
  enterMode(MODE_AUTO);                   // powers the sensors and starts the warm-up
}

/* ====================================================================== */
/*  loop: tri-mode state machine                                           */
/* ====================================================================== */
void loop() {
  unsigned long now = millis();

  handleButton();                         // mode change requested by the hardware interrupt
  handleIncoming();                       // frames from the ESP-01 and test keys
  buzzer.update();                        // non-blocking beeps
  servoArm.update();                      // releases the servo after it moved

  switch (mode) {
    case MODE_AUTO:        runAuto(now);        break;
    case MODE_MAINTENANCE: runMaintenance(now); break;
    case MODE_SLEEP:       runSleep();          break;
  }
}

/* ====================================================================== */
/*  Mode: AUTO                                                             */
/* ====================================================================== */
void runAuto(unsigned long now) {
  // 1) wait for the sensors to warm up (gas sensor heater needs ~20 s)
  if (!sensors.allReady()) {
    if (now - lastWarmNoticeMs >= WARMUP_NOTICE_MS) {
      lastWarmNoticeMs = now;
      printWarmup();
    }
    return;
  }

  if (!sensorsAnnounced) {
    sensorsAnnounced = true;
    PL(F("# All sensors are ready. AUTO operation started."));
    lastReadMs = now - READ_INTERVAL_MS;           // first reading right away
    lastLiveMs = now - LIVE_SEND_MS;
    lastLogMs  = now - LOG_INTERVAL_MS;            // first log row right away
  }

  // 2) read the sensors, drive the outputs, print the details
  if (now - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = now;
    sensors.updateAll();
    applyAutoLogic();
    printReadings(F("AUTO"));
    updateLcdAuto();
  }

  // 3) live data for the website dashboard
  if (now - lastLiveMs >= LIVE_SEND_MS) {
    lastLiveMs = now;
    sendDataFrame();
  }

  // 4) log row for the database (3 min)
  if (now - lastLogMs >= LOG_INTERVAL_MS) {
    lastLogMs = now;
    logCount++;
    P(F("# LOG #")); P(logCount); PL(F(" saved (interval reached)"));
    printReadings(F("LOG"));
    sendLogFrame();
  }
}

void applyAutoLogic() {
  const Readings &r = sensors.readings();

  bool gasAlarm = r.gasValid && r.gasDetected;
  bool hot      = r.dhtValid && r.temp > TEMP_HIGH;
  bool humid    = r.dhtValid && r.hum  > HUM_HIGH;

  // buzzer: repeating alarm on gas, short chirp when someone comes close
  if (gasAlarm) buzzer.startPattern(500, 500); else buzzer.stopPattern();
  if (r.presence && !prevPresence) {
    buzzer.chirp(150);
    PL(F("# EVENT: presence detected"));
  }
  prevPresence = r.presence;

  // servo (vent / door): open on gas, heat or humidity alert
  servoArm.moveTo((gasAlarm || hot || humid) ? SERVO_ALERT_ANGLE : SERVO_REST_ANGLE);
}

/* ====================================================================== */
/*  Mode: MAINTENANCE                                                      */
/* ====================================================================== */
void runMaintenance(unsigned long now) {
  // Automatic operation is paused. Nothing runs here on its own:
  // sensor checks and output tests arrive as commands (website or test keys).
  (void)now;
}

bool cmdSetBuzzer(bool on) {
  if (mode != MODE_MAINTENANCE) { PL(F("# REFUSED: buzzer control needs MAINTENANCE mode")); return false; }
  buzzer.setManual(on);
  P(F("# MAINT: buzzer ")); PL(on ? F("ON") : F("OFF"));
  return true;
}

bool cmdSetServo(int angle) {
  if (mode != MODE_MAINTENANCE) { PL(F("# REFUSED: servo control needs MAINTENANCE mode")); return false; }
  if (angle < 0 || angle > 180) { PL(F("# REFUSED: servo angle must be 0..180")); return false; }
  servoArm.moveTo((uint8_t)angle);
  P(F("# MAINT: servo -> ")); P(angle); PL(F(" deg"));
  return true;
}

bool checkDht() {
  if (!sensors.dht.isReady()) { P(F("# CHECK temp/hum: not ready (")); P(sensors.dht.warmupLeftMs() / 1000UL); PL(F(" s left)")); return false; }
  sensors.updateDht();
  const Readings &r = sensors.readings();
  if (!r.dhtValid) { P(F("# CHECK temp/hum: read FAILED (DHT error ")); P(sensors.dht.lastError()); PL(F(")")); return false; }
  P(F("# CHECK temp=")); P(r.temp, 1); P(F("C hum=")); P(r.hum, 1); PL(F("%"));
  if (SEND_FRAMES) {
    char t[10], h[10]; fmtFloat(t, r.temp, true); fmtFloat(h, r.hum, true);
    comm.send("CHK", "temp", t); comm.send("CHK", "hum", h);
  }
  return true;
}

bool checkDistance() {
  if (!sensors.ultrasonic.isReady()) { P(F("# CHECK dist: not ready (")); P(sensors.ultrasonic.warmupLeftMs() / 1000UL); PL(F(" s left)")); return false; }
  sensors.updateUltrasonic();
  const Readings &r = sensors.readings();
  if (!r.distValid) { PL(F("# CHECK dist: no echo (nothing in range, or TRIG/ECHO swapped: see config.h)")); return false; }
  P(F("# CHECK dist=")); P(r.distCm); PL(F("cm"));
  if (SEND_FRAMES) { char d[10]; itoa(r.distCm, d, 10); comm.send("CHK", "dist", d); }
  return true;
}

bool checkGas() {
  if (!sensors.gas.isReady()) { P(F("# CHECK gas: not ready (")); P(sensors.gas.warmupLeftMs() / 1000UL); PL(F(" s left)")); return false; }
  sensors.updateGas();
  const Readings &r = sensors.readings();
  if (!r.gasValid) { PL(F("# CHECK gas: read FAILED")); return false; }
  P(F("# CHECK gas raw=")); P(r.gasRaw); P(F(" (")); P(r.gasPercent); P(F("%) DO=")); P(r.gasDigital ? F("TRIPPED") : F("clear"));
  P(F(" detected=")); PL(r.gasDetected ? F("YES") : F("NO"));
  if (SEND_FRAMES) { char g[10]; itoa(r.gasRaw, g, 10); comm.send("CHK", "gas", g, r.gasDetected ? "1" : "0"); }
  return true;
}

bool cmdRead(const char *what) {
  if (mode == MODE_SLEEP) return false;
  bool all = (strcmp(what, "all") == 0);
  bool known = false, ok = true;
  if (all || !strcmp(what, "temp") || !strcmp(what, "hum") || !strcmp(what, "dht")) { known = true; ok &= checkDht(); }
  if (all || !strcmp(what, "dist"))                                                 { known = true; ok &= checkDistance(); }
  if (all || !strcmp(what, "gas"))                                                  { known = true; ok &= checkGas(); }
  if (!known) PL(F("# CHECK: unknown sensor (use temp, hum, dist, gas, all)"));
  return known && ok;
}

/* ====================================================================== */
/*  Mode: SLEEP                                                            */
/* ====================================================================== */
void onWakeInt() {                                         // used for both wake interrupts
  detachInterrupt(digitalPinToInterrupt(PIN_WAKE_ESP));   // level interrupts: stop them re-firing
#if USE_MODE_BUTTON
  detachInterrupt(digitalPinToInterrupt(PIN_MODE_BUTTON));
#endif
}

void runSleep() {
  goToSleep();                      // returns only after a wake-up
  PL(F("# Wake-up: returning to AUTO"));
  if (SEND_FRAMES) comm.send("WOKE");
  enterMode(MODE_AUTO);
}

void goToSleep() {
  PL(F("# --- going to SLEEP ---"));
  PL(F("#   outputs OFF, gas sensor powered down, ADC off, brown-out detector off"));
  PL(F("#   (DHT22 and HC-SR04 are wired straight to 5 V and stay powered)"));
  PL(F("#   wake sources: ESP-01 GPIO2 pulling D2 low (INT0) or the mode button on D3 (INT1)"));
  if (SEND_FRAMES) comm.send("SLEEP");

  buzzer.allOff();
  servoArm.detachNow();
#if USE_LCD
  lcd.clear();
  lcd.backlight(false);
#endif
  sensors.powerOffAll();                  // low-power: sensor power is switched off by the MCU

  Serial.flush();                         // let the last characters leave before the UART stops

  ADCSRA &= ~_BV(ADEN);                   // low-power: ADC off
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);    // deepest sleep

  noInterrupts();
  sleep_enable();
  // In power-down only a LOW level on INT0 can wake the chip (edges need the stopped clock).
  attachInterrupt(digitalPinToInterrupt(PIN_WAKE_ESP), onWakeInt, LOW);
#if USE_MODE_BUTTON
  attachInterrupt(digitalPinToInterrupt(PIN_MODE_BUTTON), onWakeInt, LOW);
#endif
  sleep_bod_disable();                    // brown-out detector OFF: must be right before sleep_cpu()
  interrupts();                           // sei(): the next instruction (sleep) is still guaranteed to run
  sleep_cpu();                            // ---- sleeping here ----

  // ---- execution continues here after the wake-up ----
  sleep_disable();
  detachInterrupt(digitalPinToInterrupt(PIN_WAKE_ESP));
  ADCSRA |= _BV(ADEN);                    // ADC back on

  noInterrupts();
  buttonFlag = false;                     // the press that woke us is not a mode change
  lastButtonMs = millis();
  interrupts();
#if USE_MODE_BUTTON
  attachInterrupt(digitalPinToInterrupt(PIN_MODE_BUTTON), onModeButton, FALLING);   // normal mode-button interrupt again
#endif

#if USE_LCD
  lcd.backlight(true);
#endif
  PL(F("# --- WOKE UP ---"));
}

/* ====================================================================== */
/*  Mode switching                                                         */
/* ====================================================================== */
const __FlashStringHelper *modeName(Mode m) {
  switch (m) {
    case MODE_AUTO:        return F("AUTO");
    case MODE_MAINTENANCE: return F("MAINTENANCE");
    case MODE_SLEEP:       return F("SLEEP");
  }
  return F("?");
}

const char *modeFrameName(Mode m) {
  switch (m) {
    case MODE_AUTO:        return "AUTO";
    case MODE_MAINTENANCE: return "MAINT";
    case MODE_SLEEP:       return "SLEEP";
  }
  return "?";
}

bool parseMode(const char *s, Mode &out) {
  if (!strcmp(s, "AUTO"))  { out = MODE_AUTO;        return true; }
  if (!strcmp(s, "MAINT")) { out = MODE_MAINTENANCE; return true; }
  if (!strcmp(s, "SLEEP")) { out = MODE_SLEEP;       return true; }
  return false;
}

void resetAutoTimers() {
  sensorsAnnounced = false;
  prevPresence = false;
  lastWarmNoticeMs = millis() - WARMUP_NOTICE_MS;   // print the warm-up notice immediately
}

void enterMode(Mode m) {
  Mode old = mode;
  mode = m;
  if (old != m) { P(F("# MODE ")); P(modeName(old)); P(F(" -> ")); PL(modeName(m)); }

  switch (m) {
    case MODE_AUTO:
      buzzer.allOff();
      sensors.powerOnAll();               // no-op if already on, otherwise starts the warm-up
      resetAutoTimers();
      lcdText("DwellGuard AUTO", "Starting...");
      break;

    case MODE_MAINTENANCE:
      buzzer.allOff();
      servoArm.moveTo(SERVO_REST_ANGLE);
      sensors.powerOnAll();
      PL(F("# MAINTENANCE: automatic operation paused. Test keys: !r read, !b buzzer, !v servo"));
      lcdText("MAINTENANCE", "auto paused");
      break;

    case MODE_SLEEP:
      break;                              // runSleep() does the work on the next loop pass
  }
  sendModeFrame();
}

bool requestMode(Mode target, const __FlashStringHelper *source) {
  if (target == mode) { P(F("# Already in ")); PL(modeName(mode)); return true; }
  P(F("# Mode request from ")); P(source); P(F(": ")); PL(modeName(target));
  enterMode(target);
  return true;
}

// The mode button uses the INT1 hardware interrupt (onModeButton). The main loop does the work.
void handleButton() {
  if (!buttonFlag) return;
  noInterrupts(); buttonFlag = false; interrupts();

  Mode next = (mode == MODE_AUTO) ? MODE_MAINTENANCE : (mode == MODE_MAINTENANCE ? MODE_SLEEP : MODE_AUTO);
  PL(F("# BUTTON (hardware interrupt) pressed"));
  requestMode(next, F("button"));
}

#if USE_MODE_BUTTON
void setupModeButton() {
  pinMode(PIN_MODE_BUTTON, INPUT_PULLUP);                                          // button D3 -> GND
  attachInterrupt(digitalPinToInterrupt(PIN_MODE_BUTTON), onModeButton, FALLING);  // INT1
}

void onModeButton() {                                   // runs in interrupt context: keep it tiny
  unsigned long now = millis();
  if (now - lastButtonMs > BUTTON_DEBOUNCE_MS) {
    lastButtonMs = now;
    buttonFlag = true;
  }
}
#else
void setupModeButton() {}
void onModeButton() {}
#endif

/* ====================================================================== */
/*  Commands: ESP-01 frames and Serial Monitor test keys                   */
/* ====================================================================== */
void handleIncoming() {
  if (comm.update()) handleFrame(comm.message());

#if DEBUG_KEYS
  int c;
  while ((c = comm.readRaw()) >= 0) {
    if (c == '!') { expectKey = true; continue; }
    if (expectKey) { expectKey = false; handleDebugKey((char)c); }
  }
#else
  while (comm.readRaw() >= 0) {}          // discard noise
#endif
}

void handleFrame(const DwellMessage &m) {
  if (!m.is("CMD")) return;
  const char *what = m.get(0);
  P(F("# RX command: ")); P(what); P(F(" ")); P(m.get(1)); P(F(" ")); PL(m.get(2));

  if (!strcmp(what, "MODE")) {
    Mode target;
    bool ok = parseMode(m.get(1), target) && requestMode(target, F("website"));
    sendAck("MODE", m.get(1), ok);
  } else if (!strcmp(what, "SET")) {
    bool ok = false;
    if (!strcmp(m.get(1), "buzzer"))     ok = cmdSetBuzzer(m.getInt(2) != 0);
    else if (!strcmp(m.get(1), "servo")) ok = cmdSetServo((int)m.getInt(2));
    sendAck("SET", m.get(1), ok);
  } else if (!strcmp(what, "READ")) {
    sendAck("READ", m.get(1), cmdRead(m.get(1)));
  } else {
    PL(F("# unknown command"));
  }
}

void handleDebugKey(char k) {
  switch (k) {
    case 'a': requestMode(MODE_AUTO, F("serial key"));        break;
    case 'm': requestMode(MODE_MAINTENANCE, F("serial key")); break;
    case 's': requestMode(MODE_SLEEP, F("serial key"));       break;
    case 'r':
      if (mode == MODE_SLEEP) break;
      if (!sensors.allReady()) { printWarmup(); break; }
      sensors.updateAll();
      printReadings(F("READ"));
      break;
    case 'b': cmdSetBuzzer(!buzzer.isOn());                                   break;
    case 'v': cmdSetServo(servoArm.angle() == SERVO_REST_ANGLE ? SERVO_ALERT_ANGLE : SERVO_REST_ANGLE); break;
    case 'p': printBootReport();                                              break;
    case 'h':
    case '?':
      PL(F("# keys: !a AUTO  !m MAINTENANCE  !s SLEEP  !r read  !b buzzer  !v servo  !p report"));
      PL(F("# wake from SLEEP: press the button on D3, or pull D2 to GND for a moment"));
      break;
    default:
      P(F("# unknown key: ")); PL(k);
  }
}

/* ====================================================================== */
/*  Serial prints                                                          */
/* ====================================================================== */
void printReadings(const __FlashStringHelper *tag) {
  const Readings &r = sensors.readings();

  P(F("# [")); P(tag); P(F("] "));

  if (r.dhtValid) { P(F("T=")); P(r.temp, 1); P(F("C H=")); P(r.hum, 1); P(F("%")); }
  else            { P(F("T=-- H=-- (DHT err ")); P(sensors.dht.lastError()); P(F(")")); }

  P(F(" | D="));
  if (r.distValid) { P(r.distCm); P(F("cm")); } else { P(F("--")); }
  P(F(" presence=")); P(r.presence ? F("YES") : F("NO"));
  P(F(" motion="));   P(r.motion   ? F("YES") : F("NO"));

  P(F(" | GAS="));
  if (r.gasValid) { P(r.gasRaw); P(F(" (")); P(r.gasPercent); P(F("%) DO=")); P(r.gasDigital ? F("on") : F("off")); }
  else            { P(F("--")); }

  P(F(" | buzzer=")); P(buzzer.isOn() ? F("ON") : F("OFF"));
  P(F(" servo="));    P(servoArm.angle());

  printAlerts();
}

void printAlerts() {
  const Readings &r = sensors.readings();
  P(F(" | alerts="));
  bool any = false;
  if (r.gasValid && r.gasDetected)     { P(F("GAS ")); any = true; }
  if (r.dhtValid && r.temp > TEMP_HIGH) { P(F("TEMP_HIGH ")); any = true; }
  if (r.dhtValid && r.temp < TEMP_LOW)  { P(F("TEMP_LOW ")); any = true; }
  if (r.dhtValid && r.hum  > HUM_HIGH)  { P(F("HUM_HIGH ")); any = true; }
  if (r.dhtValid && r.hum  < HUM_LOW)   { P(F("HUM_LOW ")); any = true; }
  if (!any) P(F("none"));
  PL();
}

void printWarmup() {
  P(F("# Warming up: DHT22 "));       P(sensors.dht.warmupLeftMs() / 1000UL);
  P(F("s, ultrasonic "));             P(sensors.ultrasonic.warmupLeftMs() / 1000UL);
  P(F("s, gas "));                    P(sensors.gas.warmupLeftMs() / 1000UL);
  PL(F("s left"));
#if USE_LCD
  char row1[17];
  unsigned long left = sensors.gas.warmupLeftMs() / 1000UL;
  if (left > 99) left = 99;
  snprintf(row1, sizeof(row1), "Gas ready %us", (unsigned)left);
  lcdText("Warming up...", row1);
#endif
}

void printBootReport() {
  PL(F("# --- configuration ---"));
  PL(F("# D0/D1 serial to ESP-01 (RX/TX)          D2  INT0  <- ESP-01 GPIO2 (wake line)"));
  PL(F("# D3  INT1  optional mode button            D4  DHT22 data"));
  PL(F("# D5  relay (ignored)                       D7  buzzer              D9  servo"));
  PL(F("# D10 gas power (N-MOSFET, active HIGH)     D11 HC-SR04 TRIG        D12 HC-SR04 ECHO"));
  PL(F("# D13 gas DO                                A0  gas AO              A4/A5 I2C LCD"));
  PL(F("# unused (driven LOW): D6, D8, A1, A2, A3"));
  P(F("# read every ")); P(READ_INTERVAL_MS / 1000UL); P(F(" s, live frame every ")); P(LIVE_SEND_MS / 1000UL);
  P(F(" s, log every ")); P(LOG_INTERVAL_MS / 1000UL); PL(F(" s"));
  P(F("# thresholds: temp ")); P(TEMP_LOW, 0); P(F("..")); P(TEMP_HIGH, 0);
  P(F(" C, humidity ")); P(HUM_LOW, 0); P(F("..")); P(HUM_HIGH, 0);
  P(F(" %, gas raw >= ")); P(GAS_THRESHOLD_RAW);
  P(F(", presence <= ")); P(PRESENCE_CM); PL(F(" cm"));
  P(F("# ESP frames: ")); P(SEND_FRAMES ? F("ON") : F("OFF"));
  P(F("   test keys: ")); PL(DEBUG_KEYS ? F("ON (!h for help)") : F("OFF"));
}

/* ====================================================================== */
/*  LCD                                                                    */
/* ====================================================================== */
void lcdText(const char *row0, const char *row1) {
#if USE_LCD
  lcd.showRow(0, row0);
  lcd.showRow(1, row1);
#else
  (void)row0; (void)row1;
#endif
}

void updateLcdAuto() {
#if USE_LCD
  const Readings &r = sensors.readings();
  char row0[17], row1[17], t[10];

  if (r.dhtValid) { dtostrf(r.temp, 1, 1, t); snprintf(row0, sizeof(row0), "T%sC H%d%%", t, (int)(r.hum + 0.5f)); }
  else            { strcpy(row0, "T--.-C H--%"); }

  if (r.gasValid && r.gasDetected)                 strcpy(row1, "!! GAS ALERT !!");
  else if (r.dhtValid && r.temp > TEMP_HIGH)       strcpy(row1, "HIGH TEMPERATURE");
  else if (r.dhtValid && r.hum  > HUM_HIGH)        strcpy(row1, "HIGH HUMIDITY");
  else if (r.distValid)                            snprintf(row1, sizeof(row1), "D%dcm G%d%s", r.distCm, r.gasRaw, r.presence ? " P" : "");
  else                                             snprintf(row1, sizeof(row1), "D--  G%d", r.gasRaw);

  lcdText(row0, row1);
#endif
}

/* ====================================================================== */
/*  Frames for the ESP-01 (only when SEND_FRAMES is 1)                     */
/* ====================================================================== */
void fmtFloat(char *out, float v, bool valid) {
  if (!valid) { strcpy(out, "na"); return; }
  dtostrf(v, 1, 1, out);
}

void sendModeFrame() {
  if (SEND_FRAMES) comm.send("MODE", modeFrameName(mode));
}

void sendDataFrame() {
  if (!SEND_FRAMES) return;
  const Readings &r = sensors.readings();
  char t[10], h[10], d[10], g[10], sv[10];
  fmtFloat(t, r.temp, r.dhtValid);
  fmtFloat(h, r.hum, r.dhtValid);
  if (r.distValid) itoa(r.distCm, d, 10); else strcpy(d, "na");
  itoa(r.gasRaw, g, 10);
  itoa(servoArm.angle(), sv, 10);
  comm.send("DATA", t, h, d,
            r.presence ? "1" : "0", r.motion ? "1" : "0",
            g, r.gasDetected ? "1" : "0",
            buzzer.isOn() ? "1" : "0", sv);
}

void sendLogFrame() {
  if (!SEND_FRAMES) return;
  const Readings &r = sensors.readings();
  char t[10], h[10], d[10], g[10];
  fmtFloat(t, r.temp, r.dhtValid);
  fmtFloat(h, r.hum, r.dhtValid);
  if (r.distValid) itoa(r.distCm, d, 10); else strcpy(d, "na");
  itoa(r.gasRaw, g, 10);
  comm.send("LOG", t, h, d, r.presence ? "1" : "0", g, r.gasDetected ? "1" : "0");
}

void sendAck(const char *cmd, const char *arg, bool ok) {
  if (SEND_FRAMES) comm.send("ACK", cmd, arg, ok ? "OK" : "ERR");
}

/* ====================================================================== */
/*  Low-power pin handling and sensor configuration                        */
/* ====================================================================== */
void parkUnusedPins() {
  // Pins that are not connected to anything are driven LOW so they can't float
  // (floating inputs waste power). From the schematic: D6, D8, A1, A2, A3 (and D3 if there is no button).
  const uint8_t unused[] = {
    6, 8, A1, A2, A3
#if !USE_MODE_BUTTON
    , 3
#endif
  };
  for (uint8_t i = 0; i < sizeof(unused); i++) {
    pinMode(unused[i], OUTPUT);
    digitalWrite(unused[i], LOW);
  }
}

SensorConfig buildSensorConfig() {
  SensorConfig c;
  c.dhtData = PIN_DHT_DATA;   c.dhtPower = PIN_DHT_POWER;   c.dhtPowerActiveHigh = DHT_POWER_ACTIVE_HIGH;
  c.dhtType = DHT_MODEL;      c.dhtWarmupMs = DHT_WARMUP_MS;

  c.usTrig = PIN_US_TRIG;     c.usEcho = PIN_US_ECHO;       c.usPower = PIN_US_POWER;
  c.usPowerActiveHigh = US_POWER_ACTIVE_HIGH;               c.usWarmupMs = US_WARMUP_MS;
  c.usMaxCm = US_MAX_CM;      c.presenceCm = PRESENCE_CM;   c.motionDeltaCm = MOTION_DELTA_CM;

  c.gasAnalog = PIN_GAS_AO;   c.gasDigital = PIN_GAS_DO;    c.gasPower = PIN_GAS_POWER;
  c.gasPowerActiveHigh = GAS_POWER_ACTIVE_HIGH;             c.gasWarmupMs = GAS_WARMUP_MS;
  c.gasThreshold = GAS_THRESHOLD_RAW;                       c.gasDoActiveLow = GAS_DO_ACTIVE_LOW;
  return c;
}
