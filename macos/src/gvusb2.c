// SPDX-License-Identifier: BSD-3-Clause
/*
 * User-space device layer for the I-O DATA GV-USB2.
 *
 * Register sequences are ported from the Linux driver
 * Copyright (c) 2019 Isaac Lozano <109lozanoi@gmail.com>
 * (dual BSD-3-Clause / GPL-2.0, used under BSD-3-Clause).
 *
 * The Linux driver leans on the kernel's GPL-only tw9910 module for part
 * of the decoder setup. Nothing from that module is used here: the TW9910
 * is programmed from the gvusb2 driver's own tables plus the chip's
 * power-on defaults, which already select 1:1 scaling and ITU-R BT.656.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gvusb2.h"

#define CTRL_TIMEOUT_MS 1000

struct regval { uint16_t reg; uint8_t val; };

int gvusb2_open(struct gvusb2 *gv)
{
	int ret;

	memset(gv, 0, sizeof(*gv));
	ret = libusb_init(&gv->ctx);
	if (ret < 0) {
		fprintf(stderr, "gvusb2: libusb_init: %s\n", libusb_strerror(ret));
		return ret;
	}
	gv->h = libusb_open_device_with_vid_pid(gv->ctx, GVUSB2_VID, GVUSB2_PID);
	if (gv->h == NULL) {
		fprintf(stderr, "gvusb2: no GV-USB2 (%04x:%04x) found or cannot open it\n",
			GVUSB2_VID, GVUSB2_PID);
		libusb_exit(gv->ctx);
		gv->ctx = NULL;
		return LIBUSB_ERROR_NO_DEVICE;
	}
	return 0;
}

void gvusb2_close(struct gvusb2 *gv)
{
	if (gv->h) {
		if (gv->claimed_video) {
			libusb_set_interface_alt_setting(gv->h, GVUSB2_VIDEO_IFACE, 0);
			libusb_release_interface(gv->h, GVUSB2_VIDEO_IFACE);
		}
		if (gv->claimed_audio) {
			libusb_set_interface_alt_setting(gv->h, GVUSB2_AUDIO_IFACE, 0);
			libusb_release_interface(gv->h, GVUSB2_AUDIO_IFACE);
		}
		libusb_close(gv->h);
	}
	if (gv->ctx)
		libusb_exit(gv->ctx);
	memset(gv, 0, sizeof(*gv));
}

/* ---- STK1150 bridge registers ------------------------------------------ */

int gvusb2_read_reg(struct gvusb2 *gv, uint16_t reg, uint8_t *val)
{
	int ret;

	*val = 0;
	ret = libusb_control_transfer(gv->h,
		LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		0x00, 0x0000, reg, val, 1, CTRL_TIMEOUT_MS);
	if (ret < 0) {
		fprintf(stderr, "gvusb2: read reg 0x%04x failed: %s\n",
			reg, libusb_strerror(ret));
		return ret;
	}
	return ret == 1 ? 0 : LIBUSB_ERROR_IO;
}

int gvusb2_write_reg(struct gvusb2 *gv, uint16_t reg, uint8_t val)
{
	int ret;

	ret = libusb_control_transfer(gv->h,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		0x01, val, reg, NULL, 0, CTRL_TIMEOUT_MS);
	if (ret < 0) {
		fprintf(stderr, "gvusb2: write reg 0x%04x failed: %s\n",
			reg, libusb_strerror(ret));
		return ret;
	}
	return 0;
}

int gvusb2_set_reg_mask(struct gvusb2 *gv, uint16_t reg, uint8_t mask, uint8_t val)
{
	uint8_t cur;
	int ret;

	ret = gvusb2_read_reg(gv, reg, &cur);
	if (ret < 0)
		return ret;
	return gvusb2_write_reg(gv, reg, (cur & ~mask) | val);
}

/* ---- I2C master inside the bridge -------------------------------------- */

static int i2c_wait(struct gvusb2 *gv, uint8_t mask)
{
	uint8_t flag;
	int i, ret;

	for (i = 0; i < 100; i++) {
		ret = gvusb2_read_reg(gv, 0x0201, &flag);
		if (ret < 0)
			return ret;
		if (flag & mask)
			return 0;
		usleep(200);
	}
	return LIBUSB_ERROR_TIMEOUT;
}

int gvusb2_i2c_read(struct gvusb2 *gv, uint8_t reg, uint8_t *val)
{
	int ret;

	if ((ret = gvusb2_write_reg(gv, 0x0203, (GVUSB2_TW9910_ADDR << 1) | 1)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0208, reg)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0200, 0x60)) < 0 ||
	    (ret = i2c_wait(gv, 0x01)) < 0)
		return ret;
	return gvusb2_read_reg(gv, 0x0209, val);
}

