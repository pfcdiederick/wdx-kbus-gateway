"""kbus-manager: web management for the kbus-gateway, reachable from the network.

The gateway itself only listens on the controller (Unix socket, 127.0.0.1).
This container publishes a web interface and talks to the gateway through
the Unix socket:

  /                      management page: status, I/O, parameters, registers, settings
                         (all links relative: the page also works behind a reverse proxy
                         prefix, e.g. https://<pfc>/kbus/ through the controller's lighttpd)
  /io                    the gateway's live I/O page
  /wda/parameters/...    the gateway's wdx data layer (GET; PATCH unless read-only)
  /api/definitions       all parameters with their definitions (one request)
  /api/settings          login settings (GET; POST JSON {login, user, password})
  /api/info              date and time of the controller (status bar)
  /wbm-assets/...        fonts, logo and favicon of the controller's own WBM (look of the page)

Login: the website itself is open by default ("Login required" on the Settings
tab closes it). Reset Statistics, Local Control and changing the login settings
ALWAYS need the login: user and password from MGMT_DATA/settings.json (password
only as a PBKDF2 hash), default admin / wago until a password is set. Mount
MGMT_DATA so the setting survives a new container.

Local control: the manager talks to the gateway through the gateway's control
socket (CONTROL_SOCKET, mounted only into this container). Only requests on it
may switch KBus/LocalControl and KBus/ResetStatistics, and write the KBUS while
local control is on; other clients (CODESYS, containers) then get 403.

Environment
  MGMT_DATA       directory for settings.json, default /data
  MGMT_USER / MGMT_PASSWORD   initial login, used only while settings.json does not exist
  MGMT_NO_LOGIN   1 = ignore MGMT_PASSWORD (no login until one is set on the page)
  MGMT_PORT       default 8080 (inside the container)
  MGMT_PREFIX     path prefix the page may be reached under, default /kbus: /kbus -> 301 /kbus/,
                  /kbus/<x> is served as /<x> (the controller's lighttpd forwards
                  http(s)://<pfc>/kbus/ unchanged, tools/deploy-docker.sh --manager);
                  the page also stays reachable without the prefix. "" = off
  MGMT_WBM_URL    WBM to take fonts and logo from, default https://<docker host>/wbm
  MGMT_READONLY   1 = no writes (PATCH answers 403), default 0
  MGMT_DEFAULT_USER / MGMT_DEFAULT_PASSWORD   login until one is set, default admin / wago
  WDX_SOCKET      default /run/kbus-gateway/wdx.sock
  CONTROL_SOCKET  default /run/kbus-gateway-control/control.sock (used when present)
"""
import base64
import hashlib
import hmac
import http.client
import json
import os
import re
import socket
import ssl
import struct
import urllib.request
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SOCKET = os.environ.get("WDX_SOCKET", "/run/kbus-gateway/wdx.sock")
CONTROL_SOCKET = os.environ.get("CONTROL_SOCKET", "/run/kbus-gateway-control/control.sock")
DEFAULT_USER = os.environ.get("MGMT_DEFAULT_USER", "admin")
DEFAULT_PASSWORD = os.environ.get("MGMT_DEFAULT_PASSWORD", "wago")
# parameters that only work after the login (and only through the control socket)
PROTECTED_IDS = (b"0-0-kbus-resetstatistics", b"0-0-kbus-localcontrol")
PORT = int(os.environ.get("MGMT_PORT", "8080"))
PREFIX = os.environ.get("MGMT_PREFIX", "/kbus").rstrip("/")
READONLY = os.environ.get("MGMT_READONLY", "0") == "1"
DATA = os.environ.get("MGMT_DATA", "/data")
SETTINGS_FILE = os.path.join(DATA, "settings.json")
HERE = os.path.dirname(os.path.abspath(__file__))
MAX_BODY = 16384
JSONAPI = "application/vnd.api+json"


class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path, timeout=3):
        super().__init__("localhost", timeout=timeout)
        self.unix_path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.unix_path)


def gateway_socket():
    """The control socket when it is mounted, else the normal socket."""
    return CONTROL_SOCKET if CONTROL_SOCKET and os.path.exists(CONTROL_SOCKET) else SOCKET


def gateway(method, path, body=None, ctype=None):
    """One request to the gateway: (status, content-type, body bytes).
    A 301 inside /wda (e.g. .../{id}/definition -> /wda/parameter-definitions/{id})
    is followed here: the browser may see this page behind a prefix (/kbus/),
    where the gateway's absolute Location would point to the controller's own WDA."""
    for _ in range(3):
        c = UnixConnection(gateway_socket())
        try:
            c.request(method, path, body, {"Content-Type": ctype} if ctype else {})
            r = c.getresponse()
            st, ct, data, loc = r.status, r.getheader("Content-Type", JSONAPI), r.read(), r.getheader("Location", "")
        finally:
            c.close()
        if st != 301 or method != "GET" or not loc.startswith("/wda/"):
            break
        path = loc
    return st, ct, data


