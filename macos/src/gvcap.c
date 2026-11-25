// SPDX-License-Identifier: BSD-3-Clause
/*
 * gvcap - lossless SD capture from an I-O DATA GV-USB2 on macOS.
 *
 * Writes raw UYVY 4:2:2 interlaced frames to stdout:
 *
 *   gvcap --input svideo | ffmpeg -f rawvideo -pix_fmt uyvy422 \
 *       -s 720x480 -r 30000/1001 -i - -c:v ffv1 out.mkv
 *
 * Stream format (from the Linux driver, confirmed on hardware):
 *   Every isochronous packet starts with a 4-byte header.
 *     byte0 bit7 set   -> status packet, marks the start of a field;
 *                         bit6 set = odd (top) field, clear = even field.
 *     byte0 bit7 clear -> data packet; byte1 is a 6-bit sequence counter,
 *                         payload is UYVY, 1440 bytes per line, no padding.
 *
 * Timing: every isochronous packet is one USB microframe (125 us), so
 * counting packets gives a clock that is shared by the video and audio
 * endpoints. Output frames are kept on that clock: if the source runs
 * slow or frames are lost a frame is duplicated, if it runs fast one is
 * dropped. Audio (48 kHz from the same device) therefore stays in sync.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gvusb2.h"

#define WIDTH           720
#define LINE_BYTES      (WIDTH * 2)

#define VID_XFERS       8
#define VID_PKTS        32		/* 4 ms per transfer */
#define AUD_XFERS       6
#define AUD_PKTS        16		/* 16 ms per transfer (1 packet/ms) */

#define FRAME_QUEUE     16		/* ~0.5 s of frames between USB and stdout */
#define AUDIO_RING      (1 << 20)	/* ~5 s of audio */
#define MAX_CATCHUP     300		/* most frames duplicated in one go */

static volatile sig_atomic_t g_stop;

/* ---- options ----------------------------------------------------------- */

static struct {
	enum gvusb2_input input;
	enum gvusb2_std std;
	long max_frames;
	const char *audio_path;
	const char *ts_path;
	int resync;
	int quiet;
	int debug_packets;
	int hdelay;
	/* picture controls; PIC_UNSET = leave the decoder's own value */
	int pic[5];
	const char *control_path;
} opt = { .resync = 1 };

#define PIC_UNSET  (-1000)
static const char *const pic_names[5] = {
	"brightness", "contrast", "saturation", "hue", "sharpness"
};
static const int pic_min[5] = { -128, 0, 0, -128, 0 };
static const int pic_max[5] = { 127, 255, 255, 127, 15 };

/* ---- stats ------------------------------------------------------------- */

static struct {
	unsigned long frames_out;	/* frames handed to the writer */
	unsigned long duplicated;	/* frames repeated to hold the clock */
	unsigned long dropped_rate;	/* frames dropped because source ran fast */
	unsigned long dropped_stall;	/* frames dropped because stdout stalled */
	unsigned long short_fields;	/* fields with missing or extra bytes */
	unsigned long lost_fields;	/* a field of a frame never arrived */
	unsigned long seq_errors;	/* packet counter discontinuities */
	unsigned long iso_errors;	/* failed isochronous packets */
	unsigned long audio_overruns;	/* audio bytes dropped (output stalled) */
} st;

