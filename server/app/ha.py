"""Home Assistant over MQTT discovery: an "M5Recorder" device per stick with
battery, last seen, queue and transcription sensors. Optional: does nothing
unless MQTT_HOST is set.

Topics (all retained):
  <discovery prefix>/<component>/m5recorder_<device>/<key>/config
  m5recorder/<device>/state          JSON with every sensor value
  m5recorder/receiver/availability   online / offline (MQTT last will)
"""

import json
import logging
import threading
from typing import Callable

from .config import Settings

log = logging.getLogger("memos.ha")

AVAILABILITY_TOPIC = "m5recorder/receiver/availability"
REFRESH_S = 600  # republish so "memos today" and "days left" stay current

# key: (component, name, extra discovery fields)
SENSORS: dict[str, tuple[str, str, dict]] = {
    "battery": ("sensor", "Battery", {
        "device_class": "battery", "unit_of_measurement": "%", "state_class": "measurement"}),
    "battery_voltage": ("sensor", "Battery voltage", {
        "device_class": "voltage", "unit_of_measurement": "V", "state_class": "measurement",
        "suggested_display_precision": 2, "entity_category": "diagnostic"}),
    "charging": ("binary_sensor", "Charging", {
        "device_class": "battery_charging", "payload_on": "ON", "payload_off": "OFF"}),
    "last_seen": ("sensor", "Last seen", {"device_class": "timestamp"}),
    "queue": ("sensor", "Memos waiting", {"state_class": "measurement", "icon": "mdi:tray-full"}),
    "set_aside": ("sensor", "Memos set aside", {"state_class": "measurement", "icon": "mdi:tray-alert"}),
    "memos_today": ("sensor", "Memos today", {"icon": "mdi:microphone-message"}),
    "days_left": ("sensor", "Estimated days left", {
        "unit_of_measurement": "d", "state_class": "measurement", "icon": "mdi:battery-clock"}),
    "transcribing": ("sensor", "Memos transcribing", {"state_class": "measurement", "icon": "mdi:text-recognition"}),
    "transcription_failures": ("sensor", "Transcription failures", {
        "state_class": "total_increasing", "icon": "mdi:alert-circle"}),
    "ignored_presses": ("sensor", "Ignored presses", {"entity_category": "diagnostic", "icon": "mdi:gesture-tap"}),
    "firmware": ("sensor", "Firmware", {"entity_category": "diagnostic", "icon": "mdi:chip"}),
}

Publish = Callable[[str, str, bool], None]
StatusFn = Callable[[str], dict]


def discovery_messages(prefix: str, device: str) -> list[tuple[str, str]]:
    """(topic, payload) for every sensor of one stick."""
    dev = {
        "identifiers": [f"m5recorder_{device}"],
        "name": "M5Recorder",
        "manufacturer": "M5Stack",
        "model": "StickS3 memo recorder",
    }
    out = []
    for key, (component, name, extra) in SENSORS.items():
        uid = f"m5recorder_{device}_{key}"
        cfg = {
            "name": name,
            "unique_id": uid,
            "object_id": uid,
            "state_topic": f"m5recorder/{device}/state",
            "value_template": "{{ value_json.%s if value_json.%s is not none else None }}" % (key, key),
            "availability_topic": AVAILABILITY_TOPIC,
            "device": dev,
            **extra,
        }
        out.append((f"{prefix}/{component}/m5recorder_{device}/{key}/config", json.dumps(cfg)))
    return out


class HaPublisher:
    def __init__(self, settings: Settings, status: StatusFn, devices: Callable[[], list[str]],
                 publish: Publish | None = None):
        self._settings = settings
        self._status = status
        self._devices = devices
        self._publish = publish
        self._announced: set[str] = set()
        self._lock = threading.Lock()
        self._client = None
        self._stop = threading.Event()

    @property
    def enabled(self) -> bool:
        return self._publish is not None

    def start(self) -> None:
        """Connects to the broker (in the background; paho reconnects by itself)."""
        s = self._settings
        if self._publish is not None or not s.mqtt_host:
            return
        import paho.mqtt.client as mqtt

        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="m5recorder-receiver")
        if s.mqtt_user:
            client.username_pw_set(s.mqtt_user, s.mqtt_password)
        client.will_set(AVAILABILITY_TOPIC, "offline", retain=True)

        def on_connect(c, userdata, flags, reason_code, properties):
            if reason_code.is_failure:
                log.error("MQTT connect failed: %s", reason_code)
                return
            log.info("MQTT connected to %s:%d", s.mqtt_host, s.mqtt_port)
            with self._lock:
                self._announced.clear()  # a restarted broker may have lost retained configs
            c.publish(AVAILABILITY_TOPIC, "online", retain=True)
            self.refresh_all()

        client.on_connect = on_connect
        self._publish = lambda topic, payload, retain: client.publish(topic, payload, retain=retain)
        client.connect_async(s.mqtt_host, s.mqtt_port)
        client.loop_start()
        self._client = client
        threading.Thread(target=self._refresh_loop, name="ha-refresh", daemon=True).start()

    def stop(self) -> None:
        self._stop.set()
        if self._client is not None:
            self._client.publish(AVAILABILITY_TOPIC, "offline", retain=True)
            self._client.loop_stop()
            self._client.disconnect()

    def _refresh_loop(self) -> None:
        while not self._stop.wait(REFRESH_S):
            self.refresh_all()

    def refresh_all(self) -> None:
        for device in self._devices():
            self.update(device)

    def update(self, device: str) -> None:
        """Publishes a stick's current status (announcing it first if new)."""
        if self._publish is None:
            return
        try:
            with self._lock:
                if device not in self._announced:
                    for topic, payload in discovery_messages(self._settings.mqtt_discovery_prefix, device):
                        self._publish(topic, payload, True)
                    self._announced.add(device)
            self._publish(f"m5recorder/{device}/state", json.dumps(self._status(device)), True)
        except Exception:  # never let HA trouble affect uploads
            log.exception("publishing to Home Assistant failed")
