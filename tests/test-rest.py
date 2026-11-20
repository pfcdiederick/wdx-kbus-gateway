#!/usr/bin/env python3
"""Tests for the wdx data layer of the KBUS gateway (/wda/parameters, JSON:API).

<host> is an IP address (TCP, --port) or, with --unix PATH, ignored ("-"):
the requests then go through the Unix socket.

  tests/test-rest.py <host> wda            SIMULATION node (KBUS_SIMULATION=1):
                                           all requests, PATCH, error codes; writes
  tests/test-rest.py <host> node           real node, READ ONLY: parameter tree,
                                           values, invalid requests; writes NOTHING
  tests/test-rest.py <host> write --do 1   real node: switches DO1 TRUE/FALSE
                                           with pauses (watch the LED); ends FALSE
  tests/test-rest.py <host> write --ao 1   real node: writes AO1, ends at 0
  tests/test-rest.py <host> forbidden      gateway started with HTTP_WRITE_ENABLE=0:
                                           PATCH of outputs gives 403, registers stay writable
  tests/test-rest.py <host> registers      SIMULATION node with tests/test-registers.conf
                                           (REGISTER_FILE): registers, batch writes
  tests/test-rest.py <host> control --control PATH
                                           SIMULATION node: ResetStatistics and LocalControl
                                           only through the control socket (CONTROL_SOCKET);
                                           local control blocks KBUS writes of other clients
  tests/test-rest.py <host> clients --max-clients N
                                           SIMULATION node: parallel clients, idle clients,
                                           503 when WDX_MAX_CLIENTS=N slots are busy

Options: --port (default 8000), --unix PATH, --pause seconds between write steps (default 3).
The simulation tests expect WDX_CLIENT_TIMEOUT_MS=1000.
Exit code 0 when everything passed.
"""
import argparse
import json
import socket
import sys
import time

P = "/wda/parameters"
args = None
failures = 0


def connect():
    if args.unix:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(args.unix)
    else:
        s = socket.create_connection((args.host, args.port))
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def raw(data, delay=0.0):
    """Send one request (a str, or a list of chunks sent as separate TCP
    writes `delay` seconds apart) and return the whole response (str)."""
    s = connect()
    s.settimeout(5)
    for i, chunk in enumerate(data if isinstance(data, list) else [data]):
        if i:
            time.sleep(delay)
        s.sendall(chunk.encode() if isinstance(chunk, str) else chunk)
    out = b""
    while True:
        try:
            d = s.recv(65536)
        except socket.timeout:
            out += b"<TIMEOUT>"
            break
        except ConnectionResetError:
            break
        if not d:
            break
        out += d
    s.close()
    return out.decode(errors="replace")


def req(method, path, body=None, ctype="application/vnd.api+json"):
    """Return (status, parsed JSON or None, raw response)."""
    head = f"{method} {path} HTTP/1.1\r\nHost: x\r\n"
    if body is not None:
        data = body if isinstance(body, str) else json.dumps(body)
        head += (f"Content-Type: {ctype}\r\n" if ctype else "") + f"Content-Length: {len(data)}\r\n\r\n" + data
    else:
        head += "\r\n"
    r = raw(head)
    status = int(r.split(" ", 2)[1]) if r.startswith("HTTP/1.0 ") else 0
    payload = r.split("\r\n\r\n", 1)[1] if "\r\n\r\n" in r else ""
    try:
        return status, (json.loads(payload) if payload else None), r
    except ValueError:
        return status, None, r


def check(name, got, ok, note=""):
    global failures
    failures += not ok
    print(f"{'PASS' if ok else 'FAIL'} {name:>6}  {str(got)[:90]}  {note}", flush=True)


def body(pid, value, typ="parameters"):
    return {"data": {"type": typ, "id": pid, "attributes": {"value": value}}}


def definition(pid):
    """GET .../{pid}/definition: 301 to /wda/parameter-definitions/{pid}, followed."""
    st, _, r = req("GET", f"{P}/{pid}/definition")
    loc = next((l.split(":", 1)[1].strip() for l in r.split("\r\n") if l.lower().startswith("location:")), None)
    if st != 301 or loc != f"/wda/parameter-definitions/{pid}":
        return st, None
    st, d, _ = req("GET", loc)
    return st, d


