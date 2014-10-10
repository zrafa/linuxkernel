/*
 * Generic scan keyboard driver
 *
 * Copyright (C) 2000 YAEGASHI Takeshi
 * Copyright (C) 2003 Andriy Skulysh
 * Copyright (C) 2006 Paul Mundt
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/kbd_kern.h>
#include <linux/input.h>
#include <linux/timer.h>
#include "scan_keyb.h"

#define SCANHZ	(HZ/20)

static int scan_jiffies;
static struct scan_keyboard *keyboards;
struct timer_list scan_timer;
static char *hpkbd_name = "Hitachi scankeyboard";
static char *hpkbd_phys = "input0";

static void check_kbd(struct scan_keyboard *kbd,
		      unsigned char *new, unsigned char *old)
{
	const unsigned char *table = kbd->table;
	int length = kbd->length;
	int need_tasklet_schedule = 0;
	unsigned int xor, bit;

	while (length-- > 0) {
		if ((xor = *new ^ *old) == 0)
			table += 8;
		else {
			for (bit = 0x01; bit < 0x100; bit <<= 1) {
				if (xor & bit) {
					input_report_key(kbd->dev, *table,
							 !(*new & bit));
					need_tasklet_schedule = 1;
				}

				table++;
			}
		}

		new++;
		old++;
	}

	if (need_tasklet_schedule) {
		input_sync(kbd->dev);
		tasklet_schedule(&keyboard_tasklet);
	}
}

static void scan_kbd(unsigned long dummy)
{
	struct scan_keyboard *kbd;

	scan_jiffies++;

	for (kbd = keyboards; kbd != NULL; kbd = kbd->next) {
		if (scan_jiffies & 1) {
			if (!kbd->scan(kbd->s0))
				check_kbd(kbd, kbd->s0, kbd->s1);
			else
				memcpy(kbd->s0, kbd->s1, kbd->length);
		} else {
			if (!kbd->scan(kbd->s1))
				check_kbd(kbd, kbd->s1, kbd->s0);
			else
				memcpy(kbd->s1, kbd->s0, kbd->length);
		}
	}

	mod_timer(&scan_timer, jiffies + SCANHZ);
}

int register_scan_keyboard(struct scan_keyboard *kbd)
{
	int i;

	kbd->s0 = kmalloc(kbd->length, GFP_KERNEL);
	if (kbd->s0 == NULL)
		goto error;

	kbd->s1 = kmalloc(kbd->length, GFP_KERNEL);
	if (kbd->s1 == NULL)
		goto error;

	memset(kbd->s0, -1, kbd->length);
	memset(kbd->s1, -1, kbd->length);

	kbd->dev = input_allocate_device();
	if (!kbd->dev)
		goto error;

	kbd->dev->name = hpkbd_name;
	kbd->dev->phys = hpkbd_phys;
	kbd->dev->evbit[0] = BIT(EV_KEY) | BIT(EV_REP);
	init_input_dev(kbd->dev);
	kbd->dev->keycode = (unsigned char *)kbd->table;
	kbd->dev->keycodesize = sizeof(unsigned char);
	kbd->dev->keycodemax = ARRAY_SIZE(kbd->table);

	for (i = 0; i < 128; i++)
		if (kbd->table[i])
			set_bit(kbd->table[i], kbd->dev->keybit);

	clear_bit(0, kbd->dev->keybit);
	input_register_device(kbd->dev);

	kbd->next = keyboards;
	keyboards = kbd;

	init_timer(&scan_timer);
	scan_timer.expires = jiffies + SCANHZ;
	scan_timer.data = 0;
	scan_timer.function = scan_kbd;
	add_timer(&scan_timer);

	return 0;

error:
	kfree(kbd->s1);
	kfree(kbd->s0);

	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(register_scan_keyboard);

void unregister_scan_keyboard(struct scan_keyboard *kbd)
{
	del_timer_sync(&scan_timer);
	keyboards = kbd->next;
	input_unregister_device(kbd->dev);
	input_free_device(kbd->dev);
}
EXPORT_SYMBOL_GPL(unregister_scan_keyboard);