int gvusb2_i2c_write(struct gvusb2 *gv, uint8_t reg, uint8_t val)
{
	int ret;

	if ((ret = gvusb2_write_reg(gv, 0x0203, GVUSB2_TW9910_ADDR << 1)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0204, reg)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0205, val)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0200, 0x05)) < 0)
		return ret;
	return i2c_wait(gv, 0x04);
}

/* ---- video ------------------------------------------------------------- */

static int write_regs(struct gvusb2 *gv, const struct regval *t)
{
	int ret;

	for (; t->reg != 0xffff; t++)
		if ((ret = gvusb2_write_reg(gv, t->reg, t->val)) < 0)
			return ret;
	return 0;
}

int gvusb2_video_init(struct gvusb2 *gv, enum gvusb2_std std, enum gvusb2_input in)
{
	static const struct regval bridge_init[] = {
		{0x000d, 0x00}, {0x000f, 0x00}, {0x0018, 0x10}, {0x001b, 0x0e},
		{0x001c, 0x46}, {0x001a, 0x14}, {0x0019, 0x00}, {0x0300, 0x12},
		{0x0350, 0x41}, {0x0351, 0x00}, {0x0352, 0x00}, {0x0353, 0x00},
		{0x0300, 0x80}, {0x0018, 0x10}, {0x0103, 0x00},
		{0x0100, 0x33},		/* capture stopped */
		{0x0202, 0x08},		/* i2c clock divider */
		{0x02ff, 0x00},		/* disable alternate i2c */
		{0xffff, 0xff}
	};
	static const struct regval decoder_init[] = {
		{0x06, 0x00},
		{0x03, 0xa2},		/* BT.656, 8-bit bus, outputs enabled */
		{0x05, 0x01},
		{0x19, 0xde}, {0x1a, 0x0f}, {0x1b, 0x00},
		{0x6e, 0x28},
		{0x06, 0x80},		/* soft reset of the decoder state machine */
		{0xffff, 0xff}
	};
	/* Bridge capture window: 1440 bytes (720 px) wide, 240/288 lines per
	 * field, starting 4 bytes / 2 lines in (the Linux driver's defaults). */
	const unsigned hstart = 4, vstart = 2;
	const unsigned hend = hstart + 1440;
	const unsigned vend = vstart + (std == GVUSB2_STD_PAL ? 288 : 240);
	const struct regval *t;
	uint8_t v, id;
	int ret;

	if (!gv->claimed_video) {
		ret = libusb_claim_interface(gv->h, GVUSB2_VIDEO_IFACE);
		if (ret < 0) {
			fprintf(stderr, "gvusb2: claim video interface: %s\n",
				libusb_strerror(ret));
			return ret;
		}
		gv->claimed_video = 1;
	}
	ret = libusb_set_interface_alt_setting(gv->h, GVUSB2_VIDEO_IFACE, GVUSB2_VIDEO_ALT);
	if (ret < 0) {
		fprintf(stderr, "gvusb2: select video alt setting: %s\n",
			libusb_strerror(ret));
		return ret;
	}

	/* decoder reset line: make it an output, pulse low, release */
	if ((ret = gvusb2_set_reg_mask(gv, 0x0002, GVUSB2_GPIO_VDEC_RESET, GVUSB2_GPIO_VDEC_RESET)) < 0 ||
	    (ret = gvusb2_set_reg_mask(gv, 0x0000, GVUSB2_GPIO_VDEC_RESET, GVUSB2_GPIO_VDEC_RESET)) < 0 ||
	    (ret = gvusb2_read_reg(gv, 0x0000, &v)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0000, v & ~GVUSB2_GPIO_VDEC_RESET)) < 0)
		return ret;
	usleep(15000);
	if ((ret = gvusb2_write_reg(gv, 0x0000, v | GVUSB2_GPIO_VDEC_RESET)) < 0)
		return ret;
	usleep(15000);

	if ((ret = write_regs(gv, bridge_init)) < 0)
		return ret;

	/* sanity check: TW9910 product ID lives in the top 5 bits of reg 0 */
	if ((ret = gvusb2_i2c_read(gv, 0x00, &id)) < 0)
		return ret;
	if ((id >> 3) != 0x0b)
		fprintf(stderr, "gvusb2: warning: unexpected decoder ID 0x%02x "
			"(expected TW9910, 0x58..0x5f)\n", id);

	for (t = decoder_init; t->reg != 0xffff; t++)
		if ((ret = gvusb2_i2c_write(gv, (uint8_t)t->reg, t->val)) < 0)
			return ret;
	usleep(20000);

	/* Fix the colour standard instead of auto-detecting it (reg 0x1c:
	 * bit3 = no auto, bits 2:0 = 0 NTSC-M, 1 PAL-BDGHI). With auto-detect
	 * the decoder free-runs at 50 Hz whenever the signal drops out, which
	 * would wreck the frame cadence of an NTSC capture. */
	if ((ret = gvusb2_i2c_write(gv, 0x1c, std == GVUSB2_STD_PAL ? 0x09 : 0x08)) < 0)
		return ret;

	/* input select: reg 0x02 bit4 = S-Video (separate Y/C), bits 3:2 = Y mux */
	if ((ret = gvusb2_i2c_read(gv, 0x02, &v)) < 0 ||
	    (ret = gvusb2_i2c_write(gv, 0x02, (v & 0xc3) |
			(in == GVUSB2_INPUT_SVIDEO ? 0x14 : 0x00))) < 0)
		return ret;

	/* decoder cropping */
	if ((ret = gvusb2_i2c_read(gv, 0x07, &v)) < 0)
		return ret;
	if (std == GVUSB2_STD_PAL) {
		/* vdelay 0x18, vactive 0x120 (288), hdelay 0x12, hactive 0x2d2 */
		if ((ret = gvusb2_i2c_write(gv, 0x07, (v & 0x0f) | 0x10)) < 0 ||
		    (ret = gvusb2_i2c_write(gv, 0x08, 0x18)) < 0 ||
		    (ret = gvusb2_i2c_write(gv, 0x09, 0x20)) < 0)
			return ret;
	} else {
		if ((ret = gvusb2_i2c_write(gv, 0x07, v & 0x0f)) < 0 ||
		    (ret = gvusb2_i2c_write(gv, 0x08, 0x13)) < 0 ||
		    (ret = gvusb2_i2c_write(gv, 0x09, 0xf4)) < 0)
			return ret;
	}
	if ((ret = gvusb2_i2c_write(gv, 0x0a, 0x12)) < 0 ||
	    (ret = gvusb2_i2c_write(gv, 0x0b, 0xd2)) < 0)
		return ret;

	/* bridge capture window */
	if ((ret = gvusb2_write_reg(gv, 0x0110, hstart & 0xff)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0111, hstart >> 8)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0112, vstart & 0xff)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0113, vstart >> 8)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0114, hend & 0xff)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0115, hend >> 8)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0116, vend & 0xff)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0117, vend >> 8)) < 0)
		return ret;

	/* "always double word" on the bridge (as the Linux driver does) */
	return gvusb2_set_reg_mask(gv, 0x05f0, 0x08, 0x08);
}

