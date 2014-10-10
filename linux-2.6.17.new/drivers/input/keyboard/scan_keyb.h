#ifndef	__DRIVER_CHAR_SCAN_KEYB_H
#define	__DRIVER_CHAR_SCAN_KEYB_H

struct scan_keyboard {
	struct scan_keyboard *next;
	int (*scan)(unsigned char *buffer);
	const unsigned char *table;
	unsigned char *s0, *s1;
	int length;
	struct input_dev *dev;
};

int register_scan_keyboard(struct scan_keyboard *);
void unregister_scan_keyboard(struct scan_keyboard *);

#endif