static void msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("gvcap: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---- frame queue (USB thread -> stdout writer) -------------------------- */

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint8_t *buf[FRAME_QUEUE];
	int head, count;
	int done;
	size_t frame_bytes;
	pthread_t thread;
} fq = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static int write_all(int fd, const uint8_t *p, size_t n)
{
	while (n > 0) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static void *frame_writer(void *arg)
{
	uint8_t *local = malloc(fq.frame_bytes);

	for (;;) {
		pthread_mutex_lock(&fq.lock);
		while (fq.count == 0 && !fq.done)
			pthread_cond_wait(&fq.cond, &fq.lock);
		if (fq.count == 0) {
			pthread_mutex_unlock(&fq.lock);
			break;
		}
		memcpy(local, fq.buf[fq.head], fq.frame_bytes);
		fq.head = (fq.head + 1) % FRAME_QUEUE;
		fq.count--;
		pthread_mutex_unlock(&fq.lock);

		if (write_all(STDOUT_FILENO, local, fq.frame_bytes) < 0) {
			if (errno != EPIPE)
				msg("stdout: %s", strerror(errno));
			g_stop = 1;
			break;
		}
	}
	free(local);
	return NULL;
}

/* returns 0 if queued, -1 if the queue is full */
static int frame_push(const uint8_t *frame)
{
	int ret = -1;

	pthread_mutex_lock(&fq.lock);
	if (fq.count < FRAME_QUEUE) {
		memcpy(fq.buf[(fq.head + fq.count) % FRAME_QUEUE], frame, fq.frame_bytes);
		fq.count++;
		pthread_cond_signal(&fq.cond);
		ret = 0;
	}
	pthread_mutex_unlock(&fq.lock);
	return ret;
}

/* ---- audio ring (USB thread -> audio file writer) ------------------------ */

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint8_t *buf;
	size_t head, count;
	int done;
	int fd;
	int enabled;			/* set once the first video frame starts */
	uint64_t bytes;			/* bytes accepted since enabled */
	pthread_t thread;
} aq = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .fd = -1 };

