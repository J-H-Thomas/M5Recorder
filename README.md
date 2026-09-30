# M5Recorder

M5stack recorder and memory device: push-to-talk voice memos on an
**M5StickS3** that end up as transcribed notes in Obsidian.

```
hold button, talk ─► stick saves the memo ─► Wi-Fi (home or phone hotspot)
  ─► HTTPS through Pangolin ─► receiver on the home server
  ─► Whisper transcript ─► Obsidian note with the audio embedded
```

- [`firmware/recorder/`](firmware/recorder/README.md): the stick's firmware (PlatformIO).
- [`server/`](server/README.md): the receiver (Docker: FastAPI + faster-whisper).
- [`firmware/wake_test/`](firmware/wake_test/README.md): the experiment that
  worked out how to start recording fast enough after deep sleep.
