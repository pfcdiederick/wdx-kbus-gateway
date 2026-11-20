# CODESYS application: KBus I/O through the wdx data layer

Reference for a CODESYS application that **reads the inputs and controls the
outputs of the KBus modules through the wdx-style data layer of kbus-gateway**
(`/wda/parameters`, JSON:API), as installed natively on a WAGO PFC300.

**New: [FB_WdaKbusIo.md](FB_WdaKbusIo.md)**: a dedicated function block for
KBus I/O every 5 ms with only standard WDA REST-API requests (channel counts
read from the gateway, one batch PATCH only when outputs change, one GET).
Recommended for new projects.

The ST code is in [st/](st/). [PROMPT-for-Claude-in-CODESYS.txt](PROMPT-for-Claude-in-CODESYS.txt)
is a ready-to-paste brief for Claude in CODESYS (MCP): it adds `FB_WdaKbusIo`,
`FB_WdaSerial` and their helpers with all code, strictly additive, builds but
does not download. Generated from the `.st` files: regenerate it after a change.

```
 PFC300 (PTXdist firmware)
 ┌───────────────────────────────────────────────────────────────────────────┐
 │ CODESYS runtime (no KBus in the device tree)                              │
 │   PLC_PRG ── FB_WdxKbusIo ── FB_HttpTransaction ── NBS.TCP_*              │
 │     axDOSet[] ─▶ PATCH /wda/parameters/0-0-kbus-digitaloutputvalues       │
 │     axDI[]    ◀─ GET   /wda/parameters?filter[id]=...values               │
 │                                       │ HTTP 127.0.0.1:8000               │
 │ kbus-gateway (native, /usr/bin, init.d) ▼  owns the KBus ── I/O modules   │
 └───────────────────────────────────────────────────────────────────────────┘
```

## Contents

