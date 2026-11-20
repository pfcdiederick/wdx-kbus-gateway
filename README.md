# KBUS HTTP Gateway

A small native application for WAGO PFC100 / PFC200 / **PFC300** controllers
that **owns the KBUS** and is the interface between **Docker containers** and
the KBUS. It reads and writes the channels of the **DI / DO / AI / AO**
modules and presents them, together with **registers** that containers use to
share data with each other, as a **wdx data layer**. It is installed as a
normal firmware package (`.ipk`) on the PTXdist firmware, starts at boot and
runs the KBUS cycle every **5 ms** with real-time priority.

```
 container A ─┐                                ┌─ KBUS provider ── KBUS (exclusive)
 container B ─┼─ /run/kbus-gateway/wdx.sock ───┤
 CODESYS     ─┘   (or TCP 127.0.0.1:8000)      └─ register provider ── memory (+ /home)
```

The wdx data layer is the **only interface**. Containers reach it through a
**Unix socket** (mount `/run/kbus-gateway` into the container); containers
with the host network (CODESYS) can also use TCP on `127.0.0.1:8000`. It
follows the WAGO wdx conventions: paths, ids, data types, GET reads, PATCH
writes, JSON:API. Registers never touch the KBUS (3.6).

```
S="--unix-socket /run/kbus-gateway/wdx.sock"
curl $S http://x/wda/parameters                                             all parameters
curl $S http://x/wda/parameters/0-0-registers-pump1-setpoint-value          one register
GET   http://<pfc-ip>:8000/wda/parameters                                   the same over TCP
GET   http://<pfc-ip>:8000/wda/parameters/0-0-kbus-digitalinputs-1-value    one input
GET   http://<pfc-ip>:8000/wda/parameters?filter[id]=0-0-kbus-digitalinputvalues,0-0-kbus-analoginputvalues
PATCH http://<pfc-ip>:8000/wda/parameters/0-0-kbus-digitaloutputs-3-value   {"data":{...,"attributes":{"value":true}}}
```

