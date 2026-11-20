# FB_WdaKbusIo: KBus I/O every 5 ms through the WDA REST-API

`FB_WdaKbusIo` is a dedicated function block for cyclic KBus I/O between a
CODESYS application and kbus-gateway. It uses **only standard requests of the
WAGO Device Access (WDA) REST-API 1.11.0** (manual:
`Information/WDX/WDA_REST-API_Manual_v1_0.pdf`, chapters 6 and 9), so the same
requests work against any WDA-conformant server that offers these parameters.

Compared with `FB_WdxKbusIo`:

| | FB_WdxKbusIo | **FB_WdaKbusIo** |
|---|---|---|
| Channel counts | set by hand (`uiNumDI` … `uiNumAO`) | **read from the gateway**, again after every rescan |
| Requests per exchange | 2–3 (PATCH DO, PATCH AO, GET) | **1** (GET); **2** only when an output changed |
| Filter | `filter[id]` (gateway extension) | **`filter[path]`** (WDA standard) |
| Writing | one PATCH per array | **one batch PATCH**, DO and AO together, all or nothing |
| Rhythm | back to back or `tInterval` | fixed start-to-start **`tInterval` (5 ms)** |
| Errors | HTTP status | HTTP status **and WDA code** (`iLastWdaCode`) |

Requires **kbus-gateway 2.4.0 or newer**.

## 1. How it works

```
 every tInterval (5 ms), start to start
 ┌──────────────────────────────────────────────────────────────────────────┐
 │ outputs changed, or after an error, or every tRefresh (1 s)?             │
 │   yes ─▶ PATCH /wda/parameters  {"data":[DO array, AO array]}   -> 204  │
 │ GET /wda/parameters?filter[path]=KBus/Healthy,KBus/ConfigurationVersion, │
 │     KBus/DigitalInputValues,KBus/DigitalOutputValues,                    │
 │     KBus/AnalogInputValues,KBus/AnalogOutputValues               -> 200  │
 │   inputs + confirmed outputs, one KBus snapshot                          │
 └──────────────────────────────────────────────────────────────────────────┘
 at the start, after an error and when KBus/ConfigurationVersion changes:
   GET /wda/parameters?filter[path]=KBus/ConfigurationVersion,
       KBus/DigitalInputs,KBus/DigitalOutputs,KBus/AnalogInputs,KBus/AnalogOutputs
   channel count = number of items of each instantiations value
```

- **The write is skipped when nothing changed.** Most exchanges are a single
  GET. `tRefresh` (1 s) still sends the outputs regularly, so a restarted
  gateway gets them back within a second.
- **Write and read run in the same task cycle** when the network library
  allows it. `FB_HttpTransaction` runs up to 4 stages per call.
- **All or nothing**: when the batch PATCH is rejected, no output changes.
  The FB reads the WDA code of the first error into `iLastWdaCode`. It then
  discovers the channel counts again and resends the outputs.

## 2. Speed: what "5 ms" needs

| Part | Time |
|---|---|
| gateway: one request, measured in the simulator | ≈ 0.25 ms |
| one request on the PFC300 incl. TCP connect/close | ≈ 1.2–2 ms |
| KBus cycle of the gateway (`KBus/CycleTime`) | 2 ms on the test PFC300 (stored), default 5 ms |
| CODESYS: one HTTP request | 1–3 task cycles |

For **`tInterval := T#5MS`**:

1. **Cyclic task of 1 ms** (2 ms at most) that calls only the I/O program.
   With a 10 ms task an exchange can never be faster than 10 ms.
2. **KBus cycle ≤ 5 ms.** The test PFC300 runs at 2 ms. Check with
   `curl --unix-socket /run/kbus-gateway/wdx.sock http://x/wda/parameters/0-0-kbus-cycletime`.
3. Watch **`tMaxExchange`**. If it is regularly above 5 ms, the next
   exchange simply starts immediately; nothing is lost, the rhythm just gets
   slower. Typical values are 2–3 ms for read only and 4–5 ms with a write.

HTTP over TCP is fast but not hard real-time. Expect occasional exchanges of
around 10 ms when the CODESYS real-time threads load the CPU. For hard
real-time I/O, put the KBus in the CODESYS device tree instead of using the
gateway.

## 3. Implementation in CODESYS, step by step

### 3.1 Project

1. Device: WAGO PFC (e.g. *750-8302 PFC300*) with the device description of
   the installed firmware.