def value(pid):
    st, d, _ = req("GET", f"{P}/{pid}")
    return d["data"]["attributes"]["value"] if st == 200 and d else None


def content_length_ok(r):
    head, _, payload = r.partition("\r\n\r\n")
    for line in head.split("\r\n"):
        if line.lower().startswith("content-length:"):
            return int(line.split(":")[1]) == len(payload.encode())
    return False


def invalid_requests(do3, ao2, arr):
    """Requests that must be rejected and write nothing."""
    cases = [
        ("e1", ("PATCH", f"{P}/{do3}", body(do3, True), "text/plain"), 415),
        ("e2", ("PATCH", f"{P}/{do3}", body(do3, True), None), 415),
        ("e3", ("PATCH", f"{P}/{do3}", "{not json"), 400),
        ("e4", ("PATCH", f"{P}/{do3}", body("0-0-kbus-digitaloutputs-4-value", True)), 400),
        ("e5", ("PATCH", f"{P}/{do3}", body(do3, True, "foo")), 400),
        ("e6", ("PATCH", f"{P}/{do3}", body(do3, 1)), 400),
        ("e9", ("PATCH", f"{P}/0-0-kbus-cyclecount", body("0-0-kbus-cyclecount", 1)), 400),
        ("e11", ("PATCH", P, body(do3, True)), 400),
        ("e12", ("GET", f"{P}/0-0-kbus-nothing"), 404),
        ("e13", ("GET", "/wda/other"), 404),
        ("e15", ("GET", P + "?filter[foo]=1"), 400),
        ("e16", ("GET", P + "?page[limit]=x"), 400),
    ]
    if ao2:
        cases += [("e7", ("PATCH", f"{P}/{ao2}", body(ao2, 70000)), 400),
                  ("e8", ("PATCH", f"{P}/{ao2}", body(ao2, -1)), 400)]
    if arr:
        cases += [("e10", ("PATCH", f"{P}/{arr}", body(arr, [True] * 251)), 400)]
    for name, a, exp in cases:
        st, d, _ = req(*a)
        check(name, st, st == exp and d is not None and "errors" in d, f"expected {exp}")
    # the removed interfaces of version 1.x
    for name, a, exp in [("r1", ("GET", "/DO/1"), 404), ("r2", ("GET", "/ALL"), 404),
                         ("r3", ("POST", "/DO/1", "TRUE", None), 405),
                         ("r4", ("POST", "/ALL", "DO=1", None), 405), ("r5", ("PUT", P), 405)]:
        st, d, _ = req(*a)
        check(name, st, st == exp and d is not None and "errors" in d, f"expected {exp} (only /wda)")


