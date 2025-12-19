# A ZERO Player v1.20

World's first video player for SF2000 and GB300 handheld consoles.

## Features

- **MJPEG and Xvid video playback** - software decoding
- **Audio support** - PCM WAV, ADPCM, MP3 (22kHz recommended)
- **Built-in file browser** - load videos directly from SD card
- **15 color modes** - Normal, Night, Warm, Sepia, Grayscale, Dither variations and more
- **Seek controls** - Left/Right (15s), Up/Down (1min), slider in menu
- **Time display** with black outline for visibility
- **Start configuration menu** - press START to access
- **Save Settings** - remembers color mode, display options, last directory
- **Key lock** - hold L+R shoulders for 2 seconds to lock/unlock controls
- **Debug panel** - FPS, frame count, audio buffer status
- **Polish character support** - filenames with Polish letters display correctly
- **Fast loading** - uses AVI index for instant start on long videos

## Controls

| Button | Action |
|--------|--------|
| START | Open/close menu |
| A | Play/pause (or select in menu) |
| B | Back (close submenu / go up in file browser) |
| Left/Right | Skip 15 seconds |
| Up/Down | Skip 1 minute |
| L/R Shoulders | Cycle menu options |
| L+R (hold 2s) | Toggle key lock |

## Installation

1. Copy `core_87000000` to your SD card: `/cores/a0_player/core_87000000`
2. Videos go to `/VIDEOS/` folder on SD card (created automatically)
3. Create a launcher file: `ROMS/a0_player;start.gba` (any name works)
4. Launch the console, go to ROMS menu and find your launcher
5. Use the built-in file browser to select videos from `/VIDEOS/`

**Alternative**: You can also launch videos directly by naming them `a0_player;videoname.gba` in ROMS folder.

## Supported Video Format

- **Container**: AVI with idx1 index
- **Video codec**: Motion JPEG (MJPEG) or Xvid (MPEG-4 ASP)
- **Resolution**: Up to 320x240 (larger videos are scaled down)
- **Frame rate**: 15 fps recommended (30 fps may have slowdowns)
- **Audio codecs**:
  - PCM WAV (22kHz mono) - best quality, largest files
  - ADPCM (22kHz mono) - good quality, smaller files
  - MP3 (22kHz mono) - smaller files, higher CPU usage
  - **Note**: 44kHz audio is currently disabled due to sync issues

## How to use automatic batch converter?
1. Download !CONVERT_no_ffmpeg_Windows.zip
2. Extract it where you have enough space.
3. Download ffmpeg.exe and copy it to that folder
4. Put your source videos in input.
5. Run "convert_custom.bat" and select your settings. Recommended: Xvid + MP3 audio.
6. When it finishes copy the videos from output_avi_xvid to your memory card - either:
- copy to /VIDEOS/ if you want to load them manually from the interface or
- copy them to /ROMS/a0_player/ and run make-romlist.bat from the main directory of the card, if you want them visible in ROMS on the console.

### Recommended Settings

- **Optimal format**: 320x240 @ 15 fps
- At **30 fps** there may be occasional slowdowns during complex scenes - use ADPCM instead of MP3 to reduce the CPU load
- **Dither color modes** may cause slight additional slowdown
- **Widescreen (16:9) content** with black bars on top/bottom requires less decoding, so 30 fps may work better for such videos
- For best experience, **15 fps is recommended**

### Converting Videos

#### Easy way (Windows, no FFmpeg required)

Download `convert_no_ffmpeg_windows.7z` from releases. Just drop all your videos into the `input` folder and run the batch script - converted videos ready for SF2000 will appear in the `output` folder. Quick and easy conversion to 15 fps or 30 fps MJPEG format.

#### Using FFmpeg

```bash
# 15 fps (recommended)
ffmpeg -i input.mp4 -vf "scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2,fps=15" -c:v mjpeg -q:v 5 output.avi

# 30 fps (may have slowdowns)
ffmpeg -i input.mp4 -vf "scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2,fps=30" -c:v mjpeg -q:v 5 output.avi

# With audio (15 fps)
ffmpeg -i input.mp4 -vf "scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2,fps=15" -c:v mjpeg -q:v 5 -c:a pcm_s16le -ar 22050 -ac 1 output.avi
```

## Settings

Settings are saved to `/VIDEOS/a0player.cfg` when you select "Save Settings" in menu:
- Color mode
- Show Time on/off
- Debug Info on/off
- Last browsed directory

Settings are loaded automatically on startup.

## Building from Source

Requires MIPS toolchain for SF2000 multicore.

```bash
make clean platform=sf2000
make platform=sf2000
```

Then link with sf2000_multicore to create `core_87000000`.

## Technical Details

- **Display**: RGB565, 320x240
- **JPEG decoder**: TJpgDec (Tiny JPEG Decompressor)
- **Architecture**: MIPS32 soft-float (no FPU)
- **Memory**: Static allocation, no malloc at runtime

## Changelog

### v1.20
- Xvid (MPEG-4 ASP) video codec support
- Fast loading for all codecs using AVI index
- Fixed audio/video sync issues
- MP3 seek fix for 22kHz audio

### v1.18
- MP3 audio codec support
- ADPCM audio codec support
- 44kHz audio temporarily disabled (sync issues)

### v0.75
- Built-in file browser for loading videos from SD card
- VIDEOS folder auto-created on first run
- Save/Load settings (color mode, time/debug display, last directory)
- Improved A/V synchronization
- Polish character support in filenames
- Color mode submenu wraps around
- Save feedback popup

### v0.71
- Initial public release
- 15 color modes
- Seek controls and time display
- Key lock feature

## License

Based on libretro API. TJpgDec by ChaN (http://elm-chan.org/).

## Author

Grzegorz Korycki





