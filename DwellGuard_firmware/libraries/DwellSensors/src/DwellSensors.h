/*
 * DwellSensors - every DwellGuard sensor in ONE library (OOP).
 *
 *   Sensor            base class: power switching + warm-up handling
 *    |- DhtSensor         DHT22 / DHT11 temperature + humidity
 *    |- UltrasonicSensor  HC-SR04 distance
 *    '- GasSensor         MQ-type gas sensor (analog + digital output)
 *
 *   SensorHub         owns the three sensors and turns raw values into
 *                     Readings (presence, motion, gas alarm ...)
 *
 * A sensor's power can be switched by the microcontroller through a MOSFET
 * (power pin = the MOSFET gate pin), so the sketch can turn it completely off
 * before sleeping. A sensor wired straight to 5 V uses DWELL_NO_PIN: it cannot
 * be switched off, but warm-up and "ready" handling work the same way.
 */
#ifndef DWELLSENSORS_H
#define DWELLSENSORS_H

#include <Arduino.h>

// Use this as a sensor's power pin when it is wired straight to 5 V (no MOSFET switch).
#define DWELL_NO_PIN 255

/* ------------------------------------------------------------------ */
/*  Base class                                                         */
/* ------------------------------------------------------------------ */
class Sensor {
 public:
  Sensor(uint8_t powerPin, bool powerActiveHigh, unsigned long warmupMs);

  virtual void begin();                  // power pin -> OUTPUT, sensor starts OFF
  virtual void powerOn();
  virtual void powerOff();
  virtual bool update() = 0;             // take a new reading, true = valid

  bool isPowered() const { return _powered; }
  bool hasPowerSwitch() const { return _powerPin != DWELL_NO_PIN; }
  bool isReady() const;                  // powered AND warm-up time elapsed
  unsigned long warmupLeftMs() const;    // 0 when ready (or when off)

 protected:
  uint8_t       _powerPin;
  bool          _activeHigh;             // true: pin HIGH = sensor ON
  unsigned long _warmupMs;
  unsigned long _poweredAt;
  bool          _powered;
};

/* ------------------------------------------------------------------ */
/*  DHT22 / DHT11                                                      */
/* ------------------------------------------------------------------ */
enum DhtType : uint8_t { DHT_TYPE_11 = 11, DHT_TYPE_22 = 22 };

class DhtSensor : public Sensor {
 public:
  DhtSensor(uint8_t dataPin, uint8_t powerPin, bool powerActiveHigh,
            DhtType type, unsigned long warmupMs);

  void  begin();
  void  powerOff();
  bool  update();

  bool  hasValue()    const { return _hasValue; }
  float temperature() const { return _temp; }     // degrees C
  float humidity()    const { return _hum; }      // % RH
  uint8_t lastError() const { return _error; }    // 0 ok, 1 no response, 2 timeout, 3 checksum, 4 range

  // Turns the 5 raw bytes into temperature / humidity (public so it can be tested)
  static bool decode(const uint8_t d[5], DhtType type, float &temp, float &hum);

 private:
  uint8_t       _dataPin;
  DhtType       _type;
  bool          _hasValue;
  float         _temp, _hum;
  unsigned long _lastReadMs;
  uint8_t       _error;
};

/* ------------------------------------------------------------------ */
/*  HC-SR04 ultrasonic                                                 */
/* ------------------------------------------------------------------ */
class UltrasonicSensor : public Sensor {
 public:
  UltrasonicSensor(uint8_t trigPin, uint8_t echoPin, uint8_t powerPin,
                   bool powerActiveHigh, unsigned long warmupMs, uint16_t maxCm);

  void  begin();
  void  powerOn();
  void  powerOff();
  bool  update();

  int   distanceCm() const { return _cm; }        // -1 = no echo / out of range

 private:
  void  idlePins();                               // while off: high-impedance (switched) or quiet (always powered)
  uint8_t       _trig, _echo;
  unsigned long _timeoutUs;
  int           _cm;
};

/* ------------------------------------------------------------------ */
/*  Gas sensor (MQ-type module with AO + DO)                           */
/* ------------------------------------------------------------------ */
class GasSensor : public Sensor {
 public:
  GasSensor(uint8_t analogPin, uint8_t digitalPin, uint8_t powerPin,
            bool powerActiveHigh, unsigned long warmupMs,
            int thresholdRaw, bool doActiveLow);

  void  begin();
  bool  update();

  int     raw()            const { return _raw; }           // 0..1023
  uint8_t percent()        const { return (uint8_t)((_raw * 100L) / 1023L); }
  bool    detected()       const { return _raw >= _threshold; }
  bool    digitalTripped() const { return _doTripped; }     // module's own comparator

 private:
  uint8_t _analog, _digital;
  int     _threshold;
  bool    _doActiveLow;
  int     _raw;
  bool    _doTripped;
};

/* ------------------------------------------------------------------ */
/*  Hub: configuration + combined readings                             */
/* ------------------------------------------------------------------ */
struct SensorConfig {
  // DHT22
  uint8_t  dhtData, dhtPower;
  bool     dhtPowerActiveHigh;
  DhtType  dhtType;
  unsigned long dhtWarmupMs;
  // HC-SR04
  uint8_t  usTrig, usEcho, usPower;
  bool     usPowerActiveHigh;
  unsigned long usWarmupMs;
  uint16_t usMaxCm;
  int      presenceCm;          // closer than this = presence
  int      motionDeltaCm;       // distance change between readings = motion
  // Gas
  uint8_t  gasAnalog, gasDigital, gasPower;
  bool     gasPowerActiveHigh;
  unsigned long gasWarmupMs;
  int      gasThreshold;        // raw ADC value that counts as gas
  bool     gasDoActiveLow;
};

struct Readings {
  bool    dhtValid;   float temp;  float hum;
  bool    distValid;  int distCm;  bool presence;  bool motion;
  bool    gasValid;   int gasRaw;  uint8_t gasPercent;  bool gasDigital;  bool gasDetected;
};

class SensorHub {
 public:
  explicit SensorHub(const SensorConfig &cfg);

  void begin();
  void powerOnAll();
  void powerOffAll();
  bool allReady() const;

  void updateAll();                       // reads every sensor that is ready
  const Readings &readings() const { return _r; }

  DhtSensor        dht;
  UltrasonicSensor ultrasonic;
  GasSensor        gas;

  // single-sensor reads (used by MAINTENANCE "sensor check")
  void updateDht();
  void updateUltrasonic();
  void updateGas();

 private:
  int  _presenceCm, _motionDeltaCm;
  int  _prevDist;
  Readings _r;
};

#endif