def wda_conformity(do3, ao2, arr):
    """WDA REST-API 1.11.0: service identity, paging links, filters, stringValue, error codes."""
    st, d, _ = req("GET", "/wda")
    a = d["data"]["attributes"] if d else {}
    check("v1", a, st == 200 and d["data"]["type"] == "service-identity" and a.get("apiVersion", "").startswith("1."),
          "GET /wda")
    st, d, _ = req("GET", P + "?page[limit]=4&page[offset]=4")
    links = d.get("links", {}) if d else {}
    check("v2", links, st == 200 and all(k in links for k in ("self", "first", "last", "next", "prev"))
          and "page[offset]=8" in links["next"] and "page[offset]=0" in links["prev"], "paging links")
    st, d, _ = req("GET", P + "?filter[path]=KBus/DigitalOutputs/3/Value")
    check("v3", [x["id"] for x in d["data"]] if d else None, st == 200 and [x["id"] for x in d["data"]] == [do3],
          "filter[path]")
    st, d, _ = req("GET", P + "?filter[device]=headstation&filter[definition.writeable]=true&page[limit]=1000")
    ok = st == 200 and d and do3 in [x["id"] for x in d["data"]] and "0-0-kbus-cyclecount" not in [x["id"] for x in d["data"]]
    check("v4", st, ok, "filter[device] + filter[definition.writeable]")
    st, d, _ = req("GET", P + "?filter[beta]=true")
    check("v5", st, st == 200 and d["data"] == [], "filter[beta]=true: nothing is beta")
    st, d, _ = req("GET", f"{P}/{ao2}")
    a = d["data"]["attributes"] if d else {}
    check("v6", a, a.get("stringValue") == str(a.get("value")), "stringValue of a uint16")
    st, _, _ = req("PATCH", f"{P}/{ao2}", {"data": {"type": "parameters", "id": ao2, "attributes": {"stringValue": "321"}}})
    check("v7", (st, value(ao2)), st == 204 and value(ao2) == 321, "write with stringValue")
    st, d, _ = req("PATCH", f"{P}/{do3}", {"data": {"type": "parameters", "id": do3, "attributes": {"stringValue": "1"}}})
    check("v8", st, st == 400 and d["errors"][0].get("code") == "25", "boolean has no stringValue -> 25")
    st, d, _ = req("PATCH", f"{P}/{ao2}", body(ao2, 70000))
    e = d["errors"][0] if d else {}
    check("v9", e, st == 400 and e.get("code") == "39" and e.get("source", {}).get("pointer") == "/data/attributes/value",
          "out of range -> 39 with source.pointer")
    st, d, _ = req("GET", f"{P}/0-0-kbus-nothing")
    check("v10", st, st == 404 and d["errors"][0].get("code") == "17", "unknown id -> 17")
    r = raw(f"GET /wda HTTP/1.1\r\nHost: x\r\nAccept: text/plain\r\n\r\n")
    check("v11", r.split("\r\n")[0], r.startswith("HTTP/1.0 406"), "Accept without JSON:API -> 406")
    st, d, _ = req("GET", "/wda/parameter-definitions?filter[writeable]=true&page[limit]=1000")
    ok = st == 200 and d and all(x["type"] == "parameter-definitions" and x["attributes"]["writeable"] for x in d["data"])
    check("v12", st, ok, "/wda/parameter-definitions, filter[writeable]")


