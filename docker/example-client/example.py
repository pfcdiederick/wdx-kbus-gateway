"""Example container application for the kbus-gateway.

Every PERIOD_S seconds it
  - reads the digital inputs and outputs and the register ChangeCounter in one request,
  - prints what changed,
  - writes its own registers Example/Heartbeat (+1) and Example/Status in one batch.
  - reads the text that a client (e.g. CODESYS) writes into the register
    Codesys/Message, prints it when it changes and answers it with respond():
    the answer goes into the register Python/Response, where the client reads it.
It writes no KBUS outputs unless MIRROR_DO is set (e.g. MIRROR_DO=8: DO8 follows DI1).
Careful: if another program (CODESYS) writes the whole output array, both fight
over that output.

Needs these lines in /etc/kbus-gateway-registers.conf (then restart the gateway):
    Example/Heartbeat  uint32
    Example/Status     string  ""
    Codesys/Message    string  ""        (optional: the text from CODESYS)
    Python/Response    string  ""        (optional: the answer for CODESYS)

Environment: WDX_SOCKET (default /run/kbus-gateway/wdx.sock), PERIOD_S (default 1),
MIRROR_DO (default off), CODESYS_TEXT (id of the text register, default
0-0-registers-codesys-message-value, "" = do not read it), PYTHON_RESPONSE (id of
the answer register, default 0-0-registers-python-response-value, "" = no answer),
TEXT_POLL_S (how often the text register is checked, default 0.1).

The text register is polled: a text that CODESYS replaces within TEXT_POLL_S is
not seen. For request/answer, let the client wait for the answer before it writes
the next text.
"""
import os
import signal
import sys
import time

from wdx_client import WdxClient, WdxError

DI, DO, CC = "0-0-kbus-digitalinputvalues", "0-0-kbus-digitaloutputvalues", "0-0-registers-changecounter"
HB, ST = "0-0-registers-example-heartbeat-value", "0-0-registers-example-status-value"
TEXT = os.environ.get("CODESYS_TEXT", "0-0-registers-codesys-message-value")
RESPONSE = os.environ.get("PYTHON_RESPONSE", "0-0-registers-python-response-value")
RESPONSE_MAX = 127                        # bytes UTF-8: size of a string register


def respond(message):
    """Answer to a text from CODESYS. The result is written into the register
    Python/Response, where the client reads it.
    Adapt this function to your application."""
    m = message.strip()
    if not m:
        return ""
    if m.lower() == "ping":
        return "pong"
    if m.lower() == "time":
        return time.strftime("%d.%m.%Y %H:%M:%S")
    return f"OK {time.strftime('%H:%M:%S')}: {m}"


def fit(text, limit=RESPONSE_MAX):
    """Cut text to at most limit bytes UTF-8 without breaking a character."""
    return text.encode("utf-8")[:limit].decode("utf-8", "ignore")

running = True


def stop(*_):
    global running
    running = False


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def main():
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    wdx = WdxClient(os.environ.get("WDX_SOCKET", "/run/kbus-gateway/wdx.sock"))
    period = float(os.environ.get("PERIOD_S", "1"))
    text_poll = float(os.environ.get("TEXT_POLL_S", "0.1"))
    mirror = int(os.environ.get("MIRROR_DO", "0") or 0)

    # wait for the gateway (it may start after this container)
    while running:
        try:
            healthy = wdx.get("0-0-kbus-healthy")
            log(f"connected to the gateway, KBUS healthy={healthy}")
            break
        except (OSError, WdxError) as e:
            log(f"waiting for the gateway: {e}")
            time.sleep(2)
    try:
        heartbeat = wdx.get(HB)
    except WdxError as e:
        log(f"registers Example/Heartbeat and Example/Status are not declared ({e}); see example.py")
        return 1

    text, response, last_text, text_err = TEXT, RESPONSE, None, None
    if text:
        try:
            last_text = wdx.get(text)
            log(f"CODESYS text: {last_text!r}")
        except WdxError as e:
            log(f"register {text} not readable ({e}): declare Codesys/Message in the register file")
            text = ""
    if text and response:
        try:
            wdx.get(response)
        except WdxError as e:
            log(f"register {response} not readable ({e}): declare Python/Response to answer CODESYS")
            response = ""

    last = {}
    next_period = time.monotonic()
    while running:
        # the text from CODESYS: checked every TEXT_POLL_S, answered at once
        if text:
            try:
                t = wdx.get(text)
                text_err = None
                if t != last_text:
                    last_text = t
                    log(f"CODESYS text: {t!r}")
                    if response:
                        answer = fit(respond(t))
                        wdx.set(response, answer)
                        log(f"answer to CODESYS: {answer!r}")
            except (OSError, WdxError) as e:
                if str(e) != text_err:                    # log an error once, not 10 times a second
                    text_err = str(e)
                    log(f"text register: {e}")
        if time.monotonic() < next_period:
            time.sleep(min(text_poll, max(0.0, next_period - time.monotonic())) if text else
                       max(0.0, next_period - time.monotonic()))
            continue
        next_period = time.monotonic() + period
        try:
            v = wdx.get_many([DI, DO, CC])
            for key, name in ((DI, "DI"), (DO, "DO")):
                if v[key] != last.get(key):
                    log(f"{name} = {''.join('1' if x else '0' for x in v[key])}")
            if last and v[CC] != last.get(CC):
                log(f"registers changed (ChangeCounter {last[CC]} -> {v[CC]})")
            last = v
            heartbeat = (heartbeat + 1) % 2**32
            writes = {HB: heartbeat, ST: f"running, DI1={'on' if v[DI] and v[DI][0] else 'off'}"}
            if mirror and v[DI]:
                writes[f"0-0-kbus-digitaloutputs-{mirror}-value"] = bool(v[DI][0])
            wdx.set_many(writes)
            last[CC] = wdx.get(CC)            # do not report our own write as a change
        except (OSError, WdxError) as e:
            log(f"error: {e}")
    try:
        wdx.set(ST, "stopped")
    except (OSError, WdxError):
        pass
    log("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