# ---- login settings ----------------------------------------------------------
ITERATIONS = 100_000              # PBKDF2; checked once per credential, then cached
USER_RE = re.compile(r"^[A-Za-z0-9_.@-]{1,32}$")


def hash_password(pw, salt=None):
    salt = salt or os.urandom(16)
    dk = hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, ITERATIONS)
    return f"pbkdf2_sha256${ITERATIONS}${salt.hex()}${dk.hex()}"


def check_password(pw, stored):
    try:
        _, it, salt, dk = stored.split("$")
        got = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), int(it))
        return hmac.compare_digest(got.hex(), dk)
    except (ValueError, AttributeError):
        return False


class Settings:
    """login on/off, user, password hash; stored in settings.json."""

    def __init__(self):
        self.lock = threading.Lock()
        self.login, self.user, self.pw_hash = False, "admin", ""
        self.ok_cache = set()           # sha256 of accepted Authorization headers
        self.source = "default"
        try:
            with open(SETTINGS_FILE) as f:
                d = json.load(f)
            self.login, self.user, self.pw_hash = bool(d["login"]), d["user"], d.get("password", "")
            self.source = SETTINGS_FILE
        except FileNotFoundError:
            pw = os.environ.get("MGMT_PASSWORD", "")
            if pw and os.environ.get("MGMT_NO_LOGIN", "0") != "1":      # older manager.env
                self.login, self.user = True, os.environ.get("MGMT_USER", "admin")
                self.pw_hash = hash_password(pw)
                self.source = "environment (MGMT_PASSWORD)"
        except (ValueError, KeyError, TypeError) as e:
            # damaged file: fail closed rather than open
            print(f"{SETTINGS_FILE} unreadable ({e}): login required, but no password works; "
                  "fix or delete the file", file=sys.stderr, flush=True)
            self.login, self.pw_hash = True, ""
            self.source = "damaged settings file"
        if self.login and not self.pw_hash and self.source != "damaged settings file":
            self.login = False
        if not self.pw_hash and self.source != "damaged settings file":
            # no password set yet: the default login (Reset Statistics, Local Control)
            self.user, self.pw_hash = DEFAULT_USER, hash_password(DEFAULT_PASSWORD)
        self.default_pw = bool(self.pw_hash) and check_password(DEFAULT_PASSWORD, self.pw_hash)

    def public(self):
        return {"login": self.login, "user": self.user, "passwordSet": bool(self.pw_hash),
                "defaultPassword": self.default_pw, "stored": os.path.isfile(SETTINGS_FILE)}

    def check(self, header):
        """Access to the website: free while the login is off."""
        return not self.login or self.verify(header)

    def verify(self, header):
        """Valid user and password (always checked: protected functions)."""
        if not header.startswith("Basic "):
            return False
        key = hashlib.sha256(header.encode()).digest()
        with self.lock:
            if key in self.ok_cache:
                return True
            user, pw_hash = self.user, self.pw_hash
        try:
            user_in, _, pw = base64.b64decode(header[6:]).decode().partition(":")
        except (ValueError, UnicodeDecodeError):
            return False
        ok = hmac.compare_digest(user_in.encode(), user.encode()) and check_password(pw, pw_hash)
        if ok:
            with self.lock:
                if self.pw_hash == pw_hash:
                    self.ok_cache.add(key)
        return ok

    def update(self, login, user, password):
        """Validate and store; returns an error text or None."""
        if not isinstance(login, bool):
            return "login must be true or false"
        user = (user or "").strip() or self.user
        if not USER_RE.match(user):
            return "user: 1-32 characters A-Z a-z 0-9 _ . @ -"
        if password is not None and not isinstance(password, str):
            return "password must be text"
        if password:
            if len(password) < 4 or len(password) > 128:
                return "password: 4-128 characters"
            new_hash = hash_password(password)
        else:
            new_hash = self.pw_hash
        if login and not new_hash:
            return "enter a password to switch the login on"
        d = {"login": login, "user": user, "password": new_hash}
        os.makedirs(DATA, exist_ok=True)
        tmp = SETTINGS_FILE + ".tmp"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(d, f)
        os.replace(tmp, SETTINGS_FILE)
        default_pw = check_password(DEFAULT_PASSWORD, new_hash)
        with self.lock:
            self.login, self.user, self.pw_hash = login, user, new_hash
            self.default_pw = default_pw
            self.ok_cache.clear()
            self.source = SETTINGS_FILE
        return None


SETTINGS = Settings()