# ---------------------------------------------------------------------------
def mode_wda():
    """SIMULATION node (12 DI, 8 DO, 4 AI, 4 AO; DI1..8 follow DO1..8, AI4 follows AO1)."""
    do3, ao2, arr = "0-0-kbus-digitaloutputs-3-value", "0-0-kbus-analogoutputs-2-value", "0-0-kbus-digitaloutputvalues"
    st, d, r = req("GET", P)
    ids = [x["id"] for x in d["data"]] if d else []
    check("w1", (st, len(ids)), st == 200 and len(ids) == d["meta"]["total"] and len(set(ids)) == len(ids),
          "all parameters, unique ids")
    check("w1b", "Content-Length", content_length_ok(r))
    paths = {x["id"]: x["attributes"]["path"] for x in d["data"]} if d else {}
    check("w2", "id = 0-0- + lower(path) with / -> -",
          bool(paths) and all(i == "0-0-" + p.lower().replace("/", "-") for i, p in paths.items()))
    st, d, _ = req("GET", P + "?page[limit]=3&page[offset]=2")
    check("w3", [x["id"] for x in d["data"]] if d else None,
          st == 200 and len(d["data"]) == 3 and d["data"][0]["id"] == "0-0-kbus-cyclecount", "paging")
    st, d, _ = req("GET", P + "?page%5Blimit%5D=1")
    check("w4", st, st == 200 and len(d["data"]) == 1, "URL-encoded page[limit]")
    want = ["0-0-kbus-digitalinputvalues", "0-0-kbus-analoginputvalues", arr]
    st, d, _ = req("GET", P + "?filter[id]=" + ",".join(want))
    got = [x["id"] for x in d["data"]] if d else []
    check("w4b", got, st == 200 and sorted(got) == sorted(want), "filter[id], one GET")
    st, d, _ = req("GET", P + "?filter%5Bid%5D=" + "%2C".join(want[:2]))
    check("w4c", st, st == 200 and len(d["data"]) == 2, "URL-encoded filter")
    st, d, _ = req("GET", f"{P}/0-0-kbus-modules")
    check("w5", d, st == 200 and d["data"]["attributes"]["dataType"] == "instantiations")
    st, d = definition(do3)
    check("w6", d, st == 200 and d["data"]["attributes"]["writeable"] is True)
    st, d = definition("0-0-kbus-digitalinputs-1-value")
    check("w7", d, st == 200 and d["data"]["attributes"]["writeable"] is False)
    st, _, r = req("PATCH", f"{P}/{do3}", body(do3, True))
    check("w8", st, st == 204 and content_length_ok(r), "PATCH DO3 true -> 204, Content-Length: 0")
    check("w9", value(do3), value(do3) is True)
    st, _, _ = req("PATCH", f"{P}/{ao2}", body(ao2, 12345))
    check("w10", (st, value(ao2)), st == 204 and value(ao2) == 12345)
    st, _, _ = req("PATCH", f"{P}/{arr}", body(arr, [True, False, True]))
    v = value(arr)
    check("w11", v, st == 204 and v[:3] == [True, False, True], "array, first 3 channels")
    time.sleep(0.1)                                     # a few KBUS cycles
    di = value("0-0-kbus-digitalinputvalues")
    check("w12", di, di is not None and di[:3] == [True, False, True], "simulated DI follow the DO")
    wda_conformity(do3, ao2, arr)
    invalid_requests(do3, ao2, arr)
    check("e14", value(do3), value(do3) is True, "unchanged after rejected writes")
    # body in a second TCP segment (browsers do this): awaited until Content-Length is complete
    def patch_head(data):
        return (f"PATCH {P}/{do3} HTTP/1.1\r\nHost: x\r\nContent-Type: application/vnd.api+json\r\n"
                f"Content-Length: {len(data)}\r\n\r\n")
    data = json.dumps(body(do3, False))
    r = raw([patch_head(data), data], delay=0.03)
    check("s1", r.split("\r\n")[0], r.startswith("HTTP/1.0 204") and value(do3) is False, "body 30 ms later -> 204")
    data = json.dumps(body(do3, True))
    r = raw([patch_head(data), data], delay=0.3)
    check("s2", r.split("\r\n")[0], r.startswith("HTTP/1.0 204") and value(do3) is True, "body 300 ms later -> 204")
    r = raw([patch_head(json.dumps(body(do3, False))), data], delay=0.03)   # 1 byte short
    check("s3", r.split("\r\n")[0], r.startswith("HTTP/1.0 408") and value(do3) is True,
          "body shorter than Content-Length -> 408 after the timeout, nothing written")
    # dynamic configuration: manual rescan -> KBus/ConfigurationVersion + 1, same tree
    v0 = value("0-0-kbus-configurationversion")
    st, _, _ = req("PATCH", f"{P}/0-0-kbus-rescan", body("0-0-kbus-rescan", True))
    time.sleep(0.5)
    v1 = value("0-0-kbus-configurationversion")
    check("c1", (st, v0, v1), st == 204 and isinstance(v0, int) and v1 == v0 + 1, "PATCH KBus/Rescan true")
    check("c2", value("0-0-kbus-healthy"), value("0-0-kbus-healthy") is True, "healthy after the rescan")
    st, d = definition("0-0-kbus-rescan")
    check("c3", d, st == 200 and d["data"]["attributes"]["writeable"] is True, "KBus/Rescan is writeable")
    # cycle timing statistics
    ids = ["0-0-kbus-cycletime", "0-0-kbus-cycleduration", "0-0-kbus-maxcycleduration",
           "0-0-kbus-maxwakeupdelay", "0-0-kbus-overruns"]
    st, d, _ = req("GET", f"{P}?filter[id]={','.join(ids)}")
    t = {x["id"]: x["attributes"]["value"] for x in d["data"]} if d else {}
    check("t1", t, st == 200 and len(t) == 5 and all(isinstance(x, int) for x in t.values()), "timing parameters")
    st, d, _ = req("PATCH", f"{P}/0-0-kbus-resetstatistics", body("0-0-kbus-resetstatistics", True))
    check("t2", st, st == 403 and d["errors"][0].get("code") == "57",
          "ResetStatistics only through the control socket (mode control)")
    # cycle time is writable (1..40 ms); restore the original value afterwards
    orig = value("0-0-kbus-cycletime")
    st, _, _ = req("PATCH", f"{P}/0-0-kbus-cycletime", body("0-0-kbus-cycletime", 10))
    check("t3", (st, value("0-0-kbus-cycletime")), st == 204 and value("0-0-kbus-cycletime") == 10, "CycleTime = 10")
    bad = [req("PATCH", f"{P}/0-0-kbus-cycletime", body("0-0-kbus-cycletime", v))[0] for v in (0, 41, "x", True)]
    check("t4", bad, bad == [400, 400, 400, 400], "CycleTime rejects 0, 41, text, boolean")
    st, _, _ = req("PATCH", f"{P}/0-0-kbus-cycletime", body("0-0-kbus-cycletime", orig))
    check("t5", (st, value("0-0-kbus-cycletime")), st == 204 and value("0-0-kbus-cycletime") == orig, "CycleTime restored")
    # built-in web page
    r = raw("GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    check("p1", r.split("\r\n")[0], r.startswith("HTTP/1.0 200") and "text/html" in r and "/wda/parameters" in r
          and content_length_ok(r), "web page at /")
    req("PATCH", f"{P}/{arr}", body(arr, [False] * 8))
    req("PATCH", f"{P}/0-0-kbus-analogoutputvalues", body("0-0-kbus-analogoutputvalues", [0, 0, 0, 0]))


