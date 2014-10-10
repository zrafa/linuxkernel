/*
 * drivers/input/keyboard/hp680_keyb.c
 *
 * HP Jornada 680/690 scan keyboard
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
HP Jornada 690(Japanese version) keyboard scan matrix

	PTC7	PTC6	PTC5	PTC4	PTC3	PTC2	PTC1	PTC0
PTD1	REC			Escape	on/off	Han/Zen	Hira	Eisu
PTD5	REC			Z	on/off	Enter	:	/
PTD7	REC						Right	Down
PTE0	REC			Windows	on/off
PTE1	REC			A	on/off	]	[	;
PTE3	REC			Tab	on/off	ShirtR	\	Up
PTE6	REC			Q	on/off	BS	@	P
PTE7	REC			1	on/off	^	-	0

	PTF7	PTF6	PTF5	PTF4	PTF3	PTF2	PTF1	PTF0
PTD1	F5	F4	F6	F7	F8	F3	F2	F1
PTD5	N	B	M	,	.	V	C	X
PTD7	Muhen	Alt			Left
PTE0			Henkan	_	Del	Space		Ctrl
PTE1	H	G	J	K	L	F	D	S
PTE3							ShiftL
PTE6	Y	T	U	I	O	R	E	W
PTE7	6	5	7	8	9	4	3	2

	PTG5	PTG4	PTG3	PTG0	PTH0
*	REC	REW	FWW	Cover	on/off


		7	6	5	4	3	2	1	0
C: 0xffff 0xdf	IP	IP	IP	IP	IP	IP	IP	IP
D: 0x6786 0x59	O	I	O	IP	I	F	O	I
E: 0x5045 0x00	O	O	F	F	O	F	O	O
F: 0xffff 0xff	IP	IP	IP	IP	IP	IP	IP	IP
G: 0xaffe 0xfd	I	I	IP	IP	IP	IP	IP	I
H: 0x70f2 0x49	O	IP	F	F	IP	IP	F	I
J: 0x0704 0x22	F	F	O	IP	F	F	O	F
K: 0x0100 0x10	F	F	F	O	F	F	F	F
L: 0x0c3c 0x26	F	F	IP	F	F	IP	IP	F

****************************************************************/

static const unsigned char hp680_japanese_table[] = {
	/* PTD1 */
	0x3a, 0x70, 0x29, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x3b, 0x3c, 0x3d, 0x42, 0x41, 0x40, 0x3e, 0x3f,
	/* PTD5 */
	0x35, 0x28, 0x1c, 0x00, 0x2c, 0x00, 0x00, 0x00,
	0x2d, 0x2e, 0x2f, 0x34, 0x33, 0x32, 0x30, 0x31,
	/* PTD7 */
	0x50, 0x4d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x4b, 0x00, 0x00, 0x38, 0x7b,
	/* PTE0 */
	0x00, 0x00, 0x00, 0x00, 0xdb, 0x00, 0x00, 0x00,
	0x1d, 0x00, 0x39, 0x53, 0x73, 0xf9, 0x00, 0x00,
	/* PTE1 */
	0x27, 0x1b, 0x2b, 0x00, 0x1e, 0x00, 0x00, 0x00,
	0x1f, 0x20, 0x21, 0x26, 0x25, 0x24, 0x22, 0x23,
	/* PTE3 */
	0x48, 0x7d, 0x36, 0x00, 0x0f, 0x00, 0x00, 0x00,
	0x00, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* PTE6 */
	0x19, 0x1a, 0x0e, 0x00, 0x10, 0x00, 0x00, 0x00,
	0x11, 0x12, 0x13, 0x18, 0x17, 0x16, 0x14, 0x15,
	/* PTE7 */
	0x0b, 0x0c, 0x0d, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x03, 0x04, 0x05, 0x0a, 0x09, 0x08, 0x06, 0x07,
	/* **** */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static int hp680_japanese_scan_kbd(unsigned char *s)
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
		*s++=ctrl_inb(PCDR);
		*s++=ctrl_inb(PFDR);
	}

	ctrl_outb(0xff, PDDR);
	ctrl_outb(0xff, PEDR);

	*s++=ctrl_inb(PGDR);
	*s++=ctrl_inb(PHDR);

	return 0;
}

static struct scan_keyboard hp680_kbd = {
	.scan		= hp680_japanese_scan_kbd,
	.table		= hp680_japanese_table,
	.length		= 18,
};

static int __init hp680_kbd_init_hw(void)
{
	printk(KERN_INFO "HP680 matrix scan keyboard registered\n");
	return register_scan_keyboard(&hp680_kbd);
}

static void __exit hp680_kbd_exit_hw(void)
{
	unregister_scan_keyboard(&hp680_kbd);
}

module_init(hp680_kbd_init_hw);
module_exit(hp680_kbd_exit_hw);
MODULE_LICENSE("GPL");
