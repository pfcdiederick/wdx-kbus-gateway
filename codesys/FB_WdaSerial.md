# FB_WdaSerial: COM1 of the controller in CODESYS, through WDX

kbus-gateway forwards the serial interface **COM1** (`/dev/serial`)
transparently over the WDX data layer. The gateway doesn't interpret the
bytes. The protocol of the device (requests, answers, checksums) is
implemented in CODESYS with `FB_WdaSerial`. That makes the interface
**dynamic**: any RS-232 / RS-485 device works without a change in the gateway.

The KBUS and COM1 run in **one gateway container**: COM1 has its own receive
thread next to the KBUS cycle.

```
 device ──RS-485/232── COM1 ── kbus-gateway ── WDX: Serial/Com1/... ── FB_WdaSerial (CODESYS)
                                     ▲
                       KBUS dashboard: Settings > COM1 (baud rate, format, frame gap)
```

Requires **kbus-gateway 2.5.0 or newer**.

## 1. Settings: made in CODESYS

The settings are made on the client side, with the inputs of `FB_WdaSerial`:

| Setting | Values | Default |
|---|---|---|
| Baud rate | 1200 … 230400 | 19200 |
| Data bits / parity / stop bits | 5–8 / None, Even, Odd / 1, 2 | 8 / Even / 1 (`8E1`) |
| Frame gap | 1–1000 ms: a pause this long ends a received frame | 10 ms |

- **Inputs:** `udiSetBaudrate`, `sSetFormat` and `uiSetFrameGap` (`0` / `''` = keep the value stored in the gateway). The FB writes them only when they differ from the current ones: after the start and whenever the inputs change, so the flash isn't written in every cycle. `xSettingsApplied` is TRUE when the current settings match the inputs, and the current settings are always in `udiBaudrate`, `sFormat` and `uiFrameGap`.
- **The settings are kept after a restart** of the gateway. They're published as `Serial/Com1/Baudrate`, `Format` and `FrameGap`.
- **The dashboard** (*Settings > COM1*, `http://<pfc-ip>/kbus`) shows them; it doesn't change them.
- **RS-232 or RS-485 is set in the WBM**, not in the dashboard: *Interfaces > Serial Interface*, and the owner must be **None**. The test PFC300 is set to RS-485.

The dashboard page also has a **monitor** that shows the counters, the last frame and the last 512 received bytes, and a **Send (test)** field.

## 2. Parameters in WDX

| Path | Type | |
|---|---|---|
| `Serial/Com1/Device`, `Open` | string, boolean | port and state |
| `Serial/Com1/Baudrate`, `Format`, `FrameGap` | uint32, string, uint16 | settings (set by CODESYS) |
| `Serial/Com1/Transmit` | string | PATCH = send these bytes |
| `Serial/Com1/TxCount`, `RxCount` | uint64 | bytes sent / received |
| `Serial/Com1/RxFrame`, `RxFrameCount` | string, uint64 | last frame and counter |
| `Serial/Com1/RxData` | string | last ≤ 512 received bytes (new bytes = the last `RxCount` − old `RxCount`) |
| `Serial/Com1/Errors` | uint64 | framing, parity and overrun errors |

**Bytes as a string:** each character is one byte (U+0000–U+00FF). Printable ASCII is sent as is; every other byte is sent as `\u00XX`. For example, `"STATUS?\r\n"` or `"\u0002AB\u0003"`.

## 3. Implementation in CODESYS

Objects (besides those of `FB_WdaKbusIo`: `GVL_KbusRest`, `F_AppendStr`,
`F_FindBytes`, `F_WdaValuePos`, `F_WdaParseScalar`, `FB_HttpTransaction` with an 8 KB buffer):

| File | Object |
|---|---|
| [st/F_WdaDecodeBytes.st](st/F_WdaDecodeBytes.st) | Function, return `INT` |
| [st/FB_WdaSerial.st](st/FB_WdaSerial.st) | Function Block |
| [st/PRG_WdaSerial_Example.st](st/PRG_WdaSerial_Example.st) | Program (example) |

```iecst
fbCom1(xEnable := TRUE, sTxData := 'STATUS?$R$N', xSend := xRequest);
IF fbCom1.xNewFrame THEN
    sAnswer := fbCom1.sFrame;              // binary: fbCom1.abFrame[0..fbCom1.uiFrameLen-1]
END_IF
```

| Input | Default | |
|---|---|---|
| `xEnable` | – | run |
| `xSend` | – | rising edge: send `abTxData[0..uiTxLen-1]`, or `sTxData` when `uiTxLen = 0` |
| `sTxData`, `abTxData`, `uiTxLen` | – | text or bytes (max. 255 / 256 bytes per send) |
| `tPoll` | `T#20MS` | time between two receive requests |
| `tTimeout` | `T#500MS` | per request |
| `udiSetBaudrate`, `sSetFormat`, `uiSetFrameGap` | `0`, `''`, `0` | COM1 settings to set; `0` / `''` = keep |

| Output | |
|---|---|
| `xNewFrame` | TRUE for one call when a new frame arrived |
| `abFrame[0..511]`, `uiFrameLen`, `sFrame` | the frame (bytes / as text) |
| `xOpen`, `udiBaudrate`, `sFormat`, `uiFrameGap` | state and current settings |
| `xSettingsApplied` | the current settings match the `udiSet*` / `sSet*` inputs |
| `udiFrames`, `udiRxBytes`, `udiTxBytes`, `udiLineErrors` | counters |
| `xSendBusy`, `xSendDone` | sending |
| `xOnline`, `iLastStatus`, `iLastWdaCode`, `sLastError` | diagnostics (e.g. code 21 = COM1 not open) |

**Request / answer:** set `xSend`, wait for `xNewFrame`. Pick the frame gap
just above the longest pause *inside* one answer of your device, and well
below the time between two answers.

## 4. Timing

The gateway receives continuously and splits frames itself; CODESYS fetches
them every `tPoll` (20 ms). A frame is available about *FrameGap + tPoll* after
its last byte. If several frames arrive within one `tPoll`, only the last one
is in `RxFrame`, but `RxData` holds all bytes. For high message rates, read
`RxData`/`RxCount` or lower `tPoll`.

## 5. Troubleshooting

| Symptom | Cause |
|---|---|
| `xOpen = FALSE`, dashboard "not open" | COM1 is used by another program (e.g. the CODESYS serial driver or the WBM owner *CODESYS*): set the owner to *None* |
| `iLastStatus = 404` | gateway older than 2.5.0, or `SERIAL_DEVICE` empty |
| `udiLineErrors` rising | wrong baud rate or format, or (RS-485) A/B swapped, no termination |
| no frames, `udiRxBytes` stays 0 | wiring; RS-232 vs RS-485 mode in the WBM |

Status of the code: the gateway part was tested with a virtual serial port
(sending binary data, receiving frames, settings). The ST code has not been
compiled in CODESYS yet.