def mode_node():
    """Real node, read only."""
    st, d, r = req("GET", P)
    check("n1", (st, d["meta"]["total"] if d else None), st == 200 and content_length_ok(r), "parameter list")
    params = {x["id"]: x["attributes"] for x in d["data"]} if d else {}
    counts = {k: len(params.get(f"0-0-kbus-{k}", {}).get("value", []))
              for k in ("digitalinputs", "digitaloutputs", "analoginputs", "analogoutputs")}
    print("channels:", ", ".join(f"{v} {k}" for k, v in counts.items()))
    check("n2", params.get("0-0-kbus-healthy", {}).get("value"), params.get("0-0-kbus-healthy", {}).get("value") is True,
          "KBus/Healthy")
    for k, n in counts.items():
        vals = params.get(f"0-0-kbus-{k[:-1]}values", {}).get("value", [])
        digital = k.startswith("digital")
        ok = len(vals) == n and all(isinstance(x, bool) if digital else (isinstance(x, int) and 0 <= x <= 65535)
                                    for x in vals)
        check("n3", f"{k[:-1]}values {vals}", ok, "array matches the channel count")
    before = {pid: a["value"] for pid, a in params.items() if pid.endswith("outputvalues")}
    do1 = "0-0-kbus-digitaloutputs-1-value" if counts["digitaloutputs"] else "0-0-kbus-healthy"
    ao1 = "0-0-kbus-analogoutputs-1-value" if counts["analogoutputs"] else None
    arr = "0-0-kbus-digitaloutputvalues" if counts["digitaloutputs"] else None
    print("--- invalid requests (must not write)")
    invalid_requests(do1, ao1, arr)
    print("--- outputs unchanged (only valid if no other client writes them)")
    for pid, v in before.items():
        check("n4", pid, value(pid) == v)


def mode_write():
    def step(name, got, ok, what):
        check(name, got, ok, f"{time.strftime('%H:%M:%S')}  {what}")
        time.sleep(args.pause)

    if args.do:
        pid = f"0-0-kbus-digitaloutputs-{args.do}-value"
        print(f"Switching DO{args.do} with {args.pause} s pauses. Watch the LED of DO{args.do}.")
        step("d1", req("PATCH", f"{P}/{pid}", body(pid, True))[0] == 204, True, "LED ON")
        step("d2", value(pid), value(pid) is True, "LED ON")
        step("d3", req("PATCH", f"{P}/{pid}", body(pid, 1))[0] == 400, True, "LED stays ON (invalid value)")
        step("d4", req("PATCH", f"{P}/{pid}", body(pid, False))[0] == 204, True, "LED OFF")
        check("end", value(pid), value(pid) is False, "LED OFF")
    if args.ao:
        pid = f"0-0-kbus-analogoutputs-{args.ao}-value"
        print(f"Writing AO{args.ao} with {args.pause} s pauses. Measure the output of AO{args.ao}.")
        step("a1", req("PATCH", f"{P}/{pid}", body(pid, 65535))[0] == 204, True, "maximum")
        step("a2", req("PATCH", f"{P}/{pid}", body(pid, 65536))[0] == 400, True, "unchanged")
        step("a3", req("PATCH", f"{P}/{pid}", body(pid, 123))[0] == 204, True, "123")
        step("a4", req("PATCH", f"{P}/{pid}", body(pid, 0))[0] == 204, True, "0")
        check("end", value(pid), value(pid) == 0)
    if not args.do and not args.ao:
        sys.exit("write: give --do N and/or --ao N")


