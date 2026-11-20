"""
WDX (kbus-gateway data layer) -> IEC 60870-5-104 gateway

Polls the KBus values from the wdx data layer of the kbus-gateway (JSON:API, through
its Unix socket /run/kbus-gateway/wdx.sock, or TCP 127.0.0.1:8000 with the host
network) and offers them on an IEC 60870-5-104 server (slave / outstation).

Currently supported signals (see [points] in the parameter file):
    DI<n>  digital input n (1-based) -> M_SP_TB_1 (single point with time tag)

The kbus-gateway is read with wdx_client.py, the client library shared by all
containers of this project (docker/lib/wdx_client.py).
"""
import argparse
import configparser
import logging
import os
import re
import signal
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional

# c104 library (Python binding of mz-automation/lib60870-C) is used as IEC 60870-5-104 server
import c104

# WDX client library (docker/lib/wdx_client.py, copied next to this file in the image)
from wdx_client import WdxClient, WdxError

ID_DIGITAL_INPUTS = "0-0-kbus-digitalinputvalues"     # KBus/DigitalInputValues, boolean[]
DEFAULT_CONFIG_FILE = "/app/wdx_iec104_config.ini"

# Signal kind -> wdx parameter id (array of the whole class) and IEC 104 type
WDX_IDS = {
    "DI": ID_DIGITAL_INPUTS,
}
IEC_TYPES = {
    "DI": c104.Type.M_SP_TB_1,
}
SIGNAL_RE = re.compile(r"^(DI)(\d+)$", re.IGNORECASE)


@dataclass
class Signal:
    name: str          # e.g. DI1
    kind: str          # DI
    index: int         # 0-based index in the wdx value array
    ioa: int


@dataclass
class Settings:
    log_level: str
    poll_ms: int
    wdx_socket: str
    wdx_host: str
    wdx_port: int
    wdx_timeout: float
    iec_ip: str
    iec_port: int
    iec_max_connections: int
    iec_common_address: int
    iec_report_ms: int
    signals: List[Signal]


def parse_args():
    parser = argparse.ArgumentParser(description="Gateway between the kbus-gateway wdx data layer and an IEC 60870-5-104 server")
    parser.add_argument(
        "--config",
        default=DEFAULT_CONFIG_FILE,
        help=f"Parameter file (default: {DEFAULT_CONFIG_FILE})",
    )
    return parser.parse_args()


def load_signals(cfg: configparser.ConfigParser) -> List[Signal]:
    """Reads '<signal> = <IOA>' lines, e.g. 'DI1 = 1'. Signals and IOAs must be unique."""
    signals: List[Signal] = []
    used_ioas = set()
    used_names = set()
    if not cfg.has_section("points"):
        return signals
    for name, ioa_text in cfg["points"].items():
        match = SIGNAL_RE.match(name.strip())
        if not match:
            raise ValueError(f"[points] {name}: unknown signal, expected DI<n> (e.g. DI1)")
        kind, number = match.group(1).upper(), int(match.group(2))
        if number < 1:
            raise ValueError(f"[points] {name}: channel numbers start at 1")
        try:
            ioa = int(ioa_text)
        except ValueError:
            raise ValueError(f"[points] {name}: IOA must be an integer, got {ioa_text!r}")
        if not 1 <= ioa <= 16777215:
            raise ValueError(f"[points] {name}: IOA must be between 1 and 16777215")
        if ioa in used_ioas:
            raise ValueError(f"[points] {name}: IOA {ioa} is used more than once")
        key = f"{kind}{number}"
        if key in used_names:
            raise ValueError(f"[points] {name}: signal is used more than once")
        used_ioas.add(ioa)
        used_names.add(key)
        signals.append(Signal(name=key, kind=kind, index=number - 1, ioa=ioa))
    return signals


