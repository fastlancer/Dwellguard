/*
 * actuators.h - the output devices from the schematic.
 *   Buzzer    non-blocking beeps (on / chirp / repeating alarm pattern)
 *   ServoArm  moves, then detaches so it does not buzz or draw current
 *   I2cLcd    16x2 HD44780 LCD behind a PCF8574 "I2C backpack" (no extra library needed)
 */
#ifndef ACTUATORS_H
#define ACTUATORS_H

#include <Arduino.h>
#include <Wire.h>
#include <Servo.h>

/* ------------------------------------------------------------------ */
class Buzzer {
 public:
  Buzzer(uint8_t pin, bool active) : _pin(pin), _active(active), _driven(false),
    _manual(false), _chirpUntil(0), _pattern(false), _patOn(500), _patOff(500),
    _patNext(0), _patPhase(false) {}

  void begin() { drive(false, true); }

  void setManual(bool on) { _manual = on; }                       // MAINTENANCE on/off
  void chirp(unsigned long ms) { _chirpUntil = millis() + ms; if (_chirpUntil == 0) _chirpUntil = 1; }
  void startPattern(unsigned long onMs, unsigned long offMs) {    // repeating alarm
    if (_pattern) return;
    _pattern = true; _patOn = onMs; _patOff = offMs;
    _patPhase = true; _patNext = millis() + onMs;
  }
  void stopPattern() { _pattern = false; _patPhase = false; }
  void allOff()      { _manual = false; _chirpUntil = 0; stopPattern(); drive(false, true); }

  bool isOn() const { return _driven; }

  void update() {                                                  // call every loop()
    unsigned long now = millis();
    bool want = _manual;

    if (_chirpUntil != 0) {
      if ((long)(now - _chirpUntil) < 0) want = true; else _chirpUntil = 0;
    }
    if (_pattern) {
      if ((long)(now - _patNext) >= 0) {
        _patPhase = !_patPhase;
        _patNext = now + (_patPhase ? _patOn : _patOff);
      }
      if (_patPhase) want = true;
    }
    drive(want, false);
  }

 private:
  void drive(bool on, bool force) {
    if (!force && on == _driven) return;
    _driven = on;
    if (_active) {
      pinMode(_pin, OUTPUT);
      digitalWrite(_pin, on ? HIGH : LOW);
    } else {
      if (on) tone(_pin, 2500); else { noTone(_pin); pinMode(_pin, OUTPUT); digitalWrite(_pin, LOW); }
    }
  }

  uint8_t _pin; bool _active, _driven, _manual;
  unsigned long _chirpUntil;
  bool _pattern; unsigned long _patOn, _patOff, _patNext; bool _patPhase;
};

/* ------------------------------------------------------------------ */
class ServoArm {
 public:
  ServoArm(uint8_t pin) : _pin(pin), _angle(255), _attached(false), _detachAt(0) {}

  void begin(uint8_t restAngle) { moveTo(restAngle); }

  void moveTo(uint8_t angle) {
    if (angle > 180) angle = 180;
    if (angle == _angle) return;                    // already there
    if (!_attached) { _servo.attach(_pin); _attached = true; }
    _servo.write(angle);
    _angle = angle;
    _detachAt = millis() + 700UL;                   // give it time to move, then release
  }

  void detachNow() { if (_attached) { _servo.detach(); _attached = false; } }
  uint8_t angle() const { return _angle; }

  void update() {                                   // call every loop()
    if (_attached && (long)(millis() - _detachAt) >= 0) detachNow();
  }

 private:
  Servo   _servo;
  uint8_t _pin, _angle;
  bool    _attached;
  unsigned long _detachAt;
};

/* ------------------------------------------------------------------ */
class I2cLcd {
 public:
  I2cLcd(uint8_t address, uint8_t cols = 16, uint8_t rows = 2)
    : _addr(address), _cols(cols), _rows(rows), _bl(BL), _present(false) {
    for (uint8_t r = 0; r < 2; r++) _shown[r][0] = '\0';
  }

  bool begin() {                                    // false = no LCD answered at this address
    Wire.beginTransmission(_addr);
    _present = (Wire.endTransmission() == 0);
    if (!_present) return false;

    delay(40);                                      // HD44780 power-up (one-time, below 50 ms)
    write4(0x30); delay(5);
    write4(0x30); delay(5);
    write4(0x30); delayMicroseconds(150);
    write4(0x20);                                   // 4-bit mode
    command(0x28);                                  // 2 lines, 5x8 font
    command(0x0C);                                  // display on, cursor off
    command(0x06);                                  // cursor moves right
    clear();
    return true;
  }

  bool present() const { return _present; }

  void clear() {
    if (!_present) return;
    command(0x01); delay(2);
    _shown[0][0] = _shown[1][0] = '\0';
  }

  void backlight(bool on) {
    if (!_present) return;
    _bl = on ? BL : 0;
    expander(0);
  }

  // Shows text on a row (padded to the full width). Only writes when the text changed.
  void showRow(uint8_t row, const char *text) {
    if (!_present || row >= _rows || row > 1) return;
    char line[17];
    uint8_t i = 0;
    while (i < _cols && i < 16 && text[i]) { line[i] = text[i]; i++; }
    while (i < _cols && i < 16) line[i++] = ' ';
    line[i] = '\0';
    if (strcmp(line, _shown[row]) == 0) return;     // nothing new: skip the slow I2C writes
    strcpy(_shown[row], line);
    command(0x80 | (row == 0 ? 0x00 : 0x40));
    for (i = 0; line[i]; i++) send((uint8_t)line[i], RS);
  }

 private:
  static const uint8_t RS = 0x01, EN = 0x04, BL = 0x08;

  void expander(uint8_t data) {
    Wire.beginTransmission(_addr);
    Wire.write((uint8_t)(data | _bl));
    Wire.endTransmission();
  }
  void write4(uint8_t nibbleInHigh, uint8_t mode = 0) {
    uint8_t d = (nibbleInHigh & 0xF0) | mode;
    Wire.beginTransmission(_addr);
    Wire.write((uint8_t)(d | _bl | EN));            // EN high
    Wire.write((uint8_t)(d | _bl));                 // EN low  (data is latched)
    Wire.endTransmission();
    delayMicroseconds(50);
  }
  void send(uint8_t value, uint8_t mode) { write4(value & 0xF0, mode); write4((uint8_t)(value << 4), mode); }
  void command(uint8_t c) { send(c, 0); }

  uint8_t _addr, _cols, _rows, _bl;
  bool    _present;
  char    _shown[2][17];
};

#endif