# ---- look of the WBM: fonts and logo come from the controller's own WBM ----------
# (not part of this project; fetched once, kept in memory; without them the page
# uses system fonts and a text logo)
WBM_ASSETS = {
    "images/wago-logo.svg": "image/svg+xml",
    "images/favicon.png": "image/png",
    **{f"fonts/open-sans-latin-{w}.woff2": "font/woff2" for w in ("300", "400", "400italic", "600", "700")},
}
_asset_cache = {}                       # path -> (time, bytes or None)


def wbm_url():
    url = os.environ.get("MGMT_WBM_URL", "")
    if url:
        return url.rstrip("/")
    try:                                # container: the docker host is the default gateway
        with open("/proc/net/route") as f:
            for line in f.readlines()[1:]:
                p = line.split()
                if p[1] == "00000000":
                    return "https://" + socket.inet_ntoa(struct.pack("<L", int(p[2], 16))) + "/wbm"
    except OSError:
        pass
    return "https://127.0.0.1/wbm"


def wbm_asset(path):
    t, data = _asset_cache.get(path, (0, None))
    if data is not None or time.time() - t < 60:          # failures are retried after 60 s
        return data
    ctx = ssl.create_default_context()
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE   # the controller's self-signed certificate
    try:
        with urllib.request.urlopen(f"{wbm_url()}/{path}", context=ctx, timeout=3) as r:
            data = r.read(2_000_000) if r.status == 200 else None
    except (OSError, ValueError):
        data = None
    _asset_cache[path] = (time.time(), data)
    return data


def jsonapi_error(status, title, detail):
    return json.dumps({"errors": [{"status": str(status), "title": title, "detail": detail}]}).encode()


