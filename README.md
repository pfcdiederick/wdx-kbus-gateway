# KBUS HTTP Gateway

A Docker application for WAGO PFC100 / PFC200 / PFC300 controllers that reads and writes
the channels of the **DI / DO / AI / AO** modules on the KBUS and makes them
available through a REST interface.

For single channels, the REST interface behaves exactly like the CODESYS
application *REST I/O server on PFC200* (reference behaviour version 1.0,
11-11-2026): same port, same requests, same response bytes. On top of that,
**bulk requests** read or write all channels in one request (see
[3.4](#34-bulk-requests-all-channels-in-one-request)).

```
GET  http://<pfc-ip>:8000/DI/1             -> DI1=TRUE      first digital input
GET  http://<pfc-ip>:8000/AI/2             -> AI2=16384     second analog input (raw value)
POST http://<pfc-ip>:8000/DO/3  body TRUE  -> DO3=TRUE      switch the third digital output on
GET  http://<pfc-ip>:8000/ALL              -> DI=0110;DO=00100000;AI=16384;AO=0     everything
POST http://<pfc-ip>:8000/ALL  body DO=00100001;AO=500  -> write all outputs, read everything
```

It is based on the WAGO example *HowTo_ADI-MyKBusApplikation* in
[`Information/`](Information/HowTo_ADI-MyKBusApplikation/README.md). It uses the
same ADI/DAL calls (`kbusdemo.c`) and the same KBUS module discovery
(`getkbusinfo.c`).

---

## Contents

1. [How it works](#1-how-it-works)
2. [Installation](#2-installation)
3. [Using the gateway](#3-using-the-gateway)
4. [Configuration](#4-configuration)
5. [Modifying the application](#5-modifying-the-application)
6. [Troubleshooting](#6-troubleshooting)
7. [Limitations and security](#7-limitations-and-security)

---

## 1. How it works

```
 Browser / SCADA / script                      PFC100 / PFC200 / PFC300
 ───────────────────────            ┌──────────────────────────────────────────────┐
                                    │ Docker container "kbus-gateway"              │
  GET  /DI/1 ───────────────────────┼─▶ REST server (port 8000)                    │
  POST /DO/1  "1"                   │        │ read / write                        │
                                    │        ▼                                     │
                                    │   channel map + process images (mutex)       │
                                    │        ▲                                     │
                                    │        │ every 10 ms, SCHED_FIFO prio 40     │
                                    │   KBUS cycle thread                          │
                                    │    WriteBytes → libpackbus_Push → ReadBytes  │
                                    │        │ dlopen()                            │
                                    └────────┼─────────────────────────────────────┘
                                             ▼  (bind-mounted from the firmware)
                                    /usr/lib/libdal.so, /usr/lib/dal/libpackbus.so,
                                    KBUS-info over D-Bus  ──▶  KBUS  ──▶  I/O modules
```

**Startup**

1. The WAGO libraries (`libdal.so` and dependencies) are loaded with `dlopen()`
   from the controller's own `/usr/lib`, which is mounted read-only into the
   container. The image therefore always uses the libraries that belong to the
   installed firmware.
2. The KBUS device (`libpackbus`) is opened and set to application state
   *Unconfigured*. The modules are then read through the KBUS-info D-Bus
   interface, as in `getkbusinfo.c`: position, type, bit offsets and sizes,
   and number of channels.
3. A **channel map** is built, as in the reference application. Channels are
   numbered from 1, separately for each type, in module order from left to
   right. At most 250 modules and 250 channels per type are handled.
   - **Digital module** (type code bit 15 set): every input bit is a `DI` and
     every output bit is a `DO`. Only channels 1–16 of a module exchange
     data; a higher channel still takes a number but reads `FALSE` (DI) or
     only keeps the written value (DO).
   - **Analog module** with only input data → `AI`, with only output data → `AO`.
     Channel *k* is the unsigned 16-bit word at byte offset 2·(k−1) of the
     module data (at most 48 bytes), raw value.
   - **750-495** gets no channels; its measured values are not available.
   - **Other modules** (data in both directions, e.g. counters or serial
     interfaces) get no channels; their outputs stay 0.
4. The KBUS is switched to application state *Running*. From then on the
   gateway drives every bus cycle itself, as in `kbusdemo.c`.

**Cycle thread.** Every `KBUS_CYCLE_MS` (default 10 ms) a real-time thread
writes the output image, triggers one cycle with `libpackbus_Push` and reads
the input image. WAGO output modules switch their outputs off if no cycle
arrives within **50 ms**, so this thread runs with `SCHED_FIFO` priority and
the REST server never blocks it.

**HTTP requests** only touch an in-memory copy of the process images:

- Reading an input returns the value of the last bus cycle.
- Writing an output changes the output image; the next cycle (≤ 10 ms later)
  sends it to the module.
- Reading an output returns the value that was last commanded.

**Shutdown.** `docker stop` sends SIGTERM. The gateway sets all outputs to 0,
runs one last cycle and closes the KBUS device.

**Digital offset.** In the process image the digital data sits behind the
analog data. The gateway asks libpackbus where the digital area starts
(`libpackbus_get_dig_offset`). If the KBUS-info offsets of digital modules
are relative to that area, they are shifted; this is detected automatically
and logged at startup. See `KBUS_DIGITAL_OFFSET_MODE`.

---

## 2. Installation

This is the complete procedure as it was carried out and tested on a
**PFC200 750-8217, firmware 04.09.50 (FW31)**, with the image built on a Linux
PC. Commands marked *PC* run in the project directory on the development PC;
commands marked *PFC* run on the controller (`ssh root@<pfc-ip>`). The
examples use `192.168.1.17`.

For a **PFC300** (64-bit) the procedure is the same apart from the points in
[2.10](#210-pfc300-64-bit). [tools/deploy.sh](tools/deploy.sh) detects the
controller type itself.

Quick route once everything is set up (steps 2.3–2.6 in one go):

```bash
tools/deploy.sh 192.168.1.17                       # PC: build, save, copy, load, start
tools/test-rest.py 192.168.1.17 node               # PC: read-only check
```

### 2.1 Requirements

| Where | What |
|---|---|
| PC | Linux with Docker and **docker buildx** (`docker buildx version`). No QEMU needed: the build cross-compiles |
| PC | Python 3 (only for the test script) and SSH/scp access to the PFC as `root` |
| PC | A **WAGO firmware SDK** checkout with its prebuilt packages, e.g. `~/Documents/WagoFirmware/pfc300-firmware-sdk` (see 2.2) |
| PFC | Docker enabled (WBM → *Docker*) |
| PFC | No PLC application using the KBUS: only **one KBUS master** is allowed (see 2.4) |

Check the PFC once (*PC*):

```bash
ssh root@192.168.1.17 'cat /etc/REVISIONS; docker info -f "{{.DockerRootDir}}"; ls -l /usr/lib/libdal.so*; /lib/libc.so.6 | head -1'
```

Note the firmware version, the `libdal.so.<version>` and the glibc version
(FW31: `libdal.so.1.0.1`, glibc 2.35).

### 2.2 Get the SDK header files

The real KBUS backend is compiled against the ADI/DAL and KBUS-info **header
files** of the WAGO SDK. The libraries themselves are not built in; at runtime
the gateway loads the controller's own libraries.

> **Not in the git repository.** The WAGO headers are proprietary, so a fresh
> clone has no `sdk/include/dal/`. Import them with one of the two ways below
> before building the real (non-simulation) image.

Two ways:

**A. From the prebuilt packages (no SDK build needed; used for the test).**
Every WAGO firmware SDK checkout contains the packages
`dal_<version>_<arch>.tgz` and `libdbuskbuscommon_<version>_<arch>.tgz` in
`ptxproj/configs/<platform>/packages/` (*PC*):

```bash
tools/import-sdk-packages.sh ~/Documents/WagoFirmware/pfc300-firmware-sdk/ptxproj/configs/wago-pfc300/packages
```

This fills `sdk/include/` with `dal/*`, `ldkc_kbus_information.h` and a
minimal `OsCommon.h` stand-in (types and attribute macros only; the real file
belongs to WAGO's OsLinux library, which has no package).

The packages of the **PFC300 SDK (64-bit)** work for a **PFC200 (32-bit)**:

- Headers are source code without architecture-specific parts. The Docker
  build compiles them with the 32-bit ARM compiler, so pointers and `size_t`
  get the PFC200 sizes.
- What must match is the **API version**: the script prints the DAL version
  of the package; it must equal `libdal.so.<version>` on the PFC (2.1).
  FW30 PFC300 `dal_1.0.1` = FW31 PFC200 `libdal.so.1.0.1`. The exported
  `ldkc_KbusInfo_*` functions were compared and are identical.
- If the versions differ, use the SDK of the PFC's firmware.

**B. From a built SDK (ptxdist).** When the SDK has been built completely
(`platform-wago-pfc*/sysroot-target/usr/include/dal/` exists):

```bash
tools/import-sdk-headers.sh ~/wago/ptxproj
```

> Without `sdk/include` only a **simulation** image can be built
> (`REQUIRE_ADI=0`), which is enough to test the REST interface (2.8).

Check (*PC*): `make` must report `with WAGO ADI/DAL backend`.

```bash
make && make clean
```

### 2.3 Build the image with buildx and save it

*PC*:

```bash
docker buildx build --platform linux/arm/v7 --build-arg REQUIRE_ADI=1 -t kbus-gateway:latest --load .
docker save kbus-gateway:latest | gzip > kbus-gateway-armv7.tar.gz
```

The archive `kbus-gateway-armv7.tar.gz` (about 26 MB) is saved in the project
directory (`-arm64` for a PFC300, see 2.10). `docker load` reads gzip directly, so it does not need unpacking.

How the build works (see [Dockerfile](Dockerfile)):

- The **build stage** runs natively on the PC and cross-compiles with
  `arm-linux-gnueabihf-gcc` (Debian trixie). The log must show
  `built build/kbus-gateway: with WAGO ADI/DAL backend`.
- The **runtime stage** (`debian:trixie-slim`, linux/arm/v7) only copies the
  binary and [docker/entrypoint.sh](docker/entrypoint.sh), so no emulation is
  required.
- `REQUIRE_ADI=1` makes the build fail when `sdk/include` is missing.

### 2.4 Prepare the PFC

1. Enable Docker in the WBM (*Docker* page) if it isn't already.
2. Make sure no PLC application uses the KBUS. Either set WBM → *PLC
   Runtime* → runtime version **None** (permanent), or stop the runtime
   temporarily: `ssh root@192.168.1.17 /etc/init.d/runtime stop`.
   A running `pp_codesys3` process without an application is no problem; the
   gateway log shows `KBUS device opened` when the KBUS is free.
3. Free space for the image (about 85 MB) in Docker's directory, e.g.
   `ssh root@192.168.1.17 df -h /media`.

### 2.5 Copy and load the image

*PC*:

```bash
scp kbus-gateway-armv7.tar.gz root@192.168.1.17:/tmp/kbus-gateway.tar.gz
ssh root@192.168.1.17 'docker load -i /tmp/kbus-gateway.tar.gz && rm /tmp/kbus-gateway.tar.gz && docker image prune -f'
```

`docker image prune` removes the previous, now untagged, image.

### 2.6 Start the container

*PFC* (`ssh root@192.168.1.17`):

```bash
docker rm -f kbus-gateway 2>/dev/null
docker run -d --name kbus-gateway --restart unless-stopped \
    --privileged --network host \
    -v /usr/lib:/host/usr/lib:ro \
    -v /var/run/dbus:/var/run/dbus \
    -v /etc/specific:/etc/specific:ro \
    -v /etc/config-tools/get_typelabel_value:/etc/config-tools/get_typelabel_value:ro \
    -v /dev/eeprom:/dev/eeprom \
    -v /dev/ttyKbus:/dev/ttyKbus \
    -v /dev/log:/dev/log \
    -e HTTP_PORT=8000 \
    kbus-gateway:latest
```

Every option is needed; each was found missing during the first test:

| Option | Why it is needed |
|---|---|
| `--privileged` | Real-time priority for the cycle thread, memory locking, and access to the KBUS hardware (`/dev/kbus0`) |
| `--network host` | The gateway listens directly on the PFC's IP address |
| `-v /usr/lib:/host/usr/lib:ro` | The firmware's ADI/DAL libraries and DAL device plugins (`/usr/lib/dal/libpackbus.so`). The entrypoint links them into `/tmp/host-libs` **without** the firmware's glibc, rebuilds `/usr/lib/dal` so its relative links resolve, and sets `LD_LIBRARY_PATH` itself |
| `-v /var/run/dbus:/var/run/dbus` | Module discovery (KBUS-info) uses the system D-Bus |
| `-v /etc/specific:/etc/specific:ro` | KBUS configuration (`kbusconf.xml`) and KBUS controller firmware read by `libpackbus` |
| `-v /etc/config-tools/get_typelabel_value:...:ro` + `-v /dev/eeprom:/dev/eeprom` | `libpackbus` runs this tool to read the device type label from the EEPROM |
| `-v /dev/ttyKbus:/dev/ttyKbus` | Serial link to the KBUS controller; `libpackbus` uses it to load the controller firmware |
| `-v /dev/log:/dev/log` | `libpackbus` reports to the PFC's syslog (`/var/log/messages`) |

Do **not** add `-e LD_LIBRARY_PATH=/host/usr/lib` (older versions of this
README did): the firmware's `libc.so.6` then clashes with the container's
loader. `/dev/eeprom` and `/dev/ttyKbus` are symlinks on the PFC; Docker does
not copy `/dev` symlinks into a container, hence the explicit mounts.

`--restart unless-stopped` starts the gateway again after a reboot or crash.

`docker run` refuses to start when a mounted path does not exist on the
controller. The last four mounts (`get_typelabel_value`, `/dev/eeprom`,
`/dev/ttyKbus`, `/dev/log`) may be missing on other controllers or firmware
versions; leave out the ones that do not exist (`ls -l <path>`).
[tools/deploy.sh](tools/deploy.sh) does that automatically and prints
`note: ... not present, not mounted`.

**Port number.** The gateway listens on port **8000** by default, the same
port as the CODESYS reference. Ports 80 and 443 are taken by the WBM. If the
port is in use, the log shows `cannot listen on port ...: Address in use
(retrying)` and the gateway keeps retrying. Another port: `-e HTTP_PORT=8081`
or `tools/deploy.sh 192.168.1.17 8081`.

**Everything in one command.** [tools/deploy.sh](tools/deploy.sh) carries out
2.3, 2.5 and 2.6. It asks the controller for its architecture (`uname -m`),
builds for `linux/arm/v7` or `linux/arm64` accordingly and saves
`kbus-gateway-armv7.tar.gz` or `kbus-gateway-arm64.tar.gz`. `SKIP_BUILD=1`
reuses the existing archive; `PLATFORM=linux/arm64` overrides the detection.

```bash
tools/deploy.sh 192.168.1.17
```

### 2.7 Check the start

*PC*:

```bash
ssh root@192.168.1.17 'docker ps -a --filter name=kbus-gateway; docker logs kbus-gateway'
```

A good start (log of the test node, one 8DO module):

```
[info]  WAGO KBUS HTTP gateway starting
[info]  config: rest=on port=8000 writes=allowed cycle=10ms rt-prio=40 mode=kbus
[info]  loaded libdal.so
[info]  loaded libdbuskbuscommon.so
[info]  KBUS device opened
[info]  application state set to 'Unconfigured'
[info]  KBUS: 1 modules, error code 0, bits AI=0 AO=0 DI=0 DO=8, image in=12000 out=12000 bytes
[info]  digital area starts at byte in=0 out=0; KBUS-info digital offsets are absolute
[info]  process image used: in 0 of 12000 bytes, out 1 of 12000 bytes
[info]  application state set to 'Running'
[info]  module  1: digital 8DO    in   0 bits @0     out   8 bits @0
[info]  channels: 0 DI, 8 DO, 0 AI, 0 AO; 0 x 750-495
[info]  KBUS cycle thread running: SCHED_FIFO priority 40, every 10 ms
[info]  REST server listening on port 8000 (e.g. GET /DI/1)
```

The status must be `Up`, not `Restarting`. Compare the module list with the
node. The `libpackbus` messages are in the PFC syslog:

```bash
ssh root@192.168.1.17 'grep -i kbus /var/log/messages | tail -20'
```

`Timeout on bootstrap header answer` followed by `Use still active SCPU-FW`
is normal when the KBUS controller firmware is already running.

### 2.8 Test

[tools/test-rest.py](tools/test-rest.py) compares every response byte for byte
with the reference behaviour (*PC*):

```bash
# 1. Real node, read only: finds the channel counts, checks reads and
#    invalid requests, writes nothing
tools/test-rest.py 192.168.1.17 node

# 2. Real node, writes: switches DO1 on/off with 3 s pauses so you can watch
#    the LED; ends with DO1 = FALSE. Check first what is connected!
tools/test-rest.py 192.168.1.17 write --do 1
tools/test-rest.py 192.168.1.17 write --ao 1          # analog output, ends at 0

# 3. All reference cases 1-31 and the special cases, on the simulation node
make run-sim &                                        # or a simulation container
tools/test-rest.py 127.0.0.1 sim
```

Every line must say `PASS` and the last line `FAILURES: 0`. A GET on an
output returns the value last written, not a read-back; watch the module's
LED (or measure) to confirm that the output really switches.

Run the tests from a PC on the same network. Security software or proxies
on the path may hold back requests that are not complete HTTP requests; in
`sim` mode the special cases `noEOL`, `min9`, `short` and `min10` then time
out. They pass when sent on the PFC itself, e.g.
`ssh root@192.168.1.17 '(printf "GET /DO/1"; sleep 1) | nc 127.0.0.1 8000'`.

**Simulation on the PFC** (REST test without touching the KBUS):

```bash
REQUIRE_ADI=0 IMAGE=kbus-gateway:sim EXTRA_RUN_ARGS="-e KBUS_SIMULATION=1 -e KBUS_PRIORITY=0" \
    tools/deploy.sh 192.168.1.17
tools/test-rest.py 192.168.1.17 sim
```

This saves `kbus-gateway-armv7-sim.tar.gz`, so the real archive is kept.
Run `tools/deploy.sh 192.168.1.17` (or with `SKIP_BUILD=1`) afterwards to go
back to the real gateway.

**Simulation on the PC.** The simulated node has a 4AI, 4AO, 8DI, 8DO and 4DI
module. DO1–8 are looped back to DI1–8, and AO1 to AI4.

```bash
make run-sim                                # native, http://localhost:8000
# or as a container:
docker buildx build --platform linux/amd64 --build-arg REQUIRE_ADI=0 -t kbus-gateway:sim --load .
docker run --rm -p 8000:8000 -e KBUS_SIMULATION=1 kbus-gateway:sim
```

### 2.9 Stop, update, remove

```bash
ssh root@192.168.1.17 docker stop kbus-gateway      # all outputs to 0, KBUS released
ssh root@192.168.1.17 docker start kbus-gateway
tools/deploy.sh 192.168.1.17                        # update to a new build
ssh root@192.168.1.17 'docker rm -f kbus-gateway; docker rmi kbus-gateway:latest'   # remove
```

Stop the container before a CODESYS application takes over the KBUS again.

### 2.10 PFC300 (64-bit)

The PFC300 (750-8302) drives the same 750/753 I/O modules over the KBUS with
the same WAGO libraries (`libdal`, `libpackbus`, `libdbuskbuscommon`). The
source code needs no change; the differences are in building and deploying.

| | PFC100 / PFC200 | PFC300 |
|---|---|---|
| CPU / `uname -m` | 32-bit ARM, `armv7l` | 64-bit ARM, `aarch64` |
| buildx platform | `linux/arm/v7` | `linux/arm64` |
| Cross compiler (chosen by the Dockerfile) | `arm-linux-gnueabihf-gcc` | `aarch64-linux-gnu-gcc` |
| Archive | `kbus-gateway-armv7.tar.gz` | `kbus-gateway-arm64.tar.gz` |
| SDK headers (2.2) | packages of a PFC200 or PFC300 SDK with the same DAL version | packages of the PFC300 SDK of the installed firmware (native match) |
| KBUS controller firmware | loaded by `libpackbus` over `/dev/ttyKbus` when needed | loaded at boot by the firmware itself (`/etc/init.d/check_kbusfw`; libpackbus is built with `NO_FIRMWARE_LOAD`) |

What stays the same (checked in the PFC300 SDK FW30-V04.08.09): glibc 2.35,
libraries in `/usr/lib`, DAL plugins as relative links in `/usr/lib/dal`,
`/dev/kbus0` and `/dev/ttyKbus`, `/etc/specific/kbusconf.xml` and
`get_typelabel_value`. The entrypoint and the run command of 2.6 therefore
apply unchanged. `/dev/eeprom` is not created by the PFC300 project files; if
it is missing on the controller, leave that mount out (deploy.sh does this
automatically).

Steps for a PFC300 (*PC*; `<pfc300-ip>` is its address):

```bash
# 1. Check the controller: architecture, firmware, DAL and glibc version, devices
ssh root@<pfc300-ip> 'uname -m; cat /etc/REVISIONS; ls -l /usr/lib/libdal.so*; /lib/libc.so.6 | head -1'
ssh root@<pfc300-ip> 'ls -l /dev/kbus0 /dev/ttyKbus /dev/eeprom /etc/specific/kbusconf.xml /etc/config-tools/get_typelabel_value'

# 2. Headers from the PFC300 SDK of that firmware (DAL version = libdal.so.<version>)
tools/import-sdk-packages.sh ~/Documents/WagoFirmware/pfc300-firmware-sdk/ptxproj/configs/wago-pfc300/packages

# 3. Build, save, copy, load, start (detects aarch64 -> linux/arm64)
tools/deploy.sh <pfc300-ip>

# 4. Check and test as in 2.7 and 2.8
ssh root@<pfc300-ip> docker logs kbus-gateway
tools/test-rest.py <pfc300-ip> node
```

By hand, step 3 is 2.3–2.6 with the other platform and archive name:

```bash
docker buildx build --platform linux/arm64 --build-arg REQUIRE_ADI=1 -t kbus-gateway:latest --load .
docker save kbus-gateway:latest | gzip > kbus-gateway-arm64.tar.gz
scp kbus-gateway-arm64.tar.gz root@<pfc300-ip>:/tmp/kbus-gateway.tar.gz
ssh root@<pfc300-ip> 'docker load -i /tmp/kbus-gateway.tar.gz && rm /tmp/kbus-gateway.tar.gz'
# then the docker run command of 2.6 on the PFC300
```

Status: tested on 16-11-2026 on a PFC300 750-8302 with firmware 04.08.09(30).
The arm64 image builds without compiler warnings, `tools/deploy.sh` ran
unchanged, the log showed `KBUS device opened`, the module (1 x 8DO) was
detected, and `GET /DO/1`–`/DO/8` answered. In that test the PFC300 also
ran CODESYS3 in a container with its KBus drivers switched off
(`KBUS=false`, see [docker-codesys3-wagopfc](https://github.com/pfcdiederick/docker-codesys3-wagopfc)),
so CODESYS can reach the gateway at `http://127.0.0.1:8000`.

**PFC100** (750-810x): 32-bit ARM like the PFC200, so the armv7 image and the
procedure of 2.1–2.9 apply as they are. Check the DAL version (2.1) and the
paths of the optional mounts (2.6).

---

## 3. Using the gateway

### 3.1 Endpoints

| Request | Condition | Response body (status 200) |
|---|---|---|
| `GET /DI/n` | n ≤ number of DI | `DIn=TRUE` or `DIn=FALSE` |
| `GET /DO/n` | n ≤ number of DO | `DOn=TRUE` or `DOn=FALSE`: the value last written |
| `GET /AI/n` | n ≤ number of AI | `AIn=<raw value>`, unsigned 0–65535 |
| `GET /AO/n` | n ≤ number of AO | `AOn=<value>`: the value last written |
| `POST /DO/n` | body `1`, `TRUE`, `0` or `FALSE` | `DOn=TRUE` / `DOn=FALSE` (new value) |
| `POST /AO/n` | body: decimal digits, 0–65535 | `AOn=<value>` (new value) |

```bash
curl http://192.168.1.17:8000/DI/1                 # DI1=TRUE
curl -X POST -d TRUE  http://192.168.1.17:8000/DO/1 # DO1=TRUE
curl -X POST -d 16384 http://192.168.1.17:8000/AO/1 # AO1=16384
```

Rules (identical to the CODESYS reference):

- Everything is **case-sensitive**: method `GET`/`POST`, `DI`/`DO`/`AI`/`AO`,
  `TRUE`/`FALSE`.
- The channel number is decimal digits only, greater than 0. Leading zeros
  are accepted (`/DO/01` is channel 1); the response writes it without them.
  A trailing slash, a query string or any other character gives 404.
- Headers are not evaluated (`Content-Length` and `Content-Type` have no
  effect) and the HTTP version is not checked.
- The POST body starts after the first empty line and ends at the first CR or
  LF. Nothing else is accepted: no lowercase, sign, decimal point, hex or
  spaces. An invalid body leaves the output unchanged.
- Values are **raw process-image values**, as in CODESYS. For example, for a
  750-559 (0–10 V), 0x7FFF = 10 V. Scale them in your application.

### 3.2 Responses

Every response is a status line, two headers, an empty line and a one-line
body without a line ending. There is no `Content-Length`; the server closes
the connection. Example (bytes):

```
HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nDO1=TRUE
```

| Status | Body | When |
|---|---|---|
| 200 OK | see 3.1 | valid request |
| 403 Forbidden | `Output writes are disabled` | `POST /DO/n` or `/AO/n` while `HTTP_WRITE_ENABLE=0` (checked before the channel count) |
| 404 Not Found | `Unknown endpoint, channel, or value` | everything else |

### 3.3 Connection handling

- One client at a time. The server reads **once, at most 512 bytes**, and
  decodes only those bytes. If headers and body arrive in separate TCP
  segments, the body is missing and the answer is 404. Send the request in
  one write (curl does this for small requests).
- There is no time-out: a client that connects and sends nothing blocks the
  server until it disconnects.
- An output keeps its value until it is written again; there is no client
  monitoring. After a start all outputs are `FALSE` / 0.

### 3.4 Bulk requests (all channels in one request)

An extension that is not in the CODESYS reference. Every single-channel
request in 3.1 costs one TCP connection. A client that polls many channels
spends most of its time on connections, and in a PLC every connection takes
several task cycles. Bulk requests exchange **all channels in one request**,
and they cost the gateway no more time than a single channel (about 0.15 ms
measured).

| Request | Body | Answer body (status 200) |
|---|---|---|
| `GET /DI`, `GET /DO` | – | `DI=0110...`: one char per channel, `0`/`1`, channel 1 first |
| `GET /AI`, `GET /AO` | – | `AI=0,16384,...`: decimal raw values, comma separated |
| `GET /ALL` | – | `DI=...;DO=...;AI=...;AO=...` (a class without channels gives e.g. `AI=`) |
| `POST /DO` | `0110-1`: `0`/`1` per channel from channel 1, `-` = leave unchanged | `DO=...` (all DO after the write) |
| `POST /AO` | `0,16384,-`: values from channel 1, `-` = leave unchanged | `AO=...` |
| `POST /ALL` | `DO=...;AO=...` (each part optional, any order) | as `GET /ALL`, after the write |

```bash
curl http://192.168.1.17:8000/ALL                                   # DI=;DO=00000001;AI=;AO=
curl -X POST -d 10000001 http://192.168.1.17:8000/DO                # DO1 and DO8 on, rest off
curl -X POST -d 'DO=1-------' http://192.168.1.17:8000/ALL          # only DO1 on
```

Rules:

- A body may hold **fewer** values than there are channels (the rest stays
  unchanged), never more: that gives 404.
- Bulk writes are **checked completely first and then applied under one lock**,
  so the next KBUS cycle sends all of them together. If one value is invalid,
  **nothing** is written (404).
- Same rules as 3.1–3.3 otherwise: case-sensitive, body after the first empty
  line up to the first CR/LF, **one read** (send the request in one TCP
  write), one client at a time. Bulk requests may be up to 4096 bytes;
  single-channel requests are still decoded from the first 512 bytes only.
- The paths without a channel number (`/DO`, `/ALL`) gave 404 before. So the
  extension changes nothing for clients of the reference behaviour;
  `tools/test-rest.py <host> sim` still passes unchanged.

Test with the simulation node: `tools/test-rest.py <host> bulk` (34 cases).

### 3.5 Live log

```bash
ssh root@192.168.1.17 docker logs -f -t kbus-gateway
```

`HTTP_LOG` (section 4) sets what the REST server logs:

| `HTTP_LOG` | Logged |
|---|---|
| `0` | no requests |
| `1` (default) | every successful write: `write DO3 = 1`, `write DO=10000001`, `write DO=1-------;AO=500` |
| `2` | **every request**, with client address, request line, status and answer, e.g. `127.0.0.1 "POST /ALL HTTP/1.0" -> 200 DI=;DO=10000000;AI=;AO=` |

Level 2 is meant for testing a client. A PLC that exchanges every 50 ms writes
20 lines per second; Docker on the PFC rotates the log (2 MB × 5 files), so the
storage does not fill up. Enable it with
`EXTRA_RUN_ARGS="-e HTTP_LOG=2" SKIP_BUILD=1 tools/deploy.sh <pfc-ip>`.

---

## 4. Configuration

Set these with `-e NAME=value` on `docker run`:

| Variable | Default | Description |
|---|---|---|
| `HTTP_PORT` | `8000` | REST port. Not 80 or 443: the PFC's web interface (WBM) already uses them (see 2.5) |
| `HTTP_ENABLE` | `1` | `0` = REST server off |
| `HTTP_WRITE_ENABLE` | `1` | `0` = `POST` on DO/AO answers 403 and writes nothing |
| `HTTP_LOG` | `1` | `0` = no request log, `1` = log every write, `2` = log every request (3.5) |
| `KBUS_CYCLE_MS` | `10` | KBUS cycle time, 1–40 ms (stay well below the 50 ms output watchdog) |
| `KBUS_PRIORITY` | `40` | SCHED_FIFO priority of the cycle thread, `0` = normal scheduling |
| `KBUS_SIMULATION` | `0` | `1` = simulated I/O, no hardware access |
| `KBUS_DAL_LIB` | `libdal.so:libdal.so.0:libdal.so.1` | `dlopen()` candidates for the ADI/DAL library |
| `KBUS_INFO_LIB` | `libdbuskbuscommon.so:...` | candidates for the KBUS-info library (only used when its symbols aren't already loaded) |
| `KBUS_DIGITAL_OFFSET_MODE` | `auto` | `auto`, `relative` or `absolute`: how digital offsets from KBUS-info are interpreted |

---

## 5. Modifying the application

### 5.1 Project layout

```
├── Dockerfile                 buildx multi-platform build (cross-compile + slim runtime)
├── Makefile                   native / cross build; enables the ADI backend when sdk/include exists
├── src/
│   ├── main.c                 startup, signals, wiring
│   ├── config.c/.h            environment variables → gw_config_t
│   ├── backend.h              backend interface (open / exchange / close)
│   ├── backend_adi.c          real KBUS via WAGO ADI/DAL + KBUS-info (dlopen)
│   ├── backend_sim.c          simulated node for testing
│   ├── gateway.c/.h           channel map, process images, real-time cycle thread
│   ├── http_server.c/.h       REST server (request decoding, responses)
│   └── log.h
├── docker/entrypoint.sh       container start: firmware libraries without the firmware's glibc
├── sdk/                       WAGO SDK headers (see 2.2 and sdk/README.md)
├── tools/
│   ├── import-sdk-packages.sh headers from the prebuilt SDK packages (2.2 A)
│   ├── import-sdk-headers.sh  headers from a built SDK sysroot (2.2 B)
│   ├── deploy.sh              build, save archive, copy, load and (re)start (2.3-2.6)
│   └── test-rest.py           reference-behaviour test (2.8)
└── Information/               original WAGO example and ADI/DAL documentation
```

### 5.2 Development loop

```bash
make run-sim                    # build natively + run in simulation on http://localhost:8000
make clean
```

The VS Code tasks (*Terminal → Run Task*) cover the same steps: native build,
simulation, buildx images and deploy.

The native build only compiles the ADI backend when `sdk/include` exists.
Without the SDK it is simulation-only. That is enough for all REST and
channel-map work.

### 5.3 Common changes

**Change the REST interface.** Requests are decoded in `decode()` in
[src/http_server.c](src/http_server.c). It is deliberately a byte-for-byte
copy of the CODESYS reference; any change makes the gateway differ from it.
Read and write channels only through the `gateway_*` functions; they take
care of locking.

**Support another module type.** Module classification is in
`build_channel_map()` in [src/gateway.c](src/gateway.c). For example, an analog
module with status bytes can get its own branch that skips the status byte and
calls `add_channel()` with the correct bit offset per channel.

**Change the simulated node.** Edit the `node[]` table and `sim_exchange()` in
[src/backend_sim.c](src/backend_sim.c). Offsets are absolute bit offsets:
analog data first, digital data behind it.

**Add another bus / fieldbus backend.** Implement the three functions of
`kbus_backend_t` ([src/backend.h](src/backend.h)) and select it in `main.c`.

### 5.4 Rules to keep

- Never do slow work (I/O, logging, allocation) in `cycle_thread()`. Every
  cycle must finish well within 50 ms, or the output modules switch off.
- Keep `KBUS_CYCLE_MS` < 50. The default of 10 ms leaves plenty of margin.
- Build against headers with the **same DAL version** as the PFC's
  `libdal.so` (2.2). The ADI interface table is version-specific.
- The runtime image glibc (Debian trixie) must be **at least as new** as the
  firmware's glibc, because the firmware libraries are loaded into the
  container's process. After a big firmware upgrade, check the startup log.

---

## 6. Troubleshooting

The first five rows are the errors met, in this order, during the first
start on the PFC200; the run command in 2.6 contains all fixes.

| Symptom (in `docker logs kbus-gateway`) | Cause / fix |
|---|---|
| `symbol lookup error: /host/usr/lib/libc.so.6: undefined symbol: __nptl_set_robust_list_avail` | `-e LD_LIBRARY_PATH=/host/usr/lib` is set: the firmware's glibc is loaded by the container's loader. Remove it; the entrypoint sets the library path |
| `ERROR: IsRegularFile(): stat() failed` + `ADI device scan failed` | The DAL plugins in `/usr/lib/dal` are not found. Do not mount `/usr/lib/dal` separately; use an image with [docker/entrypoint.sh](docker/entrypoint.sh) |
| `get_typelabel_value: not found` / `/dev/eeprom` + `opening the KBUS device failed` | Mount `get_typelabel_value` and `/dev/eeprom` (2.6). Also check that no PLC application owns the KBUS |
| `failed to load "/etc/specific/kbusconf.xml"` | Mount `/etc/specific:ro` |
| `process image larger than ... bytes` | Image built from an older version of the source; rebuild |
| Syslog: `SCPU_boot_slave: Cannot open devices` | Mount `/dev/ttyKbus` |
| `opening the KBUS device failed` (with all mounts) | A PLC application still owns the KBUS. Stop it (2.4) |
| `could not load the ADI/DAL library` | `/usr/lib` not mounted at `/host/usr/lib`. Check the library name on the PFC (`ls /usr/lib/libdal*`) and set `KBUS_DAL_LIB` |
| `version 'GLIBC_2.xx' not found` | The firmware is newer than the image's glibc: raise `DEBIAN_RELEASE` in the Dockerfile |
| `this binary was built WITHOUT the WAGO ADI/DAL headers` | Image built with `REQUIRE_ADI=0` or without `sdk/include`. Import the headers (2.2) and rebuild |
| Build: `WAGO SDK headers not found in sdk/include` | Import the headers (2.2) |
| `cannot listen on port ...: Address in use (retrying)` | The port is used by something else (e.g. a CODESYS application on 8000, or the WBM on 80/443). Choose another `HTTP_PORT` |
| `ldkc_KbusInfo_Create failed` | D-Bus socket not mounted (`-v /var/run/dbus:/var/run/dbus`) |
| `real-time scheduling not permitted` | The container is not `--privileged`. Outputs may drop out under load |
| Channel numbers point to the wrong I/O | Compare the startup module list with the node. Try `KBUS_DIGITAL_OFFSET_MODE=relative` or `absolute` |
| Outputs drop out briefly | Check the log for `KBUS cycle failing`; lower system load or the cycle time |
| Test: some special cases time out from the PC, pass on the PFC | Something on the network path holds back incomplete HTTP requests (2.8); not a gateway fault |
| Container status `Restarting` | `docker logs kbus-gateway` shows the reason; stop the loop with `docker rm -f kbus-gateway` while investigating |

**Investigating further.** The PFC has `strace`; mount it into the container
to see which files the WAGO libraries miss (this found the mounts in 2.6):

```bash
ssh root@192.168.1.17 'docker rm -f kbus-gateway; docker run --rm --privileged --network host \
  -v /usr/lib:/host/usr/lib:ro -v /var/run/dbus:/var/run/dbus -v /etc/specific:/etc/specific:ro \
  -v /etc/config-tools/get_typelabel_value:/etc/config-tools/get_typelabel_value:ro \
  -v /dev/eeprom:/dev/eeprom -v /dev/ttyKbus:/dev/ttyKbus -v /dev/log:/dev/log \
  -v /usr/bin/strace:/strace:ro --entrypoint /bin/sh kbus-gateway:latest -c \
  "sed -i \"s#^exec #exec /strace -f -e trace=%file -e status=failed #\" /usr/local/bin/entrypoint.sh; \
   timeout 20 /usr/local/bin/entrypoint.sh" 2>&1 | grep -v host-libs | tail -40'
```

Library names, mount paths and the digital-offset interpretation can differ
between firmware releases. The startup log shows which choice was made, and
the variables in section 4 override it.

---

## 7. Limitations and security

- Only plain digital modules and plain analog modules (16-bit words, data in
  one direction) are mapped. Complex modules and the 750-495 get no channels.
- Values are raw process-image values; there is no unit scaling.
- The module list is read once at startup. Restart the container after
  changing the node.
- **Only one KBUS master**: the gateway and a CODESYS application with KBUS
  access cannot run at the same time.
- Tested on real hardware with digital outputs only (one 8DO module). Digital
  inputs, analog modules and the 750-495 were tested with the simulation node.
- Not yet tested on a PFC300 or PFC100 (2.10); the arm64 image builds cleanly.
- Not yet tested: the first start after a power cycle with runtime **None**,
  where the gateway has to load the KBUS controller firmware itself
  (`/dev/ttyKbus`).
- **The REST interface is unencrypted and has no authentication**, exactly as
  the CODESYS reference. Anyone who can reach the port can read all I/O and
  switch outputs. Use it only on an isolated machine network, set
  `HTTP_WRITE_ENABLE=0` when writing is not needed, and put a TLS reverse
  proxy in front when it must be reachable more widely.
- On `docker stop` all outputs are switched to 0. If the container crashes,
  the output modules switch off on their own after 50 ms without a KBUS cycle.
