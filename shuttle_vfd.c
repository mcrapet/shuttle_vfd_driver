/*
 * Shuttle VFD (20x1 character display. Each character cell is 5x8 pixels)
 * - The display is driven by Princeton Technologies PT6314 VFD controller
 * - Cypress CY7C63723C (receives USB commands and control VFD controller)
 *
 * Tested on Shuttle XPC models: SG33G5M.
 *
 * Copyright (C) 2009-2010 Matthieu Crapet <mcrapet@gmail.com>
 * Based on some "drivers/usb/misc" sources
 *
 * LCD "prococol" : each message has a length of 8 bytes
 * - 1 nibble: command (0x1, 0x3, 0x7, 0x9, 0xD)
 *     - 0x1 : clear text and icons (len=1)
 *     - 0x7 : icons (len=4)
 *     - 0x9 : text (len=7)
 *     - 0xD : set clock data (len=7)
 *     - 0x3 : display clock (internal feature) (len=1)
 * - 1 nibble: message length (0-7)
 * - 7 bytes : message data
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/rtc.h>
#include <linux/usb.h>
#include <linux/timer.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/kernel_stat.h>
#include <linux/slab.h>
#include <asm/cputime.h>

#define SHUTTLE_VFD_VENDOR_ID           0x051C

#if defined(CONFIG_RTC_HCTOSYS_DEVICE)
#define SHUTTLE_VFD_RTC_DEVICE          CONFIG_RTC_HCTOSYS_DEVICE
#else
#define SHUTTLE_VFD_RTC_DEVICE          "rtc0"
#endif
#define SHUTTLE_VFD_GREETING_MSG        "Linux"

/* VFD physical dimensions */
#define SHUTTLE_VFD_WIDTH               20
#define SHUTTLE_VFD_HEIGHT              1  // not used

/* VFD USB control message */
#define SHUTTLE_VFD_PACKET_SIZE         8
#define SHUTTLE_VFD_DATA_SIZE           (SHUTTLE_VFD_PACKET_SIZE-1)
#define SHUTTLE_VFD_SLEEP_MS            24

/* VFD Icons (15+12). vol01-12 is managed as a single icon. */
#define SHUTTLE_VFD_ICON_CLOCK          (1 << 4)
#define SHUTTLE_VFD_ICON_RADIO          (1 << 3)
#define SHUTTLE_VFD_ICON_MUSIC          (1 << 2)
#define SHUTTLE_VFD_ICON_CD_DVD         (1 << 1)
#define SHUTTLE_VFD_ICON_TELEVISION     (1 << 0)
#define SHUTTLE_VFD_ICON_CAMERA         (1 << 9)
#define SHUTTLE_VFD_ICON_REWIND         (1 << 8)
#define SHUTTLE_VFD_ICON_RECORD         (1 << 7)
#define SHUTTLE_VFD_ICON_PLAY           (1 << 6)
#define SHUTTLE_VFD_ICON_PAUSE          (1 << 5)
#define SHUTTLE_VFD_ICON_STOP           (1 << 14)
#define SHUTTLE_VFD_ICON_FASTFORWARD    (1 << 13)
#define SHUTTLE_VFD_ICON_REVERSE        (1 << 12)
#define SHUTTLE_VFD_ICON_REPEAT         (1 << 11)
#define SHUTTLE_VFD_ICON_MUTE           (1 << 10)
#define SHUTTLE_VFD_ICON_VOL_01         (1 << 15)
#define SHUTTLE_VFD_ICON_VOL_02         (2 << 15)
#define SHUTTLE_VFD_ICON_VOL_03         (3 << 15)
#define SHUTTLE_VFD_ICON_VOL_04         (4 << 15)
#define SHUTTLE_VFD_ICON_VOL_05         (5 << 15)
#define SHUTTLE_VFD_ICON_VOL_06         (6 << 15)
#define SHUTTLE_VFD_ICON_VOL_07         (7 << 15)
#define SHUTTLE_VFD_ICON_VOL_08         (8 << 15)
#define SHUTTLE_VFD_ICON_VOL_09         (9 << 15)
#define SHUTTLE_VFD_ICON_VOL_10         (10 << 15)
#define SHUTTLE_VFD_ICON_VOL_11         (11 << 15)
#define SHUTTLE_VFD_ICON_VOL_12         (12 << 15)

