/**
 * The keyboard and touchpad controller on the MacBook8,1, MacBook9,1 and
 * MacBookPro12,1 can be driven either by USB or SPI. However the USB pins
 * are only connected on the MacBookPro12,1, all others need this driver.
 * The interface is selected using ACPI methods:
 *
 * * UIEN ("USB Interface Enable"): If invoked with argument 1, disables SPI
 *   and enables USB. If invoked with argument 0, disables USB.
 * * UIST ("USB Interface Status"): Returns 1 if USB is enabled, 0 otherwise.
 * * SIEN ("SPI Interface Enable"): If invoked with argument 1, disables USB
 *   and enables SPI. If invoked with argument 0, disables SPI.
 * * SIST ("SPI Interface Status"): Returns 1 if SPI is enabled, 0 otherwise.
 * * ISOL: Resets the four GPIO pins used for SPI. Intended to be invoked with
 *   argument 1, then once more with argument 0.
 *
 * UIEN and UIST are only provided on the MacBookPro12,1.
 */

#define pr_fmt(fmt) "applespi: " fmt

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/property.h>
#include <linux/delay.h>
#include <linux/dmi.h>

#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input-polldev.h>

#define READ_WRITE_SPEED 8000000
#define APPLESPI_PACKET_SIZE    256

#define PACKET_KEYBOARD         288
#define PACKET_TOUCHPAD         544
#define PACKET_NOTHING          53312

#define MAX_ROLLOVER 		6

#define MAX_FINGERS		6
#define MAX_FINGER_ORIENTATION	16384
//#define DEBUG_UNKNOWN_PACKET 1
//#define DEBUG_ALL_READ 1

struct keyboard_protocol {
	u16		packet_type;
	u8		unknown1[9];
	u8		counter;
	u8		unknown2[5];
	u8		modifiers;
	u8		unknown3;
	u8		keys_pressed[6];
	u8		fn_pressed;
	u16		crc_16;
	u8		unused[228];
};

/* trackpad finger structure, le16-aligned */
struct tp_finger {
	__le16 origin;          /* zero when switching track finger */
	__le16 abs_x;           /* absolute x coodinate */
	__le16 abs_y;           /* absolute y coodinate */
	__le16 rel_x;           /* relative x coodinate */
	__le16 rel_y;           /* relative y coodinate */
	__le16 tool_major;      /* tool area, major axis */
	__le16 tool_minor;      /* tool area, minor axis */
	__le16 orientation;     /* 16384 when point, else 15 bit angle */
	__le16 touch_major;     /* touch area, major axis */
	__le16 touch_minor;     /* touch area, minor axis */
	__le16 unused[2];       /* zeros */
	__le16 pressure;        /* pressure on forcetouch touchpad */
	__le16 multi;           /* one finger: varies, more fingers: constant */
	__le16 padding;
} __attribute__((packed,aligned(2)));

struct touchpad_protocol {
	u16			packet_type;
	u8			unknown1[4];
	u8			number_of_fingers;
	u8			unknown2[4];
	u8			counter;
	u8			unknown3[2];
	u8			number_of_fingers2;
	u8			unknown[2];
	u8			clicked;
	u8			rel_x;
	u8			rel_y;
	u8			unknown4[50];
	struct tp_finger	fingers[MAX_FINGERS];
	u8			unknown5[208];
};
// 20 01 00 00 00 00 14 00 10 01 00 09 00 00 08 00 02 01 xdata ydata
//                                                    ^
struct applespi_tp_info {
	int	x_min;
	int	x_max;
	int	y_min;
	int	y_max;
};

struct applespi_data {
	struct spi_device		*spi;
	struct input_dev		*keyboard_input_dev;
	struct input_dev		*touchpad_input_dev;

	u8				*tx_buffer;
	u8				*rx_buffer;

	const struct applespi_tp_info	*tp_info;

	u8				last_keys_pressed[MAX_ROLLOVER];
	u8				last_keys_fn_pressed[MAX_ROLLOVER];
	u8				last_fn_pressed;
	struct input_mt_pos		pos[MAX_FINGERS];
	int				slots[MAX_FINGERS];
	acpi_handle			handle;
	int				gpe;
	acpi_handle			sien;
	acpi_handle			sist;

	struct spi_transfer		t;
	struct spi_message		m;
};

