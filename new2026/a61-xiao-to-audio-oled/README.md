# a61 — XIAO-to-Audio OLED + Fresh 3FPS

This project extends a60 with an SSD1306 128×64 I²C OLED and an experimental low-frame-rate video mode.

## Hardware

- XIAO ESP32-S3 Sense
- microSD card
- SSD1306 128×64 I²C OLED
- Existing a60 PWM audio circuit (optional while testing video)

### OLED wiring

| OLED | XIAO ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | D4 / GPIO5 |
| SCL | D5 / GPIO6 |

The OLED uses I²C. The existing a60 microSD wiring remains on SPI:
CS 21, SCK 7, MISO 8, MOSI 9.

Install these Arduino libraries:
- AudioTools
- U8g2
- The MP3 Helix decoder dependency used by a60

## Fresh 3FPS video format

The first version does not decode MP4 on the XIAO. Convert a video on your computer into a raw 128×64, 8-bit grayscale frame stream at 3 FPS. The ESP32 reads one 8192-byte frame every ~333 ms and thresholds it to monochrome for the OLED.

Example using FFmpeg:

```bash
ffmpeg -i input.mp4 -vf "fps=3,scale=128:64:force_original_aspect_ratio=decrease,pad=128:64:(ow-iw)/2:(oh-ih)/2,format=gray" -an -f rawvideo -pix_fmt gray demo.gray
```

The output has no header. It is simply consecutive frames:
- Width: 128 pixels
- Height: 64 pixels
- Pixel format: 8-bit grayscale
- Frame size: 8192 bytes
- Frame rate: 3 FPS
- File size must be an exact multiple of 8192 bytes

Create a `/video` folder on the microSD card and put `demo.gray` inside it:

```
/music/
  track1.mp3
  track2.mp3
/video/
  demo.gray
```

The video loops when it reaches the end of the file.

## Serial commands

| Command | Action |
|---|---|
| `help` | Show commands |
| `list` | List MP3 tracks |
| `play`, `resume` | Start/resume MP3 |
| `play N` | Play MP3 track N |
| `pause`, `stop` | Pause/stop MP3 |
| `next`, `prev` | Change MP3 track |
| `volume N` | Set volume from 0–100 |
| `video` | Play `/video/demo.gray` |
| `video file /video/name.gray` | Play another raw grayscale video |
| `video pause` | Pause video |
| `video resume` | Resume video |
| `video stop` | Stop video |
| `status` | Show player status |

## Notes / limitations

- This is a proof-of-concept video mode, not an MP4 decoder. MP4 conversion happens on a computer.
- The OLED is monochrome, so grayscale is thresholded into black and white.
- The video has no embedded audio. The existing MP3 player remains available as a separate audio pipeline.
- Video playback shares the microSD SPI bus with the MP3 source. Test video and simultaneous MP3 playback on the actual board; if audio glitches, test video without MP3 first.
- The OLED status screen shows current and next MP3 track indices when video is not active.