#define SHUTTLE_VFD_BASE_MASK           (0x7FFF)
#define SHUTTLE_VFD_VOL_MASK            (0xF << 15)
#define SHUTTLE_VFD_ALL_ICONS           (0x7FFF|SHUTTLE_VFD_ICON_VOL_12)

#define DRIVER_ICON_CLEAR               (1 << 28)
#define DRIVER_ICON_SET                 (1 << 29)

#define DEC_AS_HEX(v)                   (((v)/10 * 16) + ((v)%10))
#define DRIVER_VFD_REFRESH              (jiffies + (HZ * 1)) // 1s

/* mode */
#define DRIVER_MODE_TEXT                0
#define DRIVER_MODE_HW_CLOCK            1
#define DRIVER_MODE_SW_CPU              2

/* text_style */
#define DRIVER_STYLE_TEXT_ALIGN_LEFT    0
#define DRIVER_STYLE_TEXT_ALIGN_RIGHT   1
#define DRIVER_STYLE_TEXT_ALIGN_CENTER  2


/* Module parameter */
static char message[SHUTTLE_VFD_WIDTH+1] = SHUTTLE_VFD_GREETING_MSG;
module_param_string(initial_msg, message, sizeof(message), S_IRUGO);
MODULE_PARM_DESC(initial_msg, "Set initial message (" __MODULE_STRING(SHUTTLE_VFD_WIDTH) " chars max)");

/* Table of devices that work with this driver */
static struct usb_device_id shuttle_vfd_table [] = {
	{ USB_DEVICE(SHUTTLE_VFD_VENDOR_ID, 0x0003) },
	{ USB_DEVICE(SHUTTLE_VFD_VENDOR_ID, 0x0005) },
	{ }
};
MODULE_DEVICE_TABLE(usb, shuttle_vfd_table);

struct vfd_icons {
	char *name, *altname;
	unsigned long value;
};

const struct vfd_icons icons[] = {
	{ "clk", "clock",   SHUTTLE_VFD_ICON_CLOCK},
	{ "rad", "radio",   SHUTTLE_VFD_ICON_RADIO},
	{ "mus", "music",   SHUTTLE_VFD_ICON_MUSIC},
	{ "cd",  "dvd",     SHUTTLE_VFD_ICON_CD_DVD},
	{ "tv",  "tele",    SHUTTLE_VFD_ICON_TELEVISION},
	{ "cam", "camera",  SHUTTLE_VFD_ICON_CAMERA},
	{ "rew", "rewind",  SHUTTLE_VFD_ICON_REWIND},
	{ "rec", "record",  SHUTTLE_VFD_ICON_RECORD},
	{ "pl",  "play",    SHUTTLE_VFD_ICON_PLAY},
	{ "pa",  "pause",   SHUTTLE_VFD_ICON_PAUSE},
	{ "st",  "stop",    SHUTTLE_VFD_ICON_STOP},
	{ "ff",  NULL,      SHUTTLE_VFD_ICON_FASTFORWARD},
	{ "rev", "reverse", SHUTTLE_VFD_ICON_REVERSE},
	{ "rep", "repeat",  SHUTTLE_VFD_ICON_REPEAT},
	{ "mute", "vol0",   SHUTTLE_VFD_ICON_MUTE},
	{ "all", "world",   SHUTTLE_VFD_ALL_ICONS},
	{ "clear", "none",  DRIVER_ICON_CLEAR},
	{ "=", NULL,        DRIVER_ICON_SET}
};

/* Working structure */
struct shuttle_vfd {
	struct usb_device *udev;
	struct mutex vfd_mutex;

	unsigned long icons_mask;
	unsigned long mode;
	unsigned long text_style;
	unsigned char packet[SHUTTLE_VFD_PACKET_SIZE];
	unsigned char screen[SHUTTLE_VFD_WIDTH];

	struct work_struct work;
	struct timer_list vfd_timer;
	int vfd_timer_reschedule;

	/* used for cpu load */
	cputime64_t used, total;
};

