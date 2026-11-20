# gvcap — macOS capture for the I-O DATA GV-USB2

Lossless SD capture from an I-O DATA GV-USB2 (USB 2.0 composite / S-Video
dongle, `04bb:0532`) on macOS, which has no official driver. User-space only
(libusb): no kext, no DriverKit, no root.

Device logic is ported from Isaac Lozano's Linux driver, which lives at the top
of this repository (<https://github.com/Isaac-Lozano/GV-USB2-Driver>, dual
BSD/GPL; this port uses the BSD option).

## Build

```bash
brew install libusb pkg-config
cd macos
make
```

Run from `macos/`; produces `build/gvcap` and `build/gvdump`. Builds on Intel and Apple Silicon.

## Use

```bash
build/gvcap --input composite | ffmpeg -f rawvideo -pix_fmt uyvy422 -s 720x480 -r 30000/1001 -top 1 -i - -c:v ffv1 -level 3 out.mkv
```

With audio (the device's audio is not USB Audio Class, so macOS does not see it;
gvcap reads it itself and writes raw 48 kHz 16-bit stereo to a file or FIFO):

```bash
mkfifo /tmp/gv.pcm
build/gvcap -i composite -a /tmp/gv.pcm | ffmpeg \
    -f rawvideo -pix_fmt uyvy422 -s 720x480 -r 30000/1001 -top 1 -i - \
    -f s16le -ar 48000 -ac 2 -i /tmp/gv.pcm \
    -c:v ffv1 -level 3 -c:a pcm_s16le out.mkv
```

`-top 1` matters: the frames are interlaced, top field first, and without it
ffmpeg marks them progressive, so players won't deinterlace them correctly.

Stop with Ctrl-C. `gvcap --help` lists the options (`--std pal`, `--frames`,
`--timestamps`, `--no-resync`, `--hdelay`, ...). A status line goes to stderr every 5 s with
frame, duplicate, drop and error counts and whether the decoder sees a signal.

`build/gvdump` prints the USB descriptors; `build/gvdump --regs [svideo]` also
initialises the device and dumps the TW9910 registers (reg `01` bit 7 set = no
video signal).

## How the device works

| | |
|---|---|
| Bridge | Syntek STK1150, registers via vendor control requests (`0x00` read, `0x01` write, register in wIndex) |
| Decoder | Techwell TW9910 at I2C `0x44`, reached through the bridge's I2C master (regs `0x0200`–`0x0209`); ID register reads `0x59` |
| Video | interface 0, alt 5, EP `0x82` isochronous, 3 × 1024 bytes per microframe |
| Audio | interface 2, alt 1, EP `0x84` isochronous, raw PCM 48 kHz s16le stereo (vendor class, not UAC) |

Init: pulse decoder reset via bridge GPIO → bridge register table → TW9910
table → force colour standard → select input (TW9910 reg `02`) → decoder crop →
bridge capture window (regs `0x0110`–`0x0117`) → start with bridge reg
`0x0100 = 0xb3` (stop: `0x33`).

Stream: every iso packet has a 4-byte header. Byte 0 bit 7 set = status packet
marking the start of a field (bit 6 set = top field; byte 1 counts fields).
Otherwise it is a data packet: byte 1 is a 6-bit sequence counter, the payload
is UYVY at 1440 bytes per line. Fields are woven into 720×480 (or 720×576) frames.

## Timing and sync

Each iso packet is one USB microframe (125 µs), so packet count is a clock
shared by video and audio. Output frames are held to that clock: a frame is
duplicated when the source falls a full frame behind (slow tape, lost data) and
dropped when it gets a full frame ahead, so video stays aligned with the audio.
`--timestamps file.csv` records, per output frame, its time on that clock, the
audio sample count at that point and a flag (`D` duplicate, `S` short field).
`--no-resync` turns the duplicate/drop behaviour off.

Lines that never arrive keep the previous frame's content rather than going
black, and the decoder is pinned to the chosen standard so it keeps a 59.94 Hz
cadence through signal dropouts.

## Picture

- Field order: top field first (confirmed with ffmpeg `idet` on a live tape: 300/300 TFF).
- Horizontal position: the decoder's HDELAY defaults to 10, which puts the whole
  ~708-pixel active line inside the 720-pixel frame with a few pixels of blanking
  each side. Decks differ slightly; `--hdelay N` adjusts it (lower moves the
  picture right). The old Linux default of 18 cut ~5 px off the left edge.
- Levels: BT.601 studio range as the TW9910 outputs it (black ≈ 16–20, white ≈ 235).
- The bottom line or two carry VHS head-switching noise, as on any VHS capture.

## Status

Verified on an iMac Pro (Intel, macOS 15.8) with a VHS deck on composite:
picture, sound, field order and the ffmpeg FFV1 pipeline all work; a 20 s capture
had 600/600 frames, 0 duplicated or dropped, 0 packet errors, 1 lost field.

To do:

- S-Video and PAL: implemented, untested on a live source.
- macOS 12 (Mac Pro 6,1): the code targets 12.3, but Homebrew's libusb is built
  for the machine it is installed on. Build there, or link a static libusb.
- Apple Silicon: should work unchanged; untested.
- Phase 2: CoreMediaIO Camera Extension so the device appears in OBS/QuickTime.
