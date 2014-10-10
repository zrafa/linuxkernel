/*
 *  linux/drivers/video/voyager_crtcsrfb.c -- voyager crt HWC frame buffer driver
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

static struct fb_info voyafb_info;

int voyafb_init7(void);
static int voyafb_check_var(struct fb_var_screeninfo *var,
			     struct fb_info *info);
static int voyafb_set_par(struct fb_info *info);
static int voyafb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			     u_int transp, struct fb_info *info);
static int voyafb_blank(int blank, struct fb_info *info);
static int voyafb_ioctl(struct inode*, struct file*,
			   unsigned int, unsigned long, struct fb_info*);
static int  change_mode(struct fb_var_screeninfo *var);

static	unsigned	int	pseudo_palette[16];

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
};

static struct fb_var_screeninfo voyafb_var __initdata = {
	.xres = CSR_XRES,
	.yres = CSR_YRES,
	.xres_virtual = CSR_XRES,
	.yres_virtual = CSR_YRES,
	.bits_per_pixel = CSR_BPP,
	.red = { 11,5,0 },
	.green = { 5,6,0 },
	.blue = { 0,5,0 },
	.height = -1,
	.width = -1,
	.vmode = FB_VMODE_NONINTERLACED,
	.pixclock = 10000,
	.left_margin = 16,
	.right_margin = 16,
	.upper_margin = 16,
	.lower_margin = 16,
	.hsync_len = 8,
	.vsync_len = 8,
};

static int voyafb_check_var(struct fb_var_screeninfo *var,
			     struct fb_info *info)
{
	if (var->xres > CSR_XRES || var->yres > CSR_YRES
	    || var->xres_virtual > CSR_XRES || var->yres_virtual > CSR_YRES
	    || var->bits_per_pixel != CSR_BPP
	    || var->nonstd
	    || (var->vmode & FB_VMODE_MASK) != FB_VMODE_NONINTERLACED)
		return -EINVAL;

	var->xres = var->xres_virtual = CSR_XRES;
	var->yres = var->yres_virtual = CSR_YRES;
	if(change_mode(var) != 0) {
		return -EINVAL;
	}

	return 0;
}

static int voyafb_set_par(struct fb_info *info)
{
	info->fix.line_length = CSR_XRES/2;
	info->fix.visual = FB_VISUAL_TRUECOLOR;

	info->var.red.offset = 11;
	info->var.green.offset = 5;
	info->var.blue.offset = 0;
	info->var.red.length = info->var.blue.length = 5;
	info->var.green.length = 6;
	return 0;
}

static int voyafb_blank(int blank, struct fb_info *info)
{
	return 1;
}

static int voyafb_setcolreg(unsigned regno, unsigned red, unsigned green,
			     unsigned blue, unsigned transp,
			     struct fb_info* info)
{
	red   >>= 11;
	green >>= 11;
	blue  >>= 10;

	if (regno < 16)
		((u32 *)(info->pseudo_palette))[regno] = ((red & 31) << 6) |
                                                         ((green & 31) << 11) |
                                                         ((blue & 63));
	return 0;
}

static int voyafb_ioctl(struct inode* inode, struct file* file,
			 unsigned int cmd, unsigned long arg,
			 struct fb_info* info)
{
	if(cmd == VOYAGER_IOCTL_ENABLE) {
		if(arg == 0) {
			*(volatile unsigned long *)(CRT_HWC_ADDRESS) &= 0x7fffffff;
		}
		else {
			*(volatile unsigned long *)(CRT_HWC_ADDRESS) |= 0x80000000;
		}
		return 0;
	}
	else if(cmd == VOYAGER_IOCTL_COLOR_1) {
		*(volatile unsigned long *)(CRT_HWC_COLOR_12) = arg;
		return 0;
	}
	else if(cmd == VOYAGER_IOCTL_COLOR_2) {
		*(volatile unsigned long *)(CRT_HWC_COLOR_3) = arg;
		return 0;
	}
	return -EINVAL;
}


static void __init voya_hw_init(void)
{
	*(volatile unsigned long *)(CRT_HWC_ADDRESS) = (VOY_VRAM_TOP5 & 0x00ffffff);
	*(volatile unsigned long *)(CRT_HWC_COLOR_12) = 0x5555aaaa;
	*(volatile unsigned long *)(CRT_HWC_COLOR_3) = 0x0000ffff;
	change_mode(&voyafb_var);
}



static struct fb_fix_screeninfo voyafb_fix __initdata = {
	.id =		"VOYAGER C_CSR",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.accel =	FB_ACCEL_NONE,
	.line_length =	CSR_XRES/2,
	.smem_len =	MAX_HWC_MEM_SIZE,
};


static void __init init_voya(struct fb_info *p, unsigned long addr)
{
	p->fix = voyafb_fix;
	p->fix.smem_start = addr;

	p->var = voyafb_var;

	p->fbops = &voyafb_ops;
	p->flags = FBINFO_FLAG_DEFAULT;
	p->pseudo_palette = pseudo_palette;

	fb_alloc_cmap(&p->cmap, 16, 0);

	if (register_framebuffer(p) < 0) {
		printk(KERN_ERR "VOYAGER GX CRT CSR framebuffer failed to register\n");
		return;
	}

	printk(KERN_INFO "fb%d: VOYAGER GX_CRT CSR frame buffer (%dK RAM detected)\n",
		p->node, p->fix.smem_len / 1024);

	voya_hw_init();
}

int __init voyafb_init7(void)
{
	struct fb_info *p = &voyafb_info;
	unsigned long addr, size;

        addr = VOY_VRAM_TOP6;
	size = MAX_HWC_MEM_SIZE;
	p->screen_base = ioremap((u_long)addr,
				     ALLOCATED_FB_MEM_SIZE);
	if (p->screen_base == NULL) {
		return -ENOMEM;
	}
	init_voya(p, addr);
	memset(p->screen_base, 0, MAX_HWC_MEM_SIZE);

	return 0;
}

static void __exit voyafb_exit(void)
{
	*(volatile unsigned long *)(CRT_HWC_ADDRESS) &= 0x7fffffff;
}

static	int	change_mode(struct fb_var_screeninfo *var)
{
	if((var->xoffset < 0)||(var->xoffset > XRES)||
	   (var->yoffset < 0)||(var->yoffset > YRES))
	{
		return(-1);
	}
	*(volatile unsigned long *)(CRT_HWC_LOCATION) = (var->yoffset << 16) | 
								      var->xoffset;
	return(0);
}

MODULE_LICENSE("GPL");