def mode_forbidden():
    pid = "0-0-kbus-digitaloutputs-1-value"
    st, d, _ = req("PATCH", f"{P}/{pid}", body(pid, True))
    check("f1", st, st == 403 and d is not None and "errors" in d, "writes disabled")
    st, _, _ = req("GET", f"{P}/{pid}")
    check("f2", st, st in (200, 404), "reading still works")
    st, d, _ = req("GET", P)
    regs = [x["id"] for x in d["data"] if x["id"].startswith("0-0-registers-") and x["id"].endswith("-value")
            and x["attributes"]["dataType"] == "boolean"] if d else []
    if regs:
        st, _, _ = req("PATCH", f"{P}/{regs[0]}", body(regs[0], True))
        check("f3", st, st == 204, "registers stay writable")


# ---------------------------------------------------------------------------
R = "0-0-registers-"


def batch(items):
    return {"data": [{"type": "parameters", "id": i, "attributes": {"value": v}} for i, v in items]}


def mode_registers():
    """SIMULATION node, REGISTER_FILE=tests/test-registers.conf (fresh REGISTER_STORE)."""
    sp, run, cnt, bn, name, cc = (R + "pump1-setpoint-value", R + "line-running-value", R + "counter-value",
                                  R + "batch-number-value", R + "recipe-name-value", R + "changecounter")
    st, d, _ = req("GET", P)
    attrs = {x["id"]: x["attributes"] for x in d["data"]} if d else {}
    want = {sp: ("Registers/Pump1/Setpoint/Value", "float64"), run: ("Registers/Line/Running/Value", "boolean"),
            cnt: ("Registers/Counter/Value", "int32"), bn: ("Registers/Batch/Number/Value", "uint32"),
            name: ("Registers/Recipe/Name/Value", "string"), cc: ("Registers/ChangeCounter", "uint64")}
    check("r1", len(attrs), all(i in attrs and (attrs[i]["path"], attrs[i]["dataType"]) == w for i, w in want.items()),
          "registers in the tree: path, id, dataType")
    check("r2", [value(i) for i in (sp, run, cnt, bn, name)],
          [value(i) for i in (sp, run, cnt, bn, name)] == [12.5, False, -7, 42, 'Tomato "A"'], "defaults")
    st, d = definition(sp)
    a = d["data"]["attributes"] if d else {}
    check("r3", a, a.get("writeable") is True and a.get("userSetting") is True, "definition: writeable, userSetting")
    st, d = definition(run)
    check("r3b", d["data"]["attributes"]["userSetting"] if d else None,
          d is not None and d["data"]["attributes"]["userSetting"] is False, "not a user setting")
    c0 = value(cc)
    for n, (pid, v) in enumerate([(sp, -0.125), (run, True), (cnt, -2147483648), (bn, 4294967295),
                                  (name, "é ü \\ \" line\nnew")]):
        st, _, _ = req("PATCH", f"{P}/{pid}", body(pid, v))
        check(f"r4.{n}", (st, value(pid)), st == 204 and value(pid) == v, "write + read back")
    c1 = value(cc)
    check("r5", (c0, c1), c1 == c0 + 5, "ChangeCounter +1 per changing write")
    req("PATCH", f"{P}/{cnt}", body(cnt, -2147483648))
    check("r6", value(cc), value(cc) == c1, "same value: ChangeCounter unchanged")
    for n, (pid, v, exp) in enumerate([(run, 1, 400), (cnt, 2147483648, 400), (cnt, 1.5, 400), (bn, -1, 400),
                                       (sp, "1", 400), (name, "x" * 128, 400), (name, 5, 400), (sp, 1e999, 400)]):
        st, d, _ = req("PATCH", f"{P}/{pid}", body(pid, v) if v != 1e999 else
                       '{"data":{"type":"parameters","id":"%s","attributes":{"value":1e999}}}' % pid)
        check(f"r7.{n}", st, st == exp and d and "errors" in d, f"invalid -> {exp}")
    check("r8", value(cc), value(cc) == c1, "nothing changed by invalid writes")
    st, _, _ = req("PATCH", f"{P}/{name}", body(name, "x" * 127))
    check("r9", st, st == 204 and value(name) == "x" * 127, "127 bytes is the maximum")
    st, _, _ = req("PATCH", f"{P}/{name}", body(name, "\u00e9" * 63))
    check("r9b", st, st == 204 and value(name) == "\u00e9" * 63, "UTF-8 counts bytes")
    st, _, _ = req("PATCH", f"{P}/{cc}", body(cc, 1))
    check("r10", st, st == 400, "ChangeCounter is read-only")

    # batch: registers and KBUS outputs together
    do1 = "0-0-kbus-digitaloutputs-1-value"
    c2 = value(cc)
    st, d, _ = req("PATCH", P, batch([(sp, 3.5), (run, False), (do1, True), (cnt, 1), (cnt, 2)]))
    check("b1", (st, value(sp), value(run), value(do1), value(cnt)),
          st == 204 and value(sp) == 3.5 and value(run) is False and value(do1) is True and value(cnt) == 2,
          "batch: registers + DO, last value of a duplicate wins")
    check("b2", value(cc), value(cc) == c2 + 1, "one batch = one ChangeCounter step")
    st, d, _ = req("PATCH", P, batch([(sp, 9.0), (do1, False), (cnt, "bad")]))
    check("b3", (st, value(sp), value(do1)), st == 400 and value(sp) == 3.5 and value(do1) is True,
          "one invalid value -> nothing written")
    st, d, _ = req("PATCH", P, batch([(sp, 9.0), ("0-0-registers-nothing-value", 1)]))
    codes = sorted(e.get("code") for e in d["errors"]) if d else []
    check("b4", (st, codes), st == 400 and codes == ["17", "41"] and value(sp) == 3.5,
          "unknown id -> 17 + 41 for the valid one, nothing written")
    st, d, _ = req("PATCH", P, batch([(sp, 9.0), ("0-0-kbus-cyclecount", 1)]))
    codes = sorted(e.get("code") for e in d["errors"]) if d else []
    check("b5", (st, codes), st == 400 and codes == ["31", "41"] and value(sp) == 3.5,
          "read-only parameter -> 31 + 41, nothing written")
    st, d, _ = req("PATCH", P, batch([(R + "pump1-setpoint-value", 1.0)]) | {"x": 1})
    check("b6", st, st == 204, "extra members are ignored")
    st, d, _ = req("PATCH", P, {"data": []})
    check("b7", st, st == 400, "empty batch -> 400")
    st, d, _ = req("PATCH", P, batch([(sp, float(i)) for i in range(150)]))
    check("b8", (st, value(sp)), st == 204 and value(sp) == 149.0, "150 items in one batch")
    st, d, _ = req("PATCH", P, batch([(sp, float(i)) for i in range(400)]))
    check("b9", st, st == 413, "request larger than 16 KB -> 413")
    st, d, _ = req("GET", f"{P}?filter[id]={sp},{do1},{cc}")
    ids = [x["id"] for x in d["data"]] if d else []
    check("b10", ids, st == 200 and ids == [do1, cc, sp], "filter[id] over KBUS and registers")
    req("PATCH", P, batch([(do1, False)]))


