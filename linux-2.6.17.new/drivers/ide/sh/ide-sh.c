/*
 * SuperH default IDE host driver
 *
 * Copyright (C) 2004, 2005  Paul Mundt
 *
 * Based on the old include/asm-sh/ide.h
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ide.h>

#include <asm/irq.h>

enum {
	IDE_SH_CFCARD_IO = 0x1f0,
	IDE_SH_PCMCIA_IO = 0x170,
};

static struct sh_ide_hwif {
	unsigned long base;
	int irq;
} sh_ide_hwifs[] __initdata = {
	{ IDE_SH_CFCARD_IO, IRQ_CFCARD },
	{ IDE_SH_PCMCIA_IO, IRQ_PCMCIA },
	{ 0, },
};

static inline int __init hw_setup(hw_regs_t *hw, int idx)
{
	unsigned long base = sh_ide_hwifs[idx].base;

	if (!request_region(base, 8, "ide-sh"))
		return -EBUSY;
	if (!request_region(base + 0x206, 1, "ide-sh")) {
		release_region(base, 8);
		return -EBUSY;
	}

	memset(hw, 0, sizeof(hw_regs_t));
	ide_std_init_ports(hw, base, base + 0x206);

	hw->irq		= sh_ide_hwifs[idx].irq;
	hw->chipset	= ide_generic;

	return 0;
}

static inline void __init hwif_setup(ide_hwif_t *hwif)
{
	hwif->mmio = 2;
}

void __init ide_sh_init(void)
{
	int i, idx;

	printk(KERN_INFO "ide: SuperH generic IDE interface\n");

	for (i = 0; i < MAX_HWIFS; i++) {
		ide_hwif_t *hwif;
		hw_regs_t hw;

		if (!sh_ide_hwifs[i].base) {
			printk(KERN_ERR "ide-sh: Attempting to register ide%d "
			       "when only %d interfaces are available.\n",
			       i, i - 1);
			break;
		}

		if (hw_setup(&hw, i) < 0)
			goto region_cleanup;

		idx = ide_register_hw(&hw, &hwif);
		if (idx == -1) {
			printk(KERN_ERR "ide-sh: IDE interface registration failed\n");
			i++;	/* release this interface too */
			goto region_cleanup;
		}

		hwif_setup(hwif);
	}

region_cleanup:
	for (idx = 0; idx < i; idx++) {
		unsigned long base = sh_ide_hwifs[idx].base;

		release_region(base + 0x206, 1);
		release_region(base, 8);
	}
}

