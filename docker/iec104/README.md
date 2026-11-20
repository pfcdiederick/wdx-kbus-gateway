# kbus-iec104: KBUS inputs on IEC 60870-5-104

Reads the KBUS digital inputs from the kbus-gateway's wdx data layer and
offers them on an **IEC 60870-5-104 server** (slave / outstation) for a SCADA.
The server uses [c104](https://github.com/Fraunhofer-FIT-DIEN/iec104-python),
the Python binding of [mz-automation/lib60870](https://github.com/mz-automation/lib60870).

```
KBUS ── kbus-gateway ── /run/kbus-gateway/wdx.sock ── kbus-iec104 (container) ── TCP :2404 ── SCADA
```

The container talks to the gateway only through the Unix socket; it needs no
host network. Port 2404 is published with `-p 2404:2404`.

## Files

| File | |
|---|---|
| `wdx_2_iec104.py` | the server: polls the gateway, updates the IEC 104 points |
| `wdx_iec104_config.ini` | parameters: poll rate, gateway connection, IEC 104 server, signal → IOA |
| `Dockerfile`, `Dockerfile.dockerignore` | image; build context is `docker/` (for the shared `../lib/wdx_client.py`) |

## Deploy

```bash
tools/deploy-docker.sh 192.168.1.17 --iec104 --no-gateway     # only this container
```

At the first deployment the script creates the parameter file
`/home/kbus-iec104/wdx_iec104_config.ini` on the controller from the image's
default and mounts it. Later deployments keep it. After a change:

```bash
ssh root@192.168.1.17 vi /home/kbus-iec104/wdx_iec104_config.ini
ssh root@192.168.1.17 docker restart kbus-iec104
ssh root@192.168.1.17 docker logs -f kbus-iec104
```

By hand (from the project root, then on the PFC):

```bash
docker buildx build --platform linux/arm64 -f docker/iec104/Dockerfile -t kbus-iec104:arm64 --load docker
docker run -d --name kbus-iec104 --restart unless-stopped -p 2404:2404 \
    -v /run/kbus-gateway:/run/kbus-gateway \
    -v /home/kbus-iec104/wdx_iec104_config.ini:/app/wdx_iec104_config.ini:ro kbus-iec104:arm64
```

## IEC 104 objects

Common address (ASDU address): **1** (`common_address`).

| IOA | Type | Signal | wdx parameter |
|---|---|---|---|
| 1 … 4 | `M_SP_TB_1` (30), single point with time tag CP56Time2a | DI1 … DI4 | `0-0-kbus-digitalinputvalues`, element *n* |

One line per signal in `[points]`:

```ini
[points]
DI1 = 1
DI2 = 2
```

`DI<n>` is digital input *n* (1-based, over all digital input modules, in the
gateway's order: see the I/O tab of the manager). IOAs must be unique.

## Parameters (`wdx_iec104_config.ini`)

| Section | Key | Default | |
|---|---|---|---|
| `[general]` | `log_level` | `INFO` | `DEBUG`, `INFO`, `WARNING`, `ERROR` |
| | `poll_ms` | `100` | poll interval of the gateway (minimum 10) |
| `[wdx]` | `socket` | `/run/kbus-gateway/wdx.sock` | the gateway's Unix socket |
| | `host`, `port` | (empty), `8000` | TCP instead of the socket (container with `--network host`, `host = 127.0.0.1`) |
| | `timeout` | `1.0` | seconds per request |
| `[iec104]` | `ip`, `port` | `0.0.0.0`, `2404` | listening address in the container |
| | `max_connections` | `0` | 0 = unlimited |
| | `common_address` | `1` | |
| | `report_ms` | `0` | 0 = only on a change; > 0 = also cyclically every x ms |
| `[points]` | `DI<n> = <IOA>` | | at least one |

## Behaviour

- Every `poll_ms` **one** request reads all needed inputs (one KBUS snapshot):
  `GET /wda/parameters?filter[id]=0-0-kbus-digitalinputvalues`.
- A change is sent **spontaneously** (COT 3) to all connected clients, time
  tag = time the change was detected (resolution `poll_ms`).
- A **general interrogation** (`C_IC_NA_1`, 100) returns the latest values (COT 20).
- Quality **Invalid** until the first read, while the gateway does not answer
  or answers with an error, and for channels that do not exist (no DI module,
  fewer channels than configured). **Good** again as soon as a value is read.
- Errors are logged once when they start and once when they are solved.
- `docker stop` stops cleanly (SIGTERM).

## Test

1. Connect an IEC 104 client (e.g. QTester104) to `<pfc-ip>:2404`, common
   address 1, StartDT act.
2. General interrogation: IOA 1 … 4, type 30, COT 20.
3. Switch DI1: IOA 1 arrives with COT 3; the log shows `DI1 = True -> IOA 1 (sent)`.

## Troubleshooting

| Symptom | Cause / solution |
|---|---|
| Test tool cannot connect | container not running (`docker ps`), wrong IP, firewall blocks TCP 2404 |
| Log: *No such file or directory* / *Connection refused* | gateway not running, or `/run/kbus-gateway` not mounted |
| Log: *HTTP 404* | gateway too old (no wdx data layer) |
| Log: *DI5 not available: the kbus-gateway reports 4 channel(s)* | fewer DI channels on the KBUS than configured; that IOA stays Invalid |
| Port 2404 already in use | another IEC 104 server on the controller (e.g. the old `wdx-iec104-gateway` container) |
| `.ini` changes have no effect | read only at start-up: `docker restart kbus-iec104` |

## IEC 104 reference

| Type | Name | Used for |
|---|---|---|
| 30 `M_SP_TB_1` | single-point information with time tag CP56Time2a | digital inputs |
| 100 `C_IC_NA_1` | general interrogation | read all current values |

| COT | Meaning |
|---|---|
| 3 | spontaneous |
| 6 / 7 / 10 | activation / confirmation / termination |
| 20 | interrogated by station interrogation |
| 44 / 46 / 47 | unknown type / common address / IOA |

Origin: the standalone project `docker-wdx-104` (TCP, gateway 1.x). Changes:
socket instead of host network, shared `wdx_client.py`, network errors also
set Invalid.