2. **No KBus in the device tree.** The gateway owns the KBus.
3. Library Manager: **CAA Net Base Services** (namespace `NBS`) and
   **Standard**.

### 3.2 Objects

Add these objects. In each `.st` file, the part before
`// --- implementation ---` is the declaration and the part after it is the
implementation.

| # | File | Object type | Name |
|---|---|---|---|
| 1 | [st/GVL_KbusRest.st](st/GVL_KbusRest.st) | Global Variable List | `GVL_KbusRest` |
| 2 | [st/F_AppendStr.st](st/F_AppendStr.st) | Function, return `BOOL` | `F_AppendStr` |
| 3 | [st/F_FindBytes.st](st/F_FindBytes.st) | Function, return `DINT` | `F_FindBytes` |
| 4 | [st/F_WdaValuePos.st](st/F_WdaValuePos.st) | Function, return `DINT` | `F_WdaValuePos` |
| 5 | [st/F_WdaCountItems.st](st/F_WdaCountItems.st) | Function, return `INT` | `F_WdaCountItems` |
| 6 | [st/F_WdaParseBools.st](st/F_WdaParseBools.st) | Function, return `INT` | `F_WdaParseBools` |
| 7 | [st/F_WdaParseUints.st](st/F_WdaParseUints.st) | Function, return `INT` | `F_WdaParseUints` |
| 8 | [st/F_WdaParseScalar.st](st/F_WdaParseScalar.st) | Function, return `LINT` | `F_WdaParseScalar` |
| 9 | [st/FB_HttpTransaction.st](st/FB_HttpTransaction.st) | Function Block | `FB_HttpTransaction` |
| 10 | [st/FB_WdaKbusIo.st](st/FB_WdaKbusIo.st) | Function Block | `FB_WdaKbusIo` |
| 11 | [st/PRG_WdaKbusIo_Example.st](st/PRG_WdaKbusIo_Example.st) | Program | `PRG_WdaKbusIo_Example` (or your own program) |

Objects 1–3 and 9 already exist when the project contains `FB_WdxKbusIo`.
**Replace `FB_HttpTransaction` anyway**: its receive buffer is now 8 KB
(`abRx : ARRAY[0..8191] OF BYTE`), because a WDA answer carries
`stringValue` and `links` and can exceed 4 KB with 64 DI/DO and 32 AI/AO.

### 3.3 Task

Task configuration → add a task:

| Setting | Value |
|---|---|
| Name | `KbusIoTask` |
| Type | Cyclic, **1 ms** (`t#1ms`) |
| Priority | higher than the main task (e.g. 1) |
| Watchdog | on, e.g. 5 ms, sensitivity 2 |
| POU | `PRG_WdaKbusIo_Example` (or your I/O program): **only** the I/O program |

The rest of the application stays in its own task and exchanges data with
the I/O program through global variables or the FB instance.

### 3.4 Call

```iecst
PROGRAM PRG_KbusIo
VAR
    fbIo : FB_WdaKbusIo;
END_VAR

fbIo.axDOSet[1]  := GVL_App.xPumpOn;                          // desired outputs
fbIo.auiAOSet[1] := GVL_App.uiSetpointRaw;
fbIo(xEnable := TRUE, tInterval := T#5MS);
GVL_App.xLevelHigh := fbIo.axDI[1];                           // inputs
GVL_App.rVolt      := UINT_TO_REAL(fbIo.auiAI[1]) * 10.0 / 32767.0;   // e.g. 750-559, 0–10 V
GVL_App.xIoOk      := fbIo.xReady AND fbIo.xOnline AND fbIo.xHealthy;
```

Use only **one** instance that writes outputs. Don't run `FB_WdxKbusIo` and
`FB_WdaKbusIo` at the same time, or they will overwrite each other's outputs.

### 3.5 Interface

| Input | Default | Meaning |
|---|---|---|
| `xEnable` | – | run the exchange; FALSE stops it and resets `tMaxExchange` |
| `sHost`, `uiPort` | `127.0.0.1`, `8000` | gateway (`GVL_KbusRest`) |
| `tInterval` | `T#5MS` | start-to-start time of two exchanges |
| `tRefresh` | `T#1S` | outputs are sent at least this often, also when unchanged |
| `tTimeout` | `T#200MS` | per request |
| `axDOSet[1..64]`, `auiAOSet[1..32]` | – | **desired** outputs (AO raw 0–65535) |

