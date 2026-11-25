#include "DwellSensors.h"

/* ================================================================== */
/*  Sensor (base)                                                      */
/* ================================================================== */
Sensor::Sensor(uint8_t powerPin, bool powerActiveHigh, unsigned long warmupMs)
  : _powerPin(powerPin), _activeHigh(powerActiveHigh), _warmupMs(warmupMs),
    _poweredAt(0), _powered(false) {}

void Sensor::begin() {
  if (hasPowerSwitch()) {
    // write the OFF level first so the MOSFET never glitches on
    digitalWrite(_powerPin, _activeHigh ? LOW : HIGH);
    pinMode(_powerPin, OUTPUT);
  }
  _powered = false;
}

void Sensor::powerOn() {
  if (_powered) return;
  if (hasPowerSwitch()) digitalWrite(_powerPin, _activeHigh ? HIGH : LOW);
  _powered   = true;                    // (without a switch this only restarts the warm-up timer)
  _poweredAt = millis();
}

void Sensor::powerOff() {
  if (hasPowerSwitch()) digitalWrite(_powerPin, _activeHigh ? LOW : HIGH);
  _powered = false;
}

bool Sensor::isReady() const {
  return _powered && (millis() - _poweredAt >= _warmupMs);
}

unsigned long Sensor::warmupLeftMs() const {
  if (!_powered) return 0;
  unsigned long up = millis() - _poweredAt;
  return (up >= _warmupMs) ? 0 : (_warmupMs - up);
}

/* ================================================================== */
/*  DHT22 / DHT11                                                      */
/* ================================================================== */
DhtSensor::DhtSensor(uint8_t dataPin, uint8_t powerPin, bool powerActiveHigh,
                     DhtType type, unsigned long warmupMs)
  : Sensor(powerPin, powerActiveHigh, warmupMs),
    _dataPin(dataPin), _type(type), _hasValue(false),
    _temp(0), _hum(0), _lastReadMs(0), _error(0) {}

void DhtSensor::begin() {
  Sensor::begin();
  if (hasPowerSwitch()) {             // switched off: hold the data line LOW so the
    pinMode(_dataPin, OUTPUT);        // pull-up cannot back-feed the sensor
    digitalWrite(_dataPin, LOW);
  } else {
    pinMode(_dataPin, INPUT_PULLUP);  // always powered: idle HIGH
  }
}

void DhtSensor::powerOff() {
  Sensor::powerOff();
  if (hasPowerSwitch()) { pinMode(_dataPin, OUTPUT); digitalWrite(_dataPin, LOW); }
  else                  { pinMode(_dataPin, INPUT_PULLUP); }
  _hasValue = false;
}

bool DhtSensor::decode(const uint8_t d[5], DhtType type, float &temp, float &hum) {
  if ((uint8_t)(d[0] + d[1] + d[2] + d[3]) != d[4]) return false;   // checksum

  if (type == DHT_TYPE_22) {
    hum  = (float)(((uint16_t)d[0] << 8) | d[1]) / 10.0f;
    temp = (float)(((uint16_t)(d[2] & 0x7F) << 8) | d[3]) / 10.0f;
    if (d[2] & 0x80) temp = -temp;
  } else {                                                          // DHT11
    hum  = d[0] + d[1] / 10.0f;
    temp = d[2] + d[3] / 10.0f;
  }
  return true;
}

bool DhtSensor::update() {
  if (!isReady()) return false;

  unsigned long now = millis();
  if (_hasValue && (now - _lastReadMs) < 2000UL) return true;       // sensor allows 1 read / 2 s
  _lastReadMs = now;

  uint8_t data[5] = {0, 0, 0, 0, 0};

  // 1) start signal: pull the line low (DHT22 >= 1 ms, DHT11 >= 18 ms; both < 50 ms)
  pinMode(_dataPin, OUTPUT);
  digitalWrite(_dataPin, LOW);
  delay(_type == DHT_TYPE_11 ? 20 : 2);

  // 2) read the 40 data bits (timing critical, ~5 ms with interrupts off)
  noInterrupts();
  digitalWrite(_dataPin, HIGH);
  delayMicroseconds(30);
  pinMode(_dataPin, INPUT_PULLUP);

  uint8_t err = 0;
  if (pulseIn(_dataPin, LOW, 1000) == 0) {
    err = 1;                                                        // no response
  } else {
    for (uint8_t i = 0; i < 40; i++) {
      unsigned long width = pulseIn(_dataPin, HIGH, 1000);
      if (width == 0) { err = 2; break; }                           // timeout
      data[i / 8] <<= 1;
      if (width > 48) data[i / 8] |= 1;                             // ~70 us = 1, ~26 us = 0
    }
  }
  interrupts();

  float t = 0, h = 0;
  if (err == 0 && !decode(data, _type, t, h)) err = 3;              // checksum
  if (err == 0 && (h < 0 || h > 100 || t < -40 || t > 80)) err = 4; // out of range

  _error = err;
  if (err != 0) return false;                                       // keep the previous value

  _temp = t;
  _hum  = h;
  _hasValue = true;
  return true;
}

/* ================================================================== */
/*  HC-SR04 ultrasonic                                                 */
/* ================================================================== */
UltrasonicSensor::UltrasonicSensor(uint8_t trigPin, uint8_t echoPin, uint8_t powerPin,
                                   bool powerActiveHigh, unsigned long warmupMs, uint16_t maxCm)
  : Sensor(powerPin, powerActiveHigh, warmupMs),
    _trig(trigPin), _echo(echoPin),
    _timeoutUs((unsigned long)maxCm * 58UL + 500UL), _cm(-1) {}