/* Local prototypes */
static void vfd_timer(unsigned long);
static int vfd_send_packet(struct shuttle_vfd *, unsigned char *);
static void vfd_set_clock(struct shuttle_vfd *);
static int vfd_parse_icons(const char *, size_t, unsigned long *);
static inline void vfd_reset_cursor(struct shuttle_vfd *, bool);
static inline void vfd_set_text(struct shuttle_vfd *, size_t);
static inline void vfd_set_icons(struct shuttle_vfd *);
static void vfd_periodic_work(struct work_struct *work);


static void vfd_periodic_work(struct work_struct *w)
{
	struct shuttle_vfd *vfd = container_of(w, struct shuttle_vfd, work);

	cputime64_t user, nice, system, idle, r;
	int i;

	user = nice = system = idle = cputime64_zero;
	for_each_possible_cpu(i) {
		user = cputime64_add(user, kstat_cpu(i).cpustat.user);
		nice = cputime64_add(nice, kstat_cpu(i).cpustat.nice);
		system = cputime64_add(system, kstat_cpu(i).cpustat.system);
		idle = cputime64_add(idle, kstat_cpu(i).cpustat.idle);
	}

	memset(&vfd->screen[0], 0x0, SHUTTLE_VFD_WIDTH);

	user = cputime64_add(user, nice);
	user = cputime64_add(user, system);
	idle = cputime64_add(idle, user); // total (jiffies)

	vfd->used  = cputime64_sub(user, vfd->used);
	vfd->total = cputime64_sub(idle, vfd->total);

	if (vfd->used == vfd->total)
		r = cputime64_zero;
	else
		r = 1000 * vfd->used / vfd->total;

	i = (int)r;

	snprintf(&vfd->screen[0], SHUTTLE_VFD_WIDTH, "CPU:%2d.%d%%", i/10, i%10);
	vfd_set_text(vfd, SHUTTLE_VFD_WIDTH);

	vfd->used  = user;
	vfd->total = idle;
}

static void vfd_timer(unsigned long data)
{
	struct shuttle_vfd *vfd = (struct shuttle_vfd *)data;

	if (vfd->vfd_timer_reschedule) {
		schedule_work(&vfd->work);
		vfd->vfd_timer.expires = DRIVER_VFD_REFRESH;
		add_timer(&vfd->vfd_timer);
	}
}

static int vfd_send_packet(struct shuttle_vfd *vfd, unsigned char *packet)
{
	int result;

	mutex_lock(&vfd->vfd_mutex);
	result = usb_control_msg(vfd->udev,
			usb_sndctrlpipe(vfd->udev, 0),
			0x09,    // SET_REPORT request
			0x21,    // HID class
			0x0200,  // Report Type = output ; Report ID = 0 (unused)
			0x0001,  // Interface
			(char *) (packet) ? packet : vfd->packet,
			SHUTTLE_VFD_PACKET_SIZE,
			USB_CTRL_GET_TIMEOUT / 4);

	/* this sleep inside the critical section is not very nice,
	 * but it avoids screw-up display on conccurent access */
	msleep(SHUTTLE_VFD_SLEEP_MS);

	mutex_unlock(&vfd->vfd_mutex);
	if (result < 0)
		dev_err(&vfd->udev->dev, "send packed failed: %d\n", result);

	return result;
}

static inline void vfd_reset_cursor(struct shuttle_vfd *vfd, bool eraseall)
{
	unsigned char packet[SHUTTLE_VFD_PACKET_SIZE];

	memset(&packet[0], 0, SHUTTLE_VFD_PACKET_SIZE);
	packet[0] = (1 << 4) + 1;

	if (eraseall)
		packet[1] = 1; // full clear (text + icons)
	else
		packet[1] = 2; // just reset the text cursor (keep text)

	vfd_send_packet(vfd, &packet[0]);
}

