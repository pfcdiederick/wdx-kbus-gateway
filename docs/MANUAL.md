# kbus-gateway manual

*kbus-gateway 2.6.0 for WAGO PFC100 / PFC200 / PFC300*

This manual explains how the gateway works, how to install it, and how clients
(CODESYS, containers, SCADA, scripts) use it. For a short overview, see the
[README](../README.md).

## Contents

1. [How it works](#1-how-it-works)
2. [Installation](#2-installation)
3. [The wdx data layer](#3-the-wdx-data-layer)
4. [Complex modules: raw process data, AI status, register read](#4-complex-modules-raw-process-data-ai-status-register-read)
5. [Serial interface COM1](#5-serial-interface-com1)
6. [Containers and registers](#6-containers-and-registers)
7. [Dashboard (kbus-manager)](#7-dashboard-kbus-manager)
8. [IEC 60870-5-104 server (104-gateway)](#8-iec-60870-5-104-server-104-gateway)
9. [CODESYS as a client](#9-codesys-as-a-client)
10. [Configuration](#10-configuration)
11. [Speed and determinism](#11-speed-and-determinism)
12. [Troubleshooting](#12-troubleshooting)
13. [Development](#13-development)
14. [Limitations and security](#14-limitations-and-security)
15. [Background and history](#15-background-and-history)

---

## 1. How it works

The gateway is a small C program. It **owns the KBUS** of the controller and is
the only interface to it. The CODESYS runtime runs without KBUS in its device
tree, and talks to the gateway like every other client.

```
 CODESYS (host network) ──TCP 127.0.0.1:8000──┐
 container A ─┐                               │   kbus-gateway
 container B ─┼── /run/kbus-gateway/wdx.sock ─┼─▶ wdx data layer ──▶ KBUS provider ──▶ KBUS ──▶ I/O modules
 dashboard   ─┘    (Unix socket)              │                  ──▶ register provider (memory + /home)
                                              │                  ──▶ COM1 (/dev/serial)
```

All data is presented as **parameters** of a wdx data layer. It is a subset of
the WAGO REST-API 1.11.0 (manual: `Information/WDX/WDA_REST-API_Manual_v1_0.pdf`):
- JSON:API, with **GET to read and PATCH to write**;
- two methods for actions that need an answer (section 4).

The URL paths start with `/wda/`, as in the WAGO manual.

### 1.1 Inside the program

```
                                ┌───────────────────────────────────────────────┐
 GET   /wda/parameters/...  ────┼─▶ server: Unix socket + TCP 127.0.0.1:8000  │
 PATCH /wda/parameters/...      │     one thread per client, wdx data layer     │
 POST  /wda/methods/.../runs    │        │ read / write (mutex)    registers   │
                                │        ▼                         (own lock)   │
                                │   channel map + process images in memory     │
                                │        ▲                                      │
                                │        │ every 5 ms (KBUS_CYCLE_MS), SCHED_FIFO 40
                                │   KBUS cycle thread                           │
                                │    write outputs → libpackbus_Push → read in  │
                                │   register worker (AI status, ReadRegister)   │
                                └────────┼──────────────────────────────────────┘
                                         ▼ dlopen() of the firmware's libdal, KBUS-info (D-Bus)
                                       KBUS
```

**Startup**

1. **Libraries.** The WAGO libraries (`libdal.so`, `libdbuskbuscommon.so`, the DAL
   plugins) are loaded with `dlopen()` from the firmware's own `/usr/lib`. The build
   only needs their headers (2.3).
2. **Open the KBUS.** The KBUS device is opened. The modules are read through the
   KBUS-info D-Bus interface: position, type, bit offsets and sizes, number of
   channels.
3. **Channel map.** Channels are numbered from 1 per type, in module order from left
   to right:
   - **Digital module** (type code bit 15 set): every input bit is a `DI`, every
     output bit a `DO` (channels 1–16 of a module exchange data).
   - **Analog module** with only input data → `AI`; with only output data → `AO`.
     Channel *k* is the unsigned 16-bit word at byte offset 2·(k−1), as a raw value.
   - **Complex modules** (750-495, 750-650, 750-657, 750-671 …, data in both
     directions) get **no channels**. Their bytes are exchanged raw by the client
     (section 4).
4. **Running.** The KBUS is set to *Running*. From then on the gateway drives every
   bus cycle itself.
5. **Registers** (6.3) and **COM1** (5) are started.
6. **Server.** The server starts. The parameter table is built once per bus
   configuration.

**KBUS cycle thread.** Every `KBUS_CYCLE_MS` a real-time thread runs one cycle:
1. It copies the output image under a short lock.
2. It runs one bus cycle (`libpackbus_Push`).
3. It stores the input image and numbers the completed cycle.

WAGO output modules switch off when no cycle arrives within **50 ms**, so this thread
never waits for HTTP.

**Requests** only touch the images in memory:
- Reading returns the value of the last cycle.
- A write is sent with the next cycle.
- All values of one request reach the modules in the **same** cycle.

**Dynamic configuration.** After a change of the node the KBUS reports errors. After
0.5 s of failing cycles the gateway rescans:
1. It stops the cycle (outputs to 0).
2. It reopens the KBUS and reads the modules again.
3. It rebuilds the channel map and `KBus/ConfigurationVersion` goes up by one.

You can also start a rescan with `PATCH KBus/Rescan = true` or with the dashboard
button. If the rescan does not help, the next attempts wait 5, 10, 20, 40 and then
60 s. Keep in mind:
- A module inserted in front of others shifts their channel numbers.
- After a rescan all outputs are 0 until a client writes them again.

**Shutdown.** On SIGTERM (`stop`, `docker stop`) all outputs go to 0, one last
cycle runs and the KBUS is closed. After a crash, the modules switch off on their
own after 50 ms.

**Digital offset.** Digital data sits behind the analog data in the process image.
The gateway asks libpackbus where that area starts, detects whether the KBUS-info
offsets are relative or absolute, and logs the choice (`KBUS_DIGITAL_OFFSET_MODE`
overrides it).

---

## 2. Installation

The examples use the test controller `192.168.1.17`. *PC* commands run in the
project directory on a Linux PC; *PFC* commands run on the controller
(`ssh root@<pfc-ip>`).

There are two ways to run the gateway. Only one may run at a time.

| | Native package (`.ipk`) | Docker container |
|---|---|---|
| Install | `tools/deploy.sh <pfc-ip>` | `tools/deploy-docker.sh <pfc-ip>` |
| Settings | `/etc/kbus-gateway.conf` | image defaults + `GW_ENV` |
| Measured on the PFC300 | KBUS exchange max 592 µs, request (socket) 1.33 ms median | 683 µs, 1.45 ms |

Both perform the same. The current test controller runs the **container**,
together with the dashboard.

### 2.1 Requirements

On the PC:

| What | Why |
|---|---|
| Linux PC with **Docker** (buildx) | the SDK builder image (native) or the container images |
| The **WAGO firmware SDK** of the controller, e.g. [pfc300-firmware-sdk](https://github.com/WAGO/pfc300-firmware-sdk) | toolchain (native package) and the header files |
| The **SDK headers** in `sdk/include` (2.3) | ADI/DAL, KBUS-info and register API; WAGO-proprietary, not in git |
| Python 3 | tests |
| SSH as `root` to the PFC | install |

On the PFC:
- **Only one KBUS master.** Either set WBM → *PLC Runtime* → runtime **None**, or run
  CODESYS **without KBUS** in its device tree.
- Check the firmware once:

```bash
ssh root@192.168.1.17 'uname -m; cat /etc/REVISIONS; ls -l /usr/lib/libdal.so*; opkg list-installed | grep -E "^(dal|libdbuskbuscommon) "'
```

  PFC300 FW 04.08.09(30) gives: `aarch64`, `libdal.so.1.0.1`, glibc 2.35, packages
  `dal 1.0.1` and `libdbuskbuscommon 1.0.0.0`.

### 2.2 SDK headers

`sdk/include/` must hold the WAGO headers, with
`sdk/include/dal/adi_application_interface.h` present. They carry a WAGO
"proprietary rights" notice and are therefore **not in git**. Only `OsCommon.h`, a
minimal stand-in of our own, is in the repository. Import them once.

**A. From the prebuilt SDK packages** (no SDK build needed):

```bash
tools/import-sdk-packages.sh ~/Documents/WagoFirmware/pfc300-firmware-sdk/ptxproj/configs/wago-pfc300/packages
```

This takes:
- `include/dal/*` from `dal_<version>_<arch>.tgz`;
- `ldkc_kbus_information.h` and `ldkc_kbus_register_communication.h` from
  `libdbuskbuscommon_<version>_<arch>.tgz`.

**B. From a completely built SDK sysroot:**

```bash
tools/import-sdk-headers.sh ~/wago/ptxproj
```

Notes:
- The **DAL version** must match `libdal.so.<version>` on the PFC.
- The headers have no architecture-specific parts: the PFC300 headers (DAL 1.0.1)
  also fit a PFC200 with FW31.
- Without `ldkc_kbus_register_communication.h` the gateway builds and runs without
  register access: no AI status, `KBus/ReadRegister` answers "not available".
- Without `sdk/include/dal` only the **simulation** build works (`make`). That is
  enough for development (13.2).

### 2.3 Docker container

```bash
tools/deploy-docker.sh 192.168.1.17 --manager           # gateway + dashboard (what the test PFC runs)
tools/deploy-docker.sh 192.168.1.17 --manager --no-gateway   # only the dashboard
tools/deploy-docker.sh 192.168.1.17 --client --104-gateway --no-gateway   # example client and IEC 104 server
SKIP_BUILD=1 tools/deploy-docker.sh ...                 # reuse the archives in build-docker/
tools/deploy-docker.sh 192.168.1.17 --native            # remove the gateway container, native service again
```

What the script does:
1. It builds the images for the controller's architecture and stores them in
   `build-docker/`.
2. It loads them on the controller.
3. It stops the native service and removes its boot link: only one gateway may run.
4. It starts the containers with `--restart unless-stopped`.

**Deploying restarts the gateway: the outputs are 0 for a moment.**

Other settings go in with `GW_ENV`, e.g. `GW_ENV="-e KBUS_CYCLE_MS=2"`. A cycle time
written via `KBus/CycleTime` is stored and wins over `KBUS_CYCLE_MS`.

| Gateway container option | Why |
|---|---|
| `--privileged`, `--network host` | real-time priority, KBUS hardware; TCP `127.0.0.1:8000` for CODESYS |
| `/usr/lib` → `/host/usr/lib` (ro) | the firmware's WAGO libraries; the entrypoint loads them without the firmware's glibc |
| `/var/run/dbus`, `/etc/specific`, `/dev/eeprom`, `/dev/ttyKbus`, type label | module discovery and KBUS configuration |
| `/dev/log`, `/etc/localtime` | log to `/var/log/messages` |
| `/run/kbus-gateway`, `/run/kbus-gateway-control` | the data socket and the dashboard's control socket |
| `/etc/kbus-gateway-registers.conf`, `/home/kbus-gateway` | registers and stored settings |
| `/dev/serial` | COM1 |

After a firmware update, check the container first: it depends on the firmware's
libraries.

### 2.4 Native package

```bash
tools/deploy.sh 192.168.1.17               # PC: build .ipk, copy, opkg install, show status
tools/build-ipk.sh arm64                   # only build (PFC300); armhf = PFC100/PFC200 (untested)
```

What the build does:
1. `build-ipk.sh` compiles inside WAGO's SDK builder image (`pfc300-builder`, from
   `make builder` in the firmware SDK) with the firmware's own cross compiler
   (GCC 11.3, `-Werror`).
2. It packs the result like a PTXdist package.
3. The package depends on `dal` and `libdbuskbuscommon`.

| On the controller | |
|---|---|
| `/usr/bin/kbus-gateway` | the program |
| `/etc/init.d/kbus-gateway` | `start`, `stop`, `restart`, `status` |
| `/etc/rc.d/S99_kbus-gateway` | boot link, created by the package |
| `/etc/kbus-gateway.conf` | settings (section 10); kept on updates when changed |
| `/etc/kbus-gateway-registers.conf` | register declarations (6.3) |
| `/var/log/messages` | log (`kbus-gateway[pid]`) |

- **Update:** raise [VERSION](../VERSION) and install again.
- **Remove:** `opkg remove kbus-gateway` (all outputs go to 0).

### 2.5 Check the start

`grep kbus-gateway /var/log/messages | tail -20` on the PFC. A good start (PFC300,
8DI + 8DO + 750-650, container, 2.6.0):

```
module 3 (75x-650) is a complex module (in 32 / out 32 bits): no channels, raw process data via KBus/Exchange
module  1: digital 8DI    in   8 bits @32    out   0 bits @0
module  2: digital 8DO    in   0 bits @0     out   8 bits @32
module  3: 75x-650        in  32 bits @0     out  32 bits @0
channels: 8 DI, 8 DO, 0 AI, 0 AO; 0 x 750-495 (configuration 1)
KBUS cycle thread running: SCHED_FIFO priority 40, every 2 ms
register communication (read only): AI status register 6 every 1000 ms per channel
wdx data layer listening on unix socket /run/kbus-gateway/wdx.sock (mode 0660)
control socket for the dashboard: /run/kbus-gateway-control/control.sock
COM1 /dev/serial open: 19200 8E1, frame gap 10 ms
wdx data layer listening on 127.0.0.1:8000 (GET /wda/parameters)
```

Quick test on the controller:

```bash
ssh root@192.168.1.17 "curl -sg --unix-socket /run/kbus-gateway/wdx.sock 'http://x/wda/parameters?page[limit]=5'"
```

---

## 3. The wdx data layer

### 3.1 Rules

| Rule | Example |
|---|---|
| **path** = hierarchical name | `KBus/DigitalOutputs/3/Value` |
| **id** = `0-0-` + lower-case path, `/` → `-` | `0-0-kbus-digitaloutputs-3-value` |
| **dataType** | `boolean`, `uint8`, `uint16`, `uint32`, `uint64`, `int32`, `float64`, `string`, `instantiations` |
| **dataRank** | `scalar`, or `array` (a whole class or a byte block in one parameter) |
| **value** / **stringValue** | `value` in its JSON type; numeric types also as text in `stringValue` (`"123"`, `["1","2"]`) |
| write rights | `GET /wda/parameter-definitions/{id}` → `"writeable": true` |
| persistence | `"userSetting": true` = survives a restart (persistent registers, cycle time, COM1 settings, local control) |

**Number or text?** In parameters, `value` of a numeric type is a JSON **number**,
and `stringValue` is the same as **text**. Method outArgs only have `value`, as a
number; `uint64` counters too, without quotes. Error objects carry `status` and
`code` as **text**. Bytes are arrays of numbers (`[16,0,255]`), never base64.

### 3.2 Parameter tree

```
KBus/Healthy                      boolean   R    KBUS cycle running without errors
KBus/CycleTime                    uint16    R/W  ms, 1–40; effective at the next cycle, stored
KBus/CycleCount, ErrorCount       uint64    R
KBus/ConfigurationVersion         uint32    R    +1 for every bus scan (start = 1): the tree changed
KBus/Rescan                       boolean   R/W  PATCH true = rescan the KBUS now
KBus/CycleDuration, MaxCycleDuration, MaxWakeupDelay   uint32 R   µs, timing of the cycle thread
KBus/Overruns                     uint64    R    cycles that started too late
KBus/ResetStatistics              boolean   R/W  true = counters and maxima back to 0 (dashboard only)
KBus/LocalControl                 boolean   R/W  true = only the dashboard may write the KBUS (7.3)

KBus/Modules                      instantiations [1..n]
KBus/Modules/{m}/Name             string    R    "digital 8DO", "75x-559", "75x-657"
KBus/Modules/{m}/TypeCode         uint16    R    raw type code (digital: e.g. 34817 = 0x8801)
KBus/Modules/{m}/ChannelCount     uint16    R
KBus/Modules/{m}/Position         uint16    R    KBUS position (= m)
KBus/Modules/{m}/InputBytes, OutputBytes  uint16 R   process data in bytes, rounded up
KBus/Modules/{m}/RawInput         uint8[]   R    complex modules only (section 4)
KBus/Modules/{m}/RawOutput        uint8[]   R/W  complex modules only (section 4)
KBus/ModulePositions, ModuleNames, ModuleTypeCodes, ModuleInputBytes, ModuleOutputBytes
                                  arrays    R    the module list, one array per property

KBus/DigitalInputs|DigitalOutputs|AnalogInputs|AnalogOutputs   instantiations [1..n]
KBus/DigitalInputs/{n}/Value      boolean   R
KBus/DigitalOutputs/{n}/Value     boolean   R/W
KBus/AnalogInputs/{n}/Value       uint16    R    raw value
KBus/AnalogOutputs/{n}/Value      uint16    R/W  raw value 0–65535
KBus/<Class>/{n}/Module, ModuleChannel   uint16 R   where the channel sits
KBus/AnalogInputs/{n}/Status      uint16    R    status register 6 (section 4.4)
KBus/DigitalInputValues           boolean[] R    all DI in one parameter
KBus/DigitalOutputValues          boolean[] R/W  all DO
KBus/AnalogInputValues            uint16[]  R    all AI
KBus/AnalogOutputValues           uint16[]  R/W  all AO
KBus/AnalogInputStatusValues      uint16[]  R    status register 6 of all AI (section 4.4)
KBus/AnalogInputStatusValid       boolean[] R

Registers/ChangeCounter           uint64    R    +1 for every write that changed a register
Registers/<Name>/Value            declared  R/W  (6.3)
Serial/Com1/...                             COM1 (section 5)
Gateway/Status                    string    R    "online"
Gateway/Version                   string    R    "2.6.0"
```

Values are **raw process-image values**: for a 750-559 (0–10 V), 32767 = 10 V. Scale
them in the client.

### 3.3 Requests

| Request | Answer |
|---|---|
| `GET /wda` | service identity: `serviceVersion` (gateway), `apiVersion` 1.11.0 |
| `GET /wda/parameters` | all parameters, **paged**: `page[offset]` (0), `page[limit]` (255), `links.next` |
| `GET /wda/parameters?filter[path]=A,B` | these paths; also `filter[device]`, `beta`, `deprecated`, `definition.writeable`, `definition.userSetting` |
| `GET /wda/parameters?filter[id]=a,b` | extension: exactly these ids, all from **one KBUS snapshot** |
| `GET /wda/parameters/{id}` | one parameter |
| `GET /wda/parameters/{id}/definition` | `301` → `/wda/parameter-definitions/{id}` |
| `GET /wda/parameter-definitions[/{id}]` | definitions: path, dataType, dataRank, writeable, userSetting |
| `PATCH /wda/parameters/{id}` | write one parameter → **204** |
| `PATCH /wda/parameters` | write **several** (batch, `data` is an array): all or nothing → **204** |
| `GET /wda/methods[/{id}]` | the two methods |
| `POST /wda/methods/{id}/runs` | run a method → **201** with the result (section 4) |

```bash
P=http://127.0.0.1:8000/wda/parameters          # on the controller (or --unix-socket /run/kbus-gateway/wdx.sock)
curl -g "$P?filter[id]=0-0-kbus-digitalinputvalues,0-0-kbus-analoginputvalues"
curl -X PATCH -H 'Content-Type: application/vnd.api+json' \
  -d '{"data":{"type":"parameters","id":"0-0-kbus-digitaloutputs-3-value","attributes":{"value":true}}}' \
  $P/0-0-kbus-digitaloutputs-3-value
curl -X PATCH -H 'Content-Type: application/vnd.api+json' \
  -d '{"data":[{"type":"parameters","id":"0-0-kbus-digitaloutputvalues","attributes":{"value":[true,false,true]}},
               {"type":"parameters","id":"0-0-registers-pump1-setpoint-value","attributes":{"value":3.5}}]}' $P
```

Rules for a PATCH:
- The body is JSON:API: `type` must be `parameters`, and `id` must match the URL.
- A value array may hold fewer values than channels (the rest stays as it is), never
  more.
- Numeric values may also be sent as `stringValue`.
- A rejected PATCH writes **nothing**.
- In a batch, all KBUS outputs are written under one lock, so they arrive in the same
  cycle. If an id appears twice, the last value wins.

### 3.4 Status and error codes

Every answer has `Content-Length`. Errors carry a JSON:API body:

```
{"errors":[{"status":"400","title":"Bad Request","detail":"...","code":"39","source":{"pointer":"/data/attributes/value"}}]}
```

| code | HTTP | Meaning |
|---|---|---|
| 17 | 404 | unknown id |
| 19 | 404 | a parameter id used as a method |
| 22 | 400 | method argument missing |
| 24 | 400 | wrong JSON type (e.g. `1` for a boolean) |
| 25 | 400 | wrong `stringValue` |
| 31 | 400 | read-only parameter |
| 36 | 400 | `null` cannot be written |
| 39 | 400 | value out of range, wrong length, too many values; Exchange: unknown position, wrong byte count |
| 41 | 400 | batch: not written because another value was rejected |
| 57 / 58 | 403 | KBUS writes not allowed (`HTTP_WRITE_ENABLE=0` or local control) / batch: another value was |
| 15 | 500 | value not available (AI status) |
| 21 | 500 | outputs could not be set; Exchange: no KBUS cycle within 100 ms |
| 26 | 500 | method could not run (register read failed, queue full) |

Other HTTP statuses:
- `405`: wrong method.
- `406`: the `Accept` header does not allow JSON:API.
- `415`: `Content-Type` is not `application/vnd.api+json`.

From the server:
- `408`: the request was not complete within `WDX_CLIENT_TIMEOUT_MS`.
- `413`: the request is larger than 16 KB.
- `503`: all `WDX_MAX_CLIENTS` slots are busy.

### 3.5 Connection handling

- **One request per connection** (HTTP/1.0). The server closes the connection after
  the answer.
- Up to `WDX_MAX_CLIENTS` (16) connections are served at the same time, one thread
  each.
- A client has `WDX_CLIENT_TIMEOUT_MS` (5 s) to send its complete request.
- A rescan waits for running requests. New requests wait for the rescan (under 1 s).
- Outputs keep their value until they are written again: there is no client
  watchdog. After a start or rescan all outputs are 0.

### 3.6 Differences from the WAGO REST-API manual

These are deliberate, for a local gateway:
- **Transport and access:** a Unix socket or plain HTTP; no TLS, so no `426`. There is
  no authentication: the socket permissions and `HTTP_BIND` control access.
- **Resources:** only parameters and two methods. There are no devices, method
  definitions, enums, instances, monitoring lists or File-API.
  `KBus/Rescan` / `ResetStatistics` are writable parameters instead of methods.
- **Extra filter:** `filter[id]`.
- **Method runs** are executed at once and not stored: `timeout` is 0, there is no
  `Location` header, and `GET .../runs/{id}` gives 404. `result-behavior=async` is
  refused.
- **AI status in a list:** a parameter list with an AI status that has no valid value
  shows an `error` attribute for that entry, instead of failing the whole list
  (4.4).

### 3.7 Live log and built-in web page

```bash
ssh root@192.168.1.17 'tail -f /var/log/messages | grep kbus-gateway'
```

`HTTP_LOG` sets what is logged:
- `0`: no requests.
- `1` (default): writes that **change** an output.
- `2`: every request.

The gateway also serves a small built-in live page at `http://127.0.0.1:8000/`
([web/index.html](../web/index.html)). It is reachable from a PC only with
`HTTP_BIND=0.0.0.0`; normally use the dashboard (section 7).

---

## 4. Complex modules: raw process data, AI status, register read

The gateway does not know the protocols of the 750-450, -495, -657, -671 and the
like. It passes the process data of complex modules on **raw**, byte by byte, in the
right order. The client (the CODESYS library WagoAppKbusWdx) runs the mailbox,
handshake and parameter channel itself, including register communication through
the process data (750-495 registers 31 and 57).

Which modules are raw modules:
- Modules **without channels**, with byte-aligned data, at most 64 bytes per
  direction. For these, `RawInput` / `RawOutput` exist.
- The gateway has no drivers for them. It writes their outputs only at start and
  rescan, setting them to 0.
- A driver added later (like the 671 stepper of the MQTT gateway) must refuse raw
  writes to its module (`400`, code 47). This is recorded in `src/gateway.h`.
- Two clients that write the same module overwrite each other: that is up to the
  application.

### 4.1 KBus/Exchange: write and read in one request

```
POST /wda/methods/0-0-kbus-exchange/runs?result-behavior=sync HTTP/1.0
Content-Type: application/vnd.api+json
Content-Length: <n>

{"data":{"type":"runs","attributes":{"inArgs":{"Positions":{"value":[6,7]},"Outputs":{"value":[0,0,0,0,0,0,0,0,0,0,0,1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5]}}}}}
```

| inArg | Type | Meaning |
|---|---|---|
| `Positions` | uint16[], 1–64 | KBUS positions (= *m* of `KBus/Modules/{m}`), in the order you want, each once |
| `Outputs` | uint8[] | their output bytes **concatenated** in that order, each module exactly its `OutputBytes`. **Missing or `[]` = read only** |

`result-behavior=sync` may be left out (`auto` also runs at once). inArgs may also
be sent as `stringValue` (`["6","7"]`).

Answer, captured on the PFC300 (read only, 750-650 at position 3):

```
HTTP/1.0 201 Created
Content-Type: application/vnd.api+json
Content-Length: 223
Cache-Control: no-cache
Connection: close

{"data":{"type":"runs","id":"0-0-kbus-exchange-705","attributes":{"executionStatus":"done","timeout":0,"outArgs":{"Cycle":{"value":205386},"OutputCycle":{"value":0},"Positions":{"value":[3]},"Inputs":{"value":[0,0,0,0]}}}}}
```

| outArg | JSON | Meaning |
|---|---|---|
| `Cycle` | number, uint64 | KBUS cycle in which `Inputs` were read. Keeps rising: not reset by Reset Statistics, only by a restart. Parse it as `ULINT`: at 2 ms it passes 2^32 after about 99 days. |
| `OutputCycle` | number, uint64 | the cycle that carried the written outputs; `0` = read only |
| `Positions` | number[] | as sent |
| `Inputs` | number[] | input bytes concatenated in the order of `Positions`, each module its `InputBytes` |

**Which cycle.**
- **With `Outputs`:** all output bytes of the request are written under one lock and
  reach the modules in the same cycle (`OutputCycle`). The answer holds the inputs of
  the cycle **after** it (`Cycle` ≥ `OutputCycle` + 1). A module that answers within
  one cycle (a 657 toggle bit, a 671 handshake bit) therefore shows its answer in the
  same response.
- **Read only:** the inputs of the next cycle that completes, so the data is never
  older than the request.
- **Waiting:** only the request itself waits. The KBUS thread never waits for HTTP.

**Errors.** On a 4xx nothing is written.

| Case | HTTP | code |
|---|---|---|
| unknown position, position twice, module with channels / not byte aligned, wrong total of output bytes, byte > 255 | 400 | 39 |
| `Positions` missing | 400 | 22 |
| `Positions` / `Outputs` not an array of numbers | 400 | 24 |
| `result-behavior=async` | 400 | – |
| wrong `Content-Type` | 415 | – |
| local control on, or `HTTP_WRITE_ENABLE=0` (only with `Outputs`; read only always works) | 403 | 57 |
| no completed KBUS cycle within **100 ms** (bus error, rescan) | 500 | 21 |

The `detail` text names the problem, for example
`inArgs.Outputs has 3 bytes; the modules need 4 (the sum of their OutputBytes), or none to only read`.

### 4.2 RawInput / RawOutput

- `GET /wda/parameters/0-0-kbus-modules-<m>-rawinput` returns the input bytes:
  `"value":[0,0,0,0],"stringValue":["0","0","0","0"]`.
- `PATCH .../0-0-kbus-modules-<m>-rawoutput` with exactly `OutputBytes` values
  writes the outputs → `204`; a wrong length → `400`/39.
- A `GET` of `RawOutput` returns the last written bytes.
- A PATCH gives no inputs back. For handshakes use `KBus/Exchange`.

### 4.3 Module list in one request

```
GET /wda/parameters?filter[path]=KBus/ModulePositions,KBus/ModuleNames,KBus/ModuleTypeCodes,KBus/ModuleInputBytes,KBus/ModuleOutputBytes,Gateway/Status,KBus/Healthy,KBus/ConfigurationVersion
```

The content is the same as `kbus/modules` of the MQTT gateway. Read it:
- at the start;
- whenever `KBus/ConfigurationVersion` changes.

`data` comes in the gateway's order: find the elements by `id`. For modules with
channels the byte counts are informative only; only modules with `RawInput` /
`RawOutput` (`ChannelCount` = 0) can be used in Exchange.

**Size:** 2257 bytes including headers for 3 modules. Each module adds about 38–52
bytes, so **from about 41 modules** the answer exceeds 4096 bytes (calculated from
measurements). Then split the request into two GETs.

### 4.4 AI status (register 6)

| Parameter | Type | Content |
|---|---|---|
| `0-0-kbus-analoginputstatusvalues` | uint16[] | register 6 of table *channel − 1* of every AI channel; 0 when not valid (750-450: `0x0060` = 96 = open input / ≥ 850 °C) |
| `0-0-kbus-analoginputstatusvalid` | boolean[] | `false` = not read yet, no register access, or `WDX_AI_STATUS_MS=0` |
| `0-0-kbus-analoginputs-<n>-status` | uint16 | one channel; without a valid value **500, code 15** |

How the status is read:
- A worker thread reads every channel once per `WDX_AI_STATUS_MS` (default and
  minimum 1000 ms, 0 = off). A GET returns the cached value.
- A module whose first read fails is skipped until the next scan.
- **Only modules with analog input channels are read, never modules without channels**
  (495, 657, 671 …). A firmware register access there would disturb the handshake
  that CODESYS runs through the raw data. `regcom_layout()` checks this explicitly.
- In a parameter list, a channel without a valid status gets an `error` attribute
  instead of `value`, and the rest of the list stays valid.

### 4.5 KBus/ReadRegister

Read only: the gateway has **no** function to write registers.

```
POST /wda/methods/0-0-kbus-readregister/runs?result-behavior=sync
{"data":{"type":"runs","attributes":{"inArgs":{"Position":{"value":8},"Table":{"value":2},"Register":{"value":6}}}}}

201  {"data":{"type":"runs","id":"0-0-kbus-readregister-3","attributes":{"executionStatus":"done","timeout":0,"outArgs":{"Value":{"value":96}}}}}
```

The inArgs are `Position` (an existing module), `Table` (0–255; for AI modules:
channel − 1) and `Register` (0–63). The outArg is `Value` (uint16, a number).

**A failed read still answers 201**, because the method has run.
`executionStatus` is then `"error"`, with `title`, `detail` and `code`. Captured on
the PFC300 (a digital module has no registers):

```
{"data":{"type":"runs","id":"0-0-kbus-readregister-704","attributes":{"executionStatus":"error","timeout":0,"title":"register read failed","detail":"module 1, table 0, register 8: read failed","code":"26"}}}
```

| `detail` ends with | code |
|---|---|
| `read failed` (no such table / register, digital module) | 26 |
| `bus not running` | 26 |
| `register access not available` (firmware or build without register API) | 16 |
| `register communication not running` | 16 |

HTTP errors:
- `400`/39: unknown position, `Table` > 255, `Register` > 63.
- `400`/22: argument missing.
- `500`/26: more than 16 reads waiting.

Each read takes about 20 ms in the firmware. Reads run one at a time, before the AI
status reads.

**Warning:** a firmware read on a module where CODESYS runs register communication
itself (750-495) can disturb it. Do not use it on such a module while its CODESYS
block is active.

### 4.6 Sizes and timings

**Sizes, for the CODESYS HTTP buffers** (send 1536, receive 4096 bytes including
headers). KBus/Exchange for 657 + 671 + 495 together (60 bytes out, 60 in, worst
case every byte 255):

| | Size incl. headers |
|---|---|
| Request (with `Host`, `Content-Type`, `Content-Length`) | 502 bytes (+ headers that the CODESYS block adds itself) |
| Answer (`Cycle` 11 digits ≈ 1 year at 2 ms) | 603 bytes |
| Answer, worst case (20-digit counters) | ≈ 640 bytes |
| Exchange, 1 module 4 bytes / ReadRegister | 351 / 269–336 bytes |

**Timings**, measured on the PFC300:
- Setup: curl on the controller to `127.0.0.1:8000`, a new connection per request,
  CODESYS in RUN, KBUS cycle 2 ms.
- No outputs were written.

| Request | n | Median | p95 | Max |
|---|---|---|---|---|
| `KBus/Exchange`, read only, 750-650 | 600 | 4.46 ms | 6.58 ms | 10.09 ms |
| `GET .../rawinput` | 600 | 3.08 ms | 4.52 ms | 7.97 ms |
| module-list GET | 600 | 3.70 ms | 5.50 ms | 7.42 ms |
| `KBus/ReadRegister` (digital module, fails at once) | 100 | 4.57 ms | 6.10 ms | 8.85 ms |

Exchange **with** outputs is expected to take up to one cycle more (≈ 5–7 ms). In
the simulation, with a 5 ms cycle, it measured a 10.0 ms median.

**Not yet tested with real modules:**
- 750-450, 750-495, 750-657 and 750-671. The raw data and the AI status were tested
  only in the simulation: a mirror module, the 671 model, and a 450 at 850 °C.
- A successful register read on real hardware. The firmware register API works in the
  container on the PFC300.

---

## 5. Serial interface COM1

The gateway forwards **COM1** (`/dev/serial`) transparently. It does not interpret
the bytes: the device protocol runs in the client. KBUS and COM1 run in the same
gateway; COM1 has its own receive thread.

- **RS-232 / RS-485:** set in the WBM (*Interfaces > Serial Interface*, owner
  **None**).
- **Settings:** written by the client, kept after a restart, shown (read only) on the
  dashboard (*Status > COM1*):

| Setting | Values | Default |
|---|---|---|
| `Serial/Com1/Baudrate` | 1200 … 230400 | 19200 (`SERIAL_BAUD`) |
| `Serial/Com1/Format` | data bits 5–8, parity N/E/O, stop bits 1/2, e.g. `"8E1"` | `8E1` (`SERIAL_FORMAT`) |
| `Serial/Com1/FrameGap` | 1–1000 ms: a pause this long ends a received frame | 10 (`SERIAL_FRAME_GAP_MS`) |

| Path | Type | |
|---|---|---|
| `Serial/Com1/Device`, `Open` | string, boolean | port and state |
| `Serial/Com1/Transmit` | string | PATCH = send these bytes |
| `Serial/Com1/TxCount`, `RxCount` | uint64 | bytes sent / received |
| `Serial/Com1/RxFrame`, `RxFrameCount` | string, uint64 | last frame and counter |
| `Serial/Com1/RxData` | string | last ≤ 512 received bytes (new = the last `RxCount` − old `RxCount`) |
| `Serial/Com1/Errors` | uint64 | framing, parity and overrun errors |

**Bytes as a string:** each character is one byte (U+0000–U+00FF). Printable ASCII
is sent as is; every other byte as `\u00XX`, e.g. `"\u0002AB\u0003"`.

**Timing:** a frame is available about *FrameGap + poll interval* after its last
byte. When several frames arrive between two polls, only the last one is in
`RxFrame`, but `RxData` holds all bytes. Pick the frame gap just above the longest
pause *inside* one answer of the device.

`SERIAL_DEVICE` empty switches COM1 off (default off in the simulation).
- `xOpen` / `Open = false`: COM1 is used by another program; set the WBM owner to
  *None*.
- `Errors` rising: wrong baud rate or format, or RS-485 A/B swapped.

---

## 6. Containers and registers

### 6.1 The containers

| Folder | Image | Role | Network |
|---|---|---|---|
| [docker/kbus-gateway](../docker/kbus-gateway) | `kbus-gateway:<version>-<arch>` | the gateway | host (`127.0.0.1:8000` only) |
| [docker/manager](../docker/manager) | `kbus-manager:<arch>` | dashboard (section 7) | `127.0.0.1:8090`, via the controller's lighttpd at `/kbus` |
| [docker/104-gateway](../docker/104-gateway) | `104-gateway:<arch>` | IEC 104 server (section 8) | port 2404 |
| [docker/example-client](../docker/example-client) | `kbus-example-client:<arch>` | example application | none |
| [docker/lib](../docker/lib) | – | `wdx_client.py`: Python client (`get`, `get_many`, `parameters`, `definition`, `set`, `set_many`), copied into the Python images | – |

Build contexts:
- `kbus-gateway/` builds from the project root.
- `104-gateway/` and `example-client/` build from `docker/`, for the shared library.
- `manager/` builds from its own folder.

`tools/deploy-docker.sh` does all of it (2.3).

### 6.2 Connect a container

Mount the socket **directory**, not the socket file (it is created again at every
start):

```bash
docker run -v /run/kbus-gateway:/run/kbus-gateway my-image
```

Access and network:
- The socket has mode `0660` (owner root). For non-root users set `WDX_SOCKET_GROUP`.
- No network is needed: the example client runs with `--network none`.
- Containers with `--network host` (CODESYS) can use TCP `127.0.0.1:8000` instead.

Minimal client, Python standard library only:

```python
import http.client, json, socket

class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__("localhost", timeout=2)
        self.path = path
    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.path)

def wdx(method, url, body=None):
    c = UnixConnection("/run/kbus-gateway/wdx.sock")
    c.request(method, url, json.dumps(body) if body else None,
              {"Content-Type": "application/vnd.api+json"} if body else {})
    r = c.getresponse(); data = r.read(); c.close()
    return r.status, (json.loads(data) if data else None)

P = "/wda/parameters"
st, d = wdx("GET", P + "?filter[id]=0-0-registers-changecounter,0-0-kbus-digitalinputvalues")
pid = "0-0-registers-pump1-setpoint-value"
wdx("PATCH", f"{P}/{pid}", {"data": {"type": "parameters", "id": pid, "attributes": {"value": 3.5}}})
```

The example client ([docker/example-client/example.py](../docker/example-client/example.py))
does three things:
- it reads the I/O and reports changes;
- it writes `Example/Heartbeat` and `Example/Status`;
- it answers texts written into `Codesys/Message` in `Python/Response` (`ping` →
  `pong`; switch this off with `CL_ENV="-e PYTHON_RESPONSE="`).

### 6.3 Registers

Registers carry data between clients. They never touch the KBUS. Declare them in
`/etc/kbus-gateway-registers.conf` and restart the gateway:

```
# <Name>         <type>   [default]  [persistent]
Pump1/Setpoint   float64  12.5       persistent
Line/Running     boolean
Batch/Number     uint32   0          persistent
Recipe/Name      string   ""         persistent
```

| Rule | |
|---|---|
| Name | segments of `A-Z a-z 0-9 _` separated by `/`, each starting with a letter, max 40 characters, unique (not case sensitive) |
| path / id | `Registers/Pump1/Setpoint/Value` / `0-0-registers-pump1-setpoint-value` |
| types | `boolean`, `int32`, `uint32`, `float64`, `string` (max 127 bytes UTF-8) |
| writes | always allowed (also with `HTTP_WRITE_ENABLE=0` and local control); the JSON type must match exactly |
| limit | 256 registers; invalid lines are logged and skipped |

**Persistent registers:**
- They are saved to `REGISTER_STORE` (`/home/kbus-gateway/registers.dat`) at most
  every `REGISTER_SAVE_S` (10 s) when they changed, and always at a normal stop.
- The file is replaced atomically, so a power failure loses at most the last 10 s.
- Do not write them in a fast loop: every change goes to flash.

**Notice changes:** poll `Registers/ChangeCounter` (+1 per write that changed
something) and read the registers only when it changed. For a consistent view,
read everything in one `GET ?filter[id]=...`.

**Ownership:** registers have no owner. Agree on one writer per register, and use a
batch PATCH for values that belong together.

---

## 7. Dashboard (kbus-manager)

A separate container makes the gateway manageable from a PC without opening the
gateway itself to the network:

```
 PC browser ──http(s)://<pfc-ip>/kbus──▶ lighttpd (WBM, 80/443) ──127.0.0.1:8090──▶ kbus-manager ──Unix socket──▶ kbus-gateway
```

```bash
tools/deploy-docker.sh 192.168.1.17 --manager --no-gateway     # only the dashboard
```

Open **`http://192.168.1.17/kbus`**. In the WBM's default *HTTPS only* mode this is
redirected to `https://…/kbus/` (self-signed certificate).

### 7.1 Pages

| Tab | Pages |
|---|---|
| Status | *COM1*: settings (read only) and a monitor with counters, last frame, received bytes, test send. *KBUS*: state, cycle time, counters, timing, control state, *Reset Statistics* 🔒, *Rescan KBUS*. *Modules*: name, type code, channels, bytes in / out, raw data |
| Parameters | *Control* panel with *Take / Release Local Control* 🔒; all parameters (path, id, type, value, writeable, persistent) with a filter. **Under local control** every writeable parameter gets an input field with *Set*; arrays as JSON, e.g. `[1,0,255]`. Without local control the page is read only |
| Settings | *KBUS*: cycle time 1–40 ms. *Login*: login on/off, user, password |

The page follows the WBM layout. Live values arrive as a stream (Server-Sent Events,
every 50 ms, only changes). Without the stream it polls every 900 ms.

### 7.2 Login

- Functions marked 🔒 and changing the login settings always need the login. The
  **gear button** at the top right opens it.
- Until a password is set, the login is **`admin` / `wago`**: change it on
  *Settings > Login*.
- *Require a login* closes the whole website.
- The settings are stored in `/home/kbus-manager/settings.json` (password as a PBKDF2
  hash).
- Forgotten password: delete that file and run `docker restart kbus-manager`.

### 7.3 Local control

*Parameters > Control > Take Local Control* (logged in):
- From then on **only the dashboard** may write the KBUS: outputs, raw outputs, cycle
  time, rescan.
- CODESYS and the other clients get `403` (code 57) for KBUS writes; they keep
  reading, and registers stay writable.
- The *Parameters* page now shows input fields.
- The LED *Local* is yellow. The state is kept after a restart, until *Release Local
  Control*.

How the gateway knows the dashboard:
- A second socket, `/run/kbus-gateway-control/control.sock` (directory 700, socket
  600), is mounted **only** into the gateway and the manager.
- Only requests on it may switch local control and reset the statistics, and write
  the KBUS while local control is on.

### 7.4 Settings

| Setting | |
|---|---|
| `MGMT_READONLY=1` | dashboard without writes: `MG_ENV="-e MGMT_READONLY=1" tools/deploy-docker.sh ... --manager --no-gateway` |
| `MG_PORT` / `MG_BIND` | port (8090) / `127.0.0.1` (only via `/kbus`) or `0.0.0.0` (also direct) |
| `MG_LIGHTTPD=0` | do not install the lighttpd config |
| `MGMT_PREFIX` | path prefix, default `/kbus` |

The lighttpd config (`/etc/lighttpd/apps.confd/kbus.conf`) is replaced by a firmware
update. The gateway container checks it at every start, before the gateway starts,
and repairs it; the result is in the log as `kbus-dashboard: ...`
(`KBUS_DASHBOARD_CHECK=0` switches the check off).

All data-layer requests also work through the dashboard, e.g.
`curl -kL https://192.168.1.17/kbus/wda/parameters/0-0-kbus-healthy` (with
`-u user:password` when the login is on).

---

## 8. IEC 60870-5-104 server (104-gateway)

The container `104-gateway` offers the KBUS digital inputs to a SCADA as an IEC 104 outstation. It
uses [c104](https://github.com/Fraunhofer-FIT-DIEN/iec104-python) (lib60870) and
reaches the gateway through the Unix socket.

```bash
tools/deploy-docker.sh 192.168.1.17 --104-gateway --no-gateway
```

- **Port and address:** port 2404, common address 1.
- **Points:** `DI1..DI4` → IOA 1..4, type 30 `M_SP_TB_1` (single point with
  CP56Time2a).

Behaviour:
- Every `poll_ms` (100) **one** request reads all inputs.
- A change is sent spontaneously (COT 3), and a general interrogation (100) returns
  the current values (COT 20).
- Quality is *Invalid* until the first read, while the gateway does not answer, and
  for channels that do not exist.

The parameter file is `/home/104-gateway/wdx_iec104_config.ini` on the controller.
It is created at the first deployment and kept afterwards. After a change run
`docker restart 104-gateway`.

| Section | Key | Default | |
|---|---|---|---|
| `[general]` | `log_level`, `poll_ms` | `INFO`, `100` | poll interval (min. 10) |
| `[wdx]` | `socket` / `host`, `port`, `timeout` | the socket / –, 8000, 1.0 | TCP instead of the socket with `host = 127.0.0.1` |
| `[iec104]` | `ip`, `port`, `max_connections`, `common_address`, `report_ms` | `0.0.0.0`, 2404, 0, 1, 0 | `report_ms` > 0 = also cyclic |
| `[points]` | `DI<n> = <IOA>` | `DI1 = 1` … | DI *n* over all DI modules, unique IOAs |

Test it with an IEC 104 client (e.g. QTester104):
1. Connect to `<pfc-ip>:2404` and send StartDT.
2. Send a general interrogation.
3. Switch DI1: IOA 1 arrives with COT 3.

Troubleshooting:
- **Port in use:** another IEC 104 server is running.
- **"DI5 not available":** the node has fewer DI channels than configured.
- **`.ini` changes have no effect:** they are read only at start-up; restart the
  container.

---

## 9. CODESYS as a client

The CODESYS runtime runs **without KBUS** in its device tree and uses the gateway
over HTTP on `127.0.0.1:8000` (CODESYS with the host network). The CODESYS project
`codesys/kbusgateway.project` uses the library **WagoAppKbusWdx**. That library is
maintained in CODESYS, not in this repository.

### 9.1 Rules for a client

1. **Send each request in one TCP write**, at most 16 KB, **one request per
   connection** (HTTP/1.0).
2. **The answer is complete when `Content-Length` body bytes have arrived.** There is
   no need to wait for the close.
3. **Size the buffers** for the real answer sizes (4.6, 4.3). What does not fit is
   cut off silently.
4. Use **one** instance that writes a given output or module. Two writers overwrite
   each other.
5. **Watch `KBus/ConfigurationVersion`:** after a rescan, read the channel counts and
   the module list again.
6. **Respect local control:** a `403` with code 57 means the dashboard has control.
   Keep reading; send the outputs again after the release.

### 9.2 The requests to use

| Purpose | Request |
|---|---|
| channel counts (start, after a rescan) | `GET /wda/parameters?filter[path]=KBus/ConfigurationVersion,KBus/DigitalInputs,KBus/DigitalOutputs,KBus/AnalogInputs,KBus/AnalogOutputs`: count the items of each `instantiations` value |
| cyclic read, one snapshot | `GET /wda/parameters?filter[path]=KBus/Healthy,KBus/ConfigurationVersion,KBus/LocalControl,KBus/DigitalInputValues,KBus/DigitalOutputValues,KBus/AnalogInputValues,KBus/AnalogOutputValues` |
| write DO and AO together | `PATCH /wda/parameters` with `0-0-kbus-digitaloutputvalues` and `0-0-kbus-analogoutputvalues` (batch, all or nothing), only when something changed |
| complex modules | `POST /wda/methods/0-0-kbus-exchange/runs` (4.1); module list (4.3) |
| AI status | `0-0-kbus-analoginputstatusvalues`, `...statusvalid` (4.4) |
| COM1 | `PATCH 0-0-serial-com1-transmit`; poll `RxFrameCount` / `RxFrame` or `RxCount` / `RxData` (section 5) |

### 9.3 Timing

- **Task:** call the I/O in a cyclic task of 1–2 ms that runs **only** the I/O
  program. Each HTTP request needs one to three task cycles.
- **Expected times:** with a 5 ms exchange interval, a read-only exchange takes
  2–3 ms and an exchange with a write 4–5 ms.
- **Outliers:** expect around 10 ms now and then, when the CODESYS real-time threads
  load the CPU.
- **Hard real time:** HTTP is not hard real-time. For hard real-time I/O, put the
  KBUS in the CODESYS device tree instead of using the gateway.

---

## 10. Configuration

Environment variables of the gateway:
- **Native package:** in `/etc/kbus-gateway.conf`, then run `/etc/init.d/kbus-gateway
  restart`.
- **Container:** via `GW_ENV="-e NAME=value"` (2.3).

| Variable | Default | Description |
|---|---|---|
| `WDX_SOCKET` | `/run/kbus-gateway/wdx.sock` | Unix socket; empty = off |
| `WDX_SOCKET_MODE`, `WDX_SOCKET_GROUP` | `0660`, – | permissions of the socket |
| `CONTROL_SOCKET` | `/run/kbus-gateway-control/control.sock` | dashboard socket (7.3); empty = off |
| `HTTP_ENABLE` | `1` | `0` = no TCP listener |
| `HTTP_BIND` | `127.0.0.1` | `0.0.0.0` = all networks (see section 14) |
| `HTTP_PORT` | `8000` | not 80/443 (WBM) |
| `HTTP_WRITE_ENABLE` | `1` | `0` = KBUS writes refused (403); registers stay writable |
| `HTTP_LOG` | `1` | 0 none, 1 changing writes, 2 every request |
| `HTTP_PRIORITY` | `0` (package and container: `30`) | SCHED_FIFO priority of the server, below `KBUS_PRIORITY` |
| `WDX_MAX_CLIENTS` | `16` | connections at the same time, 1–64 |
| `WDX_CLIENT_TIMEOUT_MS` | `5000` | time to send a request |
| `WDX_EVENT_MS`, `WDX_MAX_STREAMS` | `10`, `4` | `GET /events` (live values for the built-in page) |
| `WDX_AI_STATUS_MS` | `1000` | AI status read interval, min. 1000, `0` = off (4.4) |
| `REGISTER_FILE` | `/etc/kbus-gateway-registers.conf` | register declarations |
| `REGISTER_STORE`, `REGISTER_SAVE_S` | `/home/kbus-gateway/registers.dat`, `10` | persistent values |
| `KBUS_CYCLE_MS` | `5` | KBUS cycle 1–40 ms; a value written to `KBus/CycleTime` wins |
| `KBUS_SETTINGS_STORE` | `/home/kbus-gateway/kbus-settings.conf` | stored cycle time, local control, COM1 settings |
| `KBUS_PRIORITY` | `40` | SCHED_FIFO priority of the cycle thread |
| `SERIAL_DEVICE`, `SERIAL_BAUD`, `SERIAL_FORMAT`, `SERIAL_FRAME_GAP_MS` | `/dev/serial`, `19200`, `8E1`, `10` | COM1 (section 5) |
| `LOG_TARGET` | stderr (package / container: `syslog`) | `syslog` = `/var/log/messages` |
| `KBUS_SIMULATION` | `0` | `1` = simulated node (13.2) |
| `KBUS_SIM_HOTPLUG`, `KBUS_SIM_450` | –, `0` | simulation: hot-plug file; `1` = add a 750-450 |
| `KBUS_DAL_LIB`, `KBUS_INFO_LIB` | | `dlopen()` candidates |
| `KBUS_DIGITAL_OFFSET_MODE` | `auto` | `auto`, `relative`, `absolute` (1.1) |

---

## 11. Speed and determinism

**KBUS cycle.** The default is 5 ms; the test PFC300 runs 2 ms (stored). Inputs are
at most one cycle old, and a written output reaches the module within one cycle.
Check the cycle on the real node:

```bash
S="--unix-socket /run/kbus-gateway/wdx.sock"
curl -sg $S "http://x/wda/parameters?filter[id]=0-0-kbus-maxcycleduration,0-0-kbus-maxwakeupdelay,0-0-kbus-overruns,0-0-kbus-errorcount"
```

| Value | Fine | When exceeded |
|---|---|---|
| `MaxCycleDuration` | well below the cycle time | raise `KBus/CycleTime` |
| `MaxWakeupDelay` | a few 100 µs | other real-time threads (CODESYS) delay the cycle |
| `Overruns` | 0 or very rare | cycles started late |
| `ErrorCount` | not increasing | KBUS errors |

Reset the counters with *Reset Statistics* on the dashboard. The 50 ms output
watchdog of the modules leaves a large margin.

**Request times** on the PFC300 (curl on the controller, a new connection per
request, CODESYS running):

| Request | Median | p95 |
|---|---|---|
| one GET | 3.4 ms | 5.0 ms |
| one PATCH | 2.9 ms | 4.3 ms |
| `KBus/Exchange` read only | 4.5 ms | 6.6 ms |

The Unix socket saves the TCP setup (≈ 1.3 ms median). On a PC the gateway itself
needs < 0.1 ms per request.

**Outliers of about 10 ms** come from the CODESYS runtime. It runs real-time threads
at priorities 50–83, above the KBUS cycle (40) and the server (30). This is
deliberate: raising the gateway above them would disturb the PLC program.

---

## 12. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `opening the KBUS device failed` | another KBUS master: runtime not *None*, a CODESYS project with KBUS, or a second gateway (native **and** container) |
| `cannot listen on 127.0.0.1:8000: Address in use` | a second gateway or another program on the port |
| Container: `No such file or directory` / `Connection refused` on the socket | mount the **directory** `/run/kbus-gateway`; the gateway must run |
| Container: `Permission denied` on the socket | user not root and not in `WDX_SOCKET_GROUP` |
| `503` answers | more than `WDX_MAX_CLIENTS` connections at the same time |
| `403`, code 57 | `HTTP_WRITE_ENABLE=0`, or local control is on in the dashboard |
| `400`, code 39 | more values than channels, value out of range; Exchange: wrong byte count or module |
| Exchange `500`, code 21 | no KBUS cycle within 100 ms: bus error or rescan, check the log |
| AI status stays invalid | no AI module with register access, `WDX_AI_STATUS_MS=0`, or built without `ldkc_kbus_register_communication.h` |
| `/kbus` gives 404 after a firmware update | repaired at the next start of the gateway container; at once: `docker restart kbus-gateway` |
| Dashboard: "gateway not reachable" | gateway not running, or the socket directory is not mounted into the manager |
| Dashboard: no input fields on *Parameters* | log in and take local control (7.3) |
| `registers: ... line ignored` | syntax error in the register file (6.3) |
| Persistent registers back at default | name, type or `persistent` changed, or `REGISTER_STORE` not writable |
| `KBus/Overruns` rising | cycle time too short for the node |
| Cycle time ignores `KBUS_CYCLE_MS` | a stored value from `KBus/CycleTime` wins: delete `/home/kbus-gateway/kbus-settings.conf` and restart |
| `could not load the ADI/DAL library` | check `ls /usr/lib/libdal*`, set `KBUS_DAL_LIB` |
| `built WITHOUT the WAGO ADI/DAL headers` | import the headers (2.2) and rebuild |
| `version 'GLIBC_2.xx' not found` | built with a newer toolchain than the firmware (native package) |
| Channel numbers point to the wrong I/O | compare the startup module list with the node; try `KBUS_DIGITAL_OFFSET_MODE` |
| New modules not visible | look for `KBUS rescan` in the log; *Rescan KBUS* or `PATCH KBus/Rescan = true` |
| `KBUS rescan failed; next attempt in … s` | the node cannot be initialised (defective module, end module missing) |
| COM1 `Open = false` | COM1 used by another program: WBM owner *None* |

Investigate further with `strace -f -e trace=%file -e status=failed /usr/bin/kbus-gateway`
(stop the service first): it shows which files the WAGO libraries miss.

---

## 13. Development

### 13.1 Project layout

```
├── README.md, LICENSE, VERSION, Makefile
├── src/                     the program in C
├── web/index.html           built-in web page (embedded with src/web_asset.S)
├── sdk/include/             WAGO headers (imported, not in git) + OsCommon.h
├── packaging/ipk/           native package: init script, config and register files, opkg scripts
├── docker/                  kbus-gateway, manager, 104-gateway, example-client, lib/wdx_client.py
├── codesys/                 CODESYS project (uses the library WagoAppKbusWdx)
├── tools/                   build-ipk.sh, deploy.sh, deploy-docker.sh, import-sdk-*.sh
├── tests/                   test-rest.py, test-registers.conf
└── docs/MANUAL.md           this manual
generated, not in git: build/, build-ipk/, build-docker/
```

| Source | Role |
|---|---|
| `src/main.c` | startup order, signals, memory locking |
| `src/config.c/.h` | environment variables → `gw_config_t`, stored settings |
| `src/backend_adi.c` | real KBUS: `dlopen` of `libdal`, ADI scan, KBUS-info, cycle, register reads |
| `src/backend_sim.c` | simulated node |
| `src/gateway.c/.h` | channel map, process images, cycle thread, raw module data, `gateway_exchange()` (cycle counters), register reads serialised with rescans |
| `src/regcom.c/.h` | register worker: AI status, KBus/ReadRegister |
| `src/http_server.c/.h` | sockets, one thread per client, time-outs, rescans between requests |
| `src/wda.c/.h` | wdx data layer: parameter table, JSON:API, PATCH, methods |
| `src/registers.c/.h` | register provider and persistence |
| `src/serial.c/.h` | COM1 |
| `src/log.c/.h` | stderr or syslog |

### 13.2 Simulation and tests (PC, no PFC needed)

The simulated node:
- **Positions 1–5:** 750-455 (4 AI), 750-559 (4 AO), 8DI, 8DO, 4DI. That gives 12 DI,
  8 DO, 4 AI and 4 AO. DI1–8 follow DO1–8, and AI4 follows AO1.
- **Position 6:** a 750-671 model.
- **Position 7:** a 24-byte "657" whose inputs are its outputs of the previous cycle.
- **Position 8:** a 750-450 only with `KBUS_SIM_450=1`.
- **Register reads:** take 20 ms each.

```bash
make                                           # simulation-only build when sdk/include/dal is missing
make run-sim                                   # simulated node on http://localhost:8000
tests/test-rest.py 127.0.0.1 wda               # all requests, PATCH, error codes
```

| Test mode | Gateway started with |
|---|---|
| `wda`, `node`, `clients --max-clients N` | `KBUS_SIMULATION=1 WDX_CLIENT_TIMEOUT_MS=1000` (`clients`: `WDX_MAX_CLIENTS=N`) |
| `registers` | `REGISTER_FILE=tests/test-registers.conf`, empty `REGISTER_STORE` |
| `control --control PATH` | `CONTROL_SOCKET=PATH` (short path: < 108 characters) |
| `forbidden` | `HTTP_WRITE_ENABLE=0` |
| `raw` | `KBUS_SIM_450=1`: Exchange (order, cycle counters, several modules, errors), raw data, AI status, ReadRegister |
| `aioff` | `WDX_AI_STATUS_MS=0` |
| `node` / `write --do N` on a real PFC | read only / switches a real output |

The VS Code tasks (*Terminal → Run Task*) cover the same steps.

### 13.3 Common changes

| Change | Where |
|---|---|
| a parameter | `build_list()`, `path_of()`, `dtype_of()`, `write_value()` in `src/wda.c`; ids follow from the paths |
| a method | `methods[]` and `do_methods()` in `src/wda.c` |
| a module type | `build_channel_map()` in `src/gateway.c` |
| a register type | `src/registers.c`, `dtype_of()` in `src/wda.c` |
| a release | raise `VERSION`, then deploy (2.3 / 2.4) |

### 13.4 Rules to keep

- **Cycle thread:** never do slow work (I/O, logging, allocation, register reads) in
  it. Every cycle must finish well within 50 ms.
- **Priorities:** keep `HTTP_PRIORITY` below `KBUS_PRIORITY`.
- **Access functions:** use only the `gateway_*` and `registers_*` functions; they
  handle the locking.
- **Threads:** `wda_handle()` runs in several threads at once, so no static buffers in
  the request path.
- **Headers:** build against headers with the same DAL version as the PFC's
  `libdal.so`.
- **Toolchain:** build the native package with the firmware's SDK toolchain.

---

## 14. Limitations and security

- **No authentication on the data layer.** Access is controlled by the socket
  permissions and `HTTP_BIND` (default `127.0.0.1`: only programs on the
  controller). Every container with the socket mounted can read everything, switch
  outputs and write registers.
- **`HTTP_BIND=0.0.0.0`.** Anyone who can reach port 8000 can do the same. Use it
  only on an isolated network, set `HTTP_WRITE_ENABLE=0` when outputs need not be
  written, and block the port in the firewall.
- **Dashboard login.** With *Require a login* off, everyone on the network can use
  the dashboard; local control, Reset Statistics and the login settings still need
  the login. The login is HTTP Basic, encrypted only over `https://`.
- **Only one KBUS master.** The gateway and a CODESYS application with KBUS cannot
  run at the same time.
- **Values and modules.** Values are raw, with no unit scaling. The gateway has no
  module drivers: complex modules are handled by the client through raw data.
- **Changes of the node** are detected through KBUS errors. A change without errors
  needs `KBus/Rescan`.
- **Registers** have no owner.
- **Stop and crash.** On stop and removal all outputs go to 0. After a crash the
  modules switch off after 50 ms.
- **Tested hardware.**
  - On the PFC300: 8DI, 8DO and 750-650 (raw input read only).
  - In the simulation only: analog modules, 750-450, -495, -657 and -671.
  - Not installed yet: the `armhf` package for PFC100/PFC200.
- **WAGO's ParameterService.** The gateway does not register with `paramd`: its
  parameters do not appear in the firmware's own REST-API at `https://<pfc>/wda`.

---

## 15. Background and history

**Why not WAGO's own WDx?** The firmware runs WAGO's ParameterService (`paramd`)
with a provider socket, but the headers for writing a provider are not in the SDK.
The gateway follows the same pattern instead: a data layer, a KBUS provider and a
register provider, with client access over IPC. Its providers can move to `paramd`
once that interface is available.

The KBUS access follows the WAGO example *HowTo_ADI-MyKBusApplikation* (MIT): the
ADI/DAL device scan, `libpackbus`, one `libpackbus_Push` per cycle, and the module
list through KBUS-info.

| Version | Change |
|---|---|
| 1.x | replacement of a CODESYS REST server (`GET /DI/1`); first as a container, then a native `.ipk`; bulk requests |
| 2.0.0 | the wdx data layer is the only interface; built-in web page |
| 2.1.0 | dynamic configuration: automatic rescan, `KBus/ConfigurationVersion`, `KBus/Rescan` |
| 2.2.0 | containers and registers: Unix socket, several clients, registers with persistence, batch PATCH, 5 ms cycle with timing parameters |
| 2.3.0 | writable cycle time; dashboard container (login); IEC 104 container (104-gateway, called kbus-iec104 until 2.6.0); shared `wdx_client.py` |
| 2.4.0 | conformity with the WAGO REST-API manual (filters, paging links, error codes); dashboard at `/kbus`; local control |
| 2.5.0 | COM1 over the data layer; live dashboard; COM1 settings from the client |
| 2.6.0 | raw process data of complex modules (`KBus/Exchange`, `RawInput` / `RawOutput`), module list arrays, gateway status, AI status, `KBus/ReadRegister`; dashboard with editing on the *Parameters* page under local control. The register communication, `regcom.c`, the raw module functions and the 671 simulation were taken from the MQTT gateway 3.4.0 |

Problems solved on the way:

| Problem | Fix |
|---|---|
| `RTLD_DEFAULT undeclared` with the WAGO toolchain | `#define _GNU_SOURCE` in `backend_adi.c` |
| own log lines appeared as `kbus:` | WAGO's library calls `openlog("kbus")`; the gateway sets its own ident for every message |
| log flooded by cyclic writes | `HTTP_LOG=1` logs only writes that change an output |
| opkg unlink error on upgrade | archives without the root entry `./` |
| wdx id lookup slow | id table built once per configuration |