def via_control(method, path, data=None):
    """One request through the control socket (the dashboard's)."""
    saved = args.unix
    args.unix = args.control
    try:
        return req(method, path, data)
    finally:
        args.unix = saved


def mode_control():
    """SIMULATION node; --control = CONTROL_SOCKET of the gateway."""
    if not args.control:
        sys.exit("control: give --control PATH (CONTROL_SOCKET)")
    lc, rs, do1, reg = ("0-0-kbus-localcontrol", "0-0-kbus-resetstatistics",
                        "0-0-kbus-digitaloutputs-1-value", "0-0-registers-counter-value")
    st, _, _ = via_control("PATCH", f"{P}/{rs}", body(rs, True))
    check("k1", (st, value("0-0-kbus-cyclecount")), st == 204 and value("0-0-kbus-cyclecount") < 1000,
          "ResetStatistics through the control socket, CycleCount back to 0")
    st, d, _ = req("PATCH", f"{P}/{lc}", body(lc, True))
    check("k2", st, st == 403 and d["errors"][0].get("code") == "57", "LocalControl: not from other clients")
    st, _, _ = via_control("PATCH", f"{P}/{lc}", body(lc, True))
    check("k3", (st, value(lc)), st == 204 and value(lc) is True, "LocalControl on through the control socket")
    st, d, _ = req("PATCH", f"{P}/{do1}", body(do1, True))
    check("k4", st, st == 403 and "local control" in d["errors"][0]["detail"], "local control: KBUS write blocked")
    st, _, _ = req("PATCH", P, batch([(do1, True), ("0-0-kbus-cycletime", 5)]))
    check("k5", st, st == 403, "local control: batch and cycle time blocked")
    old = value(reg)
    st, _, _ = req("PATCH", f"{P}/{reg}", body(reg, 7)) if old is not None else (204, None, None)
    check("k6", st, st == 204, "local control: registers stay writable")
    if old is not None:
        req("PATCH", f"{P}/{reg}", body(reg, old))                 # restore (registers mode expects defaults)
    st, _, _ = via_control("PATCH", f"{P}/{do1}", body(do1, True))
    check("k7", (st, value(do1)), st == 204 and value(do1) is True, "the dashboard writes the KBUS")
    st, _, _ = via_control("PATCH", f"{P}/{lc}", body(lc, False))
    st2, _, _ = req("PATCH", f"{P}/{do1}", body(do1, False))
    check("k8", (st, st2, value(do1)), st == 204 and st2 == 204 and value(do1) is False,
          "LocalControl off: other clients write again")


