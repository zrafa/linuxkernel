/* -------------------------------------------------------------------- */
/* i2c-voyagergx.c:                                                     */
/* -------------------------------------------------------------------- */
/*  This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    Copyright 2003 (c) Lineo uSolutions,Inc.
    Copyright 2004 (c) Paul Mundt
*/
/* -------------------------------------------------------------------- */

#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <asm/irq.h>
#include <asm/io.h>

#include <linux/i2c.h>
#include "../algos/i2c-algo-voyager.h"

//#define I2C_USE_INTR

#if defined(I2C_USE_INTR)
#define DEFAULT_IRQ   10
#else
#define DEFAULT_IRQ   0
#endif

struct iic_voyagergx {
	unsigned long	iic_base;
	int 		iic_irq;
};
static struct iic_voyagergx gpi;

static wait_queue_head_t iic_wait;
static int iic_pending;

static void iic_voyagergx_setiic(void *data, int ctl, int val)
{
	outw(val,ctl);
}

static int iic_voyagergx_getiic(void *data, int ctl)
{
	return inw(ctl);
}

/* Put this process to sleep.  We will wake up when the
 * IIC controller interrupts.
 */
static void iic_voyagergx_waitforpin(void)
{
	int timeout = 2;

	/* If interrupts are enabled (which they are), then put the process to
	 * sleep.  This process will be awakened by two events -- either the
	 * the IIC peripheral interrupts or the timeout expires. 
	 * If interrupts are not enabled then delay for a reasonable amount 
	 * of time and return.
	 */
	if (gpi.iic_irq > 0) {
		local_irq_disable();

		if (iic_pending == 0) {
			interruptible_sleep_on_timeout(&iic_wait, timeout*HZ);
		} else {
			iic_pending = 0;
		}

		local_irq_enable();
	} else {
		udelay(100);
	}
}

#if defined(I2C_USE_INTR)
static irqreturn_t iic_voyagergx_handler(int this_irq, void *dev_id,
					 struct pt_regs *regs) 
{
	iic_pending = 1;

	pr_debug("iic_voyagergx_handler: in interrupt handler\n");
	wake_up_interruptible(&iic_wait);

	return IRQ_HANDLED;
}
#endif

/* Lock the region of memory where I/O registers exist.  Request our
 * interrupt line and register its associated handler.
 */
static int iic_hw_resrc_init(void)
{
	unsigned char		ctl;
	unsigned long		val;

	// Power Mode Gate
	val = inl(POWER_MODE0_GATE);
	val |= 0x00000040;
	outl(val, POWER_MODE0_GATE);
	val = inl(POWER_MODE1_GATE);
	val |= 0x00000040;
	outl(val, POWER_MODE1_GATE);

	// GPIO Control
	val = inl( GPIO_MUX_HIGH);
	val |= 0x0000c000;
	outl(val, GPIO_MUX_HIGH);

#if defined(I2C_USE_INTR)
	// Interrupt Mask
	val = inl(VOYAGER_INT_MASK);
	val |= 0x00800000;
	outl(val, VOYAGER_INT_MASK);
#endif

        // Enable I2c controller and select mode to high
        ctl = inb(I2C_CONTROL);
        outb((ctl | I2C_CONTROL_E | I2C_CONTROL_MODE), I2C_CONTROL);

#if defined(I2C_USE_INTR)
	if (gpi.iic_irq > 0) {
		if (request_irq(gpi.iic_irq, iic_voyagergx_handler, 0,
				"VoyagerGX IIC", 0) < 0) {
			gpi.iic_irq = 0;
		} else {
			pr_debug("Enabled IIC IRQ %d\n", gpi.iic_irq);
		}

		enable_irq(gpi.iic_irq);
	}
#endif

	return 0;
}

static struct i2c_algo_iic_data iic_voyagergx_data = {
	.setiic		= iic_voyagergx_setiic,
	.getiic		= iic_voyagergx_getiic,
	.waitforpin	= iic_voyagergx_waitforpin,
	.udelay		= 80,
	.mdelay		= 80,
	.timeout	= 100,
};

static struct i2c_adapter iic_voyagergx_ops = {
	.owner		= THIS_MODULE,
	.name		= "VoyagerGX I2C",
	.id		= I2C_HW_SMBUS_VOYAGER,
	.class		= I2C_ADAP_CLASS_SMBUS,
	.algo_data	= &iic_voyagergx_data,
};

static int __init iic_voyagergx_init(void) 
{
	struct iic_voyagergx *piic = &gpi;

	printk(KERN_INFO "Initialize VoyagerGX I2C module\n");

	piic->iic_base	= VOYAGER_BASE;
	piic->iic_irq	= DEFAULT_IRQ;

	iic_voyagergx_data.data = (void *)piic;
	init_waitqueue_head(&iic_wait);

	if (iic_hw_resrc_init() == 0) {
		if (i2c_voyager_add_bus(&iic_voyagergx_ops) < 0)
			return -ENODEV;
	} else {
		return -ENODEV;
	}

#if defined(I2C_USE_INTR)
	printk(KERN_INFO " found device at %#lx irq %d.\n", 
		piic->iic_base, piic->iic_irq);
#else
	printk(KERN_INFO " found device at %#lx\n", piic->iic_base);
#endif

	return 0;
}


static void iic_voyagergx_exit(void)
{
	if (gpi.iic_irq > 0) {
		disable_irq(gpi.iic_irq);
		free_irq(gpi.iic_irq, 0);
	}

	release_region(gpi.iic_base, 2);
}

MODULE_AUTHOR("Lineo uSolutions,Inc. <www.lineo.co.jp>");
MODULE_DESCRIPTION("I2C-Bus adapter for VoyagerGX Silicon Motion, Inc.");
MODULE_LICENSE("GPL");

module_init(iic_voyagergx_init);
module_exit(iic_voyagergx_exit); 

