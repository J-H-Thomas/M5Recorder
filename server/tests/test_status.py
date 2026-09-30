import json
import logging

import pytest
from fastapi.testclient import TestClient

from app import telemetry
from app.config import Settings
from app.ha import AVAILABILITY_TOPIC, SENSORS, discovery_messages
from app.main import _SkipHealthChecks, create_app

from test_receiver import AUTH, TOKEN, FakeTranscriber, make_wav

DEVICE = "aabbccddeeff"
TELEMETRY = {
    "X-Memo-Device": DEVICE,
    "X-Battery-mV": "3870",
    "X-Battery-Pct": "64",
    "X-Charging": "0",
    "X-Queue": "2",
    "X-Set-Aside": "1",
    "X-Ignored": "3",
    "X-Firmware": "cceeb93",
}


@pytest.fixture
def settings(tmp_path):
    return Settings(token=TOKEN, vault_dir=tmp_path / "vault", data_dir=tmp_path / "data",
                    timezone="Europe/London", max_upload_bytes=200_000)


class FakeMqtt:
    def __init__(self):
        self.messages: list[tuple[str, str, bool]] = []

    def __call__(self, topic, payload, retain):
        self.messages.append((topic, payload, retain))

    def last_state(self, device=DEVICE):
        states = [json.loads(p) for t, p, _ in self.messages if t == f"m5recorder/{device}/state"]
        return states[-1] if states else None

    def configs(self):
        return [t for t, _, _ in self.messages if t.endswith("/config")]


def app_with_mqtt(settings):
    mqtt = FakeMqtt()
    app = create_app(settings, FakeTranscriber(), start_worker=False, ha_publish=mqtt)
    return app, TestClient(app), mqtt


# --- telemetry parsing ---------------------------------------------------------

def test_telemetry_from_headers():
    t = telemetry.from_headers({k.lower(): v for k, v in TELEMETRY.items()}, DEVICE, "upload", now=100.0)
    assert (t.battery_mv, t.battery_pct, t.charging, t.queue, t.set_aside, t.ignored, t.firmware) == (
        3870, 64, False, 2, 1, 3, "cceeb93")


def test_old_firmware_sends_no_telemetry():
    assert telemetry.from_headers({"x-memo-device": DEVICE}, DEVICE, "upload") is None


def test_out_of_range_and_junk_values_are_dropped():
    h = {"x-battery-mv": "99999", "x-battery-pct": "150", "x-queue": "abc",
         "x-firmware": "v1<script>; rm -rf /" + "x" * 100}
    t = telemetry.from_headers(h, DEVICE, "upload")
    assert t.battery_mv is None and t.battery_pct is None and t.queue is None
    assert t.firmware.startswith("v1script") and len(t.firmware) <= telemetry.FIRMWARE_MAX


# --- days left -------------------------------------------------------------------

def test_days_left_from_a_steady_discharge():
    day = 86400
    history = [(0.0, 90, False), (day * 0.5, 80, False), (day, 70, False)]  # 20 % per day
    assert telemetry.days_left(history, day) == pytest.approx(3.5)


def test_days_left_ignores_readings_before_a_charge():
    day = 86400
    history = [(0.0, 20, False), (day * 0.4, 10, False), (day * 0.5, 50, True),
               (day * 0.6, 95, False), (day * 1.1, 85, False)]
    assert telemetry.days_left(history, day * 1.1) == pytest.approx(85 / 20, abs=0.1)  # rounded to 0.1 d


def test_days_left_needs_enough_data():
    assert telemetry.days_left([], 0) is None
    assert telemetry.days_left([(0.0, 90, False), (3600.0, 89, False)], 3600) is None  # 1 h span
    assert telemetry.days_left([(0.0, 80, False), (86400.0, 81, False)], 86400) is None  # rising


# --- endpoints -----------------------------------------------------------------------