/* Built-in feature, will display SHUTTLE_VFD_ICON_CLOCK */
static void vfd_set_clock(struct shuttle_vfd *vfd)
{
	struct rtc_time now;
	struct rtc_device *rtc = rtc_class_open(SHUTTLE_VFD_RTC_DEVICE);

	memset(&now, 0, sizeof(now));
	if (rtc == NULL) {
		dev_err(&vfd->udev->dev, "can't get rtc time\n");
	} else {
		int err = rtc_read_time(rtc, &now); // UTC

		if ((err == 0) && (rtc_valid_tm(&now) != 0)) {
			dev_err(&vfd->udev->dev, "invalid rtc time\n");
		}

		rtc_class_close(rtc);
	}

	vfd->packet[0] = (0xD << 4) + 7;
	vfd->packet[1] = DEC_AS_HEX(now.tm_sec);      // sec
	vfd->packet[2] = DEC_AS_HEX(now.tm_min);      // min
	vfd->packet[3] = DEC_AS_HEX(now.tm_hour);     // hours
	vfd->packet[4] = now.tm_wday;                 // day-of-week (1=monday)
	vfd->packet[5] = DEC_AS_HEX(now.tm_mday);     // day
	vfd->packet[6] = DEC_AS_HEX(now.tm_mon+1);    // month
	vfd->packet[7] = DEC_AS_HEX(now.tm_year-100); // year
	vfd_send_packet(vfd, NULL);

	msleep(20);

	memset(vfd->packet, 0, SHUTTLE_VFD_PACKET_SIZE);
	vfd->packet[0] = (3 << 4) + 1;
	vfd->packet[1] = 3;
	vfd_send_packet(vfd, NULL);
}

static inline void vfd_set_text(struct shuttle_vfd *vfd, size_t len)
{
	size_t i;
	char *p = (char *)&vfd->screen[0];

	for (i = 0; i < (len/SHUTTLE_VFD_DATA_SIZE); i++) {
		vfd->packet[0] = (9 << 4) + SHUTTLE_VFD_DATA_SIZE;
		memcpy(vfd->packet + 1, p, SHUTTLE_VFD_DATA_SIZE);
		p += SHUTTLE_VFD_DATA_SIZE;
		vfd_send_packet(vfd, NULL);
	}

	len = len % SHUTTLE_VFD_DATA_SIZE;
	if (len != 0) {
		memset(&vfd->packet[0], 0, SHUTTLE_VFD_PACKET_SIZE);
		vfd->packet[0] = (9 << 4) + len;
		memcpy(vfd->packet + 1, p, len);
		vfd_send_packet(vfd, NULL);
	}
}

static inline void vfd_set_icons(struct shuttle_vfd *vfd)
{
	unsigned char packet[SHUTTLE_VFD_PACKET_SIZE];

	memset(&packet[0], 0, SHUTTLE_VFD_PACKET_SIZE);
	packet[0] = (7 << 4) + 4;
	packet[1] = (vfd->icons_mask >> 15) & 0x1F;
	packet[2] = (vfd->icons_mask >> 10) & 0x1F;
	packet[3] = (vfd->icons_mask >>  5) & 0x1F;
	packet[4] = vfd->icons_mask & 0x1F; // each data byte is stored on 5 bits
	vfd_send_packet(vfd, &packet[0]);
}

static int vfd_parse_icons(const char *name, size_t count, unsigned long *val)
{
	int i;

	*val = 0;

	for (i = 0; i < sizeof(icons)/sizeof(struct vfd_icons); i++)
	{
		if ((strlen(icons[i].name) == count) &&
				(strncmp(name, icons[i].name, count) == 0)) {
			*val = icons[i].value;
		} else if ((icons[i].altname != NULL) &&
				(strlen(icons[i].altname) == count) &&
				(strncmp(name, icons[i].altname, count) == 0)) {
			*val = icons[i].value;
		} else if ((count == 4) && (strncmp(name, "vol", 3) == 0) &&
				(name[3] > 0x30 && name[3] <= 0x39)) {
			*val = (name[3] - 0x30) << 15;
		} else if ((count == 5) && (strncmp(name, "vol1", 4) == 0) &&
				(name[4] >= 0x30 && name[4] <= 0x32)) {
			*val = (name[4] - 0x30 + 10) << 15;
		} else {
			continue;
		}
		break;
	}

	return ((*val == 0) ? -1 : 0);
}