def load_settings(config_file: str) -> Settings:
    path = Path(config_file)
    if not path.is_file():
        raise FileNotFoundError(f"Config file not found: {config_file}")

    cfg = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=(";", "#"))
    cfg.optionxform = str
    cfg.read(path, encoding="utf-8-sig")

    for section in ("general", "wdx", "iec104"):
        if not cfg.has_section(section):
            raise ValueError(f"Config file is missing section [{section}]")

    general = cfg["general"]
    wdx = cfg["wdx"]
    iec = cfg["iec104"]

    settings = Settings(
        log_level=general.get("log_level", "INFO").upper(),
        poll_ms=general.getint("poll_ms", 100),
        wdx_socket=wdx.get("socket", "/run/kbus-gateway/wdx.sock").strip(),
        wdx_host=wdx.get("host", "").strip(),
        wdx_port=wdx.getint("port", 8000),
        wdx_timeout=wdx.getfloat("timeout", 1.0),
        iec_ip=iec.get("ip", "0.0.0.0").strip(),
        iec_port=iec.getint("port", 2404),
        iec_max_connections=iec.getint("max_connections", 0),
        iec_common_address=iec.getint("common_address", 1),
        iec_report_ms=iec.getint("report_ms", 0),
        signals=load_signals(cfg),
    )

    if settings.poll_ms < 10:
        raise ValueError("[general] poll_ms must be at least 10")
    if not settings.wdx_host and not settings.wdx_socket:
        raise ValueError("[wdx] socket (or host for TCP) is required")
    if settings.wdx_timeout <= 0:
        raise ValueError("[wdx] timeout must be more than 0")
    if not 1 <= settings.iec_common_address <= 65534:
        raise ValueError("[iec104] common_address must be between 1 and 65534")
    if settings.iec_report_ms < 0:
        raise ValueError("[iec104] report_ms must be 0 or more")
    if not settings.signals:
        raise ValueError("[points] must contain at least one '<signal> = <IOA>' line, e.g. 'DI1 = 1'")
    return settings


def wdx_target(s: Settings) -> str:
    return f"tcp {s.wdx_host}:{s.wdx_port}" if s.wdx_host else f"socket {s.wdx_socket}"


def setup_logging(level: str):
    logging.basicConfig(
        level=getattr(logging, level, logging.INFO),
        format="%(asctime)s %(levelname)s %(message)s",
    )
    return logging.getLogger("wdx_to_iec104")


