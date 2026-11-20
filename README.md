# GV-USB2 drivers

Open-source drivers for the I-O DATA **GV-USB2**, a USB 2.0 standard-definition
capture device with composite and S-Video inputs and stereo audio
(USB ID `04bb:0532`). I-O DATA only ships Windows drivers.

| Platform | What | Folder |
|---|---|---|
| Linux | Kernel driver: V4L2 video device + ALSA sound card | [`linux/`](linux/) |
| macOS | `gvcap`, a user-space capture tool (libusb, no kext) | [`macos/`](macos/) |

```
.
├── linux/      Linux kernel modules (gvusb2-video, gvusb2-sound)
├── macos/      gvcap capture tool and gvdump diagnostic tool
│   └── src/
└── README.md
```

## Linux

The original driver by Isaac Lozano, unchanged in this fork apart from moving
into `linux/`. It builds two out-of-tree modules against your running kernel and
uses the kernel's own `tw9910` decoder driver.

```bash
cd linux
make
sudo modprobe tw9910
sudo insmod gvusb2-video.ko
sudo insmod gvusb2-sound.ko
```

You need the headers for your kernel (for example `linux-headers-$(uname -r)` on
Debian/Ubuntu). The device then appears as a `/dev/video*` capture device
(720×480 or 720×576, UYVY) and an ALSA card named `gvusb2` (48 kHz stereo).
Arch users can install the `gv-usb2-driver-dkms-git` package from the AUR instead.

## macOS

`gvcap` captures losslessly to stdout, ready to pipe into ffmpeg. It runs
entirely in user space: no kernel extension, no DriverKit, no root.

### Build

```bash
brew install libusb pkg-config
cd macos
make
```

This produces `build/gvcap` and `build/gvdump`. It builds on Intel and Apple Silicon.

### Capture

From `macos/`:

```bash
build/gvcap --input composite | ffmpeg -f rawvideo -pix_fmt uyvy422 -s 720x480 -r 30000/1001 -top 1 -i - -c:v ffv1 -level 3 out.mkv
```

With audio. The device's audio is not standard USB Audio, so macOS does not list
it as an input; gvcap reads it itself and writes raw 48 kHz 16-bit stereo to a
file or FIFO:

```bash
mkfifo /tmp/gv.pcm
build/gvcap -i composite -a /tmp/gv.pcm | ffmpeg \
    -f rawvideo -pix_fmt uyvy422 -s 720x480 -r 30000/1001 -top 1 -i - \
    -f s16le -ar 48000 -ac 2 -i /tmp/gv.pcm \
    -c:v ffv1 -level 3 -c:a pcm_s16le out.mkv
```

Keep `-top 1`: the frames are interlaced, top field first, and without it ffmpeg
marks them progressive, so players won't deinterlace them correctly.

Stop with Ctrl-C. Every 5 s gvcap prints a status line to stderr with frame,
duplicate, drop and error counts and whether the decoder sees a signal.

### Options

| Option | Meaning |
|---|---|
| `-i`, `--input composite\|svideo` | Video input (default composite) |
| `-s`, `--std ntsc\|pal` | 720×480 at 29.97 fps, or 720×576 at 25 fps (default NTSC) |
| `-a`, `--audio PATH` | Also write audio to a file or FIFO |
| `-n`, `--frames N` | Stop after N frames |
| `-t`, `--timestamps PATH` | Per-frame CSV: index, time on the USB clock, audio sample count, flag |
| `--no-resync` | Never duplicate or drop frames to hold real time |
| `--hdelay N` | Horizontal picture position (lower moves the picture right; default 10) |
| `-q`, `--quiet` | No periodic status line |

`build/gvdump` prints the device's USB descriptors. `build/gvdump --regs [svideo]`
also initialises the device and dumps the decoder registers; register `01` bit 7
set means no video signal.

### Timing and A/V sync

Each isochronous packet is one USB microframe (125 µs), so the packet count is a
clock shared by video and audio. gvcap holds output frames to that clock: it
repeats a frame when the source falls a full frame behind (slow tape, lost data)
and drops one when it gets a full frame ahead, so video stays aligned with the
audio. `--timestamps` records where each frame sits on that clock and flags
duplicates (`D`) and frames with short fields (`S`).

Lines that never arrive keep the previous frame's content instead of going black,
and the decoder is pinned to the chosen standard so it keeps a steady frame rate
through signal dropouts.

### Picture

- **Field order:** top field first (confirmed with ffmpeg's `idet` filter on a live tape).
- **Horizontal position:** the default puts the whole ~708-pixel active line
  inside the 720-pixel frame with a few pixels of blanking each side. Decks
  differ slightly; adjust with `--hdelay`.
- **Levels:** BT.601 studio range as the decoder outputs it (black ≈ 16–20, white ≈ 235).
- The bottom line or two carry VHS head-switching noise, as on any VHS capture.

### Status

Tested on an iMac Pro (Intel, macOS 15.8) with a VHS deck on composite: picture,
sound, field order and the ffmpeg FFV1 pipeline all work. A 20-second capture
delivered 600/600 frames with no duplicates, drops or packet errors.

Not yet tested:

- S-Video and PAL with a live source.
- macOS 12 Monterey (Mac Pro 6,1). The code targets 12.3, but Homebrew's libusb
  is built for the macOS it was installed on, so build on that machine or link a
  static libusb.
- Apple Silicon.

Planned: a CoreMediaIO Camera Extension so the device shows up in OBS and QuickTime.

## Hardware notes

What both drivers talk to:

| Part | Details |
|---|---|
| USB bridge | Syntek STK1150. Registers via vendor control requests (`0x00` read, `0x01` write, register number in wIndex) |
| Video decoder | Techwell TW9910 at I2C address `0x44`, reached through the bridge's I2C master (bridge registers `0x0200`–`0x0209`) |
| Video stream | Interface 0, alt setting 5, endpoint `0x82`, isochronous, 3 × 1024 bytes per microframe |
| Audio stream | Interface 2, alt setting 1, endpoint `0x84`, isochronous, raw PCM 48 kHz s16le stereo (vendor class, not USB Audio) |

**Start-up:** pulse the decoder reset through a bridge GPIO, write the bridge
register table, write the decoder table, set the colour standard and input
(decoder register `02`), set the decoder crop and the bridge capture window
(bridge registers `0x0110`–`0x0117`), then start capture with bridge register
`0x0100 = 0xb3` (stop: `0x33`).

**Video stream format:** every isochronous packet starts with a 4-byte header. If
byte 0 bit 7 is set it is a status packet marking the start of a field (bit 6 set
= top field). Otherwise it is a data packet: byte 1 is a 6-bit sequence counter
and the payload is UYVY video at 1440 bytes per line. The two fields are woven
into one frame.

## License

- `linux/`: dual-licensed, BSD-3-Clause or GPL-2.0 at your choice
  ([LICENSE.bsd](linux/LICENSE.bsd), [LICENSE.gpl](linux/LICENSE.gpl)).
- `macos/`: BSD-3-Clause ([LICENSE](macos/LICENSE)). It ports device logic from
  the Linux driver under that driver's BSD option.

## Credits

- Linux driver and the reverse engineering behind both drivers: Isaac Lozano
  ([Isaac-Lozano/GV-USB2-Driver](https://github.com/Isaac-Lozano/GV-USB2-Driver)).
- macOS port: Brandon Tebedo.

The macOS port is an independent project. It is not affiliated with or endorsed
by Isaac Lozano or I-O DATA. GV-USB2 is a product name of I-O DATA DEVICE, INC.