/* attribute callback handler (text write) */
static ssize_t set_vfd_text_handler(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct shuttle_vfd *vfd = usb_get_intfdata(intf);
	size_t l;

	if (count < SHUTTLE_VFD_WIDTH) {
		memset(&vfd->screen[0], 0, SHUTTLE_VFD_WIDTH);
	}

	/* aplly text style */
	l = min(count, (size_t)SHUTTLE_VFD_WIDTH);
	if (vfd->text_style == DRIVER_STYLE_TEXT_ALIGN_RIGHT) {

		memset(&vfd->screen[0], 0x20, SHUTTLE_VFD_WIDTH - l);
		memcpy(&vfd->screen[SHUTTLE_VFD_WIDTH-l], buf, l);

	} else if(vfd->text_style == DRIVER_STYLE_TEXT_ALIGN_CENTER) {

		memset(&vfd->screen[0], 0x20, SHUTTLE_VFD_WIDTH);
		memcpy(&vfd->screen[(SHUTTLE_VFD_WIDTH-l)/2], buf, l);

	} else {
		memcpy(&vfd->screen[0], buf, l);
	}

	if (vfd->mode == DRIVER_MODE_TEXT) {
		vfd_reset_cursor(vfd, false);
		vfd_set_text(vfd, SHUTTLE_VFD_WIDTH);
	}

	return count;
}

/* attribute callback handler (icons write) */
static ssize_t set_vfd_icons_handler(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	char *p;
	size_t i, l;
	unsigned long tmp, mask;

	struct usb_interface *intf = to_usb_interface(dev);
	struct shuttle_vfd *vfd = usb_get_intfdata(intf);

	p = (char *)buf;
	mask = 0;
	i = l = 0;
	while (i < count && buf[i] != '\0') {
		if (p[l] == ',' || p[l] == ' ' || p[l] == '\n') {
			if (l > 0)
			{
				if (vfd_parse_icons(p, l, &tmp) == 0) {
					mask |= tmp;
				} else {
					dev_err(&vfd->udev->dev, "unknown icon (l=%ld), ignoring\n", l);
				}
				p = p + l + 1;
				l = 0;
			}
			else
			{
				p++;
			}
		}
		else
		{
			l++;
		}
		i++;
	}

	if (l > 0) {
		if (vfd_parse_icons(p, l, &tmp) == 0) {
			mask |= tmp;
		} else {
			dev_err(&vfd->udev->dev, "unknown icon (l=%ld), ignoring\n", l);
		}
	}

	if (mask & DRIVER_ICON_CLEAR) {
		vfd->icons_mask = 0;
	} else if (mask & DRIVER_ICON_SET) {
		vfd->icons_mask = mask & ~DRIVER_ICON_SET;
	} else if (mask >= SHUTTLE_VFD_ICON_VOL_01) {

		if ((vfd->icons_mask & SHUTTLE_VFD_VOL_MASK) ==
				(mask & SHUTTLE_VFD_VOL_MASK))
			mask &= SHUTTLE_VFD_BASE_MASK;

		vfd->icons_mask = (vfd->icons_mask & SHUTTLE_VFD_BASE_MASK) ^ mask;
	} else {
		vfd->icons_mask ^= mask;
	}

	vfd_set_icons(vfd);

	return count;
}

/* attribute callback handler (mode write) */
static ssize_t set_vfd_mode_handler(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int ret;
	unsigned long newmode;
	char cmd[6];

	struct usb_interface *intf = to_usb_interface(dev);
	struct shuttle_vfd *vfd = usb_get_intfdata(intf);

	ret = sscanf(buf, "%5s", cmd);
	if (ret != 1)
		return -EINVAL;

	if ((strcmp(cmd, "clock") == 0) || (strcmp(cmd, "clk") == 0)) {
		newmode = DRIVER_MODE_HW_CLOCK;
	} else if (strcmp(cmd, "cpu") == 0) {
		newmode = DRIVER_MODE_SW_CPU;
	} else if ((strcmp(cmd, "text") == 0) || (strcmp(cmd, "txt") == 0)) {
		newmode = DRIVER_MODE_TEXT;
	} else {
		return -EINVAL;
	}

	if (newmode != vfd->mode) {
		if (vfd->mode == DRIVER_MODE_SW_CPU)
			vfd->vfd_timer_reschedule = 0;

		vfd->mode = newmode;

		switch (newmode) {
			case DRIVER_MODE_HW_CLOCK:
				vfd_reset_cursor(vfd, true);
				vfd_set_clock(vfd);
				break;

			case DRIVER_MODE_SW_CPU:
				vfd_reset_cursor(vfd, true);
				vfd->used = vfd->total = cputime64_zero;
				vfd->vfd_timer_reschedule = 1;
				vfd->vfd_timer.expires = jiffies + HZ/2;
				add_timer(&vfd->vfd_timer);
				break;

			case DRIVER_MODE_TEXT:
				vfd_reset_cursor(vfd, true);
				vfd_set_text(vfd, SHUTTLE_VFD_WIDTH);
				break;
		}
	}

	return count;
}