| Output | Meaning |
|---|---|
| `axDI[..]`, `auiAI[..]` | inputs, one KBus snapshot |
| `axDO[..]`, `auiAO[..]` | outputs as **confirmed** by the gateway |
| `uiNumDI/DO/AI/AO` | channel counts read from the gateway (limited to the array sizes) |
| `xReady` | channel counts known; FALSE during a new discovery |
| `xOnline` | FALSE after 3 failed requests in a row |
| `xHealthy` | `KBus/Healthy`: KBus cycle running without errors |
| `xLocalControl` | `KBus/LocalControl`: the dashboard has **local control**. The FB doesn't write outputs (no errors), keeps reading, and sends all outputs again after the release |
| `udiConfigVersion` | `KBus/ConfigurationVersion` |
| `tLastExchange`, `tMaxExchange` | duration of the last / the longest exchange |
| `udiExchanges`, `udiWrites`, `udiErrors` | counters |
| `iLastStatus`, `iLastWdaCode`, `sLastError` | last HTTP status, WDA code and error detail |

### 3.6 Local control from the dashboard

In the dashboard (*I/O > Process Image > Control*, after the login with the gear button at the top right), **Take Local Control**
lets only the dashboard write the KBus. CODESYS keeps its inputs, but its
outputs are not applied: `xLocalControl` is TRUE and `axDO` / `auiAO` show what
the dashboard set. After **Release Local Control**, the FB sends `axDOSet` /
`auiAOSet` again within one exchange. Local control survives a restart of the
gateway. Use `xLocalControl` in the application, e.g. to show it on a
visualisation or to hold sequences.

`FB_WdxKbusIo` (the older FB) does not know local control: its PATCH requests
get `403` and it goes offline (`xOnline = FALSE`) until local control is
released.

## 4. Testing

1. Gateway running and at least version 2.4.0:
   `ssh root@<pfc> "curl -s --unix-socket /run/kbus-gateway/wdx.sock http://x/wda"`
   should return `"serviceVersion":"2.4.0"` or newer and `"apiVersion":"1.11.0"`.
2. Download, start, and watch online: `xReady` and `xOnline` should go TRUE,
   `uiNumDO` should show the number of DO channels, and `udiExchanges` should
   rise by about 200 per second.
3. Set `xRun := TRUE` in `PRG_WdaKbusIo_Example`. The running light moves
   over the DO LEDs, and `udiWrites` only rises on a step (and once per
   second for the refresh).
4. Check `tMaxExchange` after a few minutes (see section 2).
5. The same requests with curl, from the controller:

```bash
S="curl -sg --unix-socket /run/kbus-gateway/wdx.sock"
$S "http://x/wda/parameters?filter[path]=KBus/ConfigurationVersion,KBus/DigitalInputs,KBus/DigitalOutputs,KBus/AnalogInputs,KBus/AnalogOutputs"
$S "http://x/wda/parameters?filter[path]=KBus/Healthy,KBus/ConfigurationVersion,KBus/DigitalInputValues,KBus/DigitalOutputValues,KBus/AnalogInputValues,KBus/AnalogOutputValues"
```

## 5. Troubleshooting

| Symptom | Cause |
|---|---|
| `xReady` stays FALSE, `iLastStatus = 400`, `sLastError` mentions `filter[path]` | gateway older than 2.4.0 (it does not support WDA filters): update it |
| `iLastStatus = 404` | gateway without the WDA interface, or wrong port |
| `iLastStatus = 403`, `iLastWdaCode = 57` | `HTTP_WRITE_ENABLE=0` in `/etc/kbus-gateway.conf` |
| `iLastStatus = 400`, `iLastWdaCode = 39` | more output values than channels (modules removed); the FB discovers again by itself |
| `iLastStatus = 0`, `sLastError = 'connect failed …'` | gateway not running, or `HTTP_ENABLE=0` / `HTTP_PORT` different |
| `tLastExchange` ≈ 10 ms or more all the time | the task is slower than 1–2 ms, or other POUs run in the same task |
| analog values cut off after a number of channels | old `FB_HttpTransaction` with a 4 KB receive buffer: replace it (3.2) |

Status of the code: the requests and the answers were checked byte for byte
against the gateway in simulation. **The ST code has not been compiled in
CODESYS yet.** Check the NBS pin names in `FB_HttpTransaction` against the
library version in your project.
