/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * User-space device layer for the I-O DATA GV-USB2 (STK1150 bridge +
 * TW9910 video decoder). Device logic ported from the Linux driver
 * by Isaac Lozano (https://github.com/Isaac-Lozano/GV-USB2-Driver),
 * used here under its BSD-3-Clause option.
 */
#ifndef GVUSB2_H
#define GVUSB2_H

#include <stdint.h>
#include <libusb.h>

#define GVUSB2_VID 0x04bb
#define GVUSB2_PID 0x0532

/* USB layout (confirmed with gvdump on real hardware) */
#define GVUSB2_VIDEO_IFACE   0
#define GVUSB2_VIDEO_ALT     5      /* EP 0x82 iso, 3 x 1024 bytes/uframe */
#define GVUSB2_VIDEO_EP      0x82
#define GVUSB2_VIDEO_MAXPKT  0xc00
#define GVUSB2_AUDIO_IFACE   2
#define GVUSB2_AUDIO_ALT     1      /* EP 0x84 iso, 256 bytes/uframe */
#define GVUSB2_AUDIO_EP      0x84
#define GVUSB2_AUDIO_MAXPKT  0x100

#define GVUSB2_TW9910_ADDR   0x44

/* STK1150 GPIO bits (reg 0x0000 = value, 0x0002 = direction) */
#define GVUSB2_GPIO_VDEC_RESET   0x08 /* active low */
#define GVUSB2_GPIO_SDA_RESET    0x10 /* active low */
#define GVUSB2_GPIO_SOUND_ENABLE 0x20

enum gvusb2_input { GVUSB2_INPUT_COMPOSITE = 0, GVUSB2_INPUT_SVIDEO = 1 };
enum gvusb2_std   { GVUSB2_STD_NTSC = 0, GVUSB2_STD_PAL = 1 };

struct gvusb2 {
	libusb_context *ctx;
	libusb_device_handle *h;
	int claimed_video;
	int claimed_audio;
	int hdelay;		/* TW9910 HDELAY override, 0 = default */
};

#define GVUSB2_HDELAY_DEFAULT 0x0a	/* centres a full ~708 px active line */

int  gvusb2_open(struct gvusb2 *gv);
void gvusb2_close(struct gvusb2 *gv);

/* STK1150 bridge registers (vendor control requests) */
int gvusb2_read_reg(struct gvusb2 *gv, uint16_t reg, uint8_t *val);
int gvusb2_write_reg(struct gvusb2 *gv, uint16_t reg, uint8_t val);
int gvusb2_set_reg_mask(struct gvusb2 *gv, uint16_t reg, uint8_t mask, uint8_t val);

/* TW9910 registers, through the bridge's I2C master */
int gvusb2_i2c_read(struct gvusb2 *gv, uint8_t reg, uint8_t *val);
int gvusb2_i2c_write(struct gvusb2 *gv, uint8_t reg, uint8_t val);

/* Claims the video interface, selects the iso alt setting, resets and
 * programs bridge + decoder. Leaves the stream stopped. */
int gvusb2_video_init(struct gvusb2 *gv, enum gvusb2_std std, enum gvusb2_input in);
int gvusb2_video_start(struct gvusb2 *gv);
int gvusb2_video_stop(struct gvusb2 *gv);

/* Claims the audio interface and enables the I2S ADC path
 * (48 kHz, 16-bit LE, stereo, raw PCM on EP 0x84). */
int gvusb2_audio_init(struct gvusb2 *gv);

/* Decoder status (TW9910 reg 0x01): bit7 = video loss, bit0 = 50 Hz */
int gvusb2_video_status(struct gvusb2 *gv, uint8_t *status);

#endif
