# KBUS HTTP Gateway

A small native application for WAGO PFC100 / PFC200 / **PFC300** controllers
that reads and writes the channels of the **DI / DO / AI / AO** modules on the
KBUS and presents them as a **wdx data layer** over HTTP. It is installed as a
normal firmware package (`.ipk`) on the PTXdist firmware, starts at boot and
runs the KBUS cycle with real-time priority. No Docker is needed on the
controller.

The wdx data layer is the **only interface** (port 8000). It follows the WAGO
wdx / WDA conventions: paths, ids, data types, GET reads, PATCH writes,
JSON:API.

```
GET   http://<pfc-ip>:8000/wda/parameters                                   all parameters
GET   http://<pfc-ip>:8000/wda/parameters/0-0-kbus-digitalinputs-1-value    one input
GET   http://<pfc-ip>:8000/wda/parameters?filter[id]=0-0-kbus-digitalinputvalues,0-0-kbus-analoginputvalues
PATCH http://<pfc-ip>:8000/wda/parameters/0-0-kbus-digitaloutputs-3-value   {"data":{...,"attributes":{"value":true}}}
```

**Web page:** open `http://<pfc-ip>:8000/` in a browser for a live view of all
inputs and outputs, with buttons to switch the outputs (3.5). The page itself
uses only the wdx data layer.

A CODESYS application that reads and controls the KBUS through the data layer
is in [codesys-wdx/](codesys-wdx/README.md).

The KBUS access is based on the WAGO example *HowTo_ADI-MyKBusApplikation* in
[`Information/`](Information/HowTo_ADI-MyKBusApplikation/README.md) (same ADI/DAL
calls as `kbusdemo.c`, same module discovery as `getkbusinfo.c`).

Tested on a **PFC300 750-8302, firmware 04.08.09(30)** with a real KBUS (one
8DO module), used by a CODESYS application at the same time.

---

## Contents

