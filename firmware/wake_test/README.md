# Wake-latency test (M5StickS3)

Measures how much speech is lost between pressing the front button (KEY1) and
the microphone delivering real audio after waking from deep sleep.

## Flash

```sh
pip install platformio
cd firmware/wake_test
pio run -t upload
pio device monitor      # optional: timings are also printed here
```

If the port doesn't show up, put the StickS3 in download mode: hold the side
power button about 2 s until the green LED blinks, then upload.

## Run the test

1. After flashing, the intro screen shows. Press KEY1 (front) to sleep.
2. Press and **hold** KEY1 and straight away say "one two three".
3. The screen goes red while recording. Release to stop.
4. The recording plays back: listen for whether "one" is clipped.
5. The results screen shows:
   - `KEY1 held`: whether the button was still down when the app started
   - `setup` / `mic` / `begin`: ms from app start to each stage (the mic now
     starts before `M5.begin()`)
   - `codec`: whether the ES8311 register writes succeeded
   - `audio +`: ms after mic start before the codec sent non-zero samples
   - `speech+`: ms after mic start before speech-level audio
   - `gap`: longest run of silence (exact zeros) after audio started; should be 0
   - `rec`: recorded length
6. KEY2 (side) toggles "L3B in sleep" (on by default): keeps the codec and LCD supply on
   during sleep, to see if that shortens codec warm-up. Run a few tests each way.
7. KEY1 sleeps again for the next test (or it sleeps by itself after 30 s).

Numbers to send back: a few runs in each L3B mode, plus whether "one" was
clipped in the playback.
