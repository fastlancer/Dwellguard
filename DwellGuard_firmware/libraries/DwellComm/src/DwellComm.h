/*
 * DwellComm - communication protocol between the Arduino Pro Mini and the ESP-01.
 *
 * Every message is one text line:
 *
 *      $TYPE,field,field,...*HH\n
 *
 *   $      start of frame
 *   TYPE   message type, e.g. DATA, MODE, CMD, ACK
 *   field  up to 9 values separated by commas
 *   *HH    checksum: XOR of every character between '$' and '*', as 2 hex digits
 *   \n     end of frame
 *
 * Text that is not inside a frame (for example the Pro Mini's "# ..." debug
 * prints or the ESP's boot noise) is ignored, so debug output and protocol
 * frames can share the same serial wire.
 *
 * The library only needs an Arduino Stream, so the same code runs on the
 * Pro Mini (Serial) and on the ESP-01 (Serial).
 */
#ifndef DWELLCOMM_H
#define DWELLCOMM_H

#include <Arduino.h>

#define DC_MAX_LINE    88      // longest frame body we accept
#define DC_MAX_FIELDS  10
#define DC_FIELD_LEN   10      // 9 characters + terminator
#define DC_TYPE_LEN    10
#define DC_RAW_QUEUE   8

struct DwellMessage {
  char    type[DC_TYPE_LEN];
  char    field[DC_MAX_FIELDS][DC_FIELD_LEN];
  uint8_t count;

  bool        is(const char *t) const { return strcmp(type, t) == 0; }
  const char *get(uint8_t i)    const { return (i < count) ? field[i] : ""; }
  long        getInt(uint8_t i) const { return atol(get(i)); }
  bool        fieldIs(uint8_t i, const char *s) const { return strcmp(get(i), s) == 0; }
};

class DwellComm {
 public:
  explicit DwellComm(Stream &port);

  // Call on every loop(). Returns true when one complete, valid frame is ready.
  bool update();
  const DwellMessage &message() const { return _msg; }

  // Bytes that were NOT part of a frame (used for the Serial Monitor test keys). -1 = none.
  int readRaw();

  // Send one frame. Unused fields stay NULL.
  void send(const char *type,
            const char *f0 = NULL, const char *f1 = NULL, const char *f2 = NULL,
            const char *f3 = NULL, const char *f4 = NULL, const char *f5 = NULL,
            const char *f6 = NULL, const char *f7 = NULL, const char *f8 = NULL);

  uint16_t badFrames() const { return _bad; }

  static uint8_t checksum(const char *body, size_t len);

 private:
  bool parse();
  void pushRaw(char c);

  Stream      &_port;
  char         _buf[DC_MAX_LINE];
  uint8_t      _len;
  bool         _inFrame;
  uint16_t     _bad;
  DwellMessage _msg;
  char         _raw[DC_RAW_QUEUE];
  uint8_t      _rawHead, _rawTail;
};

#endif
