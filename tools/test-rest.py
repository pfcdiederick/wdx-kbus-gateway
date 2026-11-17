#!/usr/bin/env python3
"""Check the REST server against the CODESYS reference behaviour (README 3).

Every response is compared byte for byte with what the reference sends.

  tools/test-rest.py <host> sim            all reference cases 1-31 + special
                                           cases; needs the SIMULATION node
                                           (KBUS_SIMULATION=1), writes outputs
  tools/test-rest.py <host> bulk           bulk extension (/DO /AO /ALL, README
                                           3.4) on the SIMULATION node, writes
  tools/test-rest.py <host> wda            wdx-style interface (/wda/parameters,
                                           JSON:API) on the SIMULATION node, writes
  tools/test-rest.py <host> node           real node, read only: finds the
                                           channel counts, checks reads and
                                           invalid requests, writes NOTHING
  tools/test-rest.py <host> write --do 1   real node: switches DO1 TRUE/FALSE
                                           with pauses (watch the LED); ends FALSE
  tools/test-rest.py <host> write --ao 1   real node: writes AO1, ends at 0
  tools/test-rest.py <host> forbidden      container started with
                                           HTTP_WRITE_ENABLE=0: POST gives 403

Options: --port (default 8000), --pause seconds between write steps (default 3).
Exit code 0 when everything passed.

Note: send the tests from a PC on the same network as the PFC. Some security
software / proxies hold back data that is not a complete HTTP request; the
special cases with incomplete requests then time out (they pass when run on
the PFC itself).
"""
import argparse
import socket
import sys
import time

E404_BODY = "Unknown endpoint, channel, or value"
args = None
failures = 0


def resp(status, body):
    reason = {200: "OK", 403: "Forbidden", 404: "Not Found"}[status]
    return f"HTTP/1.0 {status} {reason}\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n{body}"


OK = lambda body: resp(200, body)
E404 = resp(404, E404_BODY)
E403 = resp(403, "Output writes are disabled")


def raw(chunks, delay=0.3):
    """Send the chunks (separate TCP writes) and return the whole response."""
    if isinstance(chunks, (str, bytes)):
        chunks = [chunks]
    s = socket.create_connection((args.host, args.port))
    s.settimeout(5)
    for i, c in enumerate(chunks):
        if i:
            time.sleep(delay)
        s.sendall(c.encode() if isinstance(c, str) else c)
    data = b""
    while True:
        try:
            d = s.recv(4096)
        except socket.timeout:
            data += b"<TIMEOUT>"
            break
        except ConnectionResetError:
            break
        if not d:
            break
        data += d
    s.close()
    return data.decode(errors="replace")


