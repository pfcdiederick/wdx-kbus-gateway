#!/usr/bin/env python3
"""Answer CODESYS by hand from Python: show the texts CODESYS writes, send your answer.

    tools/wdx-chat.py                       # PFC 192.168.1.17, through the manager (port 8080)
    tools/wdx-chat.py --url http://10.0.0.5:8080 --user admin --password secret
    tools/wdx-chat.py --send "Hello CODESYS"                 # one answer, then exit
    tools/wdx-chat.py --socket /run/kbus-gateway/wdx.sock   # on the controller / in a container

Every new text in the register Codesys/Message (written by CODESYS with
FB_WdxWriteString) is printed. Type a line and press Enter: it is written into
Python/Response, which CODESYS reads with FB_WdxReadString (PRG_TextExchange.sAnswer).
Empty line = show the current values, /quit or Ctrl+D = exit.

The example container answers CODESYS automatically as well; to answer only by
hand, deploy it without automatic answers:
    CL_ENV="-e PYTHON_RESPONSE=" tools/deploy-docker.sh <pfc-ip> --client --no-gateway
Standard library only (Python 3.8+).
"""
import argparse
import base64
import http.client
import json
import socket
import sys
import threading
import time
import urllib.error
import urllib.request

TEXT = "0-0-registers-codesys-message-value"       # CODESYS -> Python
RESPONSE = "0-0-registers-python-response-value"   # Python -> CODESYS
P = "/wda/parameters"
JSONAPI = "application/vnd.api+json"


class Wdx:
    """GET/PATCH of one parameter, over HTTP (manager, gateway) or the Unix socket."""

    def __init__(self, url=None, sock=None, user=None, password=None):
        self.url, self.sock = (url or "").rstrip("/"), sock
        self.auth = ("Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()) if user else None

    def _request(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        headers = {"Content-Type": JSONAPI} if data else {}
        if self.auth:
            headers["Authorization"] = self.auth
        if self.sock:
            c = http.client.HTTPConnection("localhost", timeout=3)
            c.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            c.sock.settimeout(3)
            c.sock.connect(self.sock)
            try:
                c.request(method, path, data, headers)
                r = c.getresponse()
                status, raw = r.status, r.read()
            finally:
                c.close()
        else:
            req = urllib.request.Request(self.url + path, data, headers, method=method)
            try:
                with urllib.request.urlopen(req, timeout=3) as r:
                    status, raw = r.status, r.read()
            except urllib.error.HTTPError as e:
                status, raw = e.code, e.read()
        doc = json.loads(raw) if raw and raw[:1] == b"{" else None
        if status >= 400:
            if status == 401:
                raise RuntimeError("401: login required (--user / --password)")
            detail = doc["errors"][0].get("detail", "") if doc and doc.get("errors") else raw[:80].decode(errors="replace")
            raise RuntimeError(f"{status}: {detail}")
        return doc

    def get(self, pid):
        return self._request("GET", f"{P}/{pid}")["data"]["attributes"]["value"]

    def set(self, pid, value):
        self._request("PATCH", f"{P}/{pid}", {"data": {"type": "parameters", "id": pid, "attributes": {"value": value}}})


def fit(text, limit=127):
    """At most 127 bytes UTF-8 (size of a string register), without breaking a character."""
    return text.encode("utf-8")[:limit].decode("utf-8", "ignore")


def main():
    ap = argparse.ArgumentParser(description="Answer CODESYS by hand (register Python/Response)")
    ap.add_argument("--url", default="http://192.168.1.17:8080", help="manager (default) or gateway URL")
    ap.add_argument("--socket", help="gateway Unix socket instead of --url (on the controller)")
    ap.add_argument("--user", help="manager login, if switched on")
    ap.add_argument("--password", help="manager login, if switched on")
    ap.add_argument("--send", help="write this answer once and exit")
    a = ap.parse_args()
    wdx = Wdx(a.url, a.socket, a.user, a.password)

    try:
        text, answer = wdx.get(TEXT), wdx.get(RESPONSE)
    except (OSError, RuntimeError) as e:
        sys.exit(f"cannot read the registers: {e}")

    if a.send is not None:
        wdx.set(RESPONSE, fit(a.send))
        print(f"answer sent: {fit(a.send)!r}")
        return 0

    print(f"connected to {a.socket or a.url}")
    print(f"  CODESYS wrote : {text!r}")
    print(f"  answer now    : {answer!r}")
    print("Type an answer + Enter to send it to CODESYS; empty line = show values; /quit = exit.\n")

    stop = threading.Event()

    def watch():                                        # print every new text from CODESYS
        last, err = text, None
        while not stop.is_set():
            try:
                t = wdx.get(TEXT)
                err = None
                if t != last:
                    last = t
                    print(f"\n[{time.strftime('%H:%M:%S')}] CODESYS: {t!r}\n> ", end="", flush=True)
            except (OSError, RuntimeError) as e:
                if str(e) != err:
                    err = str(e)
                    print(f"\n(read error: {e})\n> ", end="", flush=True)
            stop.wait(0.2)

    threading.Thread(target=watch, daemon=True).start()
    try:
        while True:
            line = input("> ")
            if line.strip() == "/quit":
                break
            if not line.strip():
                print(f"  CODESYS wrote : {wdx.get(TEXT)!r}\n  answer now    : {wdx.get(RESPONSE)!r}")
                continue
            ans = fit(line)
            try:
                wdx.set(RESPONSE, ans)
                print(f"  sent to CODESYS: {ans!r}" + ("  (cut to 127 bytes)" if ans != line else ""))
            except (OSError, RuntimeError) as e:
                print(f"  not sent: {e}")
    except (EOFError, KeyboardInterrupt):
        print()
    stop.set()
    return 0


if __name__ == "__main__":
    sys.exit(main())