static const unsigned char applespi_scancodes[] = {
	0,  0,  0,  0,
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
	KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
	KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS,
	KEY_EQUAL, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, 0,
	KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_DOT, KEY_SLASH,
	KEY_CAPSLOCK,
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
	KEY_F10, KEY_F11, KEY_F12, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
};

static const unsigned char applespi_controlcodes[] = {
	KEY_LEFTCTRL,
	KEY_LEFTSHIFT,
	KEY_LEFTALT,
	KEY_LEFTMETA,
	0,
	KEY_RIGHTSHIFT,
	KEY_RIGHTALT,
	KEY_RIGHTMETA
};

struct applespi_key_translation {
	u8  from;
	u16 to;
};

static const struct applespi_key_translation applespi_fn_codes[] = {
	{ 42,  KEY_DELETE },
	{ 40,  KEY_INSERT },
	{ 58,  KEY_BRIGHTNESSDOWN },
	{ 59,  KEY_BRIGHTNESSUP },
	{ 60,  KEY_SCALE },
	{ 61,  KEY_DASHBOARD },
	{ 62,  KEY_KBDILLUMDOWN },
	{ 63,  KEY_KBDILLUMUP },
	{ 64,  KEY_PREVIOUSSONG },
	{ 65,  KEY_PLAYPAUSE },
	{ 66,  KEY_NEXTSONG },
	{ 67,  KEY_MUTE },
	{ 68,  KEY_VOLUMEDOWN },
	{ 69,  KEY_VOLUMEUP },
	{ 82,  KEY_PAGEUP },
	{ 81,  KEY_PAGEDOWN },
	{ 80,  KEY_HOME },
	{ 79,  KEY_END },
};

static struct applespi_tp_info applespi_macbookpro131_info = { -6243, 6749, -1085, 6770 };
static struct applespi_tp_info applespi_macbookpro133_info = { -7456, 7976, -163, 9283 };
// MacBook11, MacBook12
static struct applespi_tp_info applespi_default_info = { -4828, 5345, -203, 6803 };

static const struct dmi_system_id applespi_touchpad_infos[] = {
	{
		.ident = "Apple MacBookPro13,1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,1")
		},
		.driver_data = &applespi_macbookpro131_info,
	},
	{
		.ident = "Apple MacBookPro13,2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,2")
		},
		.driver_data = &applespi_macbookpro131_info,	// same touchpad
	},
	{
		.ident = "Apple MacBookPro13,3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,3")
		},
		.driver_data = &applespi_macbookpro133_info,
	},
	{
		.ident = "Apple Generic MacBook(Pro)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
		},
		.driver_data = &applespi_default_info,
	},
};

u8 *applespi_init_commands[] = {
	"\x40\x02\x00\x00\x00\x00\x0C\x00\x52\x02\x00\x00\x02\x00\x02\x00\x02\x01\x7B\x11\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x23\xAB",
};

static ssize_t
applespi_sync(struct applespi_data *applespi, struct spi_message *message) {
	struct spi_device *spi;
	int status;

	spi = applespi->spi;

	status = spi_sync(spi, message);

	if (status == 0)
		status = message->actual_length;

	return status;
}

static inline ssize_t
applespi_sync_write_and_response(struct applespi_data *applespi) {
	/*
	The Windows driver always seems to do a 256 byte write, followed
	by a 4 byte read with CS still the same, followed by a toggling of
	CS and a 256 byte read for the real response.

	For some reason, weird things happen at the proper speed (8MHz) but
	everything seems to be OK at 400kHz
	*/
	struct spi_transfer t1 = {
		.tx_buf			= applespi->tx_buffer,
		.len			= APPLESPI_PACKET_SIZE,
		.cs_change		= 1,
		.speed_hz		= READ_WRITE_SPEED
	};

	struct spi_transfer t2 = {
		.rx_buf			= applespi->rx_buffer,
		.len			= 4,
		.cs_change		= 1,
		.speed_hz		= READ_WRITE_SPEED
	};

	struct spi_transfer t3 = {
		.rx_buf			= applespi->rx_buffer,
		.len			= APPLESPI_PACKET_SIZE,
		.speed_hz		= READ_WRITE_SPEED
	};
	struct spi_message      m;

	spi_message_init(&m);
	spi_message_add_tail(&t1, &m);
	spi_message_add_tail(&t2, &m);
	spi_message_add_tail(&t3, &m);
	return applespi_sync(applespi, &m);
}

