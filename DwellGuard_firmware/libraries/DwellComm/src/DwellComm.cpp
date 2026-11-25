#include "DwellComm.h"

static const char HEX_DIGITS[] = "0123456789ABCDEF";

DwellComm::DwellComm(Stream &port)
  : _port(port), _len(0), _inFrame(false), _bad(0), _rawHead(0), _rawTail(0) {
  memset(&_msg, 0, sizeof(_msg));
}

uint8_t DwellComm::checksum(const char *body, size_t len) {
  uint8_t c = 0;
  for (size_t i = 0; i < len; i++) c ^= (uint8_t)body[i];
  return c;
}

/* ---------------- receiving ---------------- */

void DwellComm::pushRaw(char c) {
  if (c == '\r' || c == '\n') return;
  uint8_t next = (uint8_t)((_rawHead + 1) % DC_RAW_QUEUE);
  if (next == _rawTail) return;                 // queue full: drop
  _raw[_rawHead] = c;
  _rawHead = next;
}

int DwellComm::readRaw() {
  if (_rawHead == _rawTail) return -1;
  char c = _raw[_rawTail];
  _rawTail = (uint8_t)((_rawTail + 1) % DC_RAW_QUEUE);
  return (uint8_t)c;
}

bool DwellComm::update() {
  while (_port.available()) {
    char c = (char)_port.read();

    if (c == '$') {                             // a new frame always restarts parsing
      _inFrame = true;
      _len = 0;
      continue;
    }
    if (!_inFrame) { pushRaw(c); continue; }

    if (c == '\n') {                            // end of frame
      _inFrame = false;
      _buf[_len] = '\0';
      if (parse()) return true;                 // one frame per call; the rest stays buffered
      _bad++;
      continue;
    }
    if (c == '\r') continue;

    if (_len < DC_MAX_LINE - 1) {
      _buf[_len++] = c;
    } else {                                    // too long: throw it away
      _inFrame = false;
      _bad++;
    }
  }
  return false;
}

bool DwellComm::parse() {
  // find the '*' that starts the checksum
  int star = -1;
  for (int i = (int)_len - 1; i >= 0; i--) {
    if (_buf[i] == '*') { star = i; break; }
  }
  if (star < 1 || (int)_len - star != 3) return false;

  uint8_t got = 0;
  for (int k = 1; k <= 2; k++) {
    char h = _buf[star + k];
    uint8_t v;
    if (h >= '0' && h <= '9')      v = h - '0';
    else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
    else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
    else return false;
    got = (uint8_t)((got << 4) | v);
  }
  if (got != checksum(_buf, (size_t)star)) return false;

  // split "TYPE,f0,f1,..." into the message
  memset(&_msg, 0, sizeof(_msg));
  int start = 0;
  int part  = 0;                                // 0 = type, 1.. = fields
  for (int i = 0; i <= star; i++) {
    if (i == star || _buf[i] == ',') {
      int n = i - start;
      if (part == 0) {
        if (n < 1 || n >= DC_TYPE_LEN) return false;
        memcpy(_msg.type, _buf + start, n);
      } else {
        if (part > DC_MAX_FIELDS || n >= DC_FIELD_LEN) return false;
        memcpy(_msg.field[part - 1], _buf + start, n);
        _msg.count = (uint8_t)part;
      }
      part++;
      start = i + 1;
    }
  }
  return true;
}

/* ---------------- sending ---------------- */

void DwellComm::send(const char *type,
                     const char *f0, const char *f1, const char *f2,
                     const char *f3, const char *f4, const char *f5,
                     const char *f6, const char *f7, const char *f8) {
  const char *fields[9] = { f0, f1, f2, f3, f4, f5, f6, f7, f8 };

  char    out[DC_MAX_LINE + 8];
  size_t  n = 0;
  out[n++] = '$';
  size_t bodyStart = n;

  const char *p = type;
  while (*p && n < DC_MAX_LINE - 1) out[n++] = *p++;

  for (uint8_t i = 0; i < 9; i++) {
    if (fields[i] == NULL) break;
    if (n >= DC_MAX_LINE - 1) break;
    out[n++] = ',';
    p = fields[i];
    while (*p && n < DC_MAX_LINE - 1) out[n++] = *p++;
  }

  uint8_t cs = checksum(out + bodyStart, n - bodyStart);
  out[n++] = '*';
  out[n++] = HEX_DIGITS[cs >> 4];
  out[n++] = HEX_DIGITS[cs & 0x0F];
  out[n++] = '\n';

  _port.write((const uint8_t *)out, n);
}