void UltrasonicSensor::idlePins() {
  if (hasPowerSwitch()) {
    // The module's supply is switched off, so leave both signal pins floating.
    // Driving them would power the module through its own input diodes.
    pinMode(_trig, INPUT);
    pinMode(_echo, INPUT);
  } else {
    // Always powered: keep TRIG quiet (LOW) so it never fires by accident.
    digitalWrite(_trig, LOW);
    pinMode(_trig, OUTPUT);
    pinMode(_echo, INPUT);
  }
}

void UltrasonicSensor::begin() {
  Sensor::begin();
  idlePins();
}

void UltrasonicSensor::powerOn() {
  if (_powered) return;
  Sensor::powerOn();
  digitalWrite(_trig, LOW);
  pinMode(_trig, OUTPUT);
  pinMode(_echo, INPUT);
}

void UltrasonicSensor::powerOff() {
  Sensor::powerOff();
  idlePins();
  _cm = -1;
}

bool UltrasonicSensor::update() {
  if (!isReady()) return false;

  digitalWrite(_trig, LOW);
  delayMicroseconds(2);
  digitalWrite(_trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(_trig, LOW);

  // waits at most ~25 ms (4 m round trip), well below the 50 ms limit
  unsigned long us = pulseIn(_echo, HIGH, _timeoutUs);
  if (us == 0) { _cm = -1; return false; }

  _cm = (int)(us / 58UL);                                           // 58 us per cm (round trip)
  return true;
}

/* ================================================================== */
/*  Gas sensor                                                         */
/* ================================================================== */
GasSensor::GasSensor(uint8_t analogPin, uint8_t digitalPin, uint8_t powerPin,
                     bool powerActiveHigh, unsigned long warmupMs,
                     int thresholdRaw, bool doActiveLow)
  : Sensor(powerPin, powerActiveHigh, warmupMs),
    _analog(analogPin), _digital(digitalPin), _threshold(thresholdRaw),
    _doActiveLow(doActiveLow), _raw(0), _doTripped(false) {}

void GasSensor::begin() {
  Sensor::begin();
  pinMode(_digital, INPUT);           // no pull-up: the module has its own
}

bool GasSensor::update() {
  if (!isReady()) return false;

  long sum = 0;
  for (uint8_t i = 0; i < 4; i++) sum += analogRead(_analog);      // average of 4 samples
  _raw = (int)(sum / 4);

  bool level = digitalRead(_digital) == HIGH;
  _doTripped = _doActiveLow ? !level : level;
  return true;
}

/* ================================================================== */
/*  SensorHub                                                          */
/* ================================================================== */
SensorHub::SensorHub(const SensorConfig &c)
  : dht(c.dhtData, c.dhtPower, c.dhtPowerActiveHigh, c.dhtType, c.dhtWarmupMs),
    ultrasonic(c.usTrig, c.usEcho, c.usPower, c.usPowerActiveHigh, c.usWarmupMs, c.usMaxCm),
    gas(c.gasAnalog, c.gasDigital, c.gasPower, c.gasPowerActiveHigh,
        c.gasWarmupMs, c.gasThreshold, c.gasDoActiveLow),
    _presenceCm(c.presenceCm), _motionDeltaCm(c.motionDeltaCm), _prevDist(-1) {
  memset(&_r, 0, sizeof(_r));
  _r.distCm = -1;
}

void SensorHub::begin() {
  dht.begin();
  ultrasonic.begin();
  gas.begin();
}

void SensorHub::powerOnAll() {
  dht.powerOn();
  ultrasonic.powerOn();
  gas.powerOn();
}

void SensorHub::powerOffAll() {
  dht.powerOff();
  ultrasonic.powerOff();
  gas.powerOff();
  _prevDist = -1;
  memset(&_r, 0, sizeof(_r));
  _r.distCm = -1;
}

bool SensorHub::allReady() const {
  return dht.isReady() && ultrasonic.isReady() && gas.isReady();
}

void SensorHub::updateDht() {
  _r.dhtValid = dht.update() && dht.hasValue();
  if (_r.dhtValid) { _r.temp = dht.temperature(); _r.hum = dht.humidity(); }
}

void SensorHub::updateUltrasonic() {
  _r.distValid = ultrasonic.update();
  if (_r.distValid) {
    int d = ultrasonic.distanceCm();
    _r.distCm   = d;
    _r.presence = (d <= _presenceCm);
    _r.motion   = (_prevDist >= 0) && (abs(d - _prevDist) >= _motionDeltaCm);
    _prevDist   = d;
  } else {
    _r.distCm   = -1;
    _r.presence = false;
    _r.motion   = false;
    _prevDist   = -1;
  }
}

void SensorHub::updateGas() {
  _r.gasValid = gas.update();
  if (_r.gasValid) {
    _r.gasRaw      = gas.raw();
    _r.gasPercent  = gas.percent();
    _r.gasDigital  = gas.digitalTripped();
    _r.gasDetected = gas.detected();
  } else {
    _r.gasDetected = false;
  }
}

void SensorHub::updateAll() {
  updateDht();
  updateUltrasonic();
  updateGas();
}
