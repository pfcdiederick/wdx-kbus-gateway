# kbus-gateway

A small C program for WAGO PFC100 / PFC200 / **PFC300** controllers. It **owns the
KBUS** and makes it available to CODESYS, Docker containers, SCADA and scripts
through one interface: a **wdx data layer**, which is HTTP + JSON:API following the
WAGO REST-API conventions.

```
 CODESYS (host network) ──TCP 127.0.0.1:8000──┐
 containers, dashboard ──/run/kbus-gateway/wdx.sock──┼─▶ kbus-gateway ──▶ KBUS ──▶ I/O modules
                                                   │               ──▶ registers, COM1
```

**What it offers**
- **I/O.** DI / DO / AI / AO channels, as single values or whole arrays, read with
  GET and written with PATCH. All values of one request reach the modules in the
  same KBUS cycle.
- **Complex modules** (750-495, -657, -671, -650 …). The raw process data is exchanged
  byte by byte; the method `KBus/Exchange` writes and reads in one request. The
  gateway also offers the module list, the AI status (register 6) and
  `KBus/ReadRegister`.
- **Registers.** Shared values between clients, optionally persistent.
- **COM1.** Forwarded transparently.
- **Around it, as containers:** a dashboard at `http(s)://<pfc-ip>/kbus` (status,
  modules, all parameters; editable under local control), an IEC 60870-5-104 server
  (104-gateway) and an example client.

## Quick start

```bash
tools/import-sdk-packages.sh <firmware-sdk>/ptxproj/configs/wago-pfc300/packages   # once: WAGO headers
tools/deploy-docker.sh 192.168.1.17 --manager       # gateway + dashboard as containers (restarts the gateway)
ssh root@192.168.1.17 "curl -sg --unix-socket /run/kbus-gateway/wdx.sock 'http://x/wda/parameters?page[limit]=5'"
```

Then open `http://192.168.1.17/kbus` (login `admin` / `wago` until you set a
password).

```
GET   http://127.0.0.1:8000/wda/parameters?filter[id]=0-0-kbus-digitalinputvalues,0-0-kbus-analoginputvalues
PATCH http://127.0.0.1:8000/wda/parameters/0-0-kbus-digitaloutputs-3-value   {"data":{...,"attributes":{"value":true}}}
POST  http://127.0.0.1:8000/wda/methods/0-0-kbus-exchange/runs               {"data":{"type":"runs","attributes":{"inArgs":{...}}}}
```

Development without a controller: `make run-sim` (simulated node) and
`tests/test-rest.py 127.0.0.1 wda`.

## Documentation

**[docs/MANUAL.md](docs/MANUAL.md)** covers how it works, installation (container or
native package), the complete API, the dashboard, CODESYS, configuration,
troubleshooting and development.

## Layout

| Folder | |
|---|---|
| `src/`, `web/` | the program and its built-in web page |
| `docker/` | containers: kbus-gateway, manager (dashboard), 104-gateway (IEC 104), example-client, shared Python client |
| `packaging/ipk/` | native firmware package |
| `codesys/` | CODESYS project (uses the library WagoAppKbusWdx) |
| `tools/` | build, deploy, SDK header import |
| `tests/` | test script for the simulation and a real node |
| `sdk/include/` | WAGO headers (imported, not in git) |

Version: see [VERSION](VERSION). Licence: [LICENSE](LICENSE).
