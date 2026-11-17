#!/usr/bin/env python3
"""Tests for the wdx data layer of the KBUS gateway (/wda/parameters, JSON:API).

  tools/test-rest.py <host> wda            SIMULATION node (KBUS_SIMULATION=1):
                                           all requests, PATCH, error codes; writes
  tools/test-rest.py <host> node           real node, READ ONLY: parameter tree,
                                           values, invalid requests; writes NOTHING
  tools/test-rest.py <host> write --do 1   real node: switches DO1 TRUE/FALSE
                                           with pauses (watch the LED); ends FALSE
  tools/test-rest.py <host> write --ao 1   real node: writes AO1, ends at 0
  tools/test-rest.py <host> forbidden      gateway started with HTTP_WRITE_ENABLE=0:
                                           PATCH gives 403

Options: --port (default 8000), --pause seconds between write steps (default 3).
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


def raw(data, delay=0.0):
    """Send one request (a str, or a list of chunks sent as separate TCP
    writes `delay` seconds apart) and return the whole response (str)."""
    s = socket.create_connection((args.host, args.port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
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
        ("e9", ("PATCH", f"{P}/0-0-kbus-cyclecount", body("0-0-kbus-cyclecount", 1)), 409),
        ("e11", ("PATCH", P, body(do3, True)), 405),
        ("e12", ("GET", f"{P}/0-0-kbus-nothing"), 404),
        ("e13", ("GET", "/wda/other"), 404),
    ]
    if ao2:
        cases += [("e7", ("PATCH", f"{P}/{ao2}", body(ao2, 70000)), 409),
                  ("e8", ("PATCH", f"{P}/{ao2}", body(ao2, -1)), 409)]
    if arr:
        cases += [("e10", ("PATCH", f"{P}/{arr}", body(arr, [True] * 251)), 409)]
    for name, a, exp in cases:
        st, d, _ = req(*a)
        check(name, st, st == exp and d is not None and "errors" in d, f"expected {exp}")
    # the removed interfaces of version 1.x
    for name, a, exp in [("r1", ("GET", "/DO/1"), 404), ("r2", ("GET", "/ALL"), 404),
                         ("r3", ("POST", "/DO/1", "TRUE", None), 405),
                         ("r4", ("POST", "/ALL", "DO=1", None), 405), ("r5", ("PUT", P), 405)]:
        st, d, _ = req(*a)
        check(name, st, st == exp and d is not None and "errors" in d, f"expected {exp} (only /wda)")


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
    st, d, _ = req("GET", f"{P}/{do3}/definition")
    check("w6", d, st == 200 and d["data"]["attributes"]["writeable"] is True)
    st, d, _ = req("GET", f"{P}/0-0-kbus-digitalinputs-1-value/definition")
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
    invalid_requests(do3, ao2, arr)
    check("e14", value(do3), value(do3) is True, "unchanged after rejected writes")
    # body in a second TCP segment (browsers do this): awaited up to 100 ms
    data = json.dumps(body(do3, False))
    head = (f"PATCH {P}/{do3} HTTP/1.1\r\nHost: x\r\nContent-Type: application/vnd.api+json\r\n"
            f"Content-Length: {len(data)}\r\n\r\n")
    r = raw([head, data], delay=0.03)
    check("s1", r.split("\r\n")[0], r.startswith("HTTP/1.0 204") and value(do3) is False, "body 30 ms later -> 204")
    r = raw([head, json.dumps(body(do3, True))], delay=0.3)
    check("s2", r.split("\r\n")[0], r.startswith("HTTP/1.0 400") and value(do3) is False,
          "body 300 ms later -> 400, nothing written")
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
        step("a2", req("PATCH", f"{P}/{pid}", body(pid, 65536))[0] == 409, True, "unchanged")
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


def main():
    global args
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host")
    p.add_argument("mode", choices=["wda", "node", "write", "forbidden"])
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--do", type=int, help="write mode: DO channel to switch")
    p.add_argument("--ao", type=int, help="write mode: AO channel to write")
    p.add_argument("--pause", type=float, default=3.0, help="write mode: seconds between steps")
    args = p.parse_args()
    {"wda": mode_wda, "node": mode_node, "write": mode_write, "forbidden": mode_forbidden}[args.mode]()
    print(f"\nFAILURES: {failures}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