static inline ssize_t
applespi_sync_read(struct applespi_data *applespi)
{
	struct spi_transfer t = {
		.rx_buf			= applespi->rx_buffer,
		.len			= APPLESPI_PACKET_SIZE,
		.speed_hz		= READ_WRITE_SPEED
	};
	struct spi_message      m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return applespi_sync(applespi, &m);
}

static int applespi_enable_spi(struct applespi_data *applespi)
{
	int result;
	long long unsigned int spi_status;

	/* Check if SPI is already enabled, so we can skip the delay below */
	result = acpi_evaluate_integer(applespi->sist, NULL, NULL, &spi_status);
	if (ACPI_SUCCESS(result) && spi_status)
		return 0;

	/* SIEN(1) will enable SPI communication */
	result = acpi_execute_simple_method(applespi->sien, NULL, 1);
	if (ACPI_FAILURE(result)) {
		pr_err("SIEN failed: %s", acpi_format_exception(result));
		return -ENODEV;
	}

	/*
	 * Allow the SPI interface to come up before returning. Without this
	 * delay, the SPI commands to enable multitouch mode may not reach
	 * the trackpad controller, causing pointer movement to break upon
	 * resume from sleep.
	 */
	msleep(1000);

	return 0;
}

static void applespi_init(struct applespi_data *applespi) {
	int i;
	ssize_t items = ARRAY_SIZE(applespi_init_commands);

	// Do a read to flush the trackpad
	applespi_sync_read(applespi);

	for (i = 0; i < items; i++) {
		memcpy(applespi->tx_buffer, &applespi_init_commands[i], 256);
		applespi_sync_write_and_response(applespi);
	}

	pr_info("modeswitch done.");
}

/* Lifted from the BCM5974 driver */
/* convert 16-bit little endian to signed integer */
static inline int raw2int(__le16 x) {
	return (signed short)le16_to_cpu(x);
}

static void report_finger_data(struct input_dev *input, int slot,
			       const struct input_mt_pos *pos,
			       const struct tp_finger *f) {
	input_mt_slot(input, slot);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, true);

	input_report_abs(input, ABS_MT_TOUCH_MAJOR,
			 raw2int(f->touch_major) << 1);
	input_report_abs(input, ABS_MT_TOUCH_MINOR,
			 raw2int(f->touch_minor) << 1);
	input_report_abs(input, ABS_MT_WIDTH_MAJOR,
			 raw2int(f->tool_major) << 1);
	input_report_abs(input, ABS_MT_WIDTH_MINOR,
			 raw2int(f->tool_minor) << 1);
	input_report_abs(input, ABS_MT_ORIENTATION,
			 MAX_FINGER_ORIENTATION - raw2int(f->orientation));
	input_report_abs(input, ABS_MT_POSITION_X, pos->x);
	input_report_abs(input, ABS_MT_POSITION_Y, pos->y);
}

static int report_tp_state(struct applespi_data *applespi, struct touchpad_protocol* t)
{
	const struct tp_finger *f;
	struct input_dev *input = applespi->touchpad_input_dev;
	const struct applespi_tp_info *tp_info = applespi->tp_info;
	int i, n;

	n = 0;

//	for (i = 0; i < MAX_FINGERS; i++) {
//		f = &t->fingers[i];
//		if (raw2int(f->touch_major) == 0)
//			continue;
//		applespi->pos[n].x = raw2int(f->abs_x);
//		applespi->pos[n].y = tp_info->y_min + tp_info->y_max - raw2int(f->abs_y);
//		n++;
//	}

//	printk("%hhd:%hhd\n", (int)t->rel_x, (int)t->rel_y);

//	input_mt_assign_slots(input, applespi->slots, applespi->pos, 1, 0);
//
//	for (i = 0; i < n; i++)
//		report_finger_data(input, applespi->slots[i],
//				   &applespi->pos[i], &t->fingers[i]);
//
///	input_mt_sync_frame(input);
	
	int rx = (signed char)t->rel_x;
	int yx = (signed char)t->rel_y;
	input_report_rel(input, REL_X, rx);
	input_report_rel(input, REL_Y, yx);
	input_report_key(input, BTN_LEFT, t->clicked);

	input_sync(input);
	return 0;
}

