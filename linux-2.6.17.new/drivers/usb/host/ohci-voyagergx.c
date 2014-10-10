/*
 * SM501 USB HCD for Linux Version.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Copyright 2003 (c) Lineo uSolutions,Inc.
 * Copyright 2004 (c) Paul Mundt
 */

#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <asm/mach/voyagergx_reg.h>

extern int usb_disabled(void);

static void usb_hcd_voyager_remove(struct usb_hcd *hcd, struct platform_device *pdev);
static int __devinit ohci_voyager_start(struct usb_hcd *hcd);
static int __init voyagergx_ohci_init(void);
static void __exit voyagergx_ohci_exit(void);

/*
 * VoyagerGX USB, when not used as a PCI device.
 */
#define VOYAGER_OHCI_NAME	"voyager-ohci"

static void __init voyagergx_ohci_configure(void)
{
	unsigned long val;

        // Power Mode 0 Gate
        val = inl(POWER_MODE0_GATE);
        outl((val | POWER_MODE0_GATE_UH), POWER_MODE0_GATE);

	val = inl(POWER_MODE1_GATE);
        outl((val | POWER_MODE1_GATE_UH), POWER_MODE1_GATE);

        //Miscellaneous USB Clock Selsct
        val = inl(MISC_CTRL);
	val &= ~MISC_CTRL_USBCLK_48;
        outl(val, MISC_CTRL);

        // Interrupt Mask
        val = inl(VOYAGER_INT_MASK);
        val |= 0x00000040;
        outl(val, VOYAGER_INT_MASK);
}

static int usb_hcd_voyager_probe(const struct hc_driver *driver,
				 struct platform_device *dev)
{
	struct usb_hcd *hcd;
	struct ohci_hcd *ohci;
	struct resource *res;
	int retval, irq;

	res = platform_get_resource(dev, IORESOURCE_MEM, 0);
	if (!res) {
		err("no IO resources defined");
		return -ENODEV;
	}
	irq = platform_get_irq(dev, 0);

	hcd = usb_create_hcd(driver, &dev->dev, "sm501");
	if (PTR_ERR(hcd) == 0) {
		err("usb_create_hcd failed");
		return -ENOMEM;
	}

	hcd->rsrc_start = res->start;
	hcd->rsrc_len = res->end - res->start + 1;
	hcd->regs = ioremap(hcd->rsrc_start, hcd->rsrc_len);
	if (PTR_ERR(hcd->regs) == 0) {
		err("ioremap failed");
		retval = -ENOMEM;
		goto err1;
	}

	ohci = hcd_to_ohci(hcd);
	ohci_hcd_init(ohci);

	retval = hcd_buffer_create(hcd);
	if (retval != 0) {
		err("hcd_buffer_create failed, %d", retval);
		goto err2;
	}

	retval = usb_add_hcd(hcd, irq, SA_INTERRUPT);
	if (!retval)
		return retval;	/* all done */

	/* error path */
	hcd_buffer_destroy(hcd);
err2:
	iounmap(hcd->regs);
err1:
	usb_put_hcd(hcd);

	return retval;
}

static void usb_hcd_voyager_remove(struct usb_hcd *hcd, struct platform_device *dev)
{
	hcd_buffer_destroy(hcd);
	usb_remove_hcd(hcd);
	iounmap(hcd->regs);
	usb_put_hcd(hcd);
}

static int __devinit ohci_voyager_start(struct usb_hcd *hcd)
{
	struct ohci_hcd *ohci = hcd_to_ohci(hcd);
	int ret;

	if ((ret = ohci_init(ohci)) < 0)
		return ret;

	if ((ret = ohci_run(ohci)) < 0) {
		err("can't start %s", hcd->self.bus_name);
		ohci_stop(hcd);
		return ret;
	}

	return 0;
}

static const struct hc_driver voyager_hc_driver = {
	.description		= hcd_name,
	.product_desc		= "SM501 OHCI",
	.hcd_priv_size		= sizeof(struct ohci_hcd),

	/*
	 * generic hardware linkage
	 */
	.irq			= ohci_irq,
	.flags			= HCD_USB11,

	/*
	 * basic lifecycle operations
	 */
	.start			= ohci_voyager_start,
	.stop			= ohci_stop,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue		= ohci_urb_enqueue,
	.urb_dequeue		= ohci_urb_dequeue,
	.endpoint_disable	= ohci_endpoint_disable,

	/*
	 * scheduling support
	 */
	.get_frame_number	= ohci_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data	= ohci_hub_status_data,
	.hub_control		= ohci_hub_control,
#ifdef	CONFIG_PM
	.bus_suspend 		= ohci_bus_suspend,
	.bus_resume 		= ohci_bus_resume,
#endif
	.start_port_reset	= ohci_start_port_reset,
};

static int usb_hcd_voyager_drv_probe(struct platform_device *pdev)
{
	if (usb_disabled())
		return -ENODEV;

	return usb_hcd_voyager_probe(&voyager_hc_driver, pdev);
}

static int usb_hcd_voyager_drv_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);

	usb_hcd_voyager_remove(hcd, pdev);

	return 0;
}

/*
 * Driver definitions to register with SH Bus
 */
static struct platform_driver usb_hcd_voyager_driver = {
	.driver	= {
		.owner	= THIS_MODULE,
		.name	= VOYAGER_OHCI_NAME,
	},
	.probe		= usb_hcd_voyager_drv_probe,
	.remove		= usb_hcd_voyager_drv_remove,
};

static struct resource voyager_hcd_res[] = {
	[0] = {
		.start	= VOYAGER_USBH_BASE,
		.end	= VOYAGER_USBH_BASE + 0xfff,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= VOYAGER_USBH_IRQ,
		.end	= VOYAGER_USBH_IRQ,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device usb_hcd_voyager_dev = {
	.name		= VOYAGER_OHCI_NAME,
	.id		= 0,
	.resource	= voyager_hcd_res,
	.num_resources	= ARRAY_SIZE(voyager_hcd_res),
};

static int __init voyagergx_ohci_init(void)
{
	int ret;

	if (usb_disabled())
		return -ENODEV;

	voyagergx_ohci_configure();

	ret = platform_driver_register(&usb_hcd_voyager_driver);
	if (ret)
		return -ENODEV;

	ret = platform_device_register(&usb_hcd_voyager_dev);
	if (ret) {
		platform_driver_unregister(&usb_hcd_voyager_driver);
		return -ENODEV;
	}

	return ret;
}

static void __exit voyagergx_ohci_exit(void)
{
	platform_device_unregister(&usb_hcd_voyager_dev);
	platform_driver_unregister(&usb_hcd_voyager_driver);
}

module_init(voyagergx_ohci_init);
module_exit(voyagergx_ohci_exit);