static void *audio_writer(void *arg)
{
	uint8_t *local = malloc(65536);

	/* opening a FIFO blocks until the reader shows up, so do it here */
	aq.fd = open(opt.audio_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (aq.fd < 0)
		msg("audio output %s: %s", opt.audio_path, strerror(errno));

	for (;;) {
		size_t n, first;

		pthread_mutex_lock(&aq.lock);
		while (aq.count == 0 && !aq.done)
			pthread_cond_wait(&aq.cond, &aq.lock);
		if (aq.count == 0) {
			pthread_mutex_unlock(&aq.lock);
			break;
		}
		n = aq.count < 65536 ? aq.count : 65536;
		first = AUDIO_RING - aq.head;
		if (first > n)
			first = n;
		memcpy(local, aq.buf + aq.head, first);
		memcpy(local + first, aq.buf, n - first);
		aq.head = (aq.head + n) % AUDIO_RING;
		aq.count -= n;
		pthread_mutex_unlock(&aq.lock);

		if (aq.fd >= 0 && write_all(aq.fd, local, n) < 0) {
			msg("audio output: %s", strerror(errno));
			close(aq.fd);
			aq.fd = -1;
		}
	}
	if (aq.fd >= 0)
		close(aq.fd);
	free(local);
	return NULL;
}

static void audio_push(const uint8_t *p, size_t n)
{
	size_t tail, first;

	n &= ~(size_t)3;		/* whole stereo sample frames only */
	pthread_mutex_lock(&aq.lock);
	if (!aq.enabled) {
		pthread_mutex_unlock(&aq.lock);
		return;
	}
	if (n > AUDIO_RING - aq.count) {
		st.audio_overruns += n;
		pthread_mutex_unlock(&aq.lock);
		return;
	}
	tail = (aq.head + aq.count) % AUDIO_RING;
	first = AUDIO_RING - tail;
	if (first > n)
		first = n;
	memcpy(aq.buf + tail, p, first);
	memcpy(aq.buf, p + first, n - first);
	aq.count += n;
	aq.bytes += n;
	pthread_cond_signal(&aq.cond);
	pthread_mutex_unlock(&aq.lock);
}

/* ---- video stream parser ------------------------------------------------ */

static struct {
	int height, field_lines;
	size_t field_bytes;
	double period_us;		/* nominal frame duration */

	uint8_t *frame;			/* frame being assembled (persistent: lines
					 * that never arrive keep their old content) */
	int in_sync;			/* seen the first top-field marker */
	int field;			/* 0 = top/odd, 1 = bottom/even */
	int have[2];			/* fields received for the current frame */
	size_t field_pos;
	int field_short;
	unsigned counter;		/* expected packet sequence number */
	int counter_valid;

	uint64_t uframes;		/* microframes since stream start */
	uint64_t frame_ts;		/* uframe at which the current frame started */
	int have_t0;
	uint64_t t0;			/* uframe of the first output frame */

	FILE *ts_file;
	int dbg_left;
} vp;

static void emit_one(char flag)
{
	if (opt.max_frames > 0 && (long)st.frames_out >= opt.max_frames) {
		g_stop = 1;
		return;
	}
	if (frame_push(vp.frame) < 0) {
		st.dropped_stall++;
		/* the slot on the timeline is still consumed */
	}
	if (vp.ts_file)
		fprintf(vp.ts_file, "%lu,%.3f,%llu,%c\n", st.frames_out,
			(double)(vp.frame_ts - vp.t0) * 0.125,
			(unsigned long long)(aq.bytes / 4), flag);
	st.frames_out++;
}

/* A frame is complete; put it on the output timeline. */
static void frame_done(void)
{
	char flag = vp.field_short ? 'S' : '-';

	if (!vp.have[0] || !vp.have[1])
		st.lost_fields++;

	if (!vp.have_t0) {
		vp.have_t0 = 1;
		vp.t0 = vp.frame_ts;
	}
	if (opt.resync) {
		/* where this frame sits on the microframe clock, in frames */
		double pos = (double)(vp.frame_ts - vp.t0) * 125.0 / vp.period_us;
		double lag = pos - (double)st.frames_out;
		int n = 0;

		if (lag <= -1.0) {
			st.dropped_rate++;
			return;
		}
		/* The assembled frame is the best stand-in for the missing
		 * ones, so repeat it ahead of its own slot. */
		while (lag >= 1.0 && n < MAX_CATCHUP && !g_stop) {
			emit_one('D');
			st.duplicated++;
			lag -= 1.0;
			n++;
		}
		if (lag >= 1.0) {
			/* long outage: re-anchor rather than flood the output */
			vp.t0 += (uint64_t)(lag * vp.period_us / 125.0);
			msg("gap of %.1f s in the video stream, output timeline re-anchored",
				(n + lag) * vp.period_us / 1e6);
		}
	}
	if (!g_stop)
		emit_one(flag);
}

static void field_finish(void)
{
	if (vp.field_pos != vp.field_bytes) {
		st.short_fields++;
		vp.field_short = 1;
	}
}

static void status_packet(uint8_t b)
{
	int next = (b & 0x40) ? 0 : 1;

	if (!vp.in_sync) {
		if (next != 0)
			return;		/* start on a top field */
		vp.in_sync = 1;
	} else {
		field_finish();
		/* top field always starts a new frame; a second bottom field
		 * in a row means the top field between them was lost */
		if (next == 0 || vp.have[1]) {
			frame_done();
			vp.have[0] = vp.have[1] = 0;
			vp.field_short = 0;
		}
	}
	if (!vp.have[0] && !vp.have[1]) {
		vp.frame_ts = vp.uframes;
		if (!aq.enabled) {
			pthread_mutex_lock(&aq.lock);
			aq.enabled = 1;
			pthread_mutex_unlock(&aq.lock);
		}
	}
	vp.have[next] = 1;
	vp.field = next;
	vp.field_pos = 0;
}

static void data_packet(const uint8_t *p, size_t len)
{
	while (len > 0) {
		size_t line = vp.field_pos / LINE_BYTES;
		size_t col = vp.field_pos % LINE_BYTES;
		size_t n = LINE_BYTES - col;

		if (line >= (size_t)vp.field_lines) {
			/* more data than a field holds: count it, drop it */
			vp.field_pos += len;
			return;
		}
		if (n > len)
			n = len;
		memcpy(vp.frame + (line * 2 + (size_t)vp.field) * LINE_BYTES + col, p, n);
		vp.field_pos += n;
		p += n;
		len -= n;
	}
}

static void video_packet(const uint8_t *p, size_t len)
{
	if (len < 4)
		return;

	if (vp.dbg_left > 0 && ((p[0] & 0x80) || vp.dbg_left > opt.debug_packets / 2)) {
		fprintf(stderr, "pkt uf=%llu len=%zu hdr=%02x %02x %02x %02x\n",
			(unsigned long long)vp.uframes, len, p[0], p[1], p[2], p[3]);
		vp.dbg_left--;
	}

	if (p[0] & 0x80) {
		vp.counter = (vp.counter + 1) % 64;
		status_packet(p[0]);
		return;
	}
	if (vp.counter_valid && vp.counter != p[1])
		st.seq_errors++;
	vp.counter = (p[1] + 1u) % 64;
	vp.counter_valid = 1;
	if (vp.in_sync)
		data_packet(p + 4, len - 4);
}

/* ---- USB transfers ------------------------------------------------------- */

static int g_active_xfers;

static void LIBUSB_CALL video_cb(struct libusb_transfer *xfer)
{
	int i;

	if (xfer->status == LIBUSB_TRANSFER_COMPLETED) {
		const uint8_t *p = xfer->buffer;

		for (i = 0; i < xfer->num_iso_packets; i++) {
			const struct libusb_iso_packet_descriptor *d = &xfer->iso_packet_desc[i];

			if (d->status != LIBUSB_TRANSFER_COMPLETED)
				st.iso_errors++;
			else if (!g_stop)
				video_packet(p, d->actual_length);
			p += d->length;
			vp.uframes++;
		}
	} else {
		vp.uframes += (uint64_t)xfer->num_iso_packets;
		if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
			if (!g_stop)
				msg("device disconnected");
			g_stop = 1;
		} else if (xfer->status != LIBUSB_TRANSFER_CANCELLED) {
			st.iso_errors += (unsigned long)xfer->num_iso_packets;
		}
	}
	if (g_stop || libusb_submit_transfer(xfer) < 0) {
		g_active_xfers--;
		g_stop = 1;
	}
}

