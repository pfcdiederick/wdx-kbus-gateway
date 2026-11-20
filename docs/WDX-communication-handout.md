# Exchanging data through the WDx gateway

*Hand-out for application developers: kbus-gateway 2.2.0 on WAGO PFC100 / PFC200 / PFC300*

## 1. The idea in one picture

```
 Application A (container) ─┐                                   ┌─ KBUS inputs / outputs
 Application B (container) ─┼── /run/kbus-gateway/wdx.sock ── WDx gateway
 CODESYS (host network)    ─┘   or TCP 127.0.0.1:8000           └─ Registers (shared data)
```

- The **gateway** is the only program that talks to the KBUS. Applications
  never access the KBUS or each other directly.
- Everything is a **parameter** with a path and a value: KBUS channels
  (`KBus/DigitalInputs/3/Value`) and **registers**: named values that you
  declare yourself to share data between applications
  (`Registers/Vision/Result/Code/Value`).
- One application **writes** a register, others **read** it. Registers never
  touch the KBUS.

## 2. Checklist: publishing data for another application

| # | Step | Who | Section |
|---|---|---|---|
| 1 | Agree on the registers: name, type, **one writer**, readers | both teams | 3 |
| 2 | Declare them in `/etc/kbus-gateway-registers.conf`, restart the gateway | integrator | 4 |
| 3 | Give each container access to the socket | integrator | 5 |
| 4 | Writer: write values, related values in one batch | producer | 6 |
| 5 | Reader: poll `Registers/ChangeCounter`, read in one request | consumer | 7 |
| 6 | Need to know that data was read? Use the handshake pattern | both | 8 |

## 3. Design the registers first

Write a small table and agree on it **before** coding. Example: a vision
application publishes an inspection result that CODESYS reads.

| Register (Name) | Type | Writer | Readers | Persistent | Meaning |
|---|---|---|---|---|---|
| `Vision/Result/Seq` | uint32 | Vision | CODESYS | no | +1 for every new result |
| `Vision/Result/Code` | int32 | Vision | CODESYS | no | 0 = OK, >0 = reject reason |
| `Vision/Result/Text` | string | Vision | CODESYS | no | readable reason |
| `Vision/Result/Ack` | uint32 | CODESYS | Vision | no | last `Seq` that was processed |
| `Line/Recipe` | string | MES app | all | **yes** | active recipe |

Rules that save trouble later:

- **One writer per register.** The gateway does not check who writes; two
  writers silently overwrite each other.
- **Prefix with the owner** (`Vision/...`, `Mes/...`): the owner is visible
  in every path.
- **Choose the type precisely.** A value that never goes below 0 is `uint32`.
  Use `float64` only for real measured values.
- **Persistent only when needed.** Persistent values are stored on flash
  (at most every 10 s). Counters and fast-changing values are not persistent.

## 4. Declare the registers

File on the controller: `/etc/kbus-gateway-registers.conf` (kept on updates).

```
# <Name>             <type>   [default]  [persistent]
Vision/Result/Seq    uint32
Vision/Result/Code   int32
Vision/Result/Text   string   ""
Vision/Result/Ack    uint32
Line/Recipe          string   "none"     persistent
```

Then: `/etc/init.d/kbus-gateway restart`.

> **Warning:** a restart switches all KBUS outputs to 0 for about one second.
> Plan register changes like any other change to the machine.

| Item | Rule |
|---|---|
| Name | segments of `A-Z a-z 0-9 _`, separated by `/`, each starting with a letter; max 40 characters; not case sensitive, so `Line/Recipe` and `line/recipe` are the same |
| Types | `boolean`, `int32`, `uint32`, `float64`, `string` (max 127 bytes UTF-8) |
| Default | JSON value: `true`, `-5`, `12.5`, `"text"`; omitted = `false` / `0` / `""` |
| Maximum | 256 registers |

Check the result in the log: `grep kbus-gateway /var/log/messages | grep registers`.
A line with an error is skipped and reported with its line number.

**From name to id.** Every request uses the **id**, derived from the path:

```
Name:  Vision/Result/Code
Path:  Registers/Vision/Result/Code/Value
Id:    0-0-registers-vision-result-code-value      ("0-0-" + lower case, "/" -> "-")
```

## 5. Give an application access

**Container:** mount the socket **directory** (the socket file is created
again at every gateway start, so a mounted file would go stale):

```bash
docker run -d --name vision --restart unless-stopped \
    -v /run/kbus-gateway:/run/kbus-gateway \
    my-vision-image
```

- The container needs **no network** for this (`--network none` works).
- The socket belongs to root with mode `0660`. A container that runs as
  root can connect. For a non-root user: set `WDX_SOCKET_GROUP=<group>` in
  `/etc/kbus-gateway.conf` and start the container with
  `--group-add <gid of that group>`.