int gvusb2_video_start(struct gvusb2 *gv)
{
	int ret = gvusb2_write_reg(gv, 0x0100, 0xb3);

	if (ret < 0)
		return ret;
	return gvusb2_write_reg(gv, 0x0103, 0x00);
}

int gvusb2_video_stop(struct gvusb2 *gv)
{
	int ret = gvusb2_write_reg(gv, 0x0100, 0x33);

	if (ret < 0)
		return ret;
	return gvusb2_write_reg(gv, 0x0103, 0x00);
}

int gvusb2_video_status(struct gvusb2 *gv, uint8_t *status)
{
	return gvusb2_i2c_read(gv, 0x01, status);
}

/* ---- audio ------------------------------------------------------------- */

int gvusb2_audio_init(struct gvusb2 *gv)
{
	int ret;

	if (!gv->claimed_audio) {
		ret = libusb_claim_interface(gv->h, GVUSB2_AUDIO_IFACE);
		if (ret < 0) {
			fprintf(stderr, "gvusb2: claim audio interface: %s\n",
				libusb_strerror(ret));
			return ret;
		}
		gv->claimed_audio = 1;
	}
	ret = libusb_set_interface_alt_setting(gv->h, GVUSB2_AUDIO_IFACE, GVUSB2_AUDIO_ALT);
	if (ret < 0) {
		fprintf(stderr, "gvusb2: select audio alt setting: %s\n",
			libusb_strerror(ret));
		return ret;
	}

	/* audio GPIO pins to output, ADC out of reset */
	if ((ret = gvusb2_set_reg_mask(gv, 0x0002, 0x30, 0x30)) < 0 ||
	    (ret = gvusb2_set_reg_mask(gv, 0x0000, 0x30, 0x10)) < 0 ||
	    (ret = gvusb2_write_reg(gv, 0x0500, 0x00)) < 0 ||	/* AC97 off */
	    (ret = gvusb2_write_reg(gv, 0x050c, 0x01)) < 0)	/* I2S on */
		return ret;
	return 0;
}