class WdxIec104Gateway:
    def __init__(self, settings: Settings, logger):
        self.settings = settings
        self.logger = logger
        self.wdx = WdxClient(socket_path=settings.wdx_socket, host=settings.wdx_host or None,
                             port=settings.wdx_port, timeout=settings.wdx_timeout)
        self.wdx_ids = sorted({WDX_IDS[s.kind] for s in settings.signals})

        self.server = c104.Server(
            ip=settings.iec_ip,
            port=settings.iec_port,
            max_connections=settings.iec_max_connections,
        )
        self.station = self.server.add_station(common_address=settings.iec_common_address)

        self.points: Dict[str, c104.Point] = {}
        for sig in settings.signals:
            point = self.station.add_point(io_address=sig.ioa, type=IEC_TYPES[sig.kind], report_ms=settings.iec_report_ms)
            # Invalid until the first value has been read from the kbus-gateway
            point.info = c104.SingleInfo(on=False, quality=c104.Quality.Invalid, recorded_at=self.now())
            self.points[sig.name] = point

        self.online: Optional[bool] = None       # None = not polled yet, logs only on a change
        self.missing: Dict[str, bool] = {}       # signal -> channel not in the wdx array (logged once)
        self.stop_event = threading.Event()      # set to stop run(), e.g. on SIGTERM from docker stop

    @staticmethod
    def now() -> datetime:
        return datetime.now(timezone.utc)

    def transmit(self, point: c104.Point) -> bool:
        # Send spontaneously to the connected IEC 104 clients (general interrogation also returns the value)
        if not self.server.has_active_connections:
            return False
        return point.transmit(cause=c104.Cot.SPONTANEOUS)

    def update(self, sig: Signal, value: bool, quality: c104.Quality):
        point = self.points[sig.name]
        if point.value == value and point.quality == quality:
            return
        point.info = c104.SingleInfo(on=value, quality=quality, recorded_at=self.now())
        sent = self.transmit(point)
        if quality.is_good():
            self.logger.info(
                "%s = %s -> IOA %d (%s)",
                sig.name, value, sig.ioa, "sent" if sent else "no IEC 104 client connected",
            )
        else:
            self.logger.warning("Quality of IOA %d (%s) set to Invalid", sig.ioa, sig.name)

    def poll_once(self):
        try:
            arrays = self.wdx.get_many(self.wdx_ids)
        except (WdxError, OSError, ValueError, KeyError) as exc:   # HTTP error, no connection / time-out, bad answer
            if self.online is not False:
                self.logger.error("Reading the kbus-gateway failed: %s, IEC 104 values set to Invalid", exc)
            self.online = False
            for sig in self.settings.signals:
                self.update(sig, bool(self.points[sig.name].value), c104.Quality.Invalid)
            return

        if self.online is not True:
            self.logger.info("Reading the kbus-gateway (%s)", wdx_target(self.settings))
        self.online = True

        for sig in self.settings.signals:
            values = arrays.get(WDX_IDS[sig.kind])
            if not isinstance(values, list) or sig.index >= len(values) or not isinstance(values[sig.index], bool):
                # e.g. no digital input module on the KBus (yet)
                if not self.missing.get(sig.name):
                    count = len(values) if isinstance(values, list) else 0
                    self.logger.warning(
                        "%s not available: the kbus-gateway reports %d channel(s) in %s",
                        sig.name, count, WDX_IDS[sig.kind],
                    )
                self.missing[sig.name] = True
                self.update(sig, bool(self.points[sig.name].value), c104.Quality.Invalid)
                continue
            self.missing[sig.name] = False
            self.update(sig, values[sig.index], c104.Quality())

    def run(self):
        s = self.settings
        self.server.start()
        self.logger.info("IEC 104 server listening on %s:%d (common address %d)", s.iec_ip, s.iec_port, s.iec_common_address)

        interval = s.poll_ms / 1000.0
        next_poll = time.monotonic()
        try:
            while not self.stop_event.is_set():
                self.poll_once()
                # Fixed poll rate; skip missed polls instead of catching up
                next_poll += interval
                now = time.monotonic()
                if next_poll < now:
                    next_poll = now
                self.stop_event.wait(next_poll - now)
        except KeyboardInterrupt:
            pass
        finally:
            self.logger.info("Stopping gateway...")
            self.server.stop()


def main():
    args = parse_args()
    settings = load_settings(args.config)
    logger = setup_logging(settings.log_level)

    logger.info("kbus-gateway:    %s (poll every %d ms)", wdx_target(settings), settings.poll_ms)
    logger.info("IEC 104 server:  %s:%d", settings.iec_ip, settings.iec_port)
    for sig in settings.signals:
        logger.info("Point: %s (%s[%d]) -> CA %d, IOA %d (%s)",
                    sig.name, WDX_IDS[sig.kind], sig.index, settings.iec_common_address, sig.ioa, IEC_TYPES[sig.kind].name)

    gateway = WdxIec104Gateway(settings, logger)

    def on_sigterm(*_):
        # docker stop sends SIGTERM: stop cleanly (Ctrl+C raises KeyboardInterrupt).
        # Safety net: force the exit when the clean stop takes longer than 5 s
        gateway.stop_event.set()
        timer = threading.Timer(5.0, lambda: os._exit(1))
        timer.daemon = True
        timer.start()

    signal.signal(signal.SIGTERM, on_sigterm)
    gateway.run()


if __name__ == "__main__":
    main()