**CODESYS** (container with the host network) and other programs on the
controller: TCP `127.0.0.1:8000`, the same requests. The TCP port only
accepts connections from the controller itself, unless the integrator sets
`HTTP_BIND=0.0.0.0`.

**Look at the data from your PC:** the management web interface
`http://<pfc-ip>/kbus` (optional login, see README 3.7) shows all parameters, their
definitions and the registers, and lets you set register values by hand:
useful while developing a producer or consumer.

Test the connection on the controller:

```bash
curl -s --unix-socket /run/kbus-gateway/wdx.sock http://x/wda/parameters/0-0-kbus-healthy
```

## 6. Writing data (producer)

**One value:**

```
PATCH /wda/parameters/0-0-registers-vision-result-code-value
Content-Type: application/vnd.api+json

{"data":{"type":"parameters","id":"0-0-registers-vision-result-code-value",
         "attributes":{"value":3}}}
```

Answer: `204 No Content`. The `id` in the body must be the same as in the URL.

**Values that belong together: always one batch.** All values are checked
first and then written together, or none at all. A reader never sees half an
update.

```
PATCH /wda/parameters
Content-Type: application/vnd.api+json

{"data":[
  {"type":"parameters","id":"0-0-registers-vision-result-code-value","attributes":{"value":3}},
  {"type":"parameters","id":"0-0-registers-vision-result-text-value","attributes":{"value":"scratch"}},
  {"type":"parameters","id":"0-0-registers-vision-result-seq-value","attributes":{"value":42}}
]}
```

**Value formats.** The JSON type must match exactly; the gateway does not
convert:

| Type | Valid | Rejected |
|---|---|---|
| boolean | `true`, `false` | `1`, `"true"` |
| int32 | `-2147483648` … `2147483647` (or `"stringValue": "3"`) | `1.5`, `"value": "3"` |
| uint32 | `0` … `4294967295` | `-1` |
| float64 | `12.5`, `-3`, `1e-3` (or `"stringValue": "12.5"`) | `"value": "12.5"`, NaN, infinity |
| string | `"text"`, max 127 bytes | `5` |

A rejected request writes **nothing**: `400` wrong type, out of range,
read-only or malformed JSON, `404` unknown id. Every error object names the
parameter in `detail` and carries the WDA status `code` (24 wrong type,
31 read-only, 39 out of range, 17 unknown id) and `source.pointer`, e.g.
`Registers/Vision/Result/Code/Value: value must be a JSON int32`, code `24`.
A batch returns one error object per rejected parameter, and code 41 for the
valid ones (not written).

**Don'ts:**

- Do not write faster than the readers need. Every 100 ms is plenty for
  data exchange between applications.
- Do not write persistent registers in a fast loop (flash wear).
- Do not write KBUS outputs that another program already writes. CODESYS,
  for example, writes the whole output array in every cycle and overwrites you.

## 7. Reading data (consumer)

**Read everything you need in one request.** All values come from the same
moment:

```
GET /wda/parameters?filter[id]=0-0-registers-changecounter,0-0-registers-vision-result-seq-value,0-0-registers-vision-result-code-value,0-0-registers-vision-result-text-value
```

```json
{"data":[
  {"type":"parameters","id":"0-0-registers-changecounter","attributes":{"path":"Registers/ChangeCounter","dataType":"uint64","dataRank":"scalar","value":118}},
  {"type":"parameters","id":"0-0-registers-vision-result-seq-value","attributes":{"path":"Registers/Vision/Result/Seq/Value","dataType":"uint32","dataRank":"scalar","value":42}},
  ...
]}
```

The answer lists the parameters in the gateway's order, not in the order of
the request: find them by `id`.

**Notice changes cheaply.** `Registers/ChangeCounter` goes up by one for every
write that changed at least one register. Poll only this value; read the rest
only when it changed. The counter covers **all** registers, so a change may
come from another application: compare the values you care about.

**Numbers:** a `float64` with a whole value comes back without a decimal
point (`3`, not `3.0`). Treat it as a number, not as a specific JSON type.

**Polling period:** match it to the process. One request takes 1.5–3 ms on
the controller.

## 8. Pattern: make sure the data was read (handshake)

When every result counts (no result may be overwritten before it was
processed), use a sequence number and an acknowledgement:

```
Producer (Vision)                                Consumer (CODESYS)
──────────────────                               ──────────────────
1. wait until Ack == Seq   (the last result was processed)
2. one batch: Code, Text, Seq = Seq + 1   ───▶   3. read Seq, Code, Text in ONE request
                                                 4. Seq != last processed Seq?
                                                       process Code / Text
                                          ◀───        write Ack = Seq
```

- `Seq` is written **last in the same batch** as the data, so `Seq` never
  points to old data.
- Only the producer writes `Seq`, `Code`, `Text`; only the consumer writes `Ack`.
- After a restart of the gateway all non-persistent registers return to
  their defaults (`Seq = Ack = 0`), so the handshake starts again cleanly.