1. [The wdx data layer of the KBus](#1-the-wdx-data-layer-of-the-kbus)
2. [Requests used by the CODESYS application](#2-requests-used-by-the-codesys-application)
3. [Speed and determinism](#3-speed-and-determinism)
4. [CODESYS project setup](#4-codesys-project-setup)
5. [The code](#5-the-code)
6. [Using it in your application](#6-using-it-in-your-application)
7. [Testing](#7-testing)
8. [Troubleshooting](#8-troubleshooting)

---

## 1. The wdx data layer of the KBus

The gateway presents all KBus data as **parameters** following the WAGO wdx /
WDA conventions (see [`docs/reference/WDX/`](../docs/reference/WDX/) training): one flat list, JSON:API 1.0,
**GET reads, PATCH writes one parameter**.

| Rule | KBus data |
|---|---|
| **path** = hierarchical name | `KBus/DigitalOutputs/3/Value` |
| **id** = `0-0-` + lower-case path, `/` → `-` | `0-0-kbus-digitaloutputs-3-value` |
| **dataType** | DI/DO `boolean`, AI/AO `uint16` (raw), counters `uint64`, names `string`, lists `instantiations` |
| **dataRank** | `scalar` per channel, `array` for a whole class |
| Write rights | `GET /wda/parameter-definitions/{id}` → `"writeable": true` only for DO/AO values |
| Persistence | none: process values are volatile |

### Parameter tree

```
KBus/Healthy                      boolean   R    KBus cycle running without errors
KBus/CycleTime                    uint16    R    ms
KBus/CycleCount, ErrorCount       uint64    R
KBus/ConfigurationVersion         uint32    R    +1 for every bus scan: modules added/removed
KBus/Rescan                       boolean   R/W  PATCH true = rescan the KBUS
KBus/Modules                      instantiations [1..n]
KBus/Modules/{m}/Name             string    R    e.g. "digital 8DO", "75x-559"
KBus/Modules/{m}/TypeCode         uint16    R    raw KBus terminal code
KBus/Modules/{m}/ChannelCount     uint16    R
KBus/DigitalInputs                instantiations [1..nDI]
KBus/DigitalInputs/{n}/Value      boolean   R
KBus/DigitalInputs/{n}/Module, ModuleChannel   uint16 R   where the channel sits
KBus/DigitalOutputs/{n}/Value     boolean   R/W
KBus/AnalogInputs/{n}/Value       uint16    R
KBus/AnalogOutputs/{n}/Value      uint16    R/W
KBus/DigitalInputValues           boolean[] R    all DI in one parameter
KBus/DigitalOutputValues          boolean[] R/W  all DO in one parameter
KBus/AnalogInputValues            uint16[]  R
KBus/AnalogOutputValues           uint16[]  R/W
```

Channels are numbered per class, from 1, in KBus order.

**Changed node.** The gateway rescans the KBus by itself when modules are
added or removed while it runs; `KBus/ConfigurationVersion` then goes up.
`FB_WdxKbusIo` uses fixed channel counts (`uiNumDI/DO/AI/AO`): after a change,
adjust them, and check the channel numbers, because a module inserted in front
of others shifts their numbers. Outputs are 0 after a rescan until the next
PATCH (the FB sends the complete output array every exchange, so they come
back at once).

### Requests

| Request | Answer |
|---|---|
| `GET /wda/parameters` | parameters, paged (`page[limit]` default 255, `page[offset]`, `links.next`) |
| `GET /wda/parameters?filter[id]=id1,id2` | exactly these parameters, **one KBus snapshot** |
| `GET /wda/parameters/{id}` | one parameter |
| `GET /wda/parameter-definitions/{id}` | path, dataType, dataRank, writeable |
| `PATCH /wda/parameters/{id}` | write one parameter → **204 No Content** |

Status codes (WDA REST-API): `200`, `204`, `400` malformed body / type or id
mismatch / read-only (code 31) / value out of range or too many values
(code 39), `403` writes disabled (`HTTP_WRITE_ENABLE=0`, code 57), `404`
unknown id (code 17), `405` wrong method, `415` Content-Type is not
`application/vnd.api+json`. Errors carry a JSON:API `errors` body with the WDA
status `code` (main README 3.2).

Differences from the firmware's own WDA (`https://<pfc>/wda`): plain HTTP on
port 8000 (no TLS), no authentication, only the KBus parameters. Use
`HTTP_WRITE_ENABLE=0` in `/etc/kbus-gateway.conf` to make it read-only.

**Gateway 2.2.0 and newer.** The TCP listener accepts only connections from
the controller itself by default (`HTTP_BIND=127.0.0.1`). That is exactly what
this application uses (`GVL_KbusRest.gc_sGatewayHost = '127.0.0.1'`, CODESYS
with the host network), so nothing has to change. A CODESYS runtime on another
device or in a container with its own network needs `HTTP_BIND=0.0.0.0`. The
requests of `FB_WdxKbusIo` were replayed byte for byte against 2.2.0
(simulation): 204, 204, 200, values correct. The gateway also offers
registers that CODESYS can read and write like any other parameter (main
README, 3.6).

## 2. Requests used by the CODESYS application

`FB_WdxKbusIo` runs the **same fixed sequence** every exchange.

**1 · write all DO** (only if the gateway has DO):

```
PATCH /wda/parameters/0-0-kbus-digitaloutputvalues HTTP/1.0
Content-Type: application/vnd.api+json

{"data":{"type":"parameters","id":"0-0-kbus-digitaloutputvalues","attributes":{"value":[true,false,true,false,false,false,false,true]}}}
```
→ `HTTP/1.0 204 No Content`, `Content-Length: 0`

**2 · write all AO** (only if the gateway has AO): the same with
`0-0-kbus-analogoutputvalues` and `"value":[100,200,300,400]`.

**3 · read everything in one request:**

```
GET /wda/parameters?filter[id]=0-0-kbus-digitalinputvalues,0-0-kbus-digitaloutputvalues,0-0-kbus-analoginputvalues,0-0-kbus-analogoutputvalues HTTP/1.0
```
→ `200`, body (compact JSON, no spaces):

```json
{"data":[
 {"type":"parameters","id":"0-0-kbus-digitalinputvalues","attributes":{"path":"KBus/DigitalInputValues","dataType":"boolean","dataRank":"array","value":[true,false,...]}},
 {"type":"parameters","id":"0-0-kbus-digitaloutputvalues", ... "value":[true,false,...]}},
 ...],"meta":{"total":112,"offset":0,"count":4}}
```

Rules for the client:

1. **Send each request in ONE TCP write**, at most 16 KB.
2. **One request at a time.** The gateway serves one connection after another.
3. **Every wdx answer has `Content-Length`**: the answer is complete as soon
   as that many body bytes arrived. No need to wait for the connection close.
4. A PATCH array may hold fewer values than channels (the rest stays), never
   more (400, WDA code 39). Values: `true`/`false` for DO, integers 0–65535 for AO.
5. A rejected PATCH writes **nothing**; all values of one PATCH reach the
   module in the same KBus cycle.

## 3. Speed and determinism

| Part | Time |
|---|---|
| gateway: any wdx request (measured on the PC) | < 0.1 ms |
| one complete request on the PFC300, incl. TCP connect/close (curl, native gateway) | median 1.15–1.35 ms, p95 1.8–2.0 ms, ~3 % around 10 ms (CODESYS real-time threads) |
| gateway: KBus cycle | `KBUS_CYCLE_MS` (default 10 ms, 1–40), real-time SCHED_FIFO 40 |
| CODESYS: one HTTP request | a few task cycles (connect, write, read) |
| CODESYS: one exchange | 2 requests (DO + GET), 3 with AO |

What the code does for speed and constant timing:

- **Fixed sequence**, always the same number of requests per exchange (outputs
  are always written, also when unchanged) → constant exchange time.
- **Several stages per task cycle**: connect → send → receive run in the same
  cycle when the network library allows it; the next request starts in the
  same cycle as the previous one finished.
- **`Content-Length`** ends the receive immediately.
- **One GET** with `filter[id]` for all inputs, from one KBus snapshot.
- **Arrays** instead of single channels: 8 or 64 DO cost the same one PATCH.

Settings that matter:

| Setting | Where | Recommendation |
|---|---|---|
| MainTask interval | CODESYS task configuration | **10 ms** (each request needs task cycles; 50 ms made it 5× slower) |
| `KBUS_CYCLE_MS` | `/etc/kbus-gateway.conf` | 10 ms; 5 ms for faster I/O (more CPU) |
| `tInterval` | `FB_WdxKbusIo` | `T#0MS` = back to back; a fixed value (e.g. `T#100MS`) gives a fixed rhythm with slack |
| `HTTP_LOG` | `/etc/kbus-gateway.conf` | 1 in operation (2 logs every request: testing only) |

Limits: HTTP over TCP is fast but not hard real-time. Expect the exchange time
to vary by about one task cycle. For hard real-time I/O, let CODESYS drive the
KBus directly (KBus in the device tree, no gateway). Since gateway 2.0.0 the
wdx data layer is the only interface: self-describing and standard-shaped,
the same tree for CODESYS, SCADA and scripts.

`tLastExchange` shows the real duration; compare it with the expected
2–3 × (3–5 task cycles).

## 4. CODESYS project setup

1. Device: *WAGO 750-8302 PFC300*, device description of the installed
   firmware (04.08.09(30) on the test PFC300).
2. **No KBus in the device tree.** The gateway owns the KBus.
3. Library: **CAA Net Base Services** (namespace `NBS`) + Standard.
4. Task: `MainTask`, cyclic **10 ms**, calls `PLC_PRG`.
5. The gateway must run: `ssh root@<pfc> /etc/init.d/kbus-gateway status`.

## 5. The code

| File | CODESYS object | What it does |
|---|---|---|
| [st/GVL_KbusRest.st](st/GVL_KbusRest.st) | GVL | host `127.0.0.1`, port `8000`, array sizes (64 digital, 32 analog) |
| [st/F_AppendStr.st](st/F_AppendStr.st) | Function | append text to a byte buffer (requests > 255 characters) |
| [st/F_FindBytes.st](st/F_FindBytes.st) | Function | find text in a byte buffer (answers > 255 characters) |
| [st/FB_HttpTransaction.st](st/FB_HttpTransaction.st) | FB | one HTTP request: several stages per cycle, completes on `Content-Length` |
| [st/FB_WdxKbusIo.st](st/FB_WdxKbusIo.st) | FB | **cyclic KBus I/O via wdx**: PATCH DO/AO arrays, one GET for all values |
| [st/FB_WdaKbusIo.st](st/FB_WdaKbusIo.st) + `F_Wda*` | FB + functions | **KBus I/O every 5 ms, WDA standard requests only**: see [FB_WdaKbusIo.md](FB_WdaKbusIo.md) |
| [st/PRG_WdaKbusIo_Example.st](st/PRG_WdaKbusIo_Example.st) | Program | running light with `FB_WdaKbusIo`, 1 ms task |
| [st/FB_WdxWriteString.st](st/FB_WdxWriteString.st) | FB | **write a STRING register** (text for containers), automatically on change; JSON escaping, Latin-1 → `\u00XX` |
| [st/FB_WdxReadString.st](st/FB_WdxReadString.st) | FB | **read a STRING register** every `tInterval` (answer of a container); UTF-8 → Latin-1, `xNew` on a change |
| [st/PRG_TextExchange.st](st/PRG_TextExchange.st) | Program `PRG_TextExchange` | `sText` to Python, `sAnswer` back; its own program, added to the existing task (PLC_PRG unchanged) |
| [st/PLC_PRG_Example.st](st/PLC_PRG_Example.st) | Program `PLC_PRG` | running light on DO1–DO8 (reference; also shows the text exchange inline) |

For each POU, the part before `// --- implementation ---` is the declaration,
the part after it the implementation.

### FB_WdxKbusIo interface

| Input | Meaning |
|---|---|
| `xEnable` | run the exchange |
| `uiNumDI/DO/AI/AO` | channel counts of the gateway (test PFC300: 0 / 8 / 0 / 0) |
| `axDOSet[1..64]`, `auiAOSet[1..32]` | **desired** outputs |
| `tInterval` (0) | time between exchanges, 0 = back to back |
| `tTimeout` (500 ms) | per request |

| Output | Meaning |
|---|---|
| `axDI[..]`, `auiAI[..]` | **inputs** |
| `axDO[..]`, `auiAO[..]` | outputs **confirmed** by the gateway (read back in the GET) |
| `xOnline` | FALSE after 3 failed exchanges in a row |
| `tLastExchange` | duration of the last complete exchange |
| `udiExchanges`, `udiErrors`, `iLastStatus`, `sLastError` | diagnostics |

Status of the code: the requests and the parsing were verified byte for byte
against the gateway (simulation node; for the string FBs also on the PFC300
with the example container); **the ST code has not been compiled in
CODESYS yet**. Check the NBS pin names (`TCP_Connection.xActive/hConnection`,
`TCP_Write.xDone`, `TCP_Read.xReady/szCount`, `IP_ADDR.sAddr`) against the
library version in the project.

## 6. Using it in your application

```iecst
fbIo : FB_WdxKbusIo;

fbIo.axDOSet[1] := xPumpOn;                                  // control outputs
fbIo(xEnable := TRUE, uiNumDI := 0, uiNumDO := 8, uiNumAI := 0, uiNumAO := 0);
xLevelHigh := fbIo.axDI[1];                                  // read inputs
rVolt := UINT_TO_REAL(fbIo.auiAI[1]) * 10.0 / 32767.0;       // e.g. 750-559 0-10 V
IF NOT fbIo.xOnline THEN (* safe state *) END_IF
```

### Text to a Python container and its answer

`FB_WdxWriteString` writes a STRING into a register of the gateway,
`FB_WdxReadString` reads one. With the example container
([docker/example-client](../docker/example-client/example.py)) this is a
request/answer: CODESYS writes `Codesys/Message`, Python answers with
`respond()` in `Python/Response` (within about 0.1 s).

```iecst
fbText   : FB_WdxWriteString;     // sId default 0-0-registers-codesys-message-value
fbAnswer : FB_WdxReadString;      // sId default 0-0-registers-python-response-value

fbText(sValue := sText);          // written automatically when sText changes
fbAnswer(xEnable := TRUE);        // read every 200 ms
IF fbAnswer.xNew THEN sAnswer := fbAnswer.sValue; END_IF   // 'ping' -> 'pong'
```

The objects to add: `FB_WdxWriteString`, `FB_WdxReadString` and the program
`PRG_TextExchange` (added to the existing task); no existing object changes
(PLC_PRG included).

Declare both registers on the controller (`/etc/kbus-gateway-registers.conf`,
then restart the gateway):

```
Codesys/Message      string   ""
Python/Response      string   ""
```

| | |
|---|---|
| Length | at most 127 bytes UTF-8; longer: `fbText.iStatus = 400` (WDA code 39), nothing is written |
| Characters | CODESYS STRING is Latin-1: `°`, `é`, `Ö` ... arrive in Python correctly; characters outside Latin-1 (e.g. `€`) are read as `?` |
| Errors | `iStatus` 404 = register not declared, 0 = gateway not reachable; `sError` has the text |
| By hand | `tools/wdx-chat.py` on the PC shows the CODESYS texts and sends what you type as the answer |
| Order | the container polls every 0.1 s: write the next text after the answer arrived (`fbAnswer.xNew`), otherwise a text can be replaced before Python saw it |

Find the channel counts once with:

```bash
curl http://<pfc>:8000/wda/parameters?filter[id]=0-0-kbus-digitalinputs,0-0-kbus-digitaloutputs,0-0-kbus-analoginputs,0-0-kbus-analogoutputs -g
```

## 7. Testing

```bash
P=http://192.168.1.17:8000/wda/parameters
curl -g "$P?page[limit]=5"                                  # first parameters
curl -L $P/0-0-kbus-digitaloutputs-3-value/definition       # writeable: true
curl -X PATCH -H 'Content-Type: application/vnd.api+json' \
  -d '{"data":{"type":"parameters","id":"0-0-kbus-digitaloutputs-1-value","attributes":{"value":true}}}' \
  $P/0-0-kbus-digitaloutputs-1-value                        # 204, switches DO1 (real output!)
```

Live log of every request on the PFC: set `HTTP_LOG=2` in
`/etc/kbus-gateway.conf`, `/etc/init.d/kbus-gateway restart`, then
`tail -f /var/log/messages | grep kbus-gateway`.

In CODESYS: `xRun := TRUE` in `PLC_PRG`; watch `fbIo.tLastExchange`,
`fbIo.udiErrors = 0`, `fbIo.axDO` following `fbIo.axDOSet`.

## 8. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `connect failed` | gateway not running: `/etc/init.d/kbus-gateway status`, `/var/log/messages` |
| `HTTP 404` | old gateway without wdx (< 1.1.0), or a wrong id |
| `HTTP 400` | more values than channels (`uiNumDO` too large), or AO value > 65535 (WDA code 39) |
| `HTTP 415` | Content-Type header missing in a PATCH |
| `HTTP 403` | `HTTP_WRITE_ENABLE=0` |
| `tLastExchange` large | MainTask interval > 10 ms; `HTTP_LOG=2` left on |
| Outputs switch back | another client writes the same outputs |
| Download fails (KBus) | KBus node still in the device tree |