static void LIBUSB_CALL audio_cb(struct libusb_transfer *xfer)
{
	int i;

	if (xfer->status == LIBUSB_TRANSFER_COMPLETED && !g_stop) {
		const uint8_t *p = xfer->buffer;

		for (i = 0; i < xfer->num_iso_packets; i++) {
			const struct libusb_iso_packet_descriptor *d = &xfer->iso_packet_desc[i];

			if (d->status == LIBUSB_TRANSFER_COMPLETED && d->actual_length > 0)
				audio_push(p, d->actual_length);
			p += d->length;
		}
	} else if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
		g_stop = 1;
	}
	if (g_stop || libusb_submit_transfer(xfer) < 0)
		g_active_xfers--;
}

static int start_iso(struct gvusb2 *gv, struct libusb_transfer **xfers, int count,
		     uint8_t ep, int pkts, int pktsize, libusb_transfer_cb_fn cb)
{
	int i, ret;

	for (i = 0; i < count; i++) {
		uint8_t *buf = calloc((size_t)pkts, (size_t)pktsize);

		xfers[i] = libusb_alloc_transfer(pkts);
		if (!buf || !xfers[i])
			return LIBUSB_ERROR_NO_MEM;
		libusb_fill_iso_transfer(xfers[i], gv->h, ep, buf, pkts * pktsize,
					 pkts, cb, NULL, 0);
		libusb_set_iso_packet_lengths(xfers[i], (unsigned)pktsize);
	}
	for (i = 0; i < count; i++) {
		ret = libusb_submit_transfer(xfers[i]);
		if (ret < 0) {
			msg("submit iso transfer (ep 0x%02x): %s", ep, libusb_strerror(ret));
			return ret;
		}
		g_active_xfers++;
	}
	return 0;
}

/* ---- main ---------------------------------------------------------------- */

static void on_signal(int sig)
{
	g_stop = 1;
}

static void usage(FILE *f)
{
	fprintf(f,
"usage: gvcap [options] > frames.uyvy\n"
"\n"
"  -i, --input composite|svideo   video input (default composite)\n"
"  -s, --std ntsc|pal             720x480 @ 30000/1001 or 720x576 @ 25 (default ntsc)\n"
"  -a, --audio PATH               also write audio to PATH (file or FIFO) as raw\n"
"                                 48000 Hz, 16-bit little-endian, stereo\n"
"  -n, --frames N                 stop after N frames\n"
"  -t, --timestamps PATH          write a CSV line per output frame:\n"
"                                 index, ms on the USB clock, audio samples, flag\n"
"                                 (flag: - normal, D duplicate, S short field)\n"
"      --no-resync                never duplicate/drop frames to hold real time\n"
"      --hdelay N                 horizontal picture position (decoder HDELAY);\n"
"                                 lower moves the picture right\n"
"      --brightness N             -128..127 (0 = no offset)\n"
"      --contrast N               0..255 luma gain\n"
"      --saturation N             0..255 colour gain (U and V)\n"
"      --hue N                    -128..127 (0 = no shift)\n"
"      --sharpness N              0..15\n"
"                                 (picture options left out keep the decoder's\n"
"                                 own values; the values in use are printed)\n"
"      --control PATH             read \"NAME VALUE\" lines (picture option names)\n"
"                                 from PATH, usually a FIFO, and apply them live\n"
"  -q, --quiet                    no periodic status line\n"
"      --debug-packets N          print the first N interesting packet headers\n"
"  -h, --help\n"
"\n"
"Video goes to stdout as raw UYVY 4:2:2, interlaced, top line first:\n"
"  gvcap -i composite | ffmpeg -f rawvideo -pix_fmt uyvy422 -s 720x480 \\\n"
"      -r 30000/1001 -top 1 -i - -c:v ffv1 -level 3 out.mkv\n");
}