/* attribute callback handler (style write) */
static ssize_t set_vfd_text_style_handler(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int ret;
	char cmd[7];

	struct usb_interface *intf = to_usb_interface(dev);
	struct shuttle_vfd *vfd = usb_get_intfdata(intf);

	ret = sscanf(buf, "%5s", cmd);
	if (ret != 1)
		return -EINVAL;

	if ((strcmp(cmd, "left") == 0) || (strcmp(cmd, "l") == 0)) {
		vfd->text_style = DRIVER_STYLE_TEXT_ALIGN_LEFT;
	} else if ((strcmp(cmd, "right") == 0) || (strcmp(cmd, "r") == 0)) {
		vfd->text_style = DRIVER_STYLE_TEXT_ALIGN_RIGHT;
	} else if ((strcmp(cmd, "center") == 0) || (strcmp(cmd, "c") == 0)) {
		vfd->text_style = DRIVER_STYLE_TEXT_ALIGN_CENTER;
	} else {
		return -EINVAL;
	}

	return count;
}


/* attribute callback handler (text read) */
static ssize_t get_vfd_text_handler(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	ssize_t sz = SHUTTLE_VFD_WIDTH;

	struct usb_interface *intf = to_usb_interface(dev);
	struct shuttle_vfd *vfd = usb_get_intfdata(intf);

	memcpy(buf, &vfd->screen[0], SHUTTLE_VFD_WIDTH);

	while (buf[sz-1] == '\0' || buf[sz-1] == '\n')
		sz--;
	buf[sz] = '\n';

	return sz + 1;
}

/* attribute callback handler (icons read) */
static ssize_t get_vfd_icons_handler(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	ssize_t i, sz = 0;

	struct usb_interface *intf = to_usb_interface(dev);
	struct shuttle_vfd *vfd = usb_get_intfdata(intf);

	for (i = 0; i < (sizeof(icons)/sizeof(struct vfd_icons) - 3); i++) {
		if (icons[i].value & vfd->icons_mask) {
			sz += sprintf(buf + sz, "%s ",
					icons[i].altname == NULL ? icons[i].name : icons[i].altname);
		}
	}

	if (vfd->icons_mask >= SHUTTLE_VFD_ICON_VOL_01)
		sz += sprintf(buf + sz, "vol%ld", (vfd->icons_mask >> 15) & 0xF);

	if (sz == 0)
		sz += sprintf(buf, "none");

	sz += sprintf(buf + sz, "\n");
	return sz;
}

/* attribute callback handler (mode read) */
static ssize_t get_vfd_mode_handler(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct shuttle_vfd *vfd = usb_get_intfdata(intf);

	const char *modes[] = {
		"text",
		"hw-clock",
		"sw-cpu"
	};

	return sprintf(buf, "%s\n", modes[vfd->mode]);
}

/* attribute callback handler (style read) */
static ssize_t get_vfd_text_style_handler(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct shuttle_vfd *vfd = usb_get_intfdata(intf);

	const char *modes[] = {
		"left",
		"right",
		"center"
	};

	return sprintf(buf, "%s\n", modes[vfd->text_style]);
}


static DEVICE_ATTR(text, S_IWUGO | S_IRUGO,
		get_vfd_text_handler, set_vfd_text_handler);

static DEVICE_ATTR(icons, S_IWUGO | S_IRUGO,
		get_vfd_icons_handler, set_vfd_icons_handler);

static DEVICE_ATTR(mode, S_IWUGO | S_IRUGO,
		get_vfd_mode_handler, set_vfd_mode_handler);

static DEVICE_ATTR(text_style, S_IWUGO | S_IRUGO,
		get_vfd_text_style_handler, set_vfd_text_style_handler);


