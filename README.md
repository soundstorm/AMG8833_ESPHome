# AMG8833 for ESPHome

External ESPHome component for the Panasonic AMG8833 (Grid-EYE, 8x8 IR array, I²C).

- Sensor temperature, hottest pixel, presence
- Thermal image as hex frame (text sensor) and, optionally, as an image served by the ESP's web server (BMP, SVG, live page)
- People counting with direction detection (entered / left) for doors, including several doors sharing one counter in Home Assistant

## Installation

```yaml
external_components:
  - source: github://soundstorm/AMG8833_ESPHome@main
    components: [amg8833]

i2c:
  sda: GPIO21
  scl: GPIO22

amg8833:
  address: 0x69
  image_endpoint:
    bmp: true
    viewer: true
  people_counter:
    count: { name: "People in Room" }
```

Full example: [example.yaml](example.yaml).

## Thermal image (`image_endpoint`, optional)

| Option   | Default | URL            | Purpose                                      |
|----------|---------|----------------|----------------------------------------------|
| `bmp`    | `true`  | `/thermal.bmp` | Still image, bilinearly upscaled (`scale`)   |
| `svg`    | `false` | `/thermal.svg` | Vector image, smoothed by the browser        |
| `viewer` | `false` | `/thermal`     | HTML page that refreshes every ~500 ms       |
| `scale`  | `8`     | –              | BMP edge length = 8 × scale (8 → 64x64 px)   |

Without `image_endpoint` no image is served. Use `frame_min_temperature` / `frame_max_temperature` to set the color range.

### In Home Assistant
Add the **Generic Camera** integration with *Still Image URL* `http://<esp-ip>/thermal.bmp` and let it detect the image format. This is a still image that Home Assistant polls regularly, not a live video stream.

## People counting

`people_counter` compares every pixel with a learned background, computes the centroid of the warm area and derives the direction from its movement along `axis` (`columns` or `rows`). If entering and leaving are swapped, set `invert: true`.

Mounting: sensor above the door, looking down, with one pixel axis pointing in walking direction. The background is learned during the first ~3 s after boot, so the room should be empty then.

### Background learning and recalibration
- **Continuous adaptation:** While nobody is in view, the background slowly follows temperature changes. `background_rate` (default `0.005`, `0` = off) sets how fast.
- **Automatic recalibration:** If "presence" lasts longer than `auto_recalibrate_after` (default `2min`, `0s` = off), for example because a heater or sunlight changed the scene, the background is relearned.
- **On demand:** The action `amg8833.recalibrate` relearns the empty room (takes about 3 s, no counting meanwhile). Make sure the room is empty. Expose it as a button and trigger it from Home Assistant, e.g. at night or when the room is locked:

```yaml
button:
  - platform: template
    name: "AMG8833 Recalibrate"
    on_press:
      - amg8833.recalibrate: amg   # id of the amg8833 component
```

Optional sensors: `entered`, `exited`, `count` (per device). Triggers: `on_enter`, `on_exit`.

### Diagnostics: recording test frames
For tuning (mounting, `axis`, `min_travel`, `warm_delta`) you can record what the sensor sees without touching Home Assistant or its database:

```yaml
logger:
  level: DEBUG
amg8833:
  log_frames: true
```

```bash
esphome logs device.yaml > frames.log
python3 tools/parse_log.py frames.log          # frames.csv + crossings.csv
python3 tools/parse_log.py frames.log --gif    # + frames.gif (needs numpy, matplotlib, pillow)
```

- Every frame (10 Hz) logs `FRAME,<ms>,<warm pixels>,<centroid>,<tracking>,<64 raw values>`. Raw values are in 0.25 °C steps, row by row. `warm = -1` marks warm-up frames.
- Every finished movement logs `CROSSING,<start>,<end>,<travel>,<duration ms>,<frames>,<max warm pixels>,<enter|exit|ignored>`, also with `log_frames: false` (DEBUG level).
- Switch `log_frames` off again afterwards: 10 lines per second are a lot of Wi-Fi and logger traffic.

### Several doors, one shared counter in Home Assistant
Each door fires `on_enter` / `on_exit`. All devices increment or decrement the same Home Assistant `counter`:

```yaml
amg8833:
  people_counter:
    on_enter:
      - homeassistant.action:
          action: counter.increment
          data: { entity_id: counter.people_in_room }
    on_exit:
      - homeassistant.action:
          action: counter.decrement
          data: { entity_id: counter.people_in_room }
```

The counter and an automation that resets it to 0 after 30 minutes without anyone in view at any door are in [homeassistant/people_in_room.yaml](homeassistant/people_in_room.yaml). Adjust the entity IDs of the `Person in View` sensors there. Also enable "Allow the device to perform Home Assistant actions" for each ESPHome device in Home Assistant.

## Limitations
- 8x8 pixels: single people passing one after another work, tightly packed groups and opposing traffic do not.
- The door sensors only see the doorway, not the room. People sitting still in the room are counted away by the idle reset; add a room presence sensor (see the commented condition in the automation) to avoid that.
- The per-device count is lost on reboot (`id(amg)->set_count(n)` sets it).
- The image endpoints use ESPHome's shared HTTP server (`web_server_base`), which is loaded automatically and listens on port 80. You do **not** need the `web_server:` component, but it can run alongside. The handler is only active if `image_endpoint` is set.
- Compile-tested with ESPHome 2026.9.1 (ESP32, ESP-IDF). Not yet tested on real hardware.