def test_heartbeat_records_telemetry_and_updates_ha(settings):
    app, client, mqtt = app_with_mqtt(settings)
    with client:
        r = client.post("/heartbeat", headers={**AUTH, **TELEMETRY})
        assert r.status_code == 200 and r.json() == {"status": "ok"}
    latest = app.state.store.latest_telemetry(DEVICE)
    assert latest["kind"] == "heartbeat" and latest["battery_pct"] == 64
    state = mqtt.last_state()
    assert state["battery"] == 64 and state["battery_voltage"] == 3.87 and state["charging"] == "OFF"
    assert state["queue"] == 2 and state["set_aside"] == 1 and state["firmware"] == "cceeb93"
    assert state["last_seen"] is not None


def test_heartbeat_needs_token_device_and_telemetry(settings):
    _, client, _ = app_with_mqtt(settings)
    with client:
        assert client.post("/heartbeat", headers=TELEMETRY).status_code == 401
        assert client.post("/heartbeat", headers={**AUTH, **TELEMETRY, "X-Memo-Device": "bad id!"}).status_code == 400
        assert client.post("/heartbeat", headers={**AUTH, "X-Memo-Device": DEVICE}).status_code == 400


def test_upload_records_telemetry_and_counts_memos_today(settings):
    import time
    app, client, mqtt = app_with_mqtt(settings)
    now = int(time.time())
    with client:
        r = client.post("/upload", content=make_wav(), headers={
            **AUTH, **TELEMETRY, "X-Memo-Id": f"{DEVICE}-1a2b3c4d-1", "X-Memo-Time": str(now)})
        assert r.json()["status"] == "queued"
        assert mqtt.last_state()["memos_today"] == 1
        assert mqtt.last_state()["transcribing"] == 1
        app.state.worker.run_pending()
    assert mqtt.last_state()["transcribing"] == 0  # the worker republished when done
    assert app.state.store.latest_telemetry(DEVICE)["kind"] == "upload"


def test_upload_from_old_firmware_still_works_without_telemetry(settings):
    app, client, mqtt = app_with_mqtt(settings)
    with client:
        r = client.post("/upload", content=make_wav(), headers={**AUTH, "X-Memo-Id": f"{DEVICE}-5"})
        assert r.json()["status"] == "queued"
    assert app.state.store.latest_telemetry(DEVICE) is None


# --- Home Assistant messages ----------------------------------------------------

def test_discovery_announces_every_sensor_once(settings):
    _, client, mqtt = app_with_mqtt(settings)
    with client:
        client.post("/heartbeat", headers={**AUTH, **TELEMETRY})
        client.post("/heartbeat", headers={**AUTH, **TELEMETRY})
    configs = mqtt.configs()
    assert len(configs) == len(SENSORS)  # not repeated on the second heartbeat
    assert f"homeassistant/binary_sensor/m5recorder_{DEVICE}/charging/config" in configs
    assert all(retain for _, _, retain in mqtt.messages)


def test_discovery_payloads_are_well_formed():
    for topic, payload in discovery_messages("homeassistant", DEVICE):
        cfg = json.loads(payload)
        assert cfg["unique_id"].startswith(f"m5recorder_{DEVICE}_")
        assert cfg["state_topic"] == f"m5recorder/{DEVICE}/state"
        assert cfg["availability_topic"] == AVAILABILITY_TOPIC
        assert cfg["device"]["identifiers"] == [f"m5recorder_{DEVICE}"]


def test_ha_off_without_mqtt(settings):
    app = create_app(settings, FakeTranscriber(), start_worker=False)
    with TestClient(app) as client:
        assert client.post("/heartbeat", headers={**AUTH, **TELEMETRY}).status_code == 200
    assert not app.state.ha.enabled


# --- logging ---------------------------------------------------------------------------

def test_health_checks_are_left_out_of_the_access_log():
    f = _SkipHealthChecks()

    def record(path, status):
        return logging.LogRecord("uvicorn.access", logging.INFO, "", 0, '%s - "%s %s HTTP/%s" %d',
                                 ("127.0.0.1:1", "GET", path, "1.1", status), None)

    assert not f.filter(record("/health", 200))
    assert f.filter(record("/health", 503))
    assert f.filter(record("/upload", 200))
