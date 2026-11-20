# Containers

Four images, one folder each, plus the shared Python client library. They only talk to each other through the
gateway's Unix socket `/run/kbus-gateway/wdx.sock` (mounted as the directory
`/run/kbus-gateway`).

```
 PC browser ──/kbus (80/443)▶ manager ─────────┐
 SCADA ──────:2404, IEC 104──▶ iec104 ──────────┤
                              example-client ──┼── /run/kbus-gateway/wdx.sock ── gateway ── KBUS
                              your application ┘                   (or the native .ipk instead)
```

| Folder | Image | Role | Network | Needed? |
|---|---|---|---|---|
| [gateway/](gateway/) | `kbus-gateway:<version>-<arch>` | owns the KBUS, wdx data layer, registers | host (only `127.0.0.1:8000`) | one gateway: this container **or** the native package (`tools/build-ipk.sh`) |
| [manager/](manager/) | `kbus-manager:<arch>` | web management: status, I/O, parameters, registers; optional login (Settings tab) | `127.0.0.1:8090`, reached at `http(s)://<pfc-ip>/kbus` via the controller's lighttpd | optional |
| [iec104/](iec104/README.md) | `kbus-iec104:<arch>` | IEC 60870-5-104 server: KBUS digital inputs for a SCADA | port 2404 | optional |
| [example-client/](example-client/) | `kbus-example-client:<arch>` | example application, template for your own containers | none | optional |
| [lib/](lib/) | – | `wdx_client.py`: Python client (socket or TCP), copied into the Python images | – | – |

| File | |
|---|---|
| `gateway/Dockerfile` | build context is the **project root** (it compiles `src/`, `web/`, `sdk/`); ignore file `gateway/Dockerfile.dockerignore` |
| `gateway/entrypoint.sh` | loads the firmware's WAGO libraries without the firmware's glibc |
| `manager/manager.py`, `manager.html` | the web interface (Python standard library) |
| `iec104/wdx_2_iec104.py`, `wdx_iec104_config.ini` | the IEC 104 server and its parameter file (c104) |
| `example-client/example.py` | example program (Python standard library) |
| `lib/wdx_client.py` | the client module: `get`, `get_many`, `parameters`, `definition`, `set`, `set_many` |

**Build context:** `gateway/` builds from the project root, `iec104/` and
`example-client/` from `docker/` (they copy `lib/wdx_client.py`; each has a
`Dockerfile.dockerignore`), `manager/` from its own folder.

## Build and deploy

`tools/deploy-docker.sh` builds the images for the controller's architecture,
stores them as archives in `build-docker/`, loads them on the controller and
starts them:

```bash
tools/deploy-docker.sh 192.168.1.17                              # gateway container (stops the native service)
tools/deploy-docker.sh 192.168.1.17 --manager --no-gateway       # only the manager
tools/deploy-docker.sh 192.168.1.17 --client --no-gateway        # only the example client
tools/deploy-docker.sh 192.168.1.17 --iec104 --no-gateway        # only the IEC 104 server
tools/deploy-docker.sh 192.168.1.17 --manager --client --iec104  # everything
SKIP_BUILD=1 tools/deploy-docker.sh ...                           # reuse the archives in build-docker/
tools/deploy-docker.sh 192.168.1.17 --native                     # gateway container -> native service again
```

By hand, from the project root:

```bash
docker buildx build --platform linux/arm64 -f docker/gateway/Dockerfile -t kbus-gateway:2.3.0-arm64 --load .
docker buildx build --platform linux/arm64 -t kbus-manager:arm64 --load docker/manager
docker buildx build --platform linux/arm64 -f docker/example-client/Dockerfile -t kbus-example-client:arm64 --load docker
docker buildx build --platform linux/arm64 -f docker/iec104/Dockerfile -t kbus-iec104:arm64 --load docker
```

Details: main [README](../README.md) 2.8 (gateway container), 3.6 (example
client, registers), 3.7 (manager), 3.8 (IEC 104).