1. [How it works](#1-how-it-works)
2. [Installation](#2-installation)
3. [Using the gateway: the wdx data layer](#3-using-the-gateway-the-wdx-data-layer)
4. [Configuration](#4-configuration)
5. [Speed and determinism](#5-speed-and-determinism)
6. [How it was built](#6-how-it-was-built)
7. [Modifying the application](#7-modifying-the-application)
8. [Troubleshooting](#8-troubleshooting)
9. [Limitations and security](#9-limitations-and-security)

---

## 1. How it works

```
 CODESYS / browser / SCADA / script                     WAGO PFC (PTXdist firmware)
 ──────────────────────────────────          ┌───────────────────────────────────────────────┐
                                             │ /usr/bin/kbus-gateway  (started by init.d)    │
  GET   /wda/parameters/...  ────────────────┼─▶ HTTP server thread  (port 8000)             │
  PATCH /wda/parameters/...                  │     wdx data layer (JSON:API)                 │
                                             │        │ read / write (mutex)                 │
                                             │        ▼                                      │
                                             │   channel map + process images in memory     │
                                             │        ▲                                      │
                                             │        │ every 10 ms, SCHED_FIFO prio 40      │
                                             │   KBUS cycle thread                           │
                                             │    write outputs → libpackbus_Push → read in  │
                                             │        │ dlopen() of the firmware's own       │
                                             │        ▼ /usr/lib/libdal.so, KBUS-info (D-Bus)│
                                             └────────┼──────────────────────────────────────┘
                                                      ▼
                                              KBUS ──▶ I/O modules
```

**Startup**

1. The WAGO libraries (`libdal.so`, `libdbuskbuscommon.so` and the DAL plugins
   in `/usr/lib/dal`) are loaded with `dlopen()` from the firmware's own
   `/usr/lib`. They are not linked into the program, so it always uses the
   libraries of the installed firmware; the build only needs their headers.
2. The KBUS device (`libpackbus`) is opened and set to application state
   *Unconfigured*. The modules are read through the KBUS-info D-Bus
   interface, as in `getkbusinfo.c`: position, type, bit offsets and sizes,
   and number of channels.
3. A **channel map** is built. Channels are
   numbered from 1, separately for each type, in module order from left to
   right. At most 250 modules and 250 channels per type.
   - **Digital module** (type code bit 15 set): every input bit is a `DI` and
     every output bit is a `DO`. Only channels 1–16 of a module exchange
     data; a higher channel still takes a number but reads `FALSE` (DI) or
     only keeps the written value (DO).
   - **Analog module** with only input data → `AI`, with only output data →
     `AO`. Channel *k* is the unsigned 16-bit word at byte offset 2·(k−1) of
     the module data (at most 48 bytes), raw value.
   - **750-495** gets no channels; its measured values are not available.
   - **Other modules** (data in both directions, e.g. counters or serial
     interfaces) get no channels; their outputs stay 0.
4. The KBUS is switched to application state *Running*. From then on the
   gateway drives every bus cycle itself, as in `kbusdemo.c`.
5. The HTTP server starts (optionally with real-time priority, see
   `HTTP_PRIORITY`) and builds the parameter table of the wdx data layer once
   (ids precomputed, so every lookup is cheap).

**KBUS cycle thread.** Every `KBUS_CYCLE_MS` (default 10 ms) a real-time
thread (`SCHED_FIFO`, priority 40) copies the output image, triggers one cycle
with `libpackbus_Push` and stores the input image. WAGO output modules switch
their outputs off when no cycle arrives within **50 ms**, so this thread never
waits for the HTTP server: the shared images are locked only for the copy,
not during the bus cycle.

**HTTP requests** only touch the in-memory process images:

- Reading an input returns the value of the last bus cycle.
- Writing an output changes the output image; the next cycle (≤ 10 ms later)
  sends it to the module. All values of one array write are applied under one
  lock, so they reach the modules in the same cycle.
- Reading an output returns the value that was last commanded.

**Shutdown.** `/etc/init.d/kbus-gateway stop` (also at shutdown) sends
SIGTERM. The gateway sets all outputs to 0, runs one last cycle and closes the
KBUS device. If the program crashes, the output modules switch off on their
own after 50 ms without a cycle.

**Digital offset.** In the process image the digital data sits behind the
analog data. The gateway asks libpackbus where the digital area starts
(`libpackbus_get_dig_offset`). If the KBUS-info offsets of digital modules
are relative to that area, they are shifted; this is detected automatically
and logged at startup. See `KBUS_DIGITAL_OFFSET_MODE`.

**Package on the controller**

| File | Purpose |
|---|---|
| `/usr/bin/kbus-gateway` | the program (about 56 KB) |
| `/etc/init.d/kbus-gateway` | `start`, `stop`, `restart`, `status` |
| `/etc/rc.d/S99_kbus-gateway` | boot link, created by the package |
| `/etc/kbus-gateway.conf` | settings (section 4); kept on package updates when you changed it |
| `/var/log/messages` | log (syslog, `kbus-gateway[pid]`) |

---

## 2. Installation

Commands marked *PC* run in the project directory on a Linux PC; commands
marked *PFC* run on the controller (`ssh root@<pfc-ip>`). The examples use
`192.168.1.17`.

Quick route once the PC is set up (2.1):

```bash
tools/deploy.sh 192.168.1.17               # PC: build .ipk, copy, opkg install, show status
tools/test-rest.py 192.168.1.17 node       # PC: read-only check
```

### 2.1 Requirements on the PC

| What | Why |
|---|---|
| Linux PC with **Docker** | only to run WAGO's SDK build image with the firmware toolchain (the controller needs no Docker) |
| The **WAGO firmware SDK** of your controller, e.g. [pfc300-firmware-sdk](https://github.com/WAGO/pfc300-firmware-sdk), with its builder image: `make builder` → `pfc300-builder:latest` | toolchain with the same GCC and glibc as the firmware |
| The **WAGO SDK headers** in `sdk/include` (2.3) | ADI/DAL and KBUS-info API; proprietary, not in this repository |
| Python 3 | only for the test script |
| SSH/scp access to the PFC as `root` | install |

### 2.2 Prepare the PFC

Only **one KBUS master** is allowed. Either set WBM → *PLC Runtime* → runtime
version **None**, or run the CODESYS runtime **without KBUS** in its project
(for example CODESYS in a container with the KBUS drivers switched off, see
[docker-codesys3-wagopfc](https://github.com/pfcdiederick/docker-codesys3-wagopfc)).

Check the controller once (*PC*):

```bash
ssh root@192.168.1.17 'uname -m; cat /etc/REVISIONS; ls -l /usr/lib/libdal.so*; /lib/libc.so.6 | head -1; opkg list-installed | grep -E "^(dal|libdbuskbuscommon) "'
```

PFC300 FW 04.08.09(30): `aarch64`, `libdal.so.1.0.1`, glibc 2.35, packages
`dal 1.0.1` and `libdbuskbuscommon 1.0.0.0` (the package depends on both).

### 2.3 Get the SDK header files

The real KBUS backend is compiled against the ADI/DAL and KBUS-info **header
files** of the WAGO SDK. They carry a WAGO "proprietary rights" notice and are
therefore **not in this repository**; import them once:

**A. From the prebuilt packages of the SDK (no SDK build needed).** Every
WAGO firmware SDK checkout contains `dal_<version>_<arch>.tgz` and
`libdbuskbuscommon_<version>_<arch>.tgz` in `ptxproj/configs/<platform>/packages/`:

```bash
tools/import-sdk-packages.sh ~/Documents/WagoFirmware/pfc300-firmware-sdk/ptxproj/configs/wago-pfc300/packages
```

This fills `sdk/include/` with `dal/*` and `ldkc_kbus_information.h`. 
`sdk/include/OsCommon.h` is our own minimal stand-in (types and attribute
macros only) and is in the repository.

**B. From a completely built SDK** (`platform-wago-pfc*/sysroot-target/usr/include/dal/` exists):

```bash
tools/import-sdk-headers.sh ~/wago/ptxproj
```

What must match is the **DAL API version**: the script prints the DAL version
of the package; it must equal `libdal.so.<version>` on the PFC (2.2). The
headers have no architecture-specific parts: the PFC300 SDK headers (`dal
1.0.1`) also fit a PFC200 with FW31 (`libdal.so.1.0.1`).

Without `sdk/include/dal` only the **simulation** build works (`make`), which
is enough to develop and test the wdx data layer (7.2).

### 2.4 Build the package

*PC*:

```bash
tools/build-ipk.sh arm64        # PFC300        -> build-ipk/kbus-gateway_<version>_arm64.ipk (~28 KB)
tools/build-ipk.sh armhf        # PFC100/PFC200 -> build-ipk/kbus-gateway_<version>_armhf.ipk (untested)
```

What the script does (details in [6.3](#63-the-native-package)):

1. Runs `make` inside the SDK builder image with WAGO's cross compiler
   (`aarch64-linux-gnu-gcc` 11.3 of `gcc-Toolchain-2022.08-wago.1`) and
   `-Werror`, then strips the binary.
2. Builds the package tree: program, init script, config file, `postinst`,
   `prerm`, `conffiles`, `control` (version from [VERSION](VERSION)).
3. Packs it like a PTXdist package: an `ar` archive with `debian-binary`,
   `control.tar.gz` and `data.tar.gz`.

`BUILDER_IMAGE=...` selects another SDK image.

### 2.5 Install, update, remove

**Install** — WBM → *Software Uploads* → upload the `.ipk` → *Install*, or (*PC*):

```bash
scp build-ipk/kbus-gateway_1.1.3_arm64.ipk root@192.168.1.17:/tmp/
ssh root@192.168.1.17 'opkg install /tmp/kbus-gateway_1.1.3_arm64.ipk && rm /tmp/kbus-gateway_1.1.3_arm64.ipk'
```

`opkg` installs the files, the `postinst` script creates the boot link and
(re)starts the gateway. `tools/deploy.sh <pfc-ip>` does build, copy, install
and shows the status and log in one go.

**Update** — install a newer `.ipk` the same way (raise [VERSION](VERSION)
first). `prerm` stops the old version, `postinst` starts the new one. Your
`/etc/kbus-gateway.conf` is kept if you changed it.

**Operate** (*PFC*):

```bash
/etc/init.d/kbus-gateway status|stop|start|restart
grep kbus-gateway /var/log/messages | tail -20
```

**Remove** — `opkg remove kbus-gateway` (stops it, removes files and boot link;
all outputs go to 0).

### 2.6 Check the start

*PFC*: `grep kbus-gateway /var/log/messages | tail -20`. A good start on the
PFC300 with one 8DO module:

```
kbus-gateway[21566]: WAGO KBUS HTTP gateway starting
kbus-gateway[21566]: config: rest=on port=8000 writes=allowed log=writes http-prio=30 cycle=10ms rt-prio=40 mode=kbus
kbus-gateway[21566]: loaded libdal.so
kbus-gateway[21566]: loaded libdbuskbuscommon.so
kbus-gateway[21566]: [INFO] --- Start KBUS initialization. ---       (this and the next lines tagged "kbus:" come from WAGO's KBUS library)
kbus-gateway[21566]: KBUS device opened
kbus-gateway[21566]: KBUS: 1 modules, error code 0, bits AI=0 AO=0 DI=0 DO=8, image in=12000 out=12000 bytes
kbus-gateway[21566]: application state set to 'Running'
kbus-gateway[21566]: module  1: digital 8DO    in   0 bits @0     out   8 bits @0
kbus-gateway[21566]: channels: 0 DI, 8 DO, 0 AI, 0 AO; 0 x 750-495
kbus-gateway[21566]: KBUS cycle thread running: SCHED_FIFO priority 40, every 10 ms
kbus-gateway[21566]: HTTP server thread: SCHED_FIFO priority 30
kbus-gateway[21566]: wdx data layer listening on port 8000 (GET /wda/parameters)
```

### 2.7 Test

```bash
tools/test-rest.py 192.168.1.17 node              # real node, READ ONLY: parameter tree, values, invalid requests
tools/test-rest.py 192.168.1.17 write --do 1      # real node: switches DO1 on/off with pauses (check what is connected!)
curl -g "http://192.168.1.17:8000/wda/parameters?page[limit]=5"
```

Simulation tests (all writes allowed) are in 7.2.

---

## 3. Using the gateway: the wdx data layer

### 3.1 Parameters

All KBUS data is presented as **parameters following the WAGO wdx / WDA
conventions** (see the training in `Information/WDX/`): one flat list,
JSON:API 1.0, **GET reads, PATCH writes one parameter**.

| wdx rule | KBUS data |
|---|---|
| **path** = hierarchical name | `KBus/DigitalOutputs/3/Value` |
| **id** = `0-0-` + lower-case path, `/` → `-` | `0-0-kbus-digitaloutputs-3-value` |
| **dataType** | DI/DO `boolean`, AI/AO `uint16` (raw), counters `uint64`, names `string`, lists `instantiations` |
| **dataRank** | `scalar` per channel, `array` for a whole class |
| write rights | `GET .../{id}/definition` → `"writeable": true` only for DO/AO values |
| persistence | none: process values are volatile |

Parameter tree:

```
KBus/Healthy                      boolean   R    KBUS cycle running without errors
KBus/CycleTime                    uint16    R    ms
KBus/CycleCount, ErrorCount       uint64    R
KBus/Modules                      instantiations [1..n]
KBus/Modules/{m}/Name             string    R    e.g. "digital 8DO", "75x-559"
KBus/Modules/{m}/TypeCode         uint16    R    raw KBUS terminal code
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

Channels are numbered per class from 1, in KBUS order (left to right). Values
are **raw process-image values**: for a 750-559 (0–10 V), 32767 = 10 V. Scale
them in your application.

### 3.2 Requests

| Request | Answer |
|---|---|
| `GET /wda/parameters` | all parameters (`page[limit]`, `page[offset]`) |
| `GET /wda/parameters?filter[id]=id1,id2` | exactly these parameters, from **one KBUS snapshot** |
| `GET /wda/parameters/{id}` | one parameter |
| `GET /wda/parameters/{id}/definition` | path, dataType, dataRank, writeable |
| `PATCH /wda/parameters/{id}` | write one parameter → **204 No Content** |

```bash
P=http://192.168.1.17:8000/wda/parameters
curl -g "$P?page[limit]=5"
curl -g "$P?filter[id]=0-0-kbus-digitalinputvalues,0-0-kbus-digitaloutputvalues"
curl $P/0-0-kbus-digitaloutputs-3-value/definition
curl -X PATCH -H 'Content-Type: application/vnd.api+json' \
  -d '{"data":{"type":"parameters","id":"0-0-kbus-digitaloutputs-3-value","attributes":{"value":true}}}' \
  $P/0-0-kbus-digitaloutputs-3-value
```

A PATCH body is a JSON:API document whose `type` (`parameters`) and `id` must
match the URL. Values: `true`/`false` for DO, integers 0–65535 for AO; an
array may hold fewer values than channels (the rest stays), never more. A
rejected PATCH writes **nothing**. Every answer carries `Content-Length`. Status codes: `200`,
`204`, `400` malformed body / type or id mismatch, `403` writes disabled,
`404` unknown id, `405` wrong method, `409` read-only, value out of range or
too many values, `415` Content-Type is not `application/vnd.api+json`. Errors
carry a JSON:API `errors` body. Any other path or method (for example the
`/DO/1` and `/ALL` requests of version 1.x) gets `404` or `405`.

Differences from the firmware's own WDA (`https://<pfc>/wda`): plain HTTP on
port 8000, no TLS, no authentication, only the KBUS parameters.

**CODESYS application through the wdx data layer:** [codesys-wdx/README.md](codesys-wdx/README.md),
with ST code and a ready-to-paste brief for Claude in CODESYS.

### 3.3 Connection handling

- **One client at a time.** A request may be at most 4096 bytes. When the
  body arrives in a separate TCP segment (browsers do this), the server waits
  up to **100 ms** for the number of bytes that `Content-Length` announces;
  a body that comes later is missing (400). Clients that send each request in
  one write (curl, the CODESYS function blocks) never wait.
- **One request per connection**; the server closes it after the answer.
- There is no time-out: a client that connects and sends nothing blocks the
  server until it disconnects. Clients must close on their own time-out.
- An output keeps its value until it is written again; there is no client
  monitoring. After a start all outputs are `FALSE` / 0.

### 3.4 Live log

```bash
ssh root@192.168.1.17 'tail -f /var/log/messages | grep kbus-gateway'
```

| `HTTP_LOG` | Logged |
|---|---|
| `0` | no requests |
| `1` (default) | writes that **change** an output: `write KBus/DigitalOutputs/3/Value = true`, `write KBus/DigitalOutputValues = [true,false,...]` |
| `2` | **every request**: client, request line, status, start of the answer, e.g. `127.0.0.1 "PATCH /wda/parameters/0-0-kbus-digitaloutputvalues HTTP/1.0" -> 204` |

Level 2 is for testing a client: a CODESYS application that exchanges every
50 ms writes 40 lines per second (PATCH + GET). Set it in `/etc/kbus-gateway.conf` and run
`/etc/init.d/kbus-gateway restart`.

---

### 3.5 Web page

`http://192.168.1.17:8000/` (also `/index.html`) shows a live view of the
node, built into the program (no external files, works without internet):

- header: online/offline, KBUS running/error, cycle time, cycle count, errors
- digital inputs as lamps, digital outputs as buttons (click to switch)
- analog inputs and outputs with raw value and bar; analog outputs with an
  input field and *Set*
- the module list

The page uses only the wdx data layer: one `GET /wda/parameters` at load, then
one `GET /wda/parameters?filter[id]=…` every 500 ms for the status and all
value arrays, and `PATCH /wda/parameters/0-0-kbus-…-value` for a click. It
never runs two requests at the same time. A write that the gateway rejects
(for example 403 with `HTTP_WRITE_ENABLE=0`) is shown in red at the top.

If another client writes the outputs cyclically (for example a CODESYS
application), it overrides clicks made on the page within one exchange.

Source: [web/index.html](web/index.html), embedded with `.incbin` by
[src/web_asset.S](src/web_asset.S). After editing the page, rebuild and
reinstall the package.

---

## 4. Configuration

`/etc/kbus-gateway.conf` on the PFC (environment variables for the program;
apply with `/etc/init.d/kbus-gateway restart`):

| Variable | Default (program) | Package file | Description |
|---|---|---|---|
| `HTTP_PORT` | `8000` | `8000` | port of the wdx data layer. Not 80/443: the WBM uses them |
| `HTTP_ENABLE` | `1` | `1` | `0` = HTTP server off |
| `HTTP_WRITE_ENABLE` | `1` | `1` | `0` = outputs read-only: PATCH answers 403 |
| `HTTP_LOG` | `1` | `1` | 0 none, 1 writes that change an output, 2 every request (3.4) |
| `HTTP_PRIORITY` | `0` | `30` | SCHED_FIFO priority of the HTTP server thread, 0 = normal. Keep it **below** `KBUS_PRIORITY` |
| `KBUS_CYCLE_MS` | `10` | `10` | KBUS cycle, 1–40 ms (stay well below the 50 ms output watchdog) |
| `KBUS_PRIORITY` | `40` | `40` | SCHED_FIFO priority of the KBUS cycle thread, 0 = normal |
| `LOG_TARGET` | stderr | `syslog` | `syslog` = `/var/log/messages` |
| `KBUS_SIMULATION` | `0` | – | `1` = simulated node, no hardware (development) |
| `KBUS_DAL_LIB` | `libdal.so:libdal.so.0:libdal.so.1` | – | `dlopen()` candidates for the ADI/DAL library |
| `KBUS_INFO_LIB` | `libdbuskbuscommon.so:...` | – | candidates for the KBUS-info library |
| `KBUS_DIGITAL_OFFSET_MODE` | `auto` | – | `auto`, `relative` or `absolute` (section 1) |

---

## 5. Speed and determinism

Measured on the PFC300 (FW 04.08.09(30), version 1.1.3, same wdx code as
2.0.0) with `curl` on the controller itself, a new TCP connection per request,
while a CODESYS application was running:

| Request | median | p95 | p99 | max |
|---|---|---|---|---|
| `GET ?filter[id]=...` (1000×) | 1.35 ms | 1.96 ms | 10.6 ms | 13.0 ms |
| `GET` one parameter (300×) | 1.15 ms | 1.83 ms | – | 10.7 ms |
| `GET` all 40 parameters (300×) | 1.46 ms | 2.18 ms | – | 11.8 ms |
| `PATCH` (300×, JSON parsed) | 1.25 ms | 1.92 ms | – | 11.1 ms |

On a PC the gateway itself needs < 0.1 ms per request; the rest is TCP
connect/close on the controller.

**The outliers of ~10 ms** (about 3 % of the requests) come from the CODESYS
runtime: it runs its own real-time threads at priorities 50–83 (`Schedule`
81, `TaskGapTask` 71, `WagoAsyncRtHigh` 53 …), deliberately above everything
else, including the KBUS cycle (40) and the HTTP server (30). A real-time
client (`chrt -f 20 curl`) showed the same outliers. Raising the gateway
above CODESYS would disturb the PLC program, so it is not done.
`HTTP_PRIORITY=30` protects the HTTP server against all non-real-time load
(WBM, logging, other programs).

For a CODESYS client the exchange time is set by its **task cycles** (each
request needs a few: connect, write, read), not by the gateway. One exchange
of the CODESYS reference ([codesys-wdx](codesys-wdx/README.md)) is a fixed
sequence: `PATCH` the DO array (+ the AO array) and one `GET` with
`filter[id]` for all values, so 2 (3 with AO) requests, independent of the
number of channels. With version 1.x, one request per channel took 350 ms per
channel at a 50 ms task; one request for everything took about 70 ms at a
10 ms task.

Rules for constant timing: MainTask 10 ms, a fixed sequence of requests per
exchange, the array parameters (`...values`) instead of one request per
channel, `filter[id]` for reading, `HTTP_LOG=1`. HTTP over
TCP is fast but not hard real-time; for hard real-time I/O let CODESYS drive
the KBUS directly.

---

## 6. How it was built

This chapter records how the gateway came about and why it looks like it does.

### 6.1 History

1. **Reference behaviour.** The starting point was a CODESYS application,
   *REST I/O server on PFC200* (FbREST, version 1.0, 11-11-2026), that serves
   the KBUS channels as `GET /DI/1` → `DI1=TRUE`. Version 1.x reproduced it
   byte for byte, checked by 31 reference cases.
2. **KBUS access** was taken from the WAGO example
   *HowTo_ADI-MyKBusApplikation* (`Information/`, MIT licence): ADI/DAL device
   scan, `libpackbus`, application state *Running*, one `libpackbus_Push` per
   cycle (`kbusdemo.c`), and the module list through the KBUS-info D-Bus API
   (`getkbusinfo.c`). The WAGO libraries are loaded with `dlopen()`, so only
   their headers are needed to build.
3. **First as a Docker container** (PFC200 750-8217, FW 04.09.50, then PFC300).
   That worked, but needed many bind mounts and an entrypoint that loads the
   firmware's libraries without the firmware's glibc (the Docker files are in
   the git history before version 1.1.3).
4. **Bulk requests** (`/ALL`) were added when a CODESYS client with one
   request per channel turned out to be too slow (350 ms per request at a
   50 ms task; outputs were skipped). That showed the principle: one request
   for all channels.
5. **wdx data layer.** To present the KBUS data like WAGO's own parameter
   model (training *Communicating with the WAGO CC100 via the wdx Parameter
   Model & WDA REST Interface*), the `/wda/parameters` interface was added
   with the same path/id rules, data types, JSON:API format and status codes,
   plus `filter[id]` and array parameters for speed.
6. **Native package.** To make it easy to install and as fast as possible, the
   gateway became a native `.ipk` for the PTXdist firmware (version 1.1.x),
   and the Docker files were removed.
7. **wdx only (version 2.0.0).** The single-channel and bulk interfaces were
   removed: the wdx data layer is the one interface, with its array
   parameters and `filter[id]` taking over the role of the bulk requests.
   The code of version 1.x is in the git history. A web page on top of the
   data layer replaced the old, unused page of the first version; a browser
   click was verified in headless Chrome (PATCH → 204 → output switched).

### 6.2 Source structure

The program is plain C (gnu11), about 2200 lines, no dependencies besides
glibc. Two threads matter: the KBUS cycle thread (`gateway.c`) and the HTTP
server thread (`http_server.c` + `wda.c`). They share only the process images
and channel values, behind one mutex with priority inheritance.

| File | Role |
|---|---|
| `src/main.c` | startup order, signals, memory locking (`mlockall`) |
| `src/config.c/.h` | environment variables → `gw_config_t` |
| `src/backend_adi.c` | real KBUS: `dlopen` of `libdal`, ADI device scan, KBUS-info, cycle |
| `src/backend_sim.c` | simulated node (12 DI, 8 DO, 4 AI, 4 AO; DI1–8 follow DO1–8, AI4 follows AO1) |
| `src/gateway.c/.h` | channel map, process images, real-time cycle thread, bulk read/write under one lock |
| `src/http_server.c/.h` | socket handling (one read, one answer), HTTP priority, request log |
| `src/wda.c/.h` | wdx data layer: parameter table (built once), JSON:API output, minimal JSON reader for PATCH |
| `src/log.c/.h` | stderr or syslog |
| `web/index.html` + `src/web_asset.S` | the web page (3.5), embedded into the binary |

### 6.3 The native package

- **Toolchain.** `tools/build-ipk.sh` compiles inside WAGO's SDK builder image
  (`pfc300-builder`, made by `make builder` in the firmware SDK), with the
  firmware's own cross compiler (GCC 11.3, glibc 2.35). The binary needs at
  most `GLIBC_2.34`, so it runs on the firmware without extra libraries.
- **Format.** Exactly like the packages PTXdist builds (compared with
  `bzip2_1.0.8_arm64.ipk` of the SDK): an `ar` archive with
  `debian-binary` (`2.0`), `control.tar.gz` (`control`, `postinst`, `prerm`,
  `conffiles`) and `data.tar.gz`, files owned by root, directories `755`.
- **Dependencies.** `Depends: dal, libdbuskbuscommon`, the firmware packages
  that provide the KBUS libraries.
- **Service.** A BusyBox-compatible init script (`start-stop-daemon`) like the
  firmware's own scripts; `postinst` creates `/etc/rc.d/S99_kbus-gateway` and
  starts it; `prerm` stops it before an update or removal.
- **Settings** in `/etc/kbus-gateway.conf`, declared as conffile: opkg keeps a
  changed file on updates.

### 6.4 Problems solved on the way

| Problem | Cause | Fix |
|---|---|---|
| `RTLD_DEFAULT undeclared` with the WAGO toolchain | older glibc headers need `_GNU_SOURCE` | `#define _GNU_SOURCE` in `backend_adi.c` |
| `LOG_INFO redefined` | `<syslog.h>` defines `LOG_INFO` too | syslog code in its own file `log.c` |
| own log lines appeared as `kbus:` | WAGO's KBUS library calls `openlog("kbus")` during initialisation | the gateway sets its own ident again for every message |
| log flooded (14+ lines/s) | every cyclic write from a CODESYS client was logged | `HTTP_LOG=1` logs only writes that change an output |
| `opkg`: `Removing obsolete file .` / unlink error on upgrade | the archives contained the root entry `./` | archives contain `./usr`, `./etc` ... only |
| `sleep: invalid number '0.5'` | BusyBox `sleep` takes whole seconds | init script waits in steps of 1 s |
| wdx id lookup slow | ids were formatted for every request | id table built once at startup |

---

## 7. Modifying the application

### 7.1 Project layout

```
├── Makefile                   native / cross build; ADI backend when sdk/include/dal exists
├── VERSION                    package version
├── src/                       the program (6.2)
├── web/index.html             the web page at / (3.5), built into the program
├── packaging/ipk/             init script, config file, opkg control scripts
├── sdk/                       SDK headers (imported, 2.3) + OsCommon.h stand-in
├── tools/
│   ├── build-ipk.sh           build the .ipk with the SDK toolchain (2.4)
│   ├── deploy.sh              build + copy + opkg install + status
│   ├── import-sdk-packages.sh headers from the prebuilt SDK packages (2.3 A)
│   ├── import-sdk-headers.sh  headers from a built SDK sysroot (2.3 B)
│   └── test-rest.py           tests: wdx (simulation), real node read-only / write
├── codesys-wdx/               CODESYS application via the wdx data layer (ST + brief)
├── codesys/                   CODESYS project of the earlier REST client tests (version 1.x)
└── Information/               WAGO example (MIT) and documentation
```

### 7.2 Development loop (on the PC, no PFC needed)

```bash
make run-sim                                   # native build, simulated node on http://localhost:8000
tools/test-rest.py 127.0.0.1 wda               # wdx data layer: all requests, PATCH, error codes
tools/deploy.sh 192.168.1.17                   # then: build .ipk, install on the PFC, status
```

The VS Code tasks (*Terminal → Run Task*) cover the same steps. Without
`sdk/include/dal` the native build is simulation-only, which is enough for
all data-layer and channel-map work.

### 7.3 Common changes

**Change the wdx data layer.** The parameter tree is built in `build_list()`
in [src/wda.c](src/wda.c); paths in `path_of()`, types in `dtype_of()`,
values in `write_value()`. Ids follow automatically from the paths.

**Support another module type.** Module classification is in
`build_channel_map()` in [src/gateway.c](src/gateway.c).

**Release.** Raise [VERSION](VERSION), `tools/build-ipk.sh arm64`, install.

### 7.4 Rules to keep

- Never do slow work (I/O, logging, allocation) in `cycle_thread()`. Every
  cycle must finish well within 50 ms, or the output modules switch off.
- Keep `HTTP_PRIORITY` below `KBUS_PRIORITY`.
- Read and write channels only through the `gateway_*` functions; they take
  care of locking.
- Build against headers with the **same DAL version** as the PFC's
  `libdal.so` (2.3). The ADI interface table is version-specific.
- Build with the toolchain of the controller's firmware SDK, so the binary
  does not need a newer glibc than the firmware has.

---

## 8. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `opening the KBUS device failed` | another program owns the KBUS: firmware runtime not *None*, a CODESYS project with KBUS, or a second gateway (e.g. an old Docker container) |
| `cannot listen on port 8000: Address in use (retrying)` | something else uses the port (a second gateway, a CODESYS application); choose another `HTTP_PORT` |
| `could not load the ADI/DAL library` | check `ls /usr/lib/libdal*` and set `KBUS_DAL_LIB` |
| `this binary was built WITHOUT the WAGO ADI/DAL headers` | built without `sdk/include/dal`: import the headers (2.3) and rebuild |
| `version 'GLIBC_2.xx' not found` | built with a newer toolchain than the firmware: use the SDK builder image of the firmware |
| `opkg`: dependency `dal` / `libdbuskbuscommon` missing | firmware without KBUS support (not a PFC with KBUS) |
| `real-time scheduling not permitted` | not started as root; use the init script |
| `ldkc_KbusInfo_Create failed` | D-Bus not running yet; start later or check `/run/dbus/system_bus_socket` |
| Channel numbers point to the wrong I/O | compare the startup module list with the node; try `KBUS_DIGITAL_OFFSET_MODE=relative` or `absolute` |
| Outputs drop out briefly | look for `KBUS cycle failing` in the log; lower system load or the cycle time |
| Requests take ~10 ms now and then | CODESYS real-time threads (section 5); not a gateway fault |

**Investigating further.** The PFC has `strace`:
`strace -f -e trace=%file -e status=failed /usr/bin/kbus-gateway` (stop the
service first) shows which files the WAGO libraries miss. Library names and
the digital-offset interpretation can differ between firmware releases; the
startup log shows which choice was made, and the variables in section 4
override it.

---

## 9. Limitations and security

- Only plain digital modules and plain analog modules (16-bit words, data in
  one direction) are mapped. Complex modules and the 750-495 get no channels.
- Values are raw process-image values; there is no unit scaling.
- The module list is read once at startup: run
  `/etc/init.d/kbus-gateway restart` after changing the node.
- **Only one KBUS master**: the gateway and a CODESYS application with KBUS
  access cannot run at the same time.
- Tested on real hardware with digital outputs only (one 8DO module, PFC300
  and PFC200). Digital inputs, analog modules and the 750-495 were tested with
  the simulation node. The `armhf` package for PFC100/PFC200 builds but has
  not been installed yet.
- **The wdx data layer is unencrypted and has no authentication** (unlike the
  firmware's own WDA). Anyone who can reach port 8000 can read all I/O and
  switch outputs. Use it only on an isolated machine network, set
  `HTTP_WRITE_ENABLE=0` when writing is not needed, and block the port in the
  PFC firewall for other networks.
- On `stop` and `opkg remove` all outputs are switched to 0. If the program
  crashes, the output modules switch off on their own after 50 ms.
