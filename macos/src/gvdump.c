// SPDX-License-Identifier: BSD-3-Clause
/*
 * gvdump - print the GV-USB2's USB descriptors and, with --regs, the
 * TW9910 decoder registers. Run once per machine to confirm the device
 * looks the way gvcap expects.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gvusb2.h"

static const char *xfer_type(uint8_t attr)
{
	switch (attr & 3) {
	case 0: return "control";
	case 1: return "isoc";
	case 2: return "bulk";
	default: return "interrupt";
	}
}

static void dump_descriptors(libusb_device_handle *h)
{
	libusb_device *dev = libusb_get_device(h);
	struct libusb_device_descriptor dd;
	struct libusb_config_descriptor *cfg;
	unsigned char str[128];
	int i, a, e;

	libusb_get_device_descriptor(dev, &dd);
	printf("device %04x:%04x  bcdUSB %x.%02x  bcdDevice %x.%02x  speed %d\n",
		dd.idVendor, dd.idProduct, dd.bcdUSB >> 8, dd.bcdUSB & 0xff,
		dd.bcdDevice >> 8, dd.bcdDevice & 0xff, libusb_get_device_speed(dev));
	if (libusb_get_string_descriptor_ascii(h, dd.iManufacturer, str, sizeof(str)) > 0)
		printf("  manufacturer: %s\n", str);
	if (libusb_get_string_descriptor_ascii(h, dd.iProduct, str, sizeof(str)) > 0)
		printf("  product:      %s\n", str);

	if (libusb_get_active_config_descriptor(dev, &cfg) < 0) {
		printf("  (cannot read config descriptor)\n");
		return;
	}
	printf("config %d: %d interfaces, max power %d mA\n",
		cfg->bConfigurationValue, cfg->bNumInterfaces, cfg->MaxPower * 2);
	for (i = 0; i < cfg->bNumInterfaces; i++) {
		const struct libusb_interface *itf = &cfg->interface[i];

		for (a = 0; a < itf->num_altsetting; a++) {
			const struct libusb_interface_descriptor *id = &itf->altsetting[a];

			printf("  interface %d alt %d: class %02x/%02x/%02x, %d endpoints\n",
				id->bInterfaceNumber, id->bAlternateSetting,
				id->bInterfaceClass, id->bInterfaceSubClass,
				id->bInterfaceProtocol, id->bNumEndpoints);
			for (e = 0; e < id->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
				int size = ep->wMaxPacketSize & 0x7ff;
				int mult = ((ep->wMaxPacketSize >> 11) & 3) + 1;

				printf("    ep 0x%02x %-9s wMaxPacketSize 0x%04x (%d x %d = %d bytes) interval %d\n",
					ep->bEndpointAddress, xfer_type(ep->bmAttributes),
					ep->wMaxPacketSize, mult, size, mult * size,
					ep->bInterval);
			}
		}
	}
	libusb_free_config_descriptor(cfg);
}

int main(int argc, char **argv)
{
	struct gvusb2 gv;
	int regs = argc > 1 && strcmp(argv[1], "--regs") == 0;
	int i;

	if (argc > 1 && !regs) {
		fprintf(stderr, "usage: gvdump [--regs [composite|svideo]]\n"
			"  --regs  also initialise the device and dump the TW9910\n"
			"          decoder registers (reg 01 bit7 = no video signal)\n");
		return 2;
	}
	if (gvusb2_open(&gv) < 0)
		return 1;
	dump_descriptors(gv.h);

	if (regs) {
		uint8_t v;

		/* the decoder is held in reset until the capture init runs */
		enum gvusb2_input in = argc > 2 && !strcmp(argv[2], "svideo") ?
			GVUSB2_INPUT_SVIDEO : GVUSB2_INPUT_COMPOSITE;

		if (gvusb2_video_init(&gv, GVUSB2_STD_NTSC, in) < 0) {
			gvusb2_close(&gv);
			return 1;
		}
		usleep(500000);		/* let the decoder try to lock */
		printf("TW9910 registers (i2c 0x%02x):", GVUSB2_TW9910_ADDR);
		for (i = 0; i < 0x70; i++) {
			if ((i & 15) == 0)
				printf("\n  %02x:", i);
			if (gvusb2_i2c_read(&gv, (uint8_t)i, &v) < 0)
				printf(" ??");
			else
				printf(" %02x", v);
		}
		printf("\n");
	}
	gvusb2_close(&gv);
	return 0;
}
