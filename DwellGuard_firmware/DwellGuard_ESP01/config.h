/*
 * config.h  -  EVERYTHING YOU MUST CHANGE FOR THE ESP-01 IS IN THIS FILE.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* ============ 1) your Wi-Fi (2.4 GHz only, the ESP-01 cannot use 5 GHz) ============ */
#define WIFI_SSID        "YOUR_WIFI_NAME"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

/* ============ 2) where the website is ============
 * Option A (final):  through Cloudflare, over HTTPS
 *      USE_HTTPS 1,  SERVER_HOST "dwellguard.casa",  SERVER_PORT 443
 * Option B (first tests, no internet needed): straight to the PC on your Wi-Fi, plain HTTP
 *      USE_HTTPS 0,  SERVER_HOST "192.168.1.50" (the PC's IP from ipconfig),  SERVER_PORT 3000
 *      (the server must then be started with HOST=0.0.0.0, see the guide)        */
#define USE_HTTPS        1
#define SERVER_HOST      "dwellguard.casa"
#define SERVER_PORT      443

/* ============ 3) device identity: printed by the server the first time it starts ============ */
#define DEVICE_ID        "DEV-4F2A-9B7C"
#define DEVICE_TOKEN     "PASTE_THE_DEVICE_TOKEN_HERE"

/* ============ 4) test switches ============ */
#define SIMULATE_ARDUINO 1     // 1 = no Pro Mini connected: the ESP pretends to be the Arduino (website test)
                               // 0 = real Pro Mini on the serial wire (Pro Mini must have SEND_FRAMES 1)
#define ESP_DEBUG        1     // 1 = print progress to the serial wire (lines start with "# "). Use it with a USB-serial
                               // adapter in simulation mode; set to 0 once the Pro Mini is connected
#define SERIAL_BAUD      115200

/* ============ 5) timing (normally leave as is) ============ */
#define POLL_MS          2000UL    // how often the ESP asks the website "any command for me?"
#define HTTP_TIMEOUT_MS  5000
#define RETRY_MS         3000UL    // wait this long after a failed request
#define QUEUE_MAX        10        // log rows / events kept in RAM while the website is unreachable
#define WAKE_PIN         2         // ESP-01 GPIO2 -> Pro Mini D2 (INT0), 10K pull-up to 3.3 V on that wire
#define WAKE_PULSE_MS    20        // how long GPIO2 is pulled LOW to wake the Pro Mini
#define WAKE_WAIT_MS     3000UL    // wait for the Pro Mini to confirm it woke, then pulse again
#define WAKE_TRIES       3
#define SIM_DATA_MS      5000UL    // simulation only: live reading every 5 s
#define SIM_LOG_MS       20000UL   // simulation only: a log row every 20 s (so the table fills quickly)

#endif