**Web page:** the management container (`tools/deploy-docker.sh <pfc-ip>
--manager`, 3.7) shows status, live I/O, all parameters with their
definitions and the registers at **`http://<pfc-ip>/kbus`** (through the
controller's own web server, port 80/443); a login can be switched on there. The gateway itself stays closed to the network.

**Why not WAGO's own WDx?** The firmware runs WAGO's ParameterService
(`paramd`) with a provider socket, but the headers for writing a provider are
not part of the SDK. This gateway follows the same WDx pattern (data layer,
KBUS provider, register provider, client access over IPC), so its providers
can be moved to `paramd` once that interface is available (6.1).

A CODESYS application that reads and controls the KBUS through the data layer
is in [codesys/](codesys/README.md).

The KBUS access is based on the WAGO example *HowTo_ADI-MyKBusApplikation* in
[`docs/reference/`](docs/reference/HowTo_ADI-MyKBusApplikation/README.md) (same ADI/DAL
calls as `kbusdemo.c`, same module discovery as `getkbusinfo.c`).

Tested on a **PFC300 750-8302, firmware 04.08.09(30)** with a real KBUS (one
8DO module), used by a CODESYS application at the same time.

---

## Contents

1. [How it works](#1-how-it-works)
2. [Installation](#2-installation)
3. [Using the gateway: the wdx data layer](#3-using-the-gateway-the-wdx-data-layer) (3.6: containers and registers)
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
  GET   /wda/parameters/...  ────────────────┼─▶ server: unix socket + TCP 127.0.0.1:8000  │
  PATCH /wda/parameters/...                  │     one thread per client, wdx data layer     │
                                             │        │ read / write (mutex)    registers   │
                                             │        ▼                         (own lock)   │
                                             │   channel map + process images in memory     │
                                             │        ▲                                      │
                                             │        │ every 5 ms, SCHED_FIFO prio 40       │
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
5. The registers are read from `REGISTER_FILE`, persistent values from
   `REGISTER_STORE` (3.6).
6. The server starts (optionally with real-time priority, see
   `HTTP_PRIORITY`): the Unix socket and the TCP listener. The parameter
   table of the wdx data layer is built once per configuration (ids
   precomputed, so every lookup is cheap).

**KBUS cycle thread.** Every `KBUS_CYCLE_MS` (default 5 ms) a real-time
thread (`SCHED_FIFO`, priority 40) copies the output image, triggers one cycle
with `libpackbus_Push` and stores the input image. WAGO output modules switch
their outputs off when no cycle arrives within **50 ms**, so this thread never
waits for the HTTP server: the shared images are locked only for the copy,
not during the bus cycle.

**HTTP requests** only touch the in-memory process images:

- Reading an input returns the value of the last bus cycle.
- Writing an output changes the output image; the next cycle (≤ 5 ms later)
  sends it to the module. All values of one array write are applied under one
  lock, so they reach the modules in the same cycle.
- Reading an output returns the value that was last commanded.

**Dynamic configuration.** Modules added to or removed from the KBUS are
picked up while the gateway runs:

1. After a change of the node, the KBUS master reports an error in every
   cycle. After **0.5 s of failing cycles** (100 cycles at 5 ms) the cycle
   thread asks for a rescan. It does no slow work itself.
2. The server's main thread runs the rescan while no request is being
   handled (new requests wait until it is done): stop the cycle
   (outputs to 0), close the KBUS device, open it again (WAGO's library runs a
   new KBUS initialisation), read the modules, rebuild the channel map, start
   the cycle. On the PFC300 this takes under one second.
3. `KBus/ConfigurationVersion` goes up by one and the wdx tree is rebuilt with
   the new modules and channels. The web page notices the new version and
   rebuilds itself.

A rescan can also be started by hand: `PATCH` `KBus/Rescan` = `true`, or the
**Rescan KBus** button on the web page. If errors persist after an automatic
rescan (for example a defective module), the next automatic attempt waits 5,
10, 20, 40 and then 60 s, so the bus is not reset continuously. Channel numbers
follow the new module order: a module inserted in front of others shifts their
numbers. Outputs are 0 after a rescan until a client writes them again.

WAGO recommends changing the node with the power switched off; the gateway
then simply reads the new node at the next start. The automatic rescan covers
the case that modules are changed while the controller runs.

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
tests/test-rest.py 192.168.1.17 node       # PC: read-only check
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
scp build-ipk/kbus-gateway_2.4.0_arm64.ipk root@192.168.1.17:/tmp/
ssh root@192.168.1.17 'opkg install /tmp/kbus-gateway_2.1.0_arm64.ipk && rm /tmp/kbus-gateway_2.1.0_arm64.ipk'
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
PFC300 with one 8DO module (log of version 2.1.0; from 2.2.0 the `config:`
line also lists the socket and TCP settings, the cycle runs every 5 ms, and
`registers: ...` and `listening on unix socket ...` lines follow):

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

On the controller (no network port needed):

```bash
ssh root@192.168.1.17 "curl -sg --unix-socket /run/kbus-gateway/wdx.sock 'http://x/wda/parameters?page[limit]=5'"
```

From the PC, the TCP listener must accept other networks: set
`HTTP_BIND=0.0.0.0` in `/etc/kbus-gateway.conf` and restart (and set it back
afterwards if the PC does not need access):

```bash
tests/test-rest.py 192.168.1.17 node              # real node, READ ONLY: parameter tree, values, invalid requests
tests/test-rest.py 192.168.1.17 write --do 1      # real node: switches DO1 on/off with pauses (check what is connected!)
curl -g "http://192.168.1.17:8000/wda/parameters?page[limit]=5"
```

Simulation tests (all writes allowed) are in 7.2.

### 2.8 Alternative: the gateway as a Docker container

The same program also runs as a container
([docker/gateway](docker/gateway); all containers: [docker/README.md](docker/README.md)). The native package is the recommended way (WAGO runs
its own providers natively too); the container is for installations that
manage everything with Docker. Measured on the PFC300 (8DI + 8DO, CODESYS
running, 5 ms cycle), both are equivalent:

| | native `.ipk` | container |
|---|---|---|
| KBUS exchange max, normal / with 2 extra clients | 592 / 803 µs | 683 / 913 µs |
| wake-up delay max, normal / with 2 extra clients | 46 / 103 µs | 74 / 133 µs |
| overruns, KBUS errors | 0, 0 | 0, 0 |
| request Unix socket, median / p99 | 1.33 / 2.86 ms | 1.45 / 2.93 ms |
| request TCP, median / p99 | 2.54 / 4.36 ms | 2.59 / 4.45 ms |
| CPU (one core) | 7.1 % | 7.3 % |
| size | 89 KB program | 101 MB image (30 MB compressed) |

```bash
tools/deploy-docker.sh 192.168.1.17            # build, load, stop the native service, start the container
tools/deploy-docker.sh 192.168.1.17 --client   # the same plus the example client (3.6)
tools/deploy-docker.sh 192.168.1.17 --native   # remove the gateway container, native service again
```

Only one program may own the KBUS: the script stops the native service and
removes its boot link while the container runs; `--native` restores both. The
container starts with `--restart unless-stopped`, so it comes back after a
reboot. It uses the image defaults (the same values as the package's
`/etc/kbus-gateway.conf`, which the container does **not** read); other
settings with `GW_ENV`, e.g. `GW_ENV="-e KBUS_CYCLE_MS=2" tools/deploy-docker.sh ...`.

| Mount / option | Why |
|---|---|
| `--privileged` | real-time priority, memory locking, KBUS hardware |
| `--network host` | TCP on `127.0.0.1:8000` of the controller (CODESYS) |
| `/usr/lib` → `/host/usr/lib` (ro) | the firmware's WAGO libraries; the entrypoint loads them **without** the firmware's glibc |
| `/var/run/dbus` | module discovery (KBUS-info) |
| `/etc/specific` (ro), `/etc/config-tools/get_typelabel_value` (ro), `/dev/eeprom`, `/dev/ttyKbus` | KBUS configuration, type label and controller firmware for `libpackbus` |
| `/dev/log`, `/etc/localtime` (ro) | log to `/var/log/messages` in local time |
| `/run/kbus-gateway` | the socket, so other containers and programs find it at the same path |
| `/etc/kbus-gateway-registers.conf` (ro), `/home/kbus-gateway` | register declarations and persistent values |

Optional paths that do not exist on a controller are skipped (`note: ... not
present`). After a firmware update, check the container first: it depends on
the firmware's libraries and files.

---

## 3. Using the gateway: the wdx data layer

### 3.1 Parameters

All KBUS data is presented as **parameters of the WAGO Device Access (WDA)
REST-API**, API version 1.11.0 (manual: `Information/WDX/WDA_REST-API_Manual_v1_0.pdf`).
Reading and writing I/O uses exactly the requests, bodies, paging, filters and
error objects of that API: JSON:API 1.0, **GET reads, PATCH writes**.

| wdx rule | KBUS data |
|---|---|
| **path** = hierarchical name | `KBus/DigitalOutputs/3/Value` |
| **id** = `0-0-` + lower-case path, `/` → `-` | `0-0-kbus-digitaloutputs-3-value` |
| **dataType** | DI/DO `boolean`, AI/AO `uint16` (raw), counters `uint64`, names `string`, lists `instantiations`; registers `boolean`, `int32`, `uint32`, `float64`, `string` |
| **dataRank** | `scalar` per channel, `array` for a whole class |
| **value**, **stringValue** | `value` in its JSON type; numeric types also as `stringValue` (`"123"`, `["1","2"]`) |
| write rights | `GET /wda/parameter-definitions/{id}` → `"writeable": true` for DO/AO values, registers, `CycleTime`, `Rescan`, `ResetStatistics` |
| persistence | process values are volatile; registers declared `persistent` and `CycleTime` survive a restart (`"userSetting": true`) |

Parameter tree:

```
KBus/Healthy                      boolean   R    KBUS cycle running without errors
KBus/CycleTime                    uint16    R/W  ms, 1–40; a write takes effect at the next cycle and is stored
KBus/CycleCount, ErrorCount       uint64    R
KBus/ConfigurationVersion         uint32    R    +1 for every bus scan (startup = 1): the tree below changed
KBus/Rescan                       boolean   R/W  PATCH true = rescan the KBUS now (reads false)
KBus/CycleDuration                uint32    R    µs, last KBUS exchange
KBus/MaxCycleDuration             uint32    R    µs, longest KBUS exchange
KBus/MaxWakeupDelay               uint32    R    µs, longest delay of a cycle start (scheduling jitter)
KBus/Overruns                     uint64    R    cycles that started too late for their period
KBus/ResetStatistics              boolean   R/W  PATCH true = CycleCount, ErrorCount and the three values above back to 0
                                                 (only through the control socket = the dashboard after its login, 3.7)
KBus/LocalControl                 boolean   R/W  true = only the dashboard may write the KBUS; other clients get 403
                                                 (code 57) for KBUS writes, reading and registers stay free; switched
                                                 only through the control socket; kept after a restart (3.7)
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
Registers/ChangeCounter           uint64    R    +1 for every write that changed a register
Registers/<Name>/Value            per declaration  R/W   (3.6)
```

Channels are numbered per class from 1, in KBUS order (left to right). Values
are **raw process-image values**: for a 750-559 (0–10 V), 32767 = 10 V. Scale
them in your application.

### 3.2 Requests

| Request | Answer |
|---|---|
| `GET /wda` | service identity: `serviceVersion`, `apiVersion` (1.11.0) |
| `GET /wda/parameters` | parameters, **paged**: `page[offset]` (default 0), `page[limit]` (default **255**); `links` `self`, `first`, `last`, `next`, `prev` |
| `GET /wda/parameters?filter[path]=KBus/AnalogInputValues` | filters of the manual: `path`, `device` (`0-0` / `headstation`), `beta`, `deprecated`, `definition.writeable`, `definition.userSetting`; comma-separated values, several filters must all match, any other filter → `400` |
| `GET /wda/parameters?filter[id]=id1,id2` | **extension of this gateway:** exactly these parameters, from **one KBUS snapshot** |
| `GET /wda/parameters/{id}` | one parameter |
| `GET /wda/parameters/{id}/definition` | `301` → `/wda/parameter-definitions/{id}` |
| `GET /wda/parameter-definitions[/{id}]` | definitions: path, dataType, dataRank, writeable, userSetting (paged, filters `path`, `device`, `beta`, `deprecated`, `writeable`, `userSetting`) |
| `PATCH /wda/parameters/{id}` | write one parameter → **204 No Content** |
| `PATCH /wda/parameters` | write **several** parameters at once (batch) → **204** |

```bash
P=http://192.168.1.17:8000/wda/parameters
curl -g "$P?page[limit]=5"
curl -g "$P?filter[path]=KBus/DigitalOutputs/3/Value"
curl -g "$P?filter[id]=0-0-kbus-digitalinputvalues,0-0-kbus-digitaloutputvalues"
curl -L $P/0-0-kbus-digitaloutputs-3-value/definition
curl -X PATCH -H 'Content-Type: application/vnd.api+json' \
  -d '{"data":{"type":"parameters","id":"0-0-kbus-digitaloutputs-3-value","attributes":{"value":true}}}' \
  $P/0-0-kbus-digitaloutputs-3-value
```

A PATCH body is a JSON:API document whose `type` (`parameters`) and `id` must
match the URL. Values: `true`/`false` for DO, integers 0–65535 for AO; an
array may hold fewer values than channels (the rest stays), never more.
Numeric values may also be sent as `"stringValue": "123"` (or `["1","2"]`),
as the manual recommends. A rejected PATCH writes **nothing**.

**Batch write.** `PATCH /wda/parameters` with `"data"` as an **array** of
parameters writes all of them, or none: every value is checked first, then
the KBUS outputs are written under one lock (they reach the modules in the
same cycle) and the registers under one lock. Outputs and registers can be
mixed; when an id appears twice, the last value wins.

```bash
curl --unix-socket /run/kbus-gateway/wdx.sock -X PATCH -H 'Content-Type: application/vnd.api+json' \
  -d '{"data":[{"type":"parameters","id":"0-0-registers-pump1-setpoint-value","attributes":{"value":3.5}},
               {"type":"parameters","id":"0-0-kbus-digitaloutputs-1-value","attributes":{"value":true}}]}' \
  http://x/wda/parameters
```

Every answer carries `Content-Length`. Status codes as in the manual: `200`,
`204`, `301` definition redirect, `400` malformed body / wrong value, `403` KBUS
writes disabled, `404` unknown id, `405` wrong method, `406` `Accept` without
JSON:API, `415` Content-Type is not `application/vnd.api+json`, `500` the
outputs could not be set; and from the server: `408` request not complete
within `WDX_CLIENT_TIMEOUT_MS`, `413` request larger than 16 KB, `503` all
`WDX_MAX_CLIENTS` slots busy.

Errors carry a JSON:API `errors` array; each object has `status`, `title`,
`detail`, the WDA status `code` and `source.pointer`:

| code | HTTP | meaning |
|---|---|---|
| 17 | 404 | unknown parameter id |
| 24 | 400 | wrong JSON type (e.g. `1` for a boolean) |
| 25 | 400 | wrong `stringValue` (or `stringValue` for a boolean/string) |
| 31 | 400 | parameter is read-only |
| 36 | 400 | `null` can not be written |
| 39 | 400 | value out of range, string too long, too many array values |
| 41 | 400 | batch: not written because another value was rejected |
| 57 / 58 | 403 | KBUS writes disabled (`HTTP_WRITE_ENABLE=0`) / batch: another value was |
| 21 | 500 | the outputs could not be set |

A batch PATCH returns **one error object per parameter** (code 41 for the
valid ones); the HTTP status is the common one, `400` when they differ.

Differences from the full WDA of the firmware (`https://<pfc>/wda`), all
deliberate for a local gateway: Unix socket or plain HTTP (no TLS, so no
`426`), no authentication (access is controlled by the socket's file
permissions and the TCP bind address), only the parameter resources
(no devices, methods, enums, instances, monitoring lists, features or
File-API), `KBus/Rescan` and `KBus/ResetStatistics` are writable parameters
instead of methods, and the extra filter `filter[id]`.

**CODESYS application through the wdx data layer:** [codesys/README.md](codesys/README.md),
with ST code and a ready-to-paste brief for Claude in CODESYS.

### 3.3 Connection handling

- **Several clients at the same time**: up to `WDX_MAX_CLIENTS` (default 16),
  one thread each. When all slots are busy, the next connection gets `503`.
- A client has **`WDX_CLIENT_TIMEOUT_MS`** (default 5 s) to send its request,
  including a body that `Content-Length` announces (browsers send it in a
  second TCP segment). After that: `408`, nothing written. A client that
  connects and sends nothing only holds its own slot until the time-out.
- A request may be at most **16 KB** (`413` otherwise).
- **One request per connection**; the server closes it after the answer.
- A bus rescan waits until running requests are done; requests that arrive
  during the rescan wait for it (under one second on the PFC300).
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
node, built into the program (no external files, works without internet).
From another PC it needs `HTTP_BIND=0.0.0.0` (the default `127.0.0.1` only
accepts connections from the controller itself). The page does not show the
registers.

- header: online/offline, KBUS running/error, cycle time, cycle count, errors,
  configuration version and a **Rescan KBus** button
- digital inputs as lamps, digital outputs as buttons (click to switch)
- analog inputs and outputs with raw value and bar; analog outputs with an
  input field and *Set*
- the module list

The page uses only the wdx data layer: one `GET /wda/parameters` at load, then
one `GET /wda/parameters?filter[id]=…` every 500 ms for the status, the
configuration version and all value arrays, and
`PATCH /wda/parameters/0-0-kbus-…-value` for a click. When the configuration
version changes (modules added or removed), the page loads the tree again and
shows "KBUS configuration changed". It
never runs two requests at the same time. A write that the gateway rejects
(for example 403 with `HTTP_WRITE_ENABLE=0`) is shown in red at the top.

If another client writes the outputs cyclically (for example a CODESYS
application), it overrides clicks made on the page within one exchange.

Source: [web/index.html](web/index.html), embedded with `.incbin` by
[src/web_asset.S](src/web_asset.S). After editing the page, rebuild and
reinstall the package.

### 3.6 Containers and registers

The gateway is the only program on the KBUS. Containers never access the KBUS
or each other directly: they read and write parameters of the gateway.

**Connect a container.** Mount the socket **directory** (not the socket file:
it is created again at every start of the gateway):

```bash
docker run -v /run/kbus-gateway:/run/kbus-gateway my-image
```

The socket has mode `WDX_SOCKET_MODE` (default `0660`, owner root). Processes
that run as root in the container can connect; for other users set
`WDX_SOCKET_GROUP` to a group they belong to (same group id inside the
container). No network port is needed, so the container can run without
access to the machine network. The log shows which process sent a request:
`unix(pid 1234, uid 0)` (`HTTP_LOG=2`).

Containers with `--network host` (for example the CODESYS runtime of
[codesys](codesys/README.md)) can use TCP on `127.0.0.1:8000`
instead.

**Text between CODESYS and a container.** CODESYS writes a string register
with `FB_WdxWriteString` and reads one with `FB_WdxReadString`
([codesys/README.md](codesys/README.md), section 6). The example client
answers every text in `Codesys/Message` with `respond()` in
`Python/Response`: `ping` -> `pong`, `time` -> date and time, otherwise
`OK hh:mm:ss: <text>`. Adapt `respond()` in `docker/example-client/example.py`.
To answer **by hand** from the PC: `tools/wdx-chat.py` shows every new text
from CODESYS and sends each line you type to `Python/Response` (through the
manager; `--send "text"` for one answer). Then switch the automatic answers
off: `CL_ENV="-e PYTHON_RESPONSE=" tools/deploy-docker.sh <pfc-ip> --client --no-gateway`.

A ready-made example is [docker/example-client](docker/example-client): a
Python container without network (`--network none`) that reads the I/O,
reports changes and writes the registers `Example/Heartbeat` and
`Example/Status` (declare them first, see `example.py`). Its
[wdx_client.py](docker/lib/wdx_client.py) (`get`, `get_many`,
`set`, `set_many`) can be copied into your own containers. Start it with
`tools/deploy-docker.sh <pfc-ip> --client`, or on the PFC:

```bash
docker run -d --name kbus-example-client --restart unless-stopped --network none \
    -v /run/kbus-gateway:/run/kbus-gateway kbus-example-client:arm64
```

The minimum, Python inside a container, standard library only:

```python
import http.client, json, socket

class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__("localhost", timeout=2)
        self.path = path
    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)

def wdx(method, url, body=None):
    c = UnixConnection("/run/kbus-gateway/wdx.sock")
    c.request(method, url, json.dumps(body) if body else None,
              {"Content-Type": "application/vnd.api+json"} if body else {})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, (json.loads(data) if data else None)

P = "/wda/parameters"
st, d = wdx("GET", P + "?filter[id]=0-0-registers-changecounter,0-0-kbus-digitalinputvalues")
pid = "0-0-registers-pump1-setpoint-value"
wdx("PATCH", f"{P}/{pid}", {"data": {"type": "parameters", "id": pid, "attributes": {"value": 3.5}}})
```

**Declare registers** in `/etc/kbus-gateway-registers.conf` (kept on package
updates) and restart the gateway:

```
# <Name>         <type>   [default]  [persistent]
Pump1/Setpoint   float64  12.5       persistent
Line/Running     boolean
Batch/Number     uint32   0          persistent
Recipe/Name      string   ""         persistent
```

| Rule | |
|---|---|
| Name | segments of `A-Z a-z 0-9 _` separated by `/`, each starting with a letter, max 40 characters; not case sensitive, must be unique |
| wdx path / id | `Registers/Pump1/Setpoint/Value` / `0-0-registers-pump1-setpoint-value` |
| types | `boolean`, `int32`, `uint32`, `float64` (finite), `string` (max 127 bytes UTF-8) |
| default | JSON value, strings in double quotes; omitted = `false` / `0` / `""` |
| writes | always allowed (also with `HTTP_WRITE_ENABLE=0`); the JSON type must match exactly (`1` is not a boolean, `1.5` not an int32) |
| limit | 256 registers |

Invalid lines are logged (`/var/log/messages`) and skipped; the other
registers are still available.

**Persistent registers** are written to `REGISTER_STORE` (default
`/home/kbus-gateway/registers.dat`, on the data partition) at most every
`REGISTER_SAVE_S` seconds (default 10) when they changed, and always at a
normal stop. The file is replaced atomically (temporary file, `fsync`,
`rename`), so a power failure leaves the old or the new values, never a
broken file. A power failure loses at most the last `REGISTER_SAVE_S`
seconds. After a restart, a value is restored when the register is still
declared with the same type and as `persistent`. Do not write persistent
registers in a fast loop: every change ends up on the flash.

**Notice changes.** `Registers/ChangeCounter` goes up by one for every write
that changed at least one register (a batch counts once; writing the same
value does not count). A container polls this one value and reads the
registers only when it changed. For a consistent view, read everything it
needs with one `GET ?filter[id]=...`: all values come from one snapshot.

**Who writes what.** The gateway does not know which container owns a
register; agree on it in the project (for example, one writer per register).
For values that belong together, use one batch PATCH, so readers never see
half of an update.

### 3.7 Management web interface (kbus-manager)

A separate container ([docker/manager](docker/manager)) makes the gateway
manageable from a PC without opening the gateway itself to the network:

```
 PC browser ──http(s)://<pfc-ip>/kbus──▶ lighttpd (port 80/443, WBM) ──127.0.0.1:8090──▶ kbus-manager
                                                                    ──Unix socket──▶ kbus-gateway ──▶ KBUS
```

```bash
tools/deploy-docker.sh 192.168.1.17 --manager --no-gateway   # only the manager (gateway native or container)
```

Open **`http://192.168.1.17/kbus`**. The website itself is open by default;
**Reset Statistics** and **Local Control** always need the login (default
user `admin`, password `wago`, see *Login* below).

The dashboard is served by the controller's own web server (lighttpd, the
one of the WBM): `--manager` installs `/etc/lighttpd/apps.confd/kbus.conf`
([docker/manager/kbus-lighttpd.conf](docker/manager/kbus-lighttpd.conf)),
checks it with `lighttpd -tt` and restarts lighttpd; a rejected config is
rolled back. The manager container itself only listens on `127.0.0.1:8090`.
HTTP follows the web mode of the WBM (*Ports and Services > Web services*):
in the default **HTTPS only** mode, `http://<pfc-ip>/kbus` is redirected to
`https://<pfc-ip>/kbus/` like the WBM (self-signed certificate, same browser
warning); in *HTTP and HTTPS* mode it stays on plain HTTP.

**Checked at every start.** A firmware update replaces `/etc/lighttpd`, but
`/home` (with Docker and its containers) stays. The wanted config is therefore
kept in `/home/kbus-manager/lighttpd-kbus.conf`, and the gateway container runs
[kbus-dashboard-check.sh](docker/manager/kbus-dashboard-check.sh) at every
start, **before the gateway starts**: missing or different `kbus.conf` →
install, `lighttpd -tt`, restart lighttpd; a rejected config restores the old
state, so the WBM keeps working. When everything is in order it does nothing.
The result is in the syslog (`kbus-dashboard: ...`) and in
`docker logs kbus-gateway`. The check runs in the controller's namespaces, so
the gateway container is started with `--pid host`; `-e KBUS_DASHBOARD_CHECK=0`
(`GW_ENV`) switches it off. `MG_LIGHTTPD=0` removes the wanted config and the
installed one.

**Live values.** The page receives the values as a stream (Server-Sent Events,
`/kbus/api/events`): the manager asks the gateway every 50 ms on the controller
(`MGMT_EVENT_MS`) and pushes only the changes over one open connection, so a
change shows up within about 50–100 ms. Without the stream (e.g. a proxy that
buffers), the page falls back to polling every 300 ms (I/O) or 900 ms. The
lighttpd config passes the stream on unbuffered (`server.stream-response-body = 2`
for `/kbus/api/events` in `kbus.conf`).

The page has the layout of the controller's Web-based Management (WBM): tab
menu at the top, page menu on the left, collapsible panels, status bar with
date, time and LEDs (KBUS, gateway, writes, login). Fonts, logo and favicon
come from the controller's own WBM at run time (`MGMT_WBM_URL`, default the
Docker host); without them the page uses system fonts.

| Tab | Pages |
|---|---|
| Status | *KBUS Status*: state, cycle time, cycles, errors, timing (`MaxCycleDuration`, `Overruns` ...), *Control* (remote / local), *Reset Statistics* 🔒 (cycles, errors, maximum values, overruns), *Rescan KBUS*; *Modules* |
| I/O | *Process Image*: *Control* panel with *Take / Release Local Control* 🔒; digital inputs and outputs as ON/OFF boxes (green = ON; a click switches an output), analog values with bars, analog outputs to set |
| Parameters | all parameters with path, id, type, value, writeable, userSetting; filter; the id links to `/definition` |
| Registers | all registers with type and value, input field and *Set* |
| Settings | *KBUS*: cycle time 1–40 ms; *Login*: login on/off, user and password |

**Login.** Reset Statistics, Local Control and changing the login settings
always need the login. The **gear button** at the top right, next to *WBM*,
opens the login dialog (*Login* / *Logout*); the LED *Login* is yellow when not logged
in, green when logged in. The functions marked 🔒 are **only shown while logged
in**; the login holds for the browser tab. Until a password is set,
the login is **`admin` / `wago`**: change it on *Settings > Login* (the page
warns while the default is in use). *Require a login* on the same page also
closes the whole website; the browser then asks for user and password. The
setting is stored in `/home/kbus-manager/settings.json` on the controller
(mode 600, password only as a PBKDF2 hash) and survives restarts and new
deployments. Forgotten password: `ssh root@<pfc-ip> rm /home/kbus-manager/settings.json`
and `docker restart kbus-manager` (back to `admin` / `wago`, website open).

**Local Control.** *I/O > Process Image > Control > Take Local Control* (login): from
then on only the dashboard may write the KBUS (outputs, cycle time, rescan).
CODESYS, the other containers and the TCP port get `403` (WDA code 57) for
KBUS writes; they keep reading, and registers stay writable. The LED *Local*
is yellow, and the state is kept after a restart of the gateway, until
*Release Local Control*. `FB_WdaKbusIo` pauses its writes meanwhile
(`xLocalControl`) and sends its outputs again after the release;
`FB_WdxKbusIo` goes offline until then.

How the gateway knows the dashboard: a second Unix socket,
`/run/kbus-gateway-control/control.sock` (`CONTROL_SOCKET`, directory mode
700, socket 600), mounted **only** into the gateway and the manager container.
Only requests on it may switch `KBus/LocalControl` and `KBus/ResetStatistics`,
and write the KBUS while local control is on. The manager passes those two
only on with a valid login.

The data layer is passed through, so all requests of section 3 work under
`/kbus` (with `-u user:password` when the login is on), e.g.
`curl -kL https://192.168.1.17/kbus/wda/parameters/0-0-kbus-digitaloutputs-3-value/definition`.
(`https://<pfc-ip>/wda` without `/kbus` is the firmware's own WDA service, not the gateway.)
Pages can be bookmarked (`/#/settings/cycle`). The link *WBM* (top right) opens the controller's Web-based Management.

| Setting | |
|---|---|
| login | *Settings* tab (above); `MGMT_USER` / `MGMT_PASSWORD` in `/home/kbus-manager/manager.env` are only used as long as no `settings.json` exists, `MGMT_NO_LOGIN=1` ignores them; then `MGMT_DEFAULT_USER` / `MGMT_DEFAULT_PASSWORD` (default `admin` / `wago`) |
| `MGMT_READONLY=1` | no writes (403): `MG_ENV="-e MGMT_READONLY=1" tools/deploy-docker.sh ... --manager --no-gateway` |
| `MG_PORT` | port of the manager container on the controller (default 8090) |
| `MG_BIND` | `127.0.0.1` (default): only through `/kbus`; `0.0.0.0`: also directly at `http://<pfc-ip>:MG_PORT/` |
| `MG_LIGHTTPD=0` | do not install the lighttpd config (no `/kbus`; combine with `MG_BIND=0.0.0.0`) |
| `MGMT_PREFIX` | path prefix the manager accepts, default `/kbus` |

With the website open (*Require a login* off), everyone on the network can
switch outputs and change registers; Reset Statistics, Local Control and the
login settings still need the login. The login is HTTP Basic: over `https://<pfc-ip>/kbus/` (HTTPS-only
mode, the default) it is encrypted; in *HTTP and HTTPS* mode it is not when
plain `http://` is used.

### 3.7a Serial interface COM1

The gateway also forwards **COM1** (`/dev/serial`, RS-232 or RS-485 as set in
the WBM, owner *None*) transparently over the data layer: `Serial/Com1/...`
(`Transmit`, `RxFrame`, `RxFrameCount`, `RxData`, `RxCount`, `TxCount`,
`Errors`, `Open`). The settings (baud rate, format such as `8E1`, frame gap)
are made on the client side: CODESYS sets them with `FB_WdaSerial` (inputs
`udiSetBaudrate`, `sSetFormat`, `uiSetFrameGap`); they are kept after a restart,
and the dashboard (*Settings > COM1*) shows them. CODESYS sends and
receives with `FB_WdaSerial`: [codesys/FB_WdaSerial.md](codesys/FB_WdaSerial.md).
`SERIAL_DEVICE` (default `/dev/serial`, empty = off), `SERIAL_BAUD`,
`SERIAL_FORMAT`, `SERIAL_FRAME_GAP_MS` are the defaults until a client
stores other values. KBUS and COM1 run in one gateway (container).

### 3.8 IEC 60870-5-104 server (kbus-iec104)

The container [docker/iec104](docker/iec104/README.md) offers the KBUS
digital inputs to a SCADA as IEC 104 single points with time tag
(`M_SP_TB_1`): spontaneous on a change, general interrogation, quality
Invalid while the gateway cannot be read. It uses the Unix socket like the
other containers.

```bash
tools/deploy-docker.sh 192.168.1.17 --iec104 --no-gateway
```

Port 2404, common address 1, `DI1..DI4` → IOA 1..4. Mapping and poll rate:
`/home/kbus-iec104/wdx_iec104_config.ini` on the controller, then
`docker restart kbus-iec104`. Details: [docker/iec104/README.md](docker/iec104/README.md).

---

## 4. Configuration

`/etc/kbus-gateway.conf` on the PFC (environment variables for the program;
apply with `/etc/init.d/kbus-gateway restart`):

| Variable | Default (program) | Package file | Description |
|---|---|---|---|
| `WDX_SOCKET` | `/run/kbus-gateway/wdx.sock` | same | Unix socket for containers (3.6); empty = off |
| `WDX_SOCKET_MODE` | `0660` | `0660` | file permissions of the socket (octal) |
| `WDX_SOCKET_GROUP` | – | – | group of the socket (e.g. for non-root container users) |
| `HTTP_ENABLE` | `1` | `1` | `0` = TCP listener off |
| `HTTP_BIND` | `127.0.0.1` | `127.0.0.1` | address of the TCP listener; `0.0.0.0` = all networks (web page from a PC) |
| `HTTP_PORT` | `8000` | `8000` | TCP port. Not 80/443: the WBM uses them |
| `HTTP_WRITE_ENABLE` | `1` | `1` | `0` = KBUS outputs (and `Rescan`, `ResetStatistics`) read-only: PATCH answers 403. Registers stay writable |
| `HTTP_LOG` | `1` | `1` | 0 none, 1 writes that change an output, 2 every request (3.4) |
| `HTTP_PRIORITY` | `0` | `30` | SCHED_FIFO priority of the server threads, 0 = normal. Keep it **below** `KBUS_PRIORITY` |
| `WDX_MAX_CLIENTS` | `16` | `16` | connections served at the same time, 1–64 (3.3) |
| `WDX_CLIENT_TIMEOUT_MS` | `5000` | `5000` | time a client has to send its request, 100–60000 ms |
| `REGISTER_FILE` | `/etc/kbus-gateway-registers.conf` | same | register declarations (3.6) |
| `REGISTER_STORE` | `/home/kbus-gateway/registers.dat` | same | persistent register values; empty = not stored |
| `REGISTER_SAVE_S` | `10` | `10` | persistent values are written at most every N seconds, 1–3600 |
| `KBUS_CYCLE_MS` | `5` | `5` | KBUS cycle, 1–40 ms (stay well below the 50 ms output watchdog; check section 5 after a change). A value written to `KBus/CycleTime` (data layer or manager, *Status* tab) wins and is kept in `KBUS_SETTINGS_STORE` |
| `KBUS_SETTINGS_STORE` | `/home/kbus-gateway/kbus-settings.conf` | same | cycle time written via `KBus/CycleTime`; delete the file to use `KBUS_CYCLE_MS` again; empty = not stored |
| `KBUS_PRIORITY` | `40` | `40` | SCHED_FIFO priority of the KBUS cycle thread, 0 = normal |
| `LOG_TARGET` | stderr | `syslog` | `syslog` = `/var/log/messages` |
| `KBUS_SIMULATION` | `0` | – | `1` = simulated node, no hardware (development) |
| `KBUS_SIM_HOTPLUG` | – | – | simulation only: path of a file; while it exists a 6th module (8DI) is "plugged in" (tests the rescan) |
| `KBUS_DAL_LIB` | `libdal.so:libdal.so.0:libdal.so.1` | – | `dlopen()` candidates for the ADI/DAL library |
| `KBUS_INFO_LIB` | `libdbuskbuscommon.so:...` | – | candidates for the KBUS-info library |
| `KBUS_DIGITAL_OFFSET_MODE` | `auto` | – | `auto`, `relative` or `absolute` (section 1) |

---

## 5. Speed and determinism

**KBUS cycle of 5 ms** (default since 2.2.0, was 10 ms). Inputs are at most
5 ms old and a written output reaches the module within 5 ms. Whether 5 ms
holds depends on the node (number and type of modules) and on the load of the
controller. Check it on the real node with the timing parameters:

```bash
S="--unix-socket /run/kbus-gateway/wdx.sock"
curl -s $S -X PATCH -H 'Content-Type: application/vnd.api+json' \
  -d '{"data":{"type":"parameters","id":"0-0-kbus-resetstatistics","attributes":{"value":true}}}' \
  http://x/wda/parameters/0-0-kbus-resetstatistics
# ... run the application for a while, then:
curl -sg $S "http://x/wda/parameters?filter[id]=0-0-kbus-maxcycleduration,0-0-kbus-maxwakeupdelay,0-0-kbus-overruns,0-0-kbus-errorcount"
```

| Value | Fine at 5 ms | Meaning when exceeded |
|---|---|---|
| `MaxCycleDuration` | well below 5000 µs (e.g. < 2500) | the KBUS exchange itself is too long for the period: raise `KBUS_CYCLE_MS` |
| `MaxWakeupDelay` | a few 100 µs | other real-time threads (CODESYS) delay the cycle |
| `Overruns` | 0 (or very rare) | cycles started late; the next one is planned from "now" |
| `ErrorCount` | not increasing | KBUS errors |

The output watchdog of the modules (50 ms) leaves a large margin in any case.
Shorter cycles cost CPU time (one `libpackbus_Push` per cycle); the PFC300
has two cores.

**Request times.** Measured on the PFC300 (FW 04.08.09(30), version 1.1.3, same wdx code as
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

The Unix socket saves the TCP connection setup, which was most of the time
per request in the table above; containers should use it. (Version 2.2.0 has
not been measured on the PFC yet.)

For a CODESYS client the exchange time is set by its **task cycles** (each
request needs a few: connect, write, read), not by the gateway. One exchange
of the CODESYS reference ([codesys](codesys/README.md)) is a fixed
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
   *HowTo_ADI-MyKBusApplikation* (`docs/reference/`, MIT licence): ADI/DAL device
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
8. **Dynamic configuration (2.1.0).** Modules added while the gateway was
   running were not visible (the module list was read once; the KBUS reported
   2247 errors). Now persistent cycle errors trigger a rescan, with
   `KBus/ConfigurationVersion`, `KBus/Rescan` and an updating web page.
   Tested with a simulated hot-plug (`KBUS_SIM_HOTPLUG`, module added and
   removed, web page in headless Chrome) and with a manual rescan on the
   PFC300 (KBUS closed and re-initialised, 0 errors afterwards).

9. **Containers and registers (2.2.0).** The gateway became the interface
   between containers and the KBUS, following the WDx pattern of WAGO
   (training *WDx 2024ff*): one data layer, a KBUS provider and a register
   provider, client access over IPC instead of WDA. On the PFC300 (FW
   04.08.09) WAGO's own ParameterService `paramd` runs with a provider socket
   (`/var/run/paramd/wdxprov/backend.socket`, library `libwdxlinuxoscom`),
   but its headers are not in the SDK, so the gateway does not register with
   it yet. Added: Unix socket, several clients at the same time with
   time-outs, registers (config file, five types, persistence), batch PATCH,
   TCP only on `127.0.0.1` by default, 5 ms KBUS cycle with timing
   parameters. Tested with the simulation node (`wda`, `registers`, `clients`,
   `forbidden` over TCP and the Unix socket, persistence over a restart) and
   on the PFC300 with CODESYS running: native and, again, as a container
   (2.8), with an example client container that only has the socket.
10. **Management and IEC 104 (2.3.0).** `KBus/CycleTime` became writable
   (1–40 ms, effective at the next cycle, stored in `KBUS_SETTINGS_STORE`).
   Around the gateway: the manager container (web page, optional login set
   on the page), the IEC 60870-5-104 container (from the project
   `docker-wdx-104`, now on the socket) and one shared `wdx_client.py`.

### 6.2 Source structure

The program is plain C (gnu11), about 3000 lines, no dependencies besides
glibc. The KBUS cycle thread (`gateway.c`) and the server threads
(`http_server.c` + `wda.c`: a main thread and one thread per client) share
only the process images and channel values, behind one mutex with priority
inheritance. The registers have their own mutex. A read-write lock protects
the parameter tree: requests read it, a rescan rebuilds it.

| File | Role |
|---|---|
| `src/main.c` | startup order, signals, memory locking (`mlockall`) |
| `src/config.c/.h` | environment variables → `gw_config_t` |
| `src/backend_adi.c` | real KBUS: `dlopen` of `libdal`, ADI device scan, KBUS-info, cycle |
| `src/backend_sim.c` | simulated node (12 DI, 8 DO, 4 AI, 4 AO; DI1–8 follow DO1–8, AI4 follows AO1) |
| `src/gateway.c/.h` | channel map, process images, real-time cycle thread, bulk read/write under one lock |
| `src/http_server.c/.h` | Unix socket + TCP listener, one thread per client, time-outs, rescans between requests, request log |
| `src/wda.c/.h` | wdx data layer: parameter table (built once per configuration), JSON:API output, minimal JSON reader, single and batch PATCH |
| `src/registers.c/.h` | register provider: declarations file, values, JSON parsing, persistence |
| `src/serial.c/.h` | COM1: transparent forwarding, receive thread, frames by pause (`FrameGap`), settings |
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
├── README.md, LICENSE, VERSION, Makefile
│
│   the gateway (one program, two ways to install it)
├── src/                       the program in C (6.2)
├── web/index.html             the web page at / (3.5), built into the program
├── sdk/                       SDK headers (imported, 2.3) + OsCommon.h stand-in
├── packaging/ipk/             native package: init script, config + register files, opkg scripts
├── docker/                    containers, one folder each (docker/README.md)
│   ├── gateway/               the gateway as a container (2.8): Dockerfile, entrypoint.sh
│   ├── manager/               management web interface, optional login (3.7)
│   ├── iec104/                IEC 60870-5-104 server for the KBUS inputs, port 2404 (3.8)
│   ├── example-client/        example application using the socket
│   └── lib/wdx_client.py      Python client library shared by the containers
│
│   clients
├── codesys/                   CODESYS project + ST code via the wdx data layer (README, st/, prompt)
│
│   build, deploy, test
├── tools/
│   ├── build-ipk.sh           build the .ipk with the SDK toolchain (2.4)
│   ├── deploy.sh              native: build + copy + opkg install + status
│   ├── deploy-docker.sh       containers: gateway, example client, manager, or back to native
│   ├── import-sdk-packages.sh headers from the prebuilt SDK packages (2.3 A)
│   ├── import-sdk-headers.sh  headers from a built SDK sysroot (2.3 B)
│   └── wdx-chat.py            answer CODESYS by hand (register Python/Response, 3.6)
├── tests/
│   ├── test-rest.py           wdx, registers, clients (simulation), real node read-only / write
│   └── test-registers.conf    registers for the simulation tests
│
│   documentation
├── docs/
│   ├── WDX-communication-handout.md   for developers of containers that use the gateway
│   └── reference/             WAGO material: HowTo_ADI-MyKBusApplikation (MIT), WDX training (PDFs, not in git)
│
│   generated (not in git)
├── build/                     native PC build (make)
├── build-ipk/                 .ipk packages (tools/build-ipk.sh)
└── build-docker/              image archives for the controller (tools/deploy-docker.sh)
```

### 7.2 Development loop (on the PC, no PFC needed)

```bash
make run-sim                                   # native build, simulated node on http://localhost:8000
tests/test-rest.py 127.0.0.1 wda               # wdx data layer: all requests, PATCH, error codes
tools/deploy.sh 192.168.1.17                   # then: build .ipk, install on the PFC, status
```

All simulation tests, over the Unix socket (the socket path must be shorter
than 108 characters, hence the relative path):

```bash
make
KBUS_SIMULATION=1 KBUS_PRIORITY=0 WDX_SOCKET=sim.sock WDX_CLIENT_TIMEOUT_MS=1000 WDX_MAX_CLIENTS=4 \
  REGISTER_FILE=tests/test-registers.conf REGISTER_STORE=/tmp/registers.dat build/kbus-gateway &
for m in wda registers clients; do tests/test-rest.py - $m --unix sim.sock --max-clients 4; done
```

`registers` expects the default values, so start with an empty
`REGISTER_STORE`. `forbidden` needs a gateway with `HTTP_WRITE_ENABLE=0`.

The VS Code tasks (*Terminal → Run Task*) cover the same steps. Without
`sdk/include/dal` the native build is simulation-only, which is enough for
all data-layer and channel-map work.

### 7.3 Common changes

**Change the wdx data layer.** The parameter tree is built in `build_list()`
in [src/wda.c](src/wda.c); paths in `path_of()`, types in `dtype_of()`,
values in `write_value()`. Ids follow automatically from the paths.

**Add a register type.** `reg_type_t`, `registers_parse()` and
`registers_format()` in [src/registers.c](src/registers.c), `dtype_of()` in
[src/wda.c](src/wda.c).

**Support another module type.** Module classification is in
`build_channel_map()` in [src/gateway.c](src/gateway.c).

**Release.** Raise [VERSION](VERSION), `tools/build-ipk.sh arm64`, install.

### 7.4 Rules to keep

- Never do slow work (I/O, logging, allocation) in `cycle_thread()`. Every
  cycle must finish well within 50 ms, or the output modules switch off.
- Keep `HTTP_PRIORITY` below `KBUS_PRIORITY`.
- Read and write channels only through the `gateway_*` functions, registers
  only through the `registers_*` functions; they take care of locking.
- `wda_handle()` runs in several threads at once: no static buffers in the
  request path. The parameter tree is only rebuilt (`wda_init()`) while the
  server holds the tree lock for writing.
- Build against headers with the **same DAL version** as the PFC's
  `libdal.so` (2.3). The ADI interface table is version-specific.
- Build with the toolchain of the controller's firmware SDK, so the binary
  does not need a newer glibc than the firmware has.

---

## 8. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `opening the KBUS device failed` | another program owns the KBUS: firmware runtime not *None*, a CODESYS project with KBUS, or a second gateway (e.g. an old Docker container) |
| `cannot listen on 127.0.0.1:8000: Address in use (retrying)` | something else uses the port (a second gateway, a CODESYS application); choose another `HTTP_PORT` |
| `cannot listen on /run/kbus-gateway/wdx.sock: another gateway is running` | a second instance (or an old container) still answers on the socket; stop it |
| Container: `No such file or directory` / `Connection refused` on the socket | mount the **directory** `/run/kbus-gateway`, not the socket file; the gateway must be running |
| Container: `Permission denied` on the socket | the container user is not root and not in `WDX_SOCKET_GROUP`; set the group or `WDX_SOCKET_MODE` |
| `503` answers | more than `WDX_MAX_CLIENTS` connections at the same time; clients that connect and wait hold a slot until `WDX_CLIENT_TIMEOUT_MS` |
| Web page not reachable from the PC | the gateway listens on `127.0.0.1` only (default): use the dashboard at `http://<pfc-ip>/kbus` (3.7) |
| `/kbus` gives 404 after a firmware update | repaired automatically at the next start of the gateway container (3.7); at once: `docker restart kbus-gateway`, or `tools/deploy-docker.sh <pfc-ip> --manager --no-gateway`. See `grep kbus-dashboard /var/log/messages` |
| Manager: login keeps being asked | wrong password; forgotten: delete `/home/kbus-manager/settings.json` on the controller and `docker restart kbus-manager` |
| Manager: "gateway not reachable" | the gateway is not running, or `/run/kbus-gateway` is not mounted into the manager |
| `registers: ... line ignored` in the log | syntax error in `/etc/kbus-gateway-registers.conf` (3.6) |
| Persistent registers back at their default after a restart | the name or type was changed, `persistent` removed, or `REGISTER_STORE` not writable (log: `cannot write`) |
| `KBus/Overruns` increasing | the cycle time does not hold on this node: raise it (`KBus/CycleTime` or the manager's *Status* tab, section 5) |
| Cycle time ignores `KBUS_CYCLE_MS` | a value written via `KBus/CycleTime` is stored in `/home/kbus-gateway/kbus-settings.conf` and wins: delete that file and restart |
| `could not load the ADI/DAL library` | check `ls /usr/lib/libdal*` and set `KBUS_DAL_LIB` |
| `this binary was built WITHOUT the WAGO ADI/DAL headers` | built without `sdk/include/dal`: import the headers (2.3) and rebuild |
| `version 'GLIBC_2.xx' not found` | built with a newer toolchain than the firmware: use the SDK builder image of the firmware |
| `opkg`: dependency `dal` / `libdbuskbuscommon` missing | firmware without KBUS support (not a PFC with KBUS) |
| `real-time scheduling not permitted` | not started as root; use the init script |
| `ldkc_KbusInfo_Create failed` | D-Bus not running yet; start later or check `/run/dbus/system_bus_socket` |
| Channel numbers point to the wrong I/O | compare the startup module list with the node; try `KBUS_DIGITAL_OFFSET_MODE=relative` or `absolute` |
| Outputs drop out briefly | look for `KBUS cycle failing` in the log; lower system load or the cycle time |
| Requests take ~10 ms now and then | CODESYS real-time threads (section 5); not a gateway fault |
| New modules not visible | look for `KBUS rescan` in the log; start one with the **Rescan KBus** button or `PATCH KBus/Rescan` = `true` |
| `KBUS rescan failed; next attempt in … s` | the node cannot be initialised (defective module, end module missing); fix the node, then rescan |

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
- Changes of the node are detected through KBUS errors (section 1). A change
  that causes no KBUS error is only picked up by `KBus/Rescan`, the web page
  button or a restart.
- **Only one KBUS master**: the gateway and a CODESYS application with KBUS
  access cannot run at the same time.
- Tested on real hardware with digital outputs only (one 8DO module, PFC300
  and PFC200). Digital inputs, analog modules and the 750-495 were tested with
  the simulation node. The `armhf` package for PFC100/PFC200 builds but has
  not been installed yet.
- **The wdx data layer has no authentication** (unlike the firmware's own
  WDA). Access is controlled by the file permissions of the Unix socket and,
  for TCP, by `HTTP_BIND` (default `127.0.0.1`: only programs on the
  controller). Every container that gets the socket mounted can read all I/O,
  switch outputs and write all registers. With `HTTP_BIND=0.0.0.0` anyone who
  can reach port 8000 can do the same: then use it only on an isolated
  machine network, set `HTTP_WRITE_ENABLE=0` when outputs need not be
  written, and block the port in the PFC firewall for other networks.
- Registers have no owner: every client may write every register.
- The gateway does not register with WAGO's ParameterService (`paramd`), so
  its parameters do not appear in the firmware's WDA (6.1, item 9).
- On `stop` and `opkg remove` all outputs are switched to 0. If the program
  crashes, the output modules switch off on their own after 50 ms.