- When results may be overwritten (only the latest value counts, e.g. a
  measured value), leave out `Ack` and just overwrite.

## 9. Code

### Python (container)

Copy [docker/lib/wdx_client.py](../docker/lib/wdx_client.py)
(standard library only) into your image.

Producer:

```python
import time
from wdx_client import WdxClient

R = "0-0-registers-vision-result-"
wdx = WdxClient()                                   # /run/kbus-gateway/wdx.sock

def publish(code, text):
    while True:                                     # 1. wait until the last result was processed
        v = wdx.get_many([R + "seq-value", R + "ack-value"])
        if v[R + "ack-value"] == v[R + "seq-value"]:
            break
        time.sleep(0.05)
    wdx.set_many({R + "code-value": code,           # 2. data and new Seq in one batch
                  R + "text-value": text,
                  R + "seq-value": (v[R + "seq-value"] + 1) % 2**32})

publish(3, "scratch")
```

Consumer:

```python
import time
from wdx_client import WdxClient

R = "0-0-registers-vision-result-"
wdx = WdxClient()
last_cc, last_seq = None, 0
while True:
    cc = wdx.get("0-0-registers-changecounter")
    if cc != last_cc:                               # something changed: read in one snapshot
        last_cc = cc
        v = wdx.get_many([R + "seq-value", R + "code-value", R + "text-value"])
        if v[R + "seq-value"] != last_seq:
            last_seq = v[R + "seq-value"]
            print("result", last_seq, v[R + "code-value"], v[R + "text-value"])
            wdx.set(R + "ack-value", last_seq)      # 4. acknowledge
    time.sleep(0.1)
```

Errors raise `WdxError` (with `.status` and `.detail`) or `OSError` (gateway
not running). Catch them and try again a little later.

### Shell (test and diagnosis on the controller)

```bash
S="curl -s --unix-socket /run/kbus-gateway/wdx.sock"
$S -g "http://x/wda/parameters?filter[id]=0-0-registers-vision-result-seq-value,0-0-registers-vision-result-code-value"
$S -X PATCH -H 'Content-Type: application/vnd.api+json' \
   -d '{"data":{"type":"parameters","id":"0-0-registers-vision-result-ack-value","attributes":{"value":42}}}' \
   http://x/wda/parameters/0-0-registers-vision-result-ack-value
$S http://x/wda/parameter-definitions/0-0-registers-vision-result-code-value    # type, writeable, userSetting
```

`-g` stops curl from interpreting the brackets in `filter[id]`.

### CODESYS

Use the function blocks of [codesys](../codesys/README.md)
(`FB_HttpTransaction`, TCP `127.0.0.1:8000`) with the register ids above: a
`GET ...?filter[id]=...` to read, a batch `PATCH /wda/parameters` to write.
Every request must contain the complete JSON body in one send; the answer
always carries `Content-Length`.

## 10. Reference

| Request | Purpose |
|---|---|
| `GET /wda/parameters` | all parameters (`page[limit]`, `page[offset]`) |
| `GET /wda/parameters?filter[id]=a,b` | these parameters, one snapshot |
| `GET /wda/parameters/{id}` | one parameter |
| `GET /wda/parameter-definitions/{id}` | path, dataType, writeable, userSetting (persistent) |
| `PATCH /wda/parameters/{id}` | write one parameter |
| `PATCH /wda/parameters` | write several, all or nothing |

| Status | Meaning |
|---|---|
| 200 / 204 | read OK / written |
| 400 | malformed JSON, wrong JSON type (24), read-only (31), out of range / string too long (39), `type`/`id` in the body wrong |
| 403 | KBUS outputs are read-only (`HTTP_WRITE_ENABLE=0`); registers stay writable |
| 404 | unknown id: check spelling, declaration, restart |
| 408 | request not complete within 5 s |
| 413 | request larger than 16 KB (about 150 values per batch) |
| 415 | `Content-Type: application/vnd.api+json` missing on a PATCH |
| 503 | more than 16 connections at the same time; try again |

Limits: one request per connection (the gateway closes it after the answer),
16 connections at the same time, 5 s to send a request.

## 11. Troubleshooting

| Symptom | Check |
|---|---|
| `No such file or directory` on the socket | mount the **directory** `/run/kbus-gateway`; is the gateway running (`/etc/init.d/kbus-gateway status`)? |
| `Permission denied` on the socket | container user not root and not in `WDX_SOCKET_GROUP` (section 5) |
| `404` for a register | name typo, line skipped (log), or gateway not restarted after the change |
| `400` on a write | JSON type (`1` is not a boolean, `"3"` is not a number) |
| Value changes back by itself | a second writer: check who else writes this register |
| Persistent value lost after restart | type or name changed, `persistent` removed, or power lost within 10 s of the change |
| Reader misses results | use the handshake (section 8) instead of only polling the latest value |

More background: [README](../README.md), section 3.