static void print_stats(struct gvusb2 *gv, int final)
{
	uint8_t s = 0;
	const char *sig = "";

	if (!final && gvusb2_video_status(gv, &s) == 0)
		sig = (s & 0x80) ? "  NO SIGNAL" : "  signal ok";
	fprintf(stderr,
		"gvcap: %s%lu frames, %lu duplicated, %lu dropped "
		"(%lu rate, %lu output stall), %lu short fields, %lu lost fields, "
		"%lu seq errors, %lu iso errors%s%s\n",
		final ? "total: " : "", st.frames_out, st.duplicated,
		st.dropped_rate + st.dropped_stall, st.dropped_rate, st.dropped_stall,
		st.short_fields, st.lost_fields, st.seq_errors, st.iso_errors,
		st.audio_overruns ? ", audio overruns" : "", sig);
}

/* "gvcap: picture: brightness 0, contrast 100, ..." — the values actually in
 * the decoder, printed at start and after every change (unless --quiet). */
static void print_picture(struct gvusb2 *gv)
{
	int b, c, s, h, sh;

	if (opt.quiet || gvusb2_picture_get(gv, &b, &c, &s, &h, &sh) < 0)
		return;
	msg("picture: brightness %d, contrast %d, saturation %d, hue %d, sharpness %d",
	    b, c, s, h, sh);
}

static int control_fd = -1;
static char control_buf[256];
static size_t control_len;

static int apply_control_line(struct gvusb2 *gv, char *line)
{
	char name[32];
	int value, k, ret;

	if (sscanf(line, "%31s %d", name, &value) != 2) {
		if (*line)
			msg("control: ignoring '%s'", line);
		return 0;
	}
	for (k = 0; k < 5; k++)
		if (!strcmp(name, pic_names[k]))
			break;
	if (k == 5 || value < pic_min[k] || value > pic_max[k]) {
		msg("control: ignoring '%s'", line);
		return 0;
	}
	if ((ret = gvusb2_picture_set(gv, name, value)) < 0) {
		msg("control: %s failed: %s", name, libusb_strerror(ret));
		return 0;
	}
	return 1;
}