def mode_clients():
    """Parallel clients, idle clients and the WDX_MAX_CLIENTS limit."""
    import threading
    do2 = "0-0-kbus-digitaloutputs-2-value"
    idle = [connect() for _ in range(3)]               # connected, send nothing
    t0 = time.time()
    st, _, _ = req("GET", f"{P}/0-0-kbus-healthy")
    dt = time.time() - t0
    check("q1", f"{dt * 1000:.0f} ms", st == 200 and dt < 0.3, "idle clients do not block others")
    for s in idle:
        s.close()

    results = []
    def worker(n):
        for i in range(25):
            if i % 2:
                st, _, _ = req("PATCH", f"{P}/{do2}", body(do2, bool(n % 2)))
            else:
                st, _, _ = req("GET", f"{P}?filter[id]=0-0-kbus-digitaloutputvalues,0-0-kbus-analoginputvalues")
            results.append(st)
    th = [threading.Thread(target=worker, args=(n,)) for n in range(min(args.max_clients, 8))]
    t0 = time.time()
    [t.start() for t in th]
    [t.join() for t in th]
    dt = time.time() - t0
    check("q2", f"{len(results)} requests in {dt:.2f} s", len(results) == len(th) * 25 and
          all(r in (200, 204) for r in results), "parallel clients, all answered")

    idle = [connect() for _ in range(args.max_clients)]
    time.sleep(0.2)
    r = raw("GET /wda/parameters/0-0-kbus-healthy HTTP/1.1\r\n\r\n")
    check("q3", r.split("\r\n")[0], r.startswith("HTTP/1.0 503"), f"{args.max_clients} busy slots -> 503")
    for s in idle:
        s.close()
    time.sleep(1.3)                                     # the slots time out (WDX_CLIENT_TIMEOUT_MS=1000)
    st, _, _ = req("GET", f"{P}/0-0-kbus-healthy")
    check("q4", st, st == 200, "slots free again")
    req("PATCH", f"{P}/{do2}", body(do2, False))


def main():
    global args
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host")
    p.add_argument("mode", choices=["wda", "node", "write", "forbidden", "registers", "clients", "control"])
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--unix", help="path of the Unix socket (WDX_SOCKET) instead of TCP")
    p.add_argument("--max-clients", type=int, default=16, help="clients mode: WDX_MAX_CLIENTS of the gateway")
    p.add_argument("--do", type=int, help="write mode: DO channel to switch")
    p.add_argument("--ao", type=int, help="write mode: AO channel to write")
    p.add_argument("--pause", type=float, default=3.0, help="write mode: seconds between steps")
    p.add_argument("--control", help="control mode: path of the control socket (CONTROL_SOCKET)")
    args = p.parse_args()
    {"wda": mode_wda, "node": mode_node, "write": mode_write, "forbidden": mode_forbidden,
     "registers": mode_registers, "clients": mode_clients, "control": mode_control}[args.mode]()
    print(f"\nFAILURES: {failures}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
