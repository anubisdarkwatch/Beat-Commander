# W10 Beat Conductor

A beat-synced light show for the
[Meshnology W10 LoRa AIoT Dev Kit](https://meshnology.com/products/meshnology-w10-lora-aiot-dev-kit-esp32-s3-long-range-iot-development-board)
(ESP32-S3). It listens to the room through the onboard mic, locks onto the BPM, and flashes the
1.54" screen and RGB LED on every beat. It can also broadcast each beat over LoRa so other boards can
flash in sync, for DJ sets, stages or a crowd holding boards across a venue.

Companion project: [W10-ISM-Waterfall](https://github.com/anubisdarkwatch/W10-ISM-Waterfall).

| W10 conductor (mic, LoRa TX) | Heltec V3 follower (LoRa RX) |
|---|---|
| ![Conductor](docs/conductor.jpg) | ![Follower](docs/follower.jpg) |

## What it does

- **Beat detection from the mic** (ES8311 codec): kick-drum band + full-band onset detection every 8 ms
- **Tempo by autocorrelation** over the last ~6 s, checked against beat-period harmonics so off-beat bass doesn't fool it; locks in ~10 s
- **Phase lock**: folds recent onsets at the beat period so flashes land on the kick, not the off-beat
- **Tap tempo** on BOOT overrides the mic for 15 s; the first tap sets beat 1
- **Screen**: full-panel color flash on each beat, big BPM readout, 1-2-3-4 bar counter (downbeat in amber), color shift every bar, live onset scope + level meter
- **RGB LED** (WS2812 on GPIO48) flashes with the beat
- **LoRa beat link** (off by default): 7-byte packet per beat on 916.0 MHz, SF7 / BW500, 10 dBm

Tested against synthetic tracks from 90 to 174 BPM (kick only, kick + off-beat bass + hats, noisy mixes):
tempo within 0.5 BPM, flashes within ~10 ms of the kick. On a real 128 BPM kick track it reads 127.4–128.1.

## Controls

| Action | Result |
|---|---|
| Tap **BOOT** on the beat | Tap tempo (overrides the mic for 15 s, first tap = beat 1) |
| Hold **BOOT** 1 s | LoRa beat broadcast on / off |

## Beat packet (for follower boards)

```c
struct __attribute__((packed)) BeatPacket {
  uint8_t  m0, m1;   // 'B', 'T'
  uint8_t  seq;      // increments every beat
  uint16_t bpm10;    // BPM x 10, little-endian
  uint8_t  beat;     // 0..3 position in the bar (0 = downbeat)
  uint8_t  hue;      // current color (0..255), changes every bar
};
```

LoRa: 916.0 MHz, SF7, BW 500 kHz, CR 4/5, preamble 8, explicit header, CRC on, default private sync word.
A follower just listens on those settings and flashes when a packet arrives (~10 ms airtime).
916.0 MHz sits clear of the Meshtastic US default (906.875) and the US LoRaWAN channel plan.

## Follower: Heltec WiFi LoRa 32 V3

`heltec_v3_beat_follower/heltec_v3_beat_follower.ino` turns a Heltec V3 into a follower that flashes in sync with the conductor.

- Listens on 916.0 MHz, SF7 / BW500, and flashes the white LED and the OLED (full-screen invert) on every received beat; the downbeat is brighter
- OLED shows BPM, the 1-2-3-4 bar position, RSSI / SNR and packet / loss counts, with **LIVE**, **HOLD** or **WAIT** status
- **Flywheel:** if packets drop, it keeps flashing at the last tempo for 4 s (status **HOLD**)
- Optional WS2812 strip on GPIO47 (set `STRIP_LEN`, needs Adafruit NeoPixel), colored by the conductor's hue, which changes every bar
- Prints an I2C scan at boot, so a dead OLED shows up in the serial log

Tested on a desk with no losses: 300+ packets, RSSI around −75 dBm, SNR around +10 dB.

Build: board **ESP32S3 Dev Module**, USB CDC On Boot **Disabled** (the V3 uses a USB-UART bridge), PSRAM **Disabled**,
library **Adafruit GFX**. Serial monitor at 115200.

Heltec V3 pins: LoRa NSS 8, SCK 9, MOSI 10, MISO 11, RST 12, BUSY 13, DIO1 14 (TCXO on DIO3 at 1.8 V);
OLED SDA 17, SCL 18, RST 21, power via Vext (GPIO36, LOW = on); LED 35.

Ready-made Heltec V3 devices such as the [Meshnology N33 / N35](https://meshnology.com/collections/all) (cased, with battery) run this follower as-is.
Any SX1262 board can follow: copy the radio settings and the `BeatPacket` struct.

## Hardware notes (W10)

| Function | Pins |
|---|---|
| I2C (codec, MCP23017, sensors) | SDA = GPIO8, SCL = GPIO7 |
| I2S to ES8311 | MCLK = 1, BCK = 2, LRCK = 4, DOUT = 5, DIN (mic) = 3 |
| Shared SPI (LoRa + LCD) | SCK = 12, MOSI = 13, MISO = 11 |
| LoRa NSS / NRST / BUSY | GPIO14 / MCP23017 GPA3 / GPB2 |
| LCD (ST7789 240×240) | CS = 10, DC = 16, BL = 6, RST = MCP23017 GPA1 |
| WS2812 RGB LED | GPIO48 |
| Speaker amp enable | MCP23017 GPA7 (kept off: mic only) |

Mic setup: ES8311 analog mic, PGA max, 30 dB mic gain, 16 kHz / 16-bit I2S. Audio runs in its own
FreeRTOS task on core 0 so screen redraws never drop samples.

## Build

Arduino IDE 2.x, **esp32 by Espressif** core 3.x, library **Adafruit GFX**.

Board **ESP32S3 Dev Module**: USB CDC On Boot **Enabled**, PSRAM **OPI PSRAM**.

Open `w10_beat_conductor/w10_beat_conductor.ino` and upload. Serial monitor (115200) prints the BPM,
tempo estimate, confidence, onsets per second and mic level once a second.

## Tips

- Works best with real bass: it listens for kicks. Thin clicks through laptop speakers barely register.
- Slow tracks (< ~90 BPM) may lock at double time, which still flashes in time. Tap BOOT to force it.
- If you transmit under an amateur license, add your callsign ID to the beat link.

## Roadmap

- Latency compensation on followers (~10 ms LoRa airtime)
- Followers for XIAO ESP32S3 + Wio-SX1262 and other SX1262 boards
- Bluetooth MIDI clock input from DJ software
- Tilt-controlled effects via the onboard QMI8658 IMU

## License

MIT, see [LICENSE](LICENSE).

## Credits

Built by Anubis Darkwatch with Claude.

Hardware provided by [Meshnology](https://meshnology.com): thanks for supplying the [Meshnology W10 LoRa AIoT Dev Kit](https://meshnology.com/products/meshnology-w10-lora-aiot-dev-kit-esp32-s3-long-range-iot-development-board) for this project. Pin mapping and peripheral details come from Meshnology's W10 documentation and demos.