static unsigned int applespi_code_to_key(u8 code, int fn_pressed) {
	int i;

	if (fn_pressed) {
		for (i = 0; i < ARRAY_SIZE(applespi_fn_codes); i++) {
			if (applespi_fn_codes[i].from == code) {
				return applespi_fn_codes[i].to;
			}
		}
	}

	return applespi_scancodes[code];
}

static void applespi_got_data(struct applespi_data *applespi) {
	struct keyboard_protocol keyboard_protocol;
	int i, j;
	unsigned int key;
	bool still_pressed;

	//printk("in got data...\n");
	memcpy(&keyboard_protocol, applespi->rx_buffer, APPLESPI_PACKET_SIZE);
	if (keyboard_protocol.packet_type == PACKET_NOTHING) {
		printk("NOTHING.\n");
		return;
	}
	else if (keyboard_protocol.packet_type == PACKET_KEYBOARD) {
		for (i=0; i < 6; i++) {
			still_pressed = false;
			for (j = 0; j < 6; j++) {
				if (applespi->last_keys_pressed[i] == keyboard_protocol.keys_pressed[j]) {
					still_pressed = true;
					break;
				}
			}

			if (! still_pressed) {
				key = applespi_code_to_key(applespi->last_keys_pressed[i], applespi->last_keys_fn_pressed[i]);
				input_report_key(applespi->keyboard_input_dev, key, 0);
				applespi->last_keys_fn_pressed[i] = 0;
			}
		}

		for (i=0; i<6; i++) {
			if (keyboard_protocol.keys_pressed[i] < ARRAY_SIZE(applespi_scancodes) && keyboard_protocol.keys_pressed[i] > 0) {
				key = applespi_code_to_key(keyboard_protocol.keys_pressed[i], keyboard_protocol.fn_pressed);
				input_report_key(applespi->keyboard_input_dev, key, 1);
				applespi->last_keys_fn_pressed[i] = keyboard_protocol.fn_pressed;
			}
		}

		// Check control keys
		for (i = 0; i < 8; i++) {
			if (test_bit(i, (long unsigned int *)&keyboard_protocol.modifiers)) {
				input_report_key(applespi->keyboard_input_dev, applespi_controlcodes[i], 1);
			}
			else {
				input_report_key(applespi->keyboard_input_dev, applespi_controlcodes[i], 0);
			}
		}

		// Check function key
		if (keyboard_protocol.fn_pressed && !applespi->last_fn_pressed) {
			input_report_key(applespi->keyboard_input_dev, KEY_FN, 1);
		}

		else if (!keyboard_protocol.fn_pressed && applespi->last_fn_pressed) {
			input_report_key(applespi->keyboard_input_dev, KEY_FN, 0);
		}

		applespi->last_fn_pressed = keyboard_protocol.fn_pressed;

		input_sync(applespi->keyboard_input_dev);
		memcpy(&applespi->last_keys_pressed, keyboard_protocol.keys_pressed, sizeof(applespi->last_keys_pressed));
	}
	
	else if (keyboard_protocol.packet_type == PACKET_TOUCHPAD) {

		report_tp_state(applespi, (struct touchpad_protocol*)&keyboard_protocol);
	}
//#ifdef DEBUG_UNKNOWN_PACKET
	else {
		pr_info("UNKNOWN --- %d", keyboard_protocol.packet_type);
		print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, &keyboard_protocol, APPLESPI_PACKET_SIZE, false);
	}
//#endif

	//printk("end got data..\n");
}

static void applespi_async_read_complete(void *context) {
	struct applespi_data *applespi = context;
	applespi_got_data(applespi);
#ifdef DEBUG_ALL_READ
	print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, applespi->rx_buffer, 256, false);
#endif
	acpi_finish_gpe(NULL, applespi->gpe);
}

static void applespi_async_init(struct applespi_data *applespi) {
	memset(&applespi->t, 0, sizeof applespi->t);

	applespi->t.rx_buf = applespi->rx_buffer;
	applespi->t.len = APPLESPI_PACKET_SIZE;
	applespi->t.speed_hz = READ_WRITE_SPEED;;

	spi_message_init(&applespi->m);
	applespi->m.complete = applespi_async_read_complete;
	applespi->m.context = applespi;

	spi_message_add_tail(&applespi->t, &applespi->m);
}

