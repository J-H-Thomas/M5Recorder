"""Battery and queue reports from the stick, and the status built from them."""

import time
from dataclasses import dataclass
from datetime import datetime
from zoneinfo import ZoneInfo

from .store import FAILED, GAVE_UP, PENDING, Store

FIRMWARE_MAX = 40
ESTIMATE_WINDOW_S = 3 * 86400      # battery trend over the last 3 days
ESTIMATE_MIN_SPAN_S = 6 * 3600     # need at least 6 h of discharge to estimate


@dataclass(frozen=True)
class Telemetry:
    ts: float
    device: str
    kind: str                 # "upload" or "heartbeat"
    battery_mv: int | None
    battery_pct: int | None
    charging: bool | None
    queue: int | None
    set_aside: int | None
    ignored: int | None
    firmware: str | None


def _int(headers, name: str, lo: int, hi: int) -> int | None:
    value = headers.get(name)
    if value is None:
        return None
    try:
        n = int(value)
    except ValueError:
        return None
    return n if lo <= n <= hi else None


def from_headers(headers, device: str, kind: str, now: float | None = None) -> Telemetry | None:
    """The stick's X-Battery-* / X-Queue / ... headers, or None if it sent none
    (firmware from before telemetry). Out-of-range values are dropped, not trusted."""
    if headers.get("x-battery-mv") is None and headers.get("x-queue") is None:
        return None
    charging = headers.get("x-charging")
    firmware = headers.get("x-firmware")
    if firmware is not None:
        firmware = "".join(c for c in firmware if c.isalnum() or c in "-_.+")[:FIRMWARE_MAX] or None
    return Telemetry(
        ts=time.time() if now is None else now,
        device=device,
        kind=kind,
        battery_mv=_int(headers, "x-battery-mv", 2500, 5000),
        battery_pct=_int(headers, "x-battery-pct", 0, 100),
        charging=None if charging is None else charging == "1",
        queue=_int(headers, "x-queue", 0, 100000),
        set_aside=_int(headers, "x-set-aside", 0, 100000),
        ignored=_int(headers, "x-ignored", 0, 100000),
        firmware=firmware,
    )


def days_left(history: list[tuple[float, int, bool]], now: float) -> float | None:
    """Days until empty, from the battery % trend over the last few days.

    Uses the readings since the last time the stick was charging (a charge
    resets the trend), fits a straight line, and projects it to 0 %. None when
    there isn't enough discharge yet to say."""
    points = [(ts, pct) for ts, pct, charging in history if ts >= now - ESTIMATE_WINDOW_S]
    last_charge = max((ts for ts, _, charging in history if charging), default=None)
    if last_charge is not None:
        points = [(ts, pct) for ts, pct in points if ts > last_charge]
    if len(points) < 2 or points[-1][0] - points[0][0] < ESTIMATE_MIN_SPAN_S:
        return None
    n = len(points)
    xs = [(ts - points[0][0]) / 86400 for ts, _ in points]
    ys = [pct for _, pct in points]
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx  # % per day
    if slope >= -0.1:  # flat or rising: nothing sensible to project
        return None
    return round(max(ys[-1], 0) / -slope, 1)


def status(store: Store, device: str, timezone: str, now: float | None = None) -> dict:
    """Everything Home Assistant shows for one stick."""
    now = time.time() if now is None else now
    latest = store.latest_telemetry(device) or {}
    tz = ZoneInfo(timezone)
    midnight = datetime.fromtimestamp(now, tz).replace(hour=0, minute=0, second=0, microsecond=0)
    counts = store.counts()
    last_seen = latest.get("ts")
    mv = latest.get("battery_mv")
    charging = latest.get("charging")
    return {
        "battery": latest.get("battery_pct"),
        "battery_voltage": round(mv / 1000, 2) if mv else None,
        "charging": None if charging is None else ("ON" if charging else "OFF"),
        "last_seen": datetime.fromtimestamp(last_seen, tz).isoformat(timespec="seconds") if last_seen else None,
        "queue": latest.get("queue"),
        "set_aside": latest.get("set_aside"),
        "ignored_presses": latest.get("ignored"),
        "firmware": latest.get("firmware"),
        "memos_today": store.memos_since(device, midnight.timestamp()),
        "days_left": days_left(store.battery_history(device, now - ESTIMATE_WINDOW_S), now),
        "transcribing": counts.get(PENDING, 0) + counts.get(FAILED, 0),
        "transcription_failures": counts.get(GAVE_UP, 0),
    }