static int shuttle_vfd_probe(struct usb_interface *interface,
		const struct usb_device_id *id)
{
	struct shuttle_vfd *dev = NULL;
	int retval = -ENOMEM;

	dev = kzalloc(sizeof(struct shuttle_vfd), GFP_KERNEL);
	if (dev == NULL) {
		dev_err(&interface->dev, "Out of memory\n");
		goto error_mem;
	}

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->icons_mask = 0;
	dev->mode = DRIVER_MODE_TEXT;
	dev->text_style = DRIVER_STYLE_TEXT_ALIGN_CENTER;

	usb_set_intfdata(interface, dev);

	INIT_WORK(&dev->work, vfd_periodic_work);
	mutex_init(&dev->vfd_mutex);

	/* create device attribute files */
	retval = device_create_file(&interface->dev, &dev_attr_text);
	if (retval)
		goto error;
	retval = device_create_file(&interface->dev, &dev_attr_icons);
	if (retval)
		goto error;
	retval = device_create_file(&interface->dev, &dev_attr_mode);
	if (retval)
		goto error;
	retval = device_create_file(&interface->dev, &dev_attr_text_style);
	if (retval)
		goto error;

	vfd_reset_cursor(dev, true);

	if (message[0] != '\0') {
		size_t len = strlen(message);

		if (len > SHUTTLE_VFD_WIDTH) {
			dev_dbg(&interface->dev, "initial message is too long, truncating\n");
			len = SHUTTLE_VFD_WIDTH;
		} else {
			memset(&dev->screen[0], 0x20, SHUTTLE_VFD_WIDTH);
		}

		memcpy(&dev->screen[(SHUTTLE_VFD_WIDTH-len)/2], message, len);
		vfd_set_text(dev, SHUTTLE_VFD_WIDTH);
	}

	/* setup timer (for auto-refresh features) */
	init_timer(&dev->vfd_timer);
	dev->vfd_timer.function = vfd_timer;
	dev->vfd_timer.data     = (unsigned long)dev;
	dev->vfd_timer_reschedule = 0;

	dev_info(&interface->dev, "Shuttle VFD device now attached\n");
	return 0;

error:
	device_remove_file(&interface->dev, &dev_attr_text);
	device_remove_file(&interface->dev, &dev_attr_icons);
	device_remove_file(&interface->dev, &dev_attr_mode);
	device_remove_file(&interface->dev, &dev_attr_text_style);
	usb_set_intfdata (interface, NULL);
	usb_put_dev(dev->udev);
	kfree(dev);

error_mem:
	return retval;
}

static void shuttle_vfd_disconnect(struct usb_interface *interface)
{
	struct shuttle_vfd *dev;

	dev = usb_get_intfdata(interface);

	del_timer_sync(&dev->vfd_timer);

	/* remove device attribute files */
	device_remove_file(&interface->dev, &dev_attr_text);
	device_remove_file(&interface->dev, &dev_attr_icons);
	device_remove_file(&interface->dev, &dev_attr_mode);
	device_remove_file(&interface->dev, &dev_attr_text_style);

	/* the intfdata can be set to NULL only after the
	 * device files have been removed */
	usb_set_intfdata(interface, NULL);
	usb_put_dev(dev->udev);

	kfree(dev);

	dev_info(&interface->dev, "Shuttle VFD device now disconnected\n");
}

static struct usb_driver shuttle_vfd_driver = {
	.name		= "shuttle_vfd",
	.probe		= shuttle_vfd_probe,
	.disconnect	= shuttle_vfd_disconnect,
	.id_table	= shuttle_vfd_table
};

static int __init shuttle_vfd_init(void)
{
	int err;

	err = usb_register(&shuttle_vfd_driver);
	if (err) {
		err("Function usb_register failed! Error number: %d\n", err);
	}

	return err;
}

static void __exit shuttle_vfd_exit(void)
{
	usb_deregister(&shuttle_vfd_driver);
}

module_init(shuttle_vfd_init);
module_exit(shuttle_vfd_exit);

MODULE_DESCRIPTION("Shuttle VFD driver");
MODULE_VERSION("1.04");
MODULE_AUTHOR("Matthieu Crapet <mcrapet@gmail.com>");
MODULE_LICENSE("GPL");