def get(path):
    return raw(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n")


def post(path, body, content_length=True):
    cl = f"Content-Length: {len(body)}\r\n" if content_length else ""
    return raw(f"POST {path} HTTP/1.1\r\nHost: x\r\n{cl}\r\n{body}")


def check(name, got, *expected, note=""):
    global failures
    ok = got in expected
    failures += not ok
    first = got.split("\r\n")[0] if got else "<no response>"
    body = got.split("\n")[-1]
    line = f"{'PASS' if ok else 'FAIL'} {str(name):>6}  {first:<24} | {body[:50]!r}"
    if note:
        line += f"  {note}"
    if not ok:
        line += f"   expected {[e.split(chr(10))[-1] for e in expected]}"
    print(line, flush=True)


def digital(kind, n):
    return OK(f"{kind}{n}=TRUE"), OK(f"{kind}{n}=FALSE")


def invalid_requests():
    """Requests that must give 404 whatever the node looks like (no writes)."""
    check(12, raw("POST /DO/1 HTTP/1.1\r\nHost: x\r\n\r\n"), E404)
    check(13, post("/DO/1", "true"), E404)
    check(14, post("/DO/1", "2"), E404)
    check(15, post("/AO/1", "65536"), E404)
    check(16, post("/AO/1", "-1"), E404)
    check(17, post("/AO/1", "1.5"), E404)
    check(18, post("/DI/1", "TRUE"), E404)
    check(19, post("/AI/1", "1"), E404)
    check(20, get("/DO/0"), E404)
    check(21, get("/DO/999"), E404)
    for no, p in [(23, "/DO/"), (24, "/DO/1/"), (25, "/DO/1?x=1"),
                  (26, "/do/1"), (27, "/XX/1"), (28, "/")]:
        check(no, get(p), E404)
    check(29, raw("PUT /DO/1 HTTP/1.1\r\nContent-Length: 4\r\n\r\nTRUE"), E404)


# ---------------------------------------------------------------------------
def mode_sim():
    """All reference cases on the simulation node (DI1..8 = DO1..8, AI4 = AO1)."""
    check(1, get("/DI/1"), *digital("DI", 1))
    check(2, get("/DO/1"), *digital("DO", 1))
    r = get("/AI/1")
    ok_ai = r.startswith(OK("AI1=")) and r[len(OK("AI1=")):].isdigit()
    check(3, r, r if ok_ai else OK("AI1=<raw value>"))
    check(4, get("/AO/1"), OK("AO1=0"), OK("AO1=65535"), OK("AO1=123"))
    check(5, post("/DO/1", "TRUE"), OK("DO1=TRUE"))
    check(6, post("/DO/1", "FALSE"), OK("DO1=FALSE"))
    check(7, post("/DO/1", "1"), OK("DO1=TRUE"))
    check(8, post("/DO/1", "0"), OK("DO1=FALSE"))
    check(9, post("/AO/1", "0"), OK("AO1=0"))
    check(10, post("/AO/1", "65535"), OK("AO1=65535"))
    check(11, post("/AO/1", "00123"), OK("AO1=123"))
    invalid_requests()
    check(22, get("/DO/01"), *digital("DO", 1))
    post("/DO/1", "TRUE")
    check(30, get("/DO/1"), OK("DO1=TRUE"))
    post("/AO/1", "65535")
    check(31, get("/AO/1"), OK("AO1=65535"))

    print("--- special cases (README 3.3 / reference section 9)")
    time.sleep(0.05)
    check("AI4", get("/AI/4"), OK("AI4=65535"), note="loopback of AO1, unsigned")
    check("DI1", get("/DI/1"), OK("DI1=TRUE"), note="loopback of DO1")
    check("split", raw(["POST /DO/1 HTTP/1.1\r\nContent-Length: 1\r\n\r\n", "0"]), E404,
          note="body in a 2nd TCP segment")
    check("noEOL", raw("POST /DO/1 HTTP/1.1\r\nTRUE"), E404, note="no empty line")
    check("noCL", post("/DO/1", "0", content_length=False), OK("DO1=FALSE"), note="headers ignored")
    check("crlf", post("/AO/1", "42\r\n"), OK("AO1=42"))
    check("sp1", post("/DO/1", " 1"), E404)
    check("sp2", post("/DO/1", "1 "), E404)
    check("hex", post("/AO/1", "0x10"), E404)
    check("plus", post("/AO/1", "+5"), E404)
    check("min9", raw("GET /DO/1"), *digital("DO", 1), note="exactly 9 bytes")
    check("short", raw("GET /DO/"), E404)
    check("min10", raw("POST /DO/"), E404)
    check("lower", raw("get /DO/1 HTTP/1.1\r\n\r\n"), E404)
    check("ovfl", get("/DO/4294967297"), *digital("DO", 1), note="32-bit wrap -> 1")
    check("512", raw("POST /AO/1 HTTP/1.1\r\nX: " + "a" * 600 + "\r\n\r\n5"), E404, "",
          note="body beyond 512 bytes (empty response = TCP reset, also OK)")
    check("vers", raw("GET /DO/1 FOO\r\n\r\n"), *digital("DO", 1))
    check("lead0", post("/AO/02", "7"), OK("AO2=7"))
    idle = socket.create_connection((args.host, args.port))
    time.sleep(0.3)
    s2 = socket.create_connection((args.host, args.port))
    s2.sendall(b"GET /DO/1 HTTP/1.1\r\n\r\n")
    s2.settimeout(1.5)
    try:
        blocked = s2.recv(100) == b""
    except socket.timeout:
        blocked = True
    check("idle", "blocked" if blocked else "served", "blocked", note="idle client blocks others")
    idle.close()
    s2.settimeout(3)
    check("after", s2.recv(500).decode(), *digital("DO", 1))
    s2.close()
    post("/DO/1", "0")
    post("/AO/1", "0")


def count(kind):
    """Number of channels of a class: the first n that answers 404, minus 1."""
    n = 0
    while n < 250 and get(f"/{kind}/{n + 1}").startswith(OK("")):
        n += 1
    return n


def mode_node():
    """Real node, read only."""
    counts = {k: count(k) for k in ("DI", "DO", "AI", "AO")}
    print("channels found:", ", ".join(f"{v} {k}" for k, v in counts.items()))
    before = {}
    for kind, n in counts.items():
        for i in range(1, n + 1):
            r = get(f"/{kind}/{i}")
            if kind in ("DI", "DO"):
                check(f"{kind}{i}", r, *digital(kind, i))
            else:
                v = r[len(OK(f"{kind}{i}=")):]
                check(f"{kind}{i}", r, r if r.startswith(OK(f"{kind}{i}=")) and v.isdigit()
                      and int(v) <= 65535 else OK(f"{kind}{i}=<0..65535>"))
            if kind in ("DO", "AO"):
                before[(kind, i)] = r
        check(f"{kind}{n + 1}", get(f"/{kind}/{n + 1}"), E404, note="one past the last channel")
    if counts["DO"]:
        check(22, get("/DO/01"), *digital("DO", 1))
    print("--- invalid requests (must not write)")
    invalid_requests()
    check("POST+1", post(f"/DO/{counts['DO'] + 1}", "TRUE"), E404)
    print("--- outputs unchanged")
    for (kind, i), r in before.items():
        check(f"{kind}{i}", get(f"/{kind}/{i}"), r)


def mode_write():
    def step(name, got, exp, what):
        check(name, got, exp, note=f"{time.strftime('%H:%M:%S')}  {what}")
        time.sleep(args.pause)

    if args.do:
        n = args.do
        print(f"Switching DO{n} with {args.pause} s pauses. Watch the LED of DO{n}.")
        step(5, post(f"/DO/{n}", "TRUE"), OK(f"DO{n}=TRUE"), "LED ON")
        step(30, get(f"/DO/{n}"), OK(f"DO{n}=TRUE"), "LED ON")
        step(13, post(f"/DO/{n}", "true"), E404, "LED stays ON (invalid body)")
        step(12, raw(f"POST /DO/{n} HTTP/1.1\r\n\r\n"), E404, "LED stays ON (no body)")
        step(6, post(f"/DO/{n}", "FALSE"), OK(f"DO{n}=FALSE"), "LED OFF")
        step(7, post(f"/DO/{n}", "1"), OK(f"DO{n}=TRUE"), "LED ON")
        step(14, post(f"/DO/{n}", "2"), E404, "LED stays ON (invalid body)")
        step(8, post(f"/DO/{n}", "0"), OK(f"DO{n}=FALSE"), "LED OFF")
        check("end", get(f"/DO/{n}"), OK(f"DO{n}=FALSE"), note="LED OFF")
    if args.ao:
        n = args.ao
        print(f"Writing AO{n} with {args.pause} s pauses. Measure the output of AO{n}.")
        step(10, post(f"/AO/{n}", "65535"), OK(f"AO{n}=65535"), "maximum")
        step(31, get(f"/AO/{n}"), OK(f"AO{n}=65535"), "maximum")
        step(15, post(f"/AO/{n}", "65536"), E404, "unchanged")
        step(11, post(f"/AO/{n}", "00123"), OK(f"AO{n}=123"), "123")
        step(16, post(f"/AO/{n}", "-1"), E404, "unchanged")
        step(9, post(f"/AO/{n}", "0"), OK(f"AO{n}=0"), "0")
        check("end", get(f"/AO/{n}"), OK(f"AO{n}=0"))
    if not args.do and not args.ao:
        sys.exit("write: give --do N and/or --ao N")


def mode_forbidden():
    check("403a", post("/DO/1", "TRUE"), E403)
    check("403b", post("/AO/999", "1"), E403, note="403 comes before the channel count")
    check("403c", post("/DI/1", "1"), E404)
    check("403d", get("/DO/1"), *digital("DO", 1), E404)


def mode_bulk():
    """Bulk extension on the simulation node (12 DI, 8 DO, 4 AI, 4 AO;
    DI1..8 follow DO1..8 one KBUS cycle later, AI4 follows AO1)."""
    # writes (start from a known state: earlier tests may have left values)
    post("/ALL", "DO=00000000;AO=0,0,0,0")
    check("b1", post("/DO", "10100000"), OK("DO=10100000"))
    check("b2", post("/DO", "01"), OK("DO=01100000"), note="only DO1, DO2 written")
    check("b3", post("/DO", "--1-----"), OK("DO=01100000"), note="'-' = unchanged")
    check("b4", post("/AO", "100,-,300"), OK("AO=100,0,300,0"))
    check("b5", post("/AO", "-,2"), OK("AO=100,2,300,0"))
    r = post("/ALL", "DO=11000000;AO=1,2,3,4")
    ok = r.startswith(OK("DI=")) and ";DO=11000000;AI=" in r and r.endswith(";AO=1,2,3,4")
    check("b6", r, r if ok else OK("DI=...;DO=11000000;AI=...;AO=1,2,3,4"))
    r = post("/ALL", "AO=5")
    ok = r.startswith(OK("DI=")) and ";DO=11000000;AI=" in r and r.endswith(";AO=5,2,3,4")
    check("b7", r, r if ok else OK("DI=...;DO=11000000;AI=...;AO=5,2,3,4"), note="AO part only")
    time.sleep(0.1)                                             # a few KBUS cycles
    # reads
    check("b8", get("/DO"), OK("DO=11000000"))
    check("b9", get("/AO"), OK("AO=5,2,3,4"))
    r = get("/DI")
    check("b10", r, r if r.startswith(OK("DI=11000000")) and len(r) == len(OK("DI=" + "0" * 12)) else OK("DI=11000000xxxx"))
    r = get("/AI")
    check("b11", r, r if r.startswith(OK("AI=")) and r.count(",") == 3 else OK("AI=a,b,c,d"))
    r = get("/ALL")
    parts = r[len(OK("")):].split(";") if r.startswith(OK("")) else []
    check("b12", r, r if [p[:3] for p in parts] == ["DI=", "DO=", "AI=", "AO="] else OK("DI=;DO=;AI=;AO="))
    # invalid bulk requests: nothing written
    for name, path, body in [("i1", "/DO", ""), ("i2", "/DO", "012"), ("i3", "/DO", "1" * 9),
                             ("i4", "/AO", "1,,2"), ("i5", "/AO", "65536"), ("i6", "/AO", "1,2,3,4,5"),
                             ("i7", "/AO", "1,"), ("i8", "/ALL", "DO=1;DO=0"), ("i9", "/ALL", "XX=1"),
                             ("i10", "/ALL", "DO=1;"), ("i11", "/DI", "1"), ("i12", "/AI", "1"),
                             ("i13", "/ALL", "DO=1;AO=99999")]:
        check(name, post(path, body), E404)
    check("i14", get("/DO"), OK("DO=11000000"), note="unchanged after the invalid writes")
    check("i15", get("/AO"), OK("AO=5,2,3,4"), note="i13 wrote nothing (atomic)")
    for name, path in [("i16", "/DO/"), ("i17", "/ALL/"), ("i18", "/ALLX"), ("i19", "/do"), ("i20", "/D")]:
        check(name, get(path), E404)
    # reset
    post("/ALL", "DO=00000000;AO=0,0,0,0")


def wda_req(method, path, body=None, ctype="application/vnd.api+json"):
    """Return (status, parsed JSON or None) for the wdx-style interface."""
    import json
    head = f"{method} {path} HTTP/1.1\r\nHost: x\r\n"
    if body is not None:
        data = json.dumps(body) if not isinstance(body, str) else body
        head += (f"Content-Type: {ctype}\r\n" if ctype else "") + f"Content-Length: {len(data)}\r\n\r\n" + data
    else:
        head += "\r\n"
    r = raw(head)
    status = int(r.split(" ", 2)[1]) if r.startswith("HTTP/1.0 ") else 0
    payload = r.split("\r\n\r\n", 1)[1] if "\r\n\r\n" in r else ""
    try:
        return status, json.loads(payload) if payload else None
    except ValueError:
        return status, None


def wcheck(name, got, ok, note=""):
    global failures
    failures += not ok
    print(f"{'PASS' if ok else 'FAIL'} {name:>6}  {str(got)[:90]}  {note}", flush=True)


def patch_body(pid, value, typ="parameters"):
    return {"data": {"type": typ, "id": pid, "attributes": {"value": value}}}


def mode_wda():
    """wdx-style interface on the SIMULATION node (12 DI, 8 DO, 4 AI, 4 AO)."""
    P = "/wda/parameters"
    do3 = "0-0-kbus-digitaloutputs-3-value"
    ao2 = "0-0-kbus-analogoutputs-2-value"
    st, d = wda_req("GET", P)
    ids = [x["id"] for x in d["data"]] if d else []
    wcheck("w1", (st, len(ids)), st == 200 and len(ids) == d["meta"]["total"] and len(set(ids)) == len(ids),
           "all parameters, unique ids")
    paths = {x["id"]: x["attributes"]["path"] for x in d["data"]} if d else {}
    rule = all(i == "0-0-" + p.lower().replace("/", "-") for i, p in paths.items())
    wcheck("w2", "id = 0-0- + lower(path) with / -> -", rule)
    st, d = wda_req("GET", P + "?page[limit]=3&page[offset]=2")
    wcheck("w3", (st, [x["id"] for x in d["data"]] if d else None),
           st == 200 and len(d["data"]) == 3 and d["data"][0]["id"] == "0-0-kbus-cyclecount", "paging")
    st, d = wda_req("GET", P + "?page%5Blimit%5D=1")
    wcheck("w4", st, st == 200 and len(d["data"]) == 1, "URL-encoded page[limit]")
    want = ["0-0-kbus-digitalinputvalues", "0-0-kbus-analoginputvalues", "0-0-kbus-digitaloutputvalues"]
    st, d = wda_req("GET", P + "?filter[id]=" + ",".join(want))
    got = [x["id"] for x in d["data"]] if d else []
    wcheck("w4b", got, st == 200 and sorted(got) == sorted(want), "filter[id], one GET")
    st, d = wda_req("GET", P + "?filter%5Bid%5D=" + "%2C".join(want[:2]))
    wcheck("w4c", st, st == 200 and len(d["data"]) == 2, "URL-encoded filter")
    st, d = wda_req("GET", P + "/0-0-kbus-modules")
    wcheck("w5", d, st == 200 and d["data"]["attributes"]["dataType"] == "instantiations")
    st, d = wda_req("GET", P + f"/{do3}/definition")
    wcheck("w6", d, st == 200 and d["data"]["attributes"]["writeable"] is True)
    st, d = wda_req("GET", P + "/0-0-kbus-digitalinputs-1-value/definition")
    wcheck("w7", d, st == 200 and d["data"]["attributes"]["writeable"] is False)
    # PATCH scalar
    st, _ = wda_req("PATCH", P + f"/{do3}", patch_body(do3, True))
    wcheck("w8", st, st == 204, "PATCH DO3 true")
    st, d = wda_req("GET", P + f"/{do3}")
    wcheck("w9", d, st == 200 and d["data"]["attributes"]["value"] is True)
    st, _ = wda_req("PATCH", P + f"/{ao2}", patch_body(ao2, 12345))
    st2, d = wda_req("GET", P + f"/{ao2}")
    wcheck("w10", (st, d["data"]["attributes"]["value"] if d else None), st == 204 and d["data"]["attributes"]["value"] == 12345)
    # PATCH array
    arr = "0-0-kbus-digitaloutputvalues"
    st, _ = wda_req("PATCH", P + f"/{arr}", patch_body(arr, [True, False, True]))
    st2, d = wda_req("GET", P + f"/{arr}")
    wcheck("w11", d["data"]["attributes"]["value"] if d else None,
           st == 204 and d["data"]["attributes"]["value"][:3] == [True, False, True], "array, first 3 channels")
    # errors
    for name, args_, exp in [
        ("e1", ("PATCH", P + f"/{do3}", patch_body(do3, True), "text/plain"), 415),
        ("e2", ("PATCH", P + f"/{do3}", patch_body(do3, True), None), 415),
        ("e3", ("PATCH", P + f"/{do3}", "{not json"), 400),
        ("e4", ("PATCH", P + f"/{do3}", patch_body("0-0-kbus-digitaloutputs-4-value", True)), 400),
        ("e5", ("PATCH", P + f"/{do3}", patch_body(do3, True, "foo")), 400),
        ("e6", ("PATCH", P + f"/{do3}", patch_body(do3, 1)), 400),
        ("e7", ("PATCH", P + f"/{ao2}", patch_body(ao2, 70000)), 409),
        ("e8", ("PATCH", P + f"/{ao2}", patch_body(ao2, -1)), 409),
        ("e9", ("PATCH", P + "/0-0-kbus-digitalinputs-1-value",
                patch_body("0-0-kbus-digitalinputs-1-value", True)), 409),
        ("e10", ("PATCH", P + f"/{arr}", patch_body(arr, [True] * 9)), 409),
        ("e11", ("PATCH", P, patch_body(do3, True)), 405),
        ("e12", ("GET", P + "/0-0-kbus-nothing"), 404),
        ("e13", ("GET", "/wda/other"), 404),
    ]:
        st, d = wda_req(*args_)
        wcheck(name, st, st == exp and d is not None and "errors" in d, f"expected {exp}")
    st, d = wda_req("GET", P + f"/{do3}")
    wcheck("e14", d, d["data"]["attributes"]["value"] is True, "unchanged after rejected writes")
    # the old interface still works next to it
    check("e15", get("/DO/3"), OK("DO3=TRUE"), note="single-channel API unchanged")
    post("/ALL", "DO=00000000;AO=0,0,0,0")


def main():
    global args
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host")
    p.add_argument("mode", choices=["sim", "bulk", "wda", "node", "write", "forbidden"])
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--do", type=int, help="write mode: DO channel to switch")
    p.add_argument("--ao", type=int, help="write mode: AO channel to write")
    p.add_argument("--pause", type=float, default=3.0, help="write mode: seconds between steps")
    args = p.parse_args()
    {"sim": mode_sim, "bulk": mode_bulk, "wda": mode_wda, "node": mode_node, "write": mode_write,
     "forbidden": mode_forbidden}[args.mode]()
    print(f"\nFAILURES: {failures}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
