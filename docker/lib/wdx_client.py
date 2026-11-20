"""Minimal client for the wdx data layer of the kbus-gateway (standard library only).

Talks to the gateway through its Unix socket (mount /run/kbus-gateway into the
container) or, with a host/port, over TCP.

    from wdx_client import WdxClient
    wdx = WdxClient()                                    # /run/kbus-gateway/wdx.sock
    di = wdx.get("0-0-kbus-digitalinputvalues")          # one value
    vals = wdx.get_many(["0-0-kbus-digitalinputvalues",  # several values, one snapshot
                         "0-0-registers-changecounter"])
    wdx.set("0-0-registers-pump1-setpoint-value", 3.5)   # one parameter
    wdx.set_many({"0-0-registers-a-value": 1,            # several, all or nothing
                  "0-0-registers-b-value": "x"})

Ids: "0-0-" + lower-case path with "/" -> "-", e.g.
Registers/Pump1/Setpoint/Value -> 0-0-registers-pump1-setpoint-value.
"""
import http.client
import json
import socket

P = "/wda/parameters"
JSONAPI = "application/vnd.api+json"


class WdxError(Exception):
    """The gateway rejected a request (status and JSON:API error detail)."""

    def __init__(self, status, detail, errors=()):
        super().__init__(f"{status}: {detail}")
        self.status = status
        self.detail = detail
        self.errors = list(errors)      # JSON:API error objects (code = WDA status code)


class _UnixConnection(http.client.HTTPConnection):
    def __init__(self, path, timeout):
        super().__init__("localhost", timeout=timeout)
        self.unix_path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.unix_path)


class WdxClient:
    def __init__(self, socket_path="/run/kbus-gateway/wdx.sock", host=None, port=8000, timeout=2.0):
        self.socket_path, self.host, self.port, self.timeout = socket_path, host, port, timeout

    def _request(self, method, url, body=None):
        if self.host:
            c = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        else:
            c = _UnixConnection(self.socket_path, self.timeout)
        try:
            data = json.dumps(body) if body is not None else None
            c.request(method, url, data, {"Content-Type": JSONAPI} if data else {})
            r = c.getresponse()
            raw = r.read()
        finally:
            c.close()
        doc = json.loads(raw) if raw else None
        if r.status >= 400:
            errs = doc.get("errors", []) if doc else []
            raise WdxError(r.status, "; ".join(e.get("detail", "") for e in errs), errs)
        return doc

    # ---- reading -----------------------------------------------------------
    def get(self, pid):
        """Value of one parameter."""
        return self._request("GET", f"{P}/{pid}")["data"]["attributes"]["value"]

    def get_many(self, ids):
        """{id: value} for several parameters, all from one snapshot."""
        doc = self._request("GET", f"{P}?filter[id]={','.join(ids)}")
        return {x["id"]: x["attributes"]["value"] for x in doc["data"]}

    def parameters(self):
        """All parameters: [{id, path, dataType, dataRank, value[, stringValue]}, ...].
        The collection is paged (255 per page): links.next is followed."""
        out, url = [], P
        while url:
            doc = self._request("GET", url)
            out += [dict(id=x["id"], **x["attributes"]) for x in doc["data"]]
            url = doc.get("links", {}).get("next")
        return out

    def definition(self, pid):
        """path, dataType, dataRank, writeable, userSetting (persistent), beta, deprecated."""
        return self._request("GET", f"/wda/parameter-definitions/{pid}")["data"]["attributes"]

    # ---- writing -----------------------------------------------------------
    def set(self, pid, value):
        self._request("PATCH", f"{P}/{pid}",
                      {"data": {"type": "parameters", "id": pid, "attributes": {"value": value}}})

    def set_many(self, values):
        """Write several parameters (registers and/or outputs): all or nothing."""
        self._request("PATCH", P, {"data": [{"type": "parameters", "id": i, "attributes": {"value": v}}
                                            for i, v in values.items()]})
