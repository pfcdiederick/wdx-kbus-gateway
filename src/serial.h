// Serial interface COM1 (/dev/serial): transparent forwarding through the wdx data layer.
//
// The gateway does not interpret the bytes: what a client writes to
// Serial/Com1/Transmit is sent as is, what arrives is kept in a receive
// buffer (Serial/Com1/RxData, RxCount) and split into frames by a pause of
// FrameGap ms (Serial/Com1/RxFrame, RxFrameCount). The protocol lives in the
// client (CODESYS: FB_WdaSerial). RS-232 / RS-485 is the mode set in the WBM
// (Interfaces > Serial Interface, owner "None"); the RS-485 direction is
// switched by the driver.
//
// In JSON, bytes are a string with one character per byte (U+0000..U+00FF):
// printable ASCII as is, every other byte as \u00XX.
#ifndef KBUS_GW_SERIAL_H
#define KBUS_GW_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define SERIAL_BUF      512     // receive buffer, longest frame, longest transmit (bytes)
#define SERIAL_JSON_MAX (6 * SERIAL_BUF + 3)

typedef struct {
    int      enabled;            // SERIAL_DEVICE set
    int      open;               // device open
    uint32_t baud;
    char     format[4];          // e.g. "8E1"
    uint32_t frame_gap_ms;
    uint64_t tx_count, rx_count, frame_count, errors;
    uint8_t  rx[SERIAL_BUF];     // last received bytes, oldest first
    size_t   rx_len;
    uint8_t  frame[SERIAL_BUF];  // last complete frame
    size_t   frame_len;
} serial_snapshot_t;

// Start the receive thread (does nothing when SERIAL_DEVICE is empty). 0 = ok.
int  serial_start(const gw_config_t *cfg);
void serial_stop(void);
int  serial_enabled(void);
void serial_snapshot(serial_snapshot_t *s);

// Validate settings: 0 = ok.
int  serial_valid_baud(long baud);
int  serial_valid_format(const char *fmt, size_t len);
// Apply new settings (any of them; baud 0 / fmt NULL / gap 0 = unchanged). 0 = ok.
int  serial_configure(uint32_t baud, const char *fmt, uint32_t gap_ms);

// Send bytes (blocks until written or 2 s). 0 = ok, -1 = port not open / write failed.
int  serial_write(const uint8_t *data, size_t len);

// JSON string <-> bytes. serial_parse_bytes: s = the JSON string literal with its
// quotes; returns 0, 400 (no string / malformed) or 409 (character > U+00FF or too long).
int    serial_parse_bytes(const char *s, size_t n, uint8_t *out, size_t cap, size_t *len);
size_t serial_format_bytes(const uint8_t *data, size_t len, char *buf, size_t cap);

#endif