static u32 applespi_notify(acpi_handle gpe_device, u32 gpe, void *context) {
	struct applespi_data *applespi = context;

	/* Can this be reused? */
	applespi_async_init(applespi);

	spi_async(applespi->spi, &applespi->m);
	return ACPI_INTERRUPT_HANDLED;
}

static int applespi_probe(struct spi_device *spi) {
	struct applespi_data *applespi;
	int result, i;
	long long unsigned int gpe, usb_status;

	/* Allocate driver data */
	applespi = devm_kzalloc(&spi->dev, sizeof(*applespi), GFP_KERNEL);
	if (!applespi)
		return -ENOMEM;

	applespi->spi = spi;

	/* Create our buffers */
	applespi->tx_buffer = devm_kmalloc(&spi->dev, APPLESPI_PACKET_SIZE, GFP_KERNEL);
	applespi->rx_buffer = devm_kmalloc(&spi->dev, APPLESPI_PACKET_SIZE, GFP_KERNEL);

	if (!applespi->tx_buffer || !applespi->rx_buffer)
		return -ENOMEM;

	/* Store the driver data */
	spi_set_drvdata(spi, applespi);

	/* Set up touchpad dimensions */
	applespi->tp_info = dmi_first_match(applespi_touchpad_infos)->driver_data;

	/* Setup the keyboard input dev */
	applespi->keyboard_input_dev = devm_input_allocate_device(&spi->dev);

	if (!applespi->keyboard_input_dev)
		return -ENOMEM;

	applespi->keyboard_input_dev->name = "Apple SPI Keyboard";
	applespi->keyboard_input_dev->phys = "applespi/input0";
	applespi->keyboard_input_dev->dev.parent = &spi->dev;
	applespi->keyboard_input_dev->id.bustype = BUS_SPI;

	applespi->keyboard_input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_LED) | BIT_MASK(EV_REL);
	applespi->keyboard_input_dev->ledbit[0] = BIT_MASK(LED_CAPSL);

	for (i = 0; i<ARRAY_SIZE(applespi_scancodes); i++)
		if (applespi_scancodes[i])
			input_set_capability(applespi->keyboard_input_dev, EV_KEY, applespi_scancodes[i]);

	for (i = 0; i<ARRAY_SIZE(applespi_controlcodes); i++)
		if (applespi_controlcodes[i])
			input_set_capability(applespi->keyboard_input_dev, EV_KEY, applespi_controlcodes[i]);

	for (i = 0; i<ARRAY_SIZE(applespi_fn_codes); i++)
		if (applespi_fn_codes[i].to)
			input_set_capability(applespi->keyboard_input_dev, EV_KEY, applespi_fn_codes[i].to);

	input_set_capability(applespi->keyboard_input_dev, EV_KEY, KEY_FN);

	result = input_register_device(applespi->keyboard_input_dev);
	if (result) {
		pr_err("Unabled to register keyboard input device (%d)", result);
		return -ENODEV;
	}

	/* Now, set up the touchpad as a seperate input device */
	applespi->touchpad_input_dev = devm_input_allocate_device(&spi->dev);

	if (!applespi->touchpad_input_dev)
		return -ENOMEM;

	applespi->touchpad_input_dev->name = "Apple SPI Touchpad";
	applespi->touchpad_input_dev->phys = "applespi/input1";
	applespi->touchpad_input_dev->dev.parent = &spi->dev;
	applespi->touchpad_input_dev->id.bustype = BUS_SPI;

	applespi->touchpad_input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);

	__set_bit(EV_KEY, applespi->touchpad_input_dev->evbit);
	__set_bit(EV_REL, applespi->touchpad_input_dev->evbit);

	__set_bit(BTN_LEFT, applespi->touchpad_input_dev->keybit);

	__set_bit(INPUT_PROP_POINTER, applespi->touchpad_input_dev->propbit);
	//__set_bit(INPUT_PROP_BUTTONPAD, applespi->touchpad_input_dev->propbit);

	/* finger touch area */