class Handler(BaseHTTPRequestHandler):
    server_version = "kbus-manager"
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stdout.write("%s %s %s\n" % (time.strftime("%H:%M:%S"), self.address_string(), fmt % args))

    def send(self, status, ctype, body, extra=None, cache=False):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "max-age=86400" if cache else "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "SAMEORIGIN")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def authorized(self):
        return SETTINGS.check(self.headers.get("Authorization", ""))

    def logged_in(self):
        return SETTINGS.verify(self.headers.get("Authorization", ""))

    def login_required(self, what):
        """401 WITHOUT WWW-Authenticate: the page shows its own login dialog."""
        time.sleep(0.5)                                   # slows down password guessing
        self.send(401, JSONAPI, jsonapi_error(401, "Login required", f"{what}: log in first"),
                  {"X-Login": "required"})

    def deny(self):
        time.sleep(0.5)                                   # slows down password guessing
        self.send(401, "text/plain", b"login required\n",
                  {"WWW-Authenticate": 'Basic realm="kbus-manager", charset="UTF-8"'})

    def parse_request(self):
        """Strip MGMT_PREFIX: /kbus/api/info -> /api/info. The bare prefix is
        redirected to prefix + "/" so the relative links of the page resolve."""
        if not super().parse_request():
            return False
        self.prefix_redirect = None
        if PREFIX:
            rest = self.path[len(PREFIX):]
            if self.path.startswith(PREFIX) and (rest == "" or rest.startswith("?")):
                self.prefix_redirect = PREFIX + "/" + rest
            elif self.path.startswith(PREFIX + "/"):
                self.path = rest
        return True

    def proxy(self, method, body=None):
        try:
            st, ct, data = gateway(method, self.path, body, self.headers.get("Content-Type"))
        except OSError as e:
            return self.send(502, JSONAPI, jsonapi_error(502, "Bad Gateway", f"kbus-gateway not reachable: {e}"))
        self.send(st, ct, data)

    def do_GET(self):
        if self.prefix_redirect:
            return self.send(301, "text/plain", b"", {"Location": self.prefix_redirect})
        if not self.authorized():
            return self.deny()
        p = self.path.split("?", 1)[0]
        if p in ("/", "/index.html"):
            with open(os.path.join(HERE, "manager.html"), "rb") as f:
                page = f.read().replace(b"__READONLY__", b"true" if READONLY else b"false")
            return self.send(200, "text/html; charset=utf-8", page)
        if p == "/io":
            try:
                st, ct, data = gateway("GET", "/")
            except OSError as e:
                return self.send(502, "text/plain", f"kbus-gateway not reachable: {e}\n".encode())
            return self.send(st, ct, data)
        if p == "/api/definitions":
            return self.definitions()
        if p == "/api/settings":
            return self.send(200, "application/json", json.dumps(SETTINGS.public()).encode())
        if p == "/api/login":                            # check user + password (login dialog)
            if not self.logged_in():
                return self.login_required("login")
            return self.send(200, "application/json",
                             json.dumps({"user": SETTINGS.user, "defaultPassword": SETTINGS.default_pw}).encode())
        if p == "/api/info":
            return self.send(200, "application/json",
                             json.dumps({"date": time.strftime("%d.%m.%Y"), "time": time.strftime("%H:%M:%S")}).encode())
        if p.startswith("/wbm-assets/"):
            name = p[len("/wbm-assets/"):]
            if name not in WBM_ASSETS:
                return self.send(404, "text/plain", b"not found\n")
            data = wbm_asset(name)
            if data is None:
                return self.send(404, "text/plain", b"WBM not reachable\n")
            return self.send(200, WBM_ASSETS[name], data, cache=True)
        if p.startswith("/wda/"):
            return self.proxy("GET")
        self.send(404, "text/plain", b"not found\n")

    do_HEAD = do_GET

    def do_PATCH(self):
        if not self.authorized():
            return self.deny()
        if not self.path.startswith("/wda/"):
            return self.send(404, "text/plain", b"not found\n")
        if READONLY:
            return self.send(403, JSONAPI, jsonapi_error(403, "Forbidden", "kbus-manager is read-only (MGMT_READONLY=1)"))
        n = int(self.headers.get("Content-Length", "0") or 0)
        if n > MAX_BODY:
            return self.send(413, JSONAPI, jsonapi_error(413, "Payload Too Large", "max 16384 bytes"))
        body = self.rfile.read(n)
        target = self.path.encode() + b" " + body
        if any(pid in target for pid in PROTECTED_IDS) and not self.logged_in():
            return self.login_required("Reset Statistics / Local Control")
        self.proxy("PATCH", body)

    def definitions(self):
        """All parameters with value and definition, collected over the socket
        (two paged collections: /wda/parameters and /wda/parameter-definitions)."""
        try:
            params, defs = [], {}
            for url, sink in (("/wda/parameters", params), ("/wda/parameter-definitions", None)):
                while url:
                    st, _, data = gateway("GET", url)
                    if st != 200:
                        return self.send(st, JSONAPI, data)
                    doc = json.loads(data)
                    for x in doc["data"]:
                        if sink is None:
                            defs[x["id"]] = x["attributes"]
                        else:
                            sink.append(x)
                    url = doc.get("links", {}).get("next")
            out = []
            for x in params:
                a = dict(id=x["id"], **x["attributes"])
                da = defs.get(x["id"], {})
                a["writeable"], a["userSetting"] = da.get("writeable"), da.get("userSetting")
                out.append(a)
        except OSError as e:
            return self.send(502, JSONAPI, jsonapi_error(502, "Bad Gateway", f"kbus-gateway not reachable: {e}"))
        self.send(200, "application/json", json.dumps({"data": out, "readonly": READONLY}).encode())

    def do_POST(self):
        if not self.authorized():
            return self.deny()
        if self.path != "/api/settings":
            return self.send(405, "text/plain", b"use GET or PATCH\n")
        if not self.logged_in():                          # else anyone could set a new password
            return self.login_required("changing the login settings")
        # JSON only: a cross-site form cannot send it without a CORS preflight (CSRF)
        if self.headers.get("Content-Type", "").split(";")[0].strip() != "application/json":
            return self.send(415, "application/json", b'{"error": "Content-Type must be application/json"}')
        n = int(self.headers.get("Content-Length", "0") or 0)
        if n > 4096:
            return self.send(413, "application/json", b'{"error": "too large"}')
        try:
            d = json.loads(self.rfile.read(n) or b"{}")
            err = SETTINGS.update(d.get("login"), d.get("user"), d.get("password"))
        except (ValueError, AttributeError):
            err = "invalid JSON"
        except OSError as e:
            err = f"cannot store the settings: {e}"
        if err:
            return self.send(400, "application/json", json.dumps({"error": err}).encode())
        print(f"settings changed: login {'on, user ' + SETTINGS.user if SETTINGS.login else 'off'}", flush=True)
        self.send(200, "application/json", json.dumps(SETTINGS.public()).encode())

    def do_PUT(self):
        self.send(405, "text/plain", b"use GET, PATCH or POST\n")

    do_DELETE = do_PUT


def main():
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    srv.daemon_threads = True
    login = (f"login on, user {SETTINGS.user}" if SETTINGS.login else "no login") + f" (from {SETTINGS.source})"
    sock = "control socket " + CONTROL_SOCKET if gateway_socket() == CONTROL_SOCKET else \
        f"socket {SOCKET} (no control socket: no Local Control / Reset Statistics)"
    print(f"kbus-manager: gateway via {sock}" +
          (", DEFAULT login (change the password on the Settings tab)" if SETTINGS.default_pw else ""), flush=True)
    print(f"kbus-manager on port {PORT}{', also under ' + PREFIX + '/' if PREFIX else ''}, {login}, {'read-only' if READONLY else 'writes allowed'}, "
          f"gateway socket {SOCKET}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
