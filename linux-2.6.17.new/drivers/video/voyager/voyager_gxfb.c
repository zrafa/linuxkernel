/*
 *  linux/drivers/video/voyager_gxfb.c -- voyager panel frame buffer driver
 *
 *     Copyright (C) 2003 Renesas Technology Sales Co.,Ltd.
 *     Copyright (C) 2003 Atom Create Engineering Co.,Ltd.
 *     Anthor : Atom Create Engineering Co.,Ltd.
 *                   Kenichi Sakuma
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 *
 * 1.00
 *  - initial version (ks)
 * 1.01
 *  - Kernel 2.6 correspondence
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/mach/voyagergx_reg.h>
#include <video/voyager.h>

#ifdef CONFIG_FB_VOYAGER_GX_MP
#define	MAX_VRAM	MAX_FRAMEBUFFER_MEM_SIZE
#else
#define	MAX_VRAM	1024*768*4
#endif

#ifdef CONFIG_FB_VOYAGER_GX_MP
extern int voyafb_init2(void);
extern int voyafb_init3(void);
extern int voyafb_init4(void);
extern int voyafb_init5(void);
extern int voyafb_init6(void);
extern int voyafb_init7(void);
#endif

static struct fb_info voyafb_info;

static int voyafb_init(void);
static int voyafbmem_init(void);
static int voyafb_check_var(struct fb_var_screeninfo *var,
						struct fb_info *info);
static int voyafb_set_par(struct fb_info *info);
static int voyafb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
					u_int transp, struct fb_info *info);
static int voyafb_blank(int blank, struct fb_info *info);
static int voyafb_ioctl(struct inode*, struct file*,
				unsigned int, unsigned long, struct fb_info*);
static int voyafb_pan_display(struct fb_var_screeninfo *var,
						struct fb_info *info);
static void vsyncwait(int delay);
static int change_mode(void);

typedef	struct {
	__u32	xres;
	__u32	yres;
	__u32	clock;
	__u32	h_total;
	__u32	h_sync;
	__u32	v_total;
	__u32	v_sync;
} VOYA_CRT_DATA;

static	VOYA_CRT_DATA	clock_data[] = {
	{ 640, 480, 0x10021801, 0x33f027f, 0x4a028b, 0x20c01df, 0x201e9, },
	{ 800, 600, 0x023a1801, 0x454031f, 0x9b0350, 0x2730257, 0x40258, },
	{1024, 768, 0x283a1801, 0x5460400, 0x800438, 0x3340300, 0x3031b, },
	{-1, -1, -1, -1, -1, -1, -1, },
};
static int clock_data_index = 0;
static int clock_data_bpp = BPP;

static unsigned int pseudo_palette[17];

static struct fb_ops voyafb_ops = {
	.owner		= THIS_MODULE,
	.fb_check_var	= voyafb_check_var,
	.fb_set_par	= voyafb_set_par,
	.fb_setcolreg	= voyafb_setcolreg,
	.fb_blank	= voyafb_blank,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
	.fb_ioctl       = voyafb_ioctl,
	.fb_pan_display = voyafb_pan_display,
};

static struct fb_var_screeninfo voyafb_var __initdata = {
	.xres = XRES,
	.yres = YRES,
	.xres_virtual = XRES,
	.yres_virtual = YRES,
	.bits_per_pixel = BPP,
	.red = { 11,5,0 },
	.green = { 5,6,0 },
	.blue = { 0,5,0 },
	.height = -1,
	.width = -1,
	.vmode = FB_VMODE_NONINTERLACED,
	.pixclock = 10000,
	.left_margin = 0,
	.right_margin = 0,
	.upper_margin = 0,
	.lower_margin = 0,
	.hsync_len = 0,
	.vsync_len = 0,
};

static int voyafb_pan_display(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	info->var.xoffset = 0;
	info->var.yoffset = 0;
	info->var.vmode &= ~FB_VMODE_YWRAP;

	return 0;
}

static int voyafb_check_var(struct fb_var_screeninfo *var,
			     struct fb_info *info)
{
#ifndef CONFIG_FB_VOYAGER_GX_MP
	int	i;
#endif

#ifdef CONFIG_FB_VOYAGER_GX_MP
	if (var->xres > XRES || var->yres > YRES
	    || var->xres_virtual > XRES || var->yres_virtual > YRES
	    || var->bits_per_pixel != BPP
	    || var->nonstd
	    || (var->vmode & FB_VMODE_MASK) != FB_VMODE_NONINTERLACED)
		return -EINVAL;

	var->xres = var->xres_virtual = XRES;
	var->yres = var->yres_virtual = YRES;
#else
	if ((var->bits_per_pixel != 8)  &&
	    (var->bits_per_pixel != 16) &&
	    (var->bits_per_pixel != 32))
		return -EINVAL;

	for (i=0; clock_data[i].xres != -1; i++)
		if ((clock_data[i].xres == var->xres) &&
		    (clock_data[i].yres == var->yres))
			break;

	if (clock_data[i].xres == -1)
		return -EINVAL;

	clock_data_index = i;
	clock_data_bpp = var->bits_per_pixel;
#endif

	return 0;
}

static int voyafb_set_par(struct fb_info *info)
{
#ifdef CONFIG_FB_VOYAGER_GX_MP
	info->fix.line_length = XRES * 2;
	info->fix.visual = FB_VISUAL_TRUECOLOR;

	info->var.bits_per_pixel = 16;
	info->var.red.offset = 11;
	info->var.green.offset = 5;
	info->var.blue.offset = 0;
	info->var.red.length = info->var.blue.length = 5;
	info->var.green.length = 6;
#else
	if (clock_data_bpp == 8) {
		info->fix.line_length = clock_data[clock_data_index].xres;
		info->var.transp.offset  = 0;
		info->var.transp.length  = 0;
		info->var.red.length = info->var.blue.length = info->var.green.length = 8;
		info->var.red.offset = info->var.blue.offset = info->var.green.offset = 0;
		info->var.bits_per_pixel = 8;
		info->fix.visual = FB_VISUAL_PSEUDOCOLOR;
	} else if (clock_data_bpp == 16) {
		info->fix.line_length = clock_data[clock_data_index].xres * 2;
		info->var.transp.offset  = 0;
		info->var.transp.length  = 0;
		info->var.red.length     = 5;
		info->var.blue.length    = 5;
		info->var.green.length   = 6;
		info->var.red.offset     = 11;
		info->var.green.offset   = 5;
		info->var.blue.offset    = 0;
		info->var.bits_per_pixel = 16;
		info->fix.visual = FB_VISUAL_TRUECOLOR;
	} else {
		info->fix.line_length = clock_data[clock_data_index].xres * 4;
		info->var.transp.offset  = 24;
		info->var.transp.length  = 8;
		info->var.red.length     = 8;
		info->var.blue.length    = 8;
		info->var.green.length   = 8;
		info->var.red.offset     = 16;
		info->var.green.offset   = 8;
		info->var.blue.offset    = 0;
		info->var.bits_per_pixel = 32;
		info->fix.visual = FB_VISUAL_TRUECOLOR;
	}
	change_mode();
#endif

	return 0;
}

static int voyafb_blank(int blank, struct fb_info *info)
{
	return 0;
}

static int voyafb_setcolreg(unsigned regno, unsigned red, unsigned green,
			     unsigned blue, unsigned transp,
			     struct fb_info* info)
{
	int	*palette;

	if (regno > 256)
		return 1;

	palette = (int *)PANEL_PALETTE_RAM;
	palette[regno] = (red & 0xff) << 16 | (green & 0xff) << 8 | (blue & 0xff);
	((u32 *)(info->pseudo_palette))[regno] = (red & 0xff) << 16 | (green & 0xff) << 8 | (blue & 0xff);

	return 0;
}

static int voyafb_ioctl(struct inode* inode, struct file* file,
			 unsigned int cmd, unsigned long arg,
			 struct fb_info* info)
{
	static	long	*po;
	int	*wk;

	if (cmd == VOYAGER_IOCTL_DEBUG_ADD) {
		po = (long *)arg;
		return 0;
	} else if (cmd == VOYAGER_IOCTL_DEBUG_GET) {
		wk = (int *)arg;
		*wk = *po;
		return 0;
	} else if (cmd == VOYAGER_IOCTL_DEBUG_PUT) {
		*po = arg;
		return 0;
	} else if (cmd == VOYAGER_IOCTL_ENABLE) {
		if (arg == 0)
			*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) &= 0xfffffffb;
		else
			*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) |= 0x04;
		return 0;
	} else if (cmd == VOYAGER_IOCTL_TYPE) {
		*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) &= 0xfffffffc;
		*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) |= (arg & 0x03);
		return 0;
	}

	return -EINVAL;
}

static void __init voya_hw_init(void)
{
	int	i;

	//DAC enable
	*(volatile unsigned long *)(MISC_CTRL) &= 0xffffefff;

	//Power Gate
	*(volatile unsigned long *)(POWER_MODE0_GATE) |= 0x7f;
	*(volatile unsigned long *)(POWER_MODE1_GATE) |= 0x7f;

	//Power Clock
	*(volatile unsigned long *)(POWER_MODE0_CLOCK) = 0x10021801;
	*(volatile unsigned long *)(POWER_MODE1_CLOCK) = 0x10021801;

	//Power Mode Control
	*(volatile unsigned long *)(POWER_MODE_CTRL) = 0;

	//Miscellaneous Timing
	*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) &= 0xfffffffb;
	*(volatile unsigned long *)(VIDEO_DISPLAY_CTRL) &= 0xfffffffb;
	*(volatile unsigned long *)(VIDEO_ALPHA_DISPLAY_CTRL) &= 0xfffffffb;
	*(volatile unsigned long *)(ALPHA_DISPLAY_CTRL) &= 0xfffffffb;
	*(volatile unsigned long *)(PANEL_HWC_ADDRESS) &= 0x7fffffff;
	*(volatile unsigned long *)(CRT_DISPLAY_CTRL) &= 0xfffffffb;
	*(volatile unsigned long *)(CRT_HWC_ADDRESS) &= 0x7fffffff;

	change_mode();

	vsyncwait(4);
	*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) |= 0x1000000;
	vsyncwait(4);
	*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) |= 0x3000000;
	vsyncwait(4);
	*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) |= 0x7000000;

	//palet initialize
	for (i=0; i<256; i++) {
		*(volatile unsigned long *)(PANEL_PALETTE_RAM+(i*4)) = (i << 16)+(i << 8)+i;
	}
}

static struct fb_fix_screeninfo voyafb_fix __initdata = {
	.id =		"VOYAGER PANEL",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.accel =	FB_ACCEL_NONE,
	.line_length =	XRES * (BPP / 8),
	.smem_len =	MAX_VRAM,
};


static void __init init_voya(struct fb_info *p, unsigned long addr)
{
	p->fix = voyafb_fix;
	p->fix.smem_start = addr;

	p->var = voyafb_var;

	p->fbops = &voyafb_ops;
	p->flags = FBINFO_DEFAULT;
	p->pseudo_palette = pseudo_palette;

	fb_alloc_cmap(&p->cmap, 16, 0);

	if (register_framebuffer(p) < 0) {
		printk(KERN_ERR "VOYAGER GX PANEL framebuffer failed to register\n");
		return;
	}

	printk(KERN_INFO "fb%d: VOYAGER GX_PANEL frame buffer (%dK RAM detected)\n",
		p->node, p->fix.smem_len / 1024);

	voya_hw_init();
}

static int __init voyafb_init(void)
{
	struct fb_info *p = &voyafb_info;
	unsigned long addr;

	if (fb_get_options("voyager_panel_fb", NULL))
		return -ENODEV;

        addr = VOY_VRAM_TOP0;
	p->screen_base = ioremap((u_long)addr, ALLOCATED_FB_MEM_SIZE);
	if (p->screen_base == NULL)
		return -ENOMEM;

	init_voya(p, addr);

	return 0;
}

static void __exit voyafb_exit(void)
{
	*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) &= 0xfffffffb;
}

static int __init voyafbmem_init(void)
{
	voyafb_init();
#ifdef CONFIG_FB_VOYAGER_GX_MP
	voyafb_init2();
	voyafb_init3();
	voyafb_init4();
	voyafb_init5();
	voyafb_init6();
	voyafb_init7();
#endif

	return 0;
}

module_init(voyafbmem_init);

static void vsyncwait(int delay)
{
	int	reg;

	while(delay-- > 0) {
		do {
			reg = *(volatile unsigned long *)(CMD_INTPR_STATUS);
		} while(reg & 0x1000);
		do {
			reg = *(volatile unsigned long *)(CMD_INTPR_STATUS);
		} while(!reg & 0x1000);
	}
}

static int change_mode()
{
	int	size,xres,yres;

	xres = clock_data[clock_data_index].xres;
	yres = clock_data[clock_data_index].yres;
	size = clock_data_bpp / 8;

	//Power Clock
	*(volatile unsigned long *)(POWER_MODE0_CLOCK) = clock_data[clock_data_index].clock;
	*(volatile unsigned long *)(POWER_MODE1_CLOCK) = clock_data[clock_data_index].clock;
	//PANEL register SET
	*(volatile unsigned long *)(PANEL_FB_ADDRESS) = 0x80000000 + (VOY_VRAM_TOP0 & 0x00ffffff);
	*(volatile unsigned long *)(PANEL_FB_WIDTH) = (xres * size) << 16 | (xres * size);
	*(volatile unsigned long *)(PANEL_WINDOW_WIDTH) = (xres << 16);
	*(volatile unsigned long *)(PANEL_WINDOW_HEIGHT) = (yres << 16);
	*(volatile unsigned long *)(PANEL_PLANE_TL) = 0;
	*(volatile unsigned long *)(PANEL_PLANE_BR) = ((yres-1) << 16) | (xres-1);
	*(volatile unsigned long *)(PANEL_HORIZONTAL_TOTAL) = clock_data[clock_data_index].h_total;
	*(volatile unsigned long *)(PANEL_HORIZONTAL_SYNC) = clock_data[clock_data_index].h_sync;
	*(volatile unsigned long *)(PANEL_VERTICAL_TOTAL) = clock_data[clock_data_index].v_total;
	*(volatile unsigned long *)(PANEL_VERTICAL_SYNC) = clock_data[clock_data_index].v_sync;

	*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) &= 0xffffcea8;
	if (size == 1)
		*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) |= 0x3104;
	else if (size == 2)
		*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) |= 0x3105;
	else
		*(volatile unsigned long *)(PANEL_DISPLAY_CTRL) |= 0x3106;

	return(0);
}

MODULE_LICENSE("GPL");