//	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_TOUCH_MAJOR, 0, 2048, 0, 0);
//	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_TOUCH_MINOR, 0, 2048, 0, 0);
//
//	/* finger approach area */
//	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_WIDTH_MAJOR, 0, 2048, 0, 0);
//	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_WIDTH_MINOR, 0, 2048, 0, 0);
//
//	/* finger orientation */
//	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_ORIENTATION, -MAX_FINGER_ORIENTATION, MAX_FINGER_ORIENTATION, 0, 0);
//
//	/* finger position */
//	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_POSITION_X, applespi->tp_info->x_min, applespi->tp_info->x_max, 0, 0);
//	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_POSITION_Y, applespi->tp_info->y_min, applespi->tp_info->y_max, 0, 0);
//
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_TOOL_FINGER);
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_TOUCH);
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_LEFT);
	input_set_capability(applespi->touchpad_input_dev, EV_REL, REL_X);
	input_set_capability(applespi->touchpad_input_dev, EV_REL, REL_Y);
	

	//input_mt_init_slots(applespi->touchpad_input_dev, MAX_FINGERS,
	//	INPUT_MT_POINTER | INPUT_MT_DROP_UNUSED | INPUT_MT_TRACK);

	result = input_register_device(applespi->touchpad_input_dev);
	if (result) {
		pr_err("Unabled to register touchpad input device (%d)", result);
		return -ENODEV;
	}

	applespi->handle = ACPI_HANDLE(&spi->dev);

	/* Check if the USB interface is present and enabled already */
	result = acpi_evaluate_integer(applespi->handle, "UIST", NULL, &usb_status);
	if (ACPI_SUCCESS(result) && usb_status) {
		/* Let the USB driver take over instead */
		pr_info("USB interface already enabled");
		return -ENODEV;
	}

	/* Cache ACPI method handles */
	if (ACPI_FAILURE(acpi_get_handle(applespi->handle, "SIEN", &applespi->sien)) ||
	    ACPI_FAILURE(acpi_get_handle(applespi->handle, "SIST", &applespi->sist))) {
		pr_err("Failed to get required ACPI method handle");
		return -ENODEV;
	}

	/* Switch on the SPI interface */
	result = applespi_enable_spi(applespi);
	if (result)
		return result;

	/* Switch the touchpad into multitouch mode */
	applespi_init(applespi);

	/*
	 * The applespi device doesn't send interrupts normally (as is described
	 * in its DSDT), but rather seems to use ACPI GPEs.
	 */
	result = acpi_evaluate_integer(applespi->handle, "_GPE", NULL, &gpe);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to obtain GPE for SPI slave device: %s", acpi_format_exception(result));
		return -ENODEV;
	}
	applespi->gpe = (int)gpe;

	result = acpi_install_gpe_handler(NULL, applespi->gpe, ACPI_GPE_LEVEL_TRIGGERED, applespi_notify, applespi);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to install GPE handler for GPE %d: %s", applespi->gpe, acpi_format_exception(result));
		return -ENODEV;
	}

	result = acpi_enable_gpe(NULL, applespi->gpe);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to enable GPE handler for GPE %d: %s", applespi->gpe, acpi_format_exception(result));
		acpi_remove_gpe_handler(NULL, applespi->gpe, applespi_notify);
		return -ENODEV;
	}

	pr_info("module probe done.");

	return 0;
}

static int applespi_remove(struct spi_device *spi)
{
	struct applespi_data *applespi = spi_get_drvdata(spi);

	acpi_disable_gpe(NULL, applespi->gpe);
	acpi_remove_gpe_handler(NULL, applespi->gpe, applespi_notify);

	pr_info("module remove done.");
	return 0;
}

#ifdef CONFIG_PM
static int applespi_suspend(struct device *dev)
{
	pr_info("device suspend done.");
	return 0;
}

static int applespi_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct applespi_data *applespi = spi_get_drvdata(spi);

	/* Switch on the SPI interface */
	applespi_enable_spi(applespi);

	/* Switch the touchpad into multitouch mode */
	applespi_init(applespi);

	pr_info("device resume done.");

	return 0;
}
#endif

static const struct acpi_device_id applespi_acpi_match[] = {
	{ "APP000D", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, applespi_acpi_match);

static UNIVERSAL_DEV_PM_OPS(applespi_pm_ops, applespi_suspend,
                            applespi_resume, NULL);

static struct spi_driver applespi_driver = {
	.driver		= {
		.name			= "applespi",
		.owner			= THIS_MODULE,

		.acpi_match_table	= ACPI_PTR(applespi_acpi_match),
		.pm			= &applespi_pm_ops,
	},
	.probe		= applespi_probe,
	.remove		= applespi_remove,
};
module_spi_driver(applespi_driver)

MODULE_LICENSE("GPL");
