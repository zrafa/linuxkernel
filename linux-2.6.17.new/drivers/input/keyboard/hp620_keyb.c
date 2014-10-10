/*
 * drivers/input/keyboard/hp680_keyb.c
 *
 * HP Jornada 620 scan keyboard
 *
 *  Copyright (C) 2005  Andriy Skulysh
 *  Copyright (C) 2006  Paul Mundt
 *
 * Splited from drivers/input/keyboard/hp600_keyb.c
 *
 *	Copyright (C) 2000 YAEGASHI Takeshi
 *	HP600 keyboard scan routine and translation table
 *	Copyright (C) 2000 Niibe Yutaka
 *	HP620 keyboard translation table
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <asm/delay.h>
#include <asm/io.h>
#include "scan_keyb.h"

#define PCDR 0xa4000124
#define PDDR 0xa4000126
#define PEDR 0xa4000128
#define PFDR 0xa400012a
#define PGDR 0xa400012c
#define PHDR 0xa400012e
#define PJDR 0xa4000130
#define PKDR 0xa4000132
#define PLDR 0xa4000134

/****************************************************************
HP Jornada 620(Japanese version) keyboard scan matrix

	PTC7	PTC6	PTC5	PTC4	PTC3	PTC2	PTC1	PTC0
PTD1	EREC		BS	Ctrl	on/off	-	0	9
PTD5	EREC		BS	Ctrl	on/off	^	P	O
PTD7	EREC		BS	Ctrl	on/off	]	@	L
PTE0	EREC		BS	Ctrl	on/off	Han/Zen [	;
PTE1	EREC		BS	Ctrl	on/off	Enter	:	/
PTE3	EREC		BS	Ctrl	on/off		Right	Up
PTE6	EREC		BS	Ctrl	on/off		Down	Left
PTE7	EREC		BS	Ctrl	on/off		F8	F7

	PTF7	PTF6	PTF5	PTF4	PTF3	PTF2	PTF1	PTF0
PTD1	8	7	6	5	4	3	2	1
PTD5	I	U	Y	T	R	E	W	Q
PTD7	K	J	H	G	F	D	S	A
PTE0						ESC	Tab	Shift
PTE1	.			V			Caps	Hira
PTE3	,	M	N	B	Muhen	C	X
PTE6	_	\	Henkan	Space		Alt	Z
PTE7	F6	F5	F4	F3	F2	F1		REC

	PTH0
*	on/off

		7	6	5	4	3	2	1	0
C: 0xffff 0xff	IP	IP	IP	IP	IP	IP	IP	IP
D: 0x4404 0xaf	O	F	O	F	F	F	O	F
E: 0x5045 0xff	O	O	F	F	O	F	O	O
F: 0xffff 0xff	IP	IP	IP	IP	IP	IP	IP	IP
G: 0xd5ff 0x00	IP	O	O	O	IP	IP	IP	IP
H: 0x63ff 0xd1	O	I	F	IP	IP	IP	IP	IP
J: 0x0004 0x02	F	F	F	F	F	F	O	F
K: 0x0401 0xff	F	F	O	F	F	F	F	O
L: 0x0c00 0x20	F	F	IP	F	F	F	F	F

ADCSR: 0x08
ADCR: 0x3f

 ****************************************************************/

/****************************************************************
Japanese 109 keyboard scan code layout

                                              E02A-     E1-
01    3B 3C 3D 3E  3F 40 41 42  43 44 57 58   E037  46  1045

29 02 03 04 05 06 07 08 09 0A 0B 0C 0D 7D 0E  E052 E047 E049   45 E035 37  4A
0F  10 11 12 13 14 15 16 17 18 19 1A 1B   1C  E053 E04F E051   47  48  49  4E
3A   1E 1F 20 21 22 23 24 25 26 27 28 2B                       4B  4C  4D
2A    2C 2D 2E 2F 30 31 32 33 34 35 73    36       E048        4F  50  51  E0-
1D  DB  38  7B   39   79 70  E038 DC DD E01D  E04B E050 E04D     52    53  1C

****************************************************************/

static const unsigned char hp620_japanese_table[] = {
	/* PTD1 */
	0x0a, 0x0b, 0x0c, 0x00, 0x00, 0x0e, 0x00, 0x00,
	0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
	/* PTD5 */
	0x18, 0x19, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	/* PTD7 */
	0x26, 0x1a, 0x2b, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
	/* PTE0 */
	0x27, 0x1b, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x2a, 0x0f, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* PTE1 */
	0x35, 0x28, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x70, 0x3a, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x34,
	/* PTE3 */
	0x48, 0x4d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x2d, 0x2e, 0x7b, 0x30, 0x31, 0x32, 0x33,
	/* PTE6 */
	0x4b, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x2c, 0x38, 0x00, 0x39, 0x79, 0x7d, 0x73,
	/* PTE7 */
	0x41, 0x42, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
	/* **** */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static int hp620_japanese_scan_kbd(unsigned char *s)
{
	int i;
	unsigned char matrix_switch[] = {
		0xfd, 0xff,	/* PTD1 */
		0xdf, 0xff,	/* PTD5 */
		0x7f, 0xff,	/* PTD7 */
		0xff, 0xfe,	/* PTE0 */
		0xff, 0xfd,	/* PTE1 */
		0xff, 0xf7,	/* PTE3 */
		0xff, 0xbf,	/* PTE6 */
		0xff, 0x7f,	/* PTE7 */
	}, *t=matrix_switch;

	for(i=0; i<8; i++) {
		ctrl_outb(*t++, PDDR);
		ctrl_outb(*t++, PEDR);
		udelay(50);
		*s++=ctrl_inb(PCDR);
		*s++=ctrl_inb(PFDR);
	}

	ctrl_outb(0xff, PDDR);
	ctrl_outb(0xff, PEDR);

	*s++=ctrl_inb(PGDR);
	*s++=ctrl_inb(PHDR);

	return 0;
}

static struct scan_keyboard hp620_kbd = {
	.scan		= hp620_japanese_scan_kbd,
	.table		= hp620_japanese_table,
	.length		= 18,
};

static int __init hp620_kbd_init_hw(void)
{
	printk(KERN_INFO "HP620 matrix scan keyboard registered\n");
	return register_scan_keyboard(&hp620_kbd);
}

static void __exit hp620_kbd_exit_hw(void)
{
	unregister_scan_keyboard(&hp620_kbd);
}

module_init(hp620_kbd_init_hw);
module_exit(hp620_kbd_exit_hw);
MODULE_LICENSE("GPL");