static void poll_control(struct gvusb2 *gv)
{
	int changed = 0;

	for (;;) {
		ssize_t n = read(control_fd, control_buf + control_len,
				 sizeof(control_buf) - 1 - control_len);
		char *nl;

		if (n <= 0)	/* no data, or no writer connected yet */
			break;
		control_len += (size_t)n;
		control_buf[control_len] = '\0';
		while ((nl = strchr(control_buf, '\n')) != NULL) {
			*nl = '\0';
			changed |= apply_control_line(gv, control_buf);
			control_len -= (size_t)(nl + 1 - control_buf);
			memmove(control_buf, nl + 1, control_len + 1);
		}
		if (control_len == sizeof(control_buf) - 1)
			control_len = 0;	/* overlong line: drop it */
	}
	if (changed)	/* one read-back per batch, not per command */
		print_picture(gv);
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{"input", required_argument, 0, 'i'},
		{"std", required_argument, 0, 's'},
		{"audio", required_argument, 0, 'a'},
		{"frames", required_argument, 0, 'n'},
		{"timestamps", required_argument, 0, 't'},
		{"no-resync", no_argument, 0, 1},
		{"quiet", no_argument, 0, 'q'},
		{"debug-packets", required_argument, 0, 2},
		{"hdelay", required_argument, 0, 3},
		{"brightness", required_argument, 0, 10},
		{"contrast", required_argument, 0, 11},
		{"saturation", required_argument, 0, 12},
		{"hue", required_argument, 0, 13},
		{"sharpness", required_argument, 0, 14},
		{"control", required_argument, 0, 4},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};
	struct libusb_transfer *vx[VID_XFERS] = {0}, *ax[AUD_XFERS] = {0};
	struct gvusb2 gv;
	struct sigaction sa;
	uint64_t last_report = 0, last_control = 0;
	int c, i, ret, rc = 1;

	for (i = 0; i < 5; i++)
		opt.pic[i] = PIC_UNSET;
	while ((c = getopt_long(argc, argv, "i:s:a:n:t:qh", longopts, NULL)) != -1) {
		switch (c) {
		case 'i':
			if (!strcmp(optarg, "svideo") || !strcmp(optarg, "s-video"))
				opt.input = GVUSB2_INPUT_SVIDEO;
			else if (!strcmp(optarg, "composite"))
				opt.input = GVUSB2_INPUT_COMPOSITE;
			else {
				msg("unknown input '%s'", optarg);
				return 2;
			}
			break;
		case 's':
			if (!strcmp(optarg, "ntsc"))
				opt.std = GVUSB2_STD_NTSC;
			else if (!strcmp(optarg, "pal"))
				opt.std = GVUSB2_STD_PAL;
			else {
				msg("unknown standard '%s'", optarg);
				return 2;
			}
			break;
		case 'a':
			/* "gvcap -a -i svideo" would otherwise take "-i" as the
			 * audio file name and swallow the next option */
			if (optarg[0] == '-') {
				msg("--audio needs a file path, got '%s' "
				    "(use ./%s for a file that really starts with -)",
				    optarg, optarg);
				return 2;
			}
			opt.audio_path = optarg;
			break;
		case 'n': opt.max_frames = atol(optarg); break;
		case 't': opt.ts_path = optarg; break;
		case 'q': opt.quiet = 1; break;
		case 1:   opt.resync = 0; break;
		case 2:   opt.debug_packets = atoi(optarg); break;
		case 3:   opt.hdelay = (int)strtol(optarg, NULL, 0);
			  if (opt.hdelay < 1 || opt.hdelay > 0x3ff) {
				msg("--hdelay must be 1..1023");
				return 2;
			  }
			  break;
		case 4:   opt.control_path = optarg; break;
		case 10: case 11: case 12: case 13: case 14: {
			int k = c - 10;
			char *end;
			long v = strtol(optarg, &end, 0);

			if (*optarg == '\0' || *end != '\0' || v < pic_min[k] || v > pic_max[k]) {
				msg("--%s must be %d..%d", pic_names[k], pic_min[k], pic_max[k]);
				return 2;
			}
			opt.pic[k] = (int)v;
			break;
		}
		case 'h': usage(stdout); return 0;
		default:  usage(stderr); return 2;
		}
	}
	if (isatty(STDOUT_FILENO)) {
		msg("refusing to write raw video to a terminal; pipe or redirect stdout");
		usage(stderr);
		return 2;
	}

	vp.height = opt.std == GVUSB2_STD_PAL ? 576 : 480;
	vp.field_lines = vp.height / 2;
	vp.field_bytes = (size_t)vp.field_lines * LINE_BYTES;
	vp.period_us = opt.std == GVUSB2_STD_PAL ? 40000.0 : 1001000.0 / 30.0;
	vp.dbg_left = opt.debug_packets;
	fq.frame_bytes = (size_t)vp.height * LINE_BYTES;
	vp.frame = malloc(fq.frame_bytes);
	for (i = 0; i < FRAME_QUEUE; i++)
		fq.buf[i] = malloc(fq.frame_bytes);
	/* start from black (UYVY 80 10 80 10), not green */
	for (size_t k = 0; k < fq.frame_bytes; k += 2) {
		vp.frame[k] = 0x80;
		vp.frame[k + 1] = 0x10;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (opt.ts_path) {
		vp.ts_file = fopen(opt.ts_path, "w");
		if (!vp.ts_file) {
			msg("%s: %s", opt.ts_path, strerror(errno));
			return 1;
		}
		fprintf(vp.ts_file, "frame,usb_ms,audio_samples,flag\n");
	}

	if (gvusb2_open(&gv) < 0)
		return 1;
	gv.hdelay = opt.hdelay;
	if ((ret = gvusb2_video_init(&gv, opt.std, opt.input)) < 0) {
		msg("device init failed: %s", libusb_strerror(ret));
		goto out;
	}
	for (i = 0; i < 5; i++) {
		if (opt.pic[i] == PIC_UNSET)
			continue;
		if ((ret = gvusb2_picture_set(&gv, pic_names[i], opt.pic[i])) < 0) {
			msg("setting %s failed: %s", pic_names[i], libusb_strerror(ret));
			goto out;
		}
	}
	print_picture(&gv);
	if (opt.control_path) {
		/* non-blocking so a FIFO opens without waiting for a writer */
		control_fd = open(opt.control_path, O_RDONLY | O_NONBLOCK);
		if (control_fd < 0) {
			msg("control %s: %s", opt.control_path, strerror(errno));
			goto out;
		}
	}
	if (opt.audio_path) {
		if ((ret = gvusb2_audio_init(&gv)) < 0) {
			msg("audio init failed: %s", libusb_strerror(ret));
			goto out;
		}
		aq.buf = malloc(AUDIO_RING);
		pthread_create(&aq.thread, NULL, audio_writer, NULL);
	}
	pthread_create(&fq.thread, NULL, frame_writer, NULL);

	if (!opt.quiet)
		msg("%s, %s input, %dx%d UYVY%s", opt.std == GVUSB2_STD_PAL ? "PAL" : "NTSC",
		    opt.input == GVUSB2_INPUT_SVIDEO ? "S-Video" : "composite",
		    WIDTH, vp.height, opt.audio_path ? ", audio 48000 Hz s16le stereo" : "");

	if ((ret = gvusb2_video_start(&gv)) < 0)
		goto stop;
	if (start_iso(&gv, vx, VID_XFERS, GVUSB2_VIDEO_EP, VID_PKTS,
		      GVUSB2_VIDEO_MAXPKT, video_cb) < 0)
		goto stop;
	if (opt.audio_path &&
	    start_iso(&gv, ax, AUD_XFERS, GVUSB2_AUDIO_EP, AUD_PKTS,
		      GVUSB2_AUDIO_MAXPKT, audio_cb) < 0)
		goto stop;

	rc = 0;
	while (!g_stop) {
		struct timeval tv = { 0, 100000 };

		ret = libusb_handle_events_timeout_completed(gv.ctx, &tv, NULL);
		if (ret < 0 && ret != LIBUSB_ERROR_INTERRUPTED) {
			msg("usb event loop: %s", libusb_strerror(ret));
			rc = 1;
			break;
		}
		/* picture commands, polled every ~50 ms of stream time */
		if (control_fd >= 0 && vp.uframes - last_control >= 400) {
			last_control = vp.uframes;
			poll_control(&gv);
		}
		/* every 5 s of stream time */
		if (!opt.quiet && vp.uframes - last_report >= 40000) {
			last_report = vp.uframes;
			print_stats(&gv, 0);
		}
	}

stop:
	g_stop = 1;
	for (i = 0; i < VID_XFERS; i++)
		if (vx[i])
			libusb_cancel_transfer(vx[i]);
	for (i = 0; i < AUD_XFERS; i++)
		if (ax[i])
			libusb_cancel_transfer(ax[i]);
	for (i = 0; i < 50 && g_active_xfers > 0; i++) {
		struct timeval tv = { 0, 100000 };

		libusb_handle_events_timeout_completed(gv.ctx, &tv, NULL);
	}
	gvusb2_video_stop(&gv);

	pthread_mutex_lock(&fq.lock);
	fq.done = 1;
	pthread_cond_signal(&fq.cond);
	pthread_mutex_unlock(&fq.lock);
	pthread_join(fq.thread, NULL);
	/*
	 * All video is written: close it now, not at exit. A reader such as
	 * ffmpeg interleaves video and audio, so it can stop reading audio while
	 * it waits for more video. If the audio writer still has a backlog to
	 * flush, keeping stdout open here would leave everyone waiting forever.
	 */
	close(STDOUT_FILENO);
	if (opt.audio_path) {
		pthread_mutex_lock(&aq.lock);
		aq.done = 1;
		pthread_cond_signal(&aq.cond);
		pthread_mutex_unlock(&aq.lock);
		pthread_join(aq.thread, NULL);
	}
	if (vp.ts_file)
		fclose(vp.ts_file);
	if (control_fd >= 0)
		close(control_fd);
	print_stats(&gv, 1);
out:
	gvusb2_close(&gv);
	return rc;
}
