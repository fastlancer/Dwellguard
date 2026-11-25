/*
 * config.h - everything you may need to change lives here.
 * Pin names follow the KiCad schematic (ProMini_5V_16MHz).
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <DwellSensors.h>

/* ================= feature switches ================= */
#define DEBUG_PRINT      1    // 1 = human-readable prints in the Serial Monitor (lines start with "# ")
#define DEBUG_KEYS       1    // 1 = type !a !m !s ... in the Serial Monitor to test without the ESP
                              //     (set to 0 when the ESP-01 is connected, so boot noise can't trigger keys)
#define SEND_FRAMES      0    // 1 = also send $FRAMES for the ESP-01 (turn on when the ESP is connected)
#define USE_LCD          1    // 1 = 16x2 I2C LCD on A4/A5
#define USE_MODE_BUTTON  1    // 1 = optional push button D3 -> GND (INT1 hardware interrupt) switches mode;
                              //     0 = no button, D3 is parked as an unused pin
#define BUZZER_ACTIVE    1    // 1 = active buzzer module (on/off), 0 = passive (uses tone())

#define SERIAL_BAUD      115200UL   // Serial Monitor AND ESP-01 must use the same speed

/* ================= pins (from the schematic) =================
 * Pro Mini 5V / 16 MHz. Sensors are powered straight from V5, except the gas sensor,
 * whose GND (GND2) is switched by the N-MOSFET Q1.                                        */
// Serial: D0 (RX1) <- ESP-01 TX, D1 (TX1) -> ESP-01 RX (through the 1K / 2K divider). Shared with USB.
const uint8_t PIN_WAKE_ESP      = 2;   // D2  INT0  <- ESP-01 GPIO2 ("IO2"), 10K pull-up to 3.3 V on that net.
                                       //     NEVER enable INPUT_PULLUP here: 5 V would reach the ESP-01 pin.
const uint8_t PIN_MODE_BUTTON   = 3;   // D3  INT1  OPTIONAL push button D3 -> GND (not on the schematic yet)
const uint8_t PIN_DHT_DATA      = 4;   // D4  GPIO4  DHT22 data (3-pin connector V5 / GPIO4 / GND)
const uint8_t PIN_RELAY         = 5;   // D5  GPIO5  relay (ignored for now, left idle)
const uint8_t PIN_BUZZER        = 7;   // D7  GPIO7  buzzer signal
const uint8_t PIN_SERVO         = 9;   // D9  GPIO9  servo signal
const uint8_t PIN_GAS_POWER     = 10;  // D10 GPIO10 gas sensor power: gate of N-MOSFET Q1 (via 220R), ACTIVE HIGH
const uint8_t PIN_US_TRIG       = 11;  // D11 GPIO11T HC-SR04 TRIG  (connector pin 3)
const uint8_t PIN_US_ECHO       = 12;  // D12 GPIO12E HC-SR04 ECHO  (connector pin 2)
const uint8_t PIN_GAS_DO        = 13;  // D13 GPIO13 gas sensor digital output (shares the onboard LED pin: fine)
const uint8_t PIN_GAS_AO        = A0;  // A0  gas sensor analog output
// A4 (SDA) / A5 (SCL): I2C LCD.   Not connected in the schematic: D6, D8, A1, A2, A3 (and D3 without a button)
//
// !! The ULTRASONIC connector is wired  1:V5  2:ECHO(D12)  3:TRIG(D11)  4:GND.
// !! A standard HC-SR04 module has the order  VCC, TRIG, ECHO, GND. Plugged in straight, TRIG lands on D12
// !! and ECHO on D11. If the boot test says "no echo", swap the two numbers above (TRIG = 12, ECHO = 11).

/* ================= power switching polarity =================
 * Gas   : N-MOSFET low-side switch  -> pin HIGH = sensor ON   (flip the flag if it never turns on)
 * DHT22 and HC-SR04 are wired straight to V5: DWELL_NO_PIN = no power switch.                        */
const uint8_t PIN_DHT_POWER = DWELL_NO_PIN;
const uint8_t PIN_US_POWER  = DWELL_NO_PIN;
const bool DHT_POWER_ACTIVE_HIGH = true;     // unused while there is no switch
const bool US_POWER_ACTIVE_HIGH  = true;     // unused while there is no switch
const bool GAS_POWER_ACTIVE_HIGH = true;

/* ================= sensor settings ================= */
const DhtType DHT_MODEL          = DHT_TYPE_22;
const unsigned long DHT_WARMUP_MS = 2000UL;
const unsigned long US_WARMUP_MS  = 500UL;     // sensors without a power switch only wait this long after boot / wake-up
const unsigned long GAS_WARMUP_MS = 20000UL;   // MQ sensors need a long warm-up (heater)
const uint16_t US_MAX_CM          = 400;
const int   PRESENCE_CM           = 100;       // object closer than this = presence
const int   MOTION_DELTA_CM       = 15;        // distance change between readings = motion
const int   GAS_THRESHOLD_RAW     = 400;       // 0..1023, CALIBRATE in clean air, then in gas
const bool  GAS_DO_ACTIVE_LOW     = true;      // most MQ modules pull DO LOW when gas is detected

/* ================= thresholds (same as the website) ================= */
const float TEMP_HIGH = 35.0f;
const float TEMP_LOW  = 10.0f;
const float HUM_HIGH  = 70.0f;
const float HUM_LOW   = 30.0f;

/* ================= outputs ================= */
const uint8_t SERVO_REST_ANGLE  = 0;     // e.g. vent / door closed
const uint8_t SERVO_ALERT_ANGLE = 90;    // e.g. vent / door opened on gas, heat or humidity alert
const uint8_t LCD_ADDRESS       = 0x27;  // try 0x3F if the LCD stays blank (run an I2C scanner)

/* ================= timing (all non-blocking, millis()) ================= */
const unsigned long READ_INTERVAL_MS    = 2000UL;     // sensor reading + status print
const unsigned long LIVE_SEND_MS        = 5000UL;     // live DATA frame to the ESP
const unsigned long LOG_INTERVAL_MS     = 180000UL;   // 3 min = full points (5 min is the maximum)
const unsigned long BUTTON_DEBOUNCE_MS  = 250UL;
const unsigned long WARMUP_NOTICE_MS    = 5000UL;     // how often "warming up" is printed

#endif
