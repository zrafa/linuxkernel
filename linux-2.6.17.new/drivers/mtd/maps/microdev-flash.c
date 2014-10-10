/*
 * drivers/mtd/maps/microdev-flash.c
 *
 * Flash Mapping for the SuperH SH4-202 MicroDev.
 *
 *  Copyright (C) 2004  SuperH, Inc.
 *  Copyright (C) 2004  Paul Mundt
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>
#include <linux/mtd/partitions.h>
#include <asm/io.h>

/*
 * The SH4-202 MicroDev has 32M of Intel StrataFlash mapped into the
 * beginning of the FEMI address space (mapped to P0). MicroDev flash
 * consists of 2x16M chips, mapped contiguously, and accessed with a
 * 32-bit buswidth.
 *
 * Additionally, the MicroDev also supports booting from an EEPROM.
 * In the event that the EEPROM is configured, it is mapped in the
 * same address space window as the StrataFlash. As such, we can only
 * use one at a time.
 *
 * Since the EEPROM is only accessible with an 8-bit buswidth, flash
 * is preferrable as far as performance is concerned.
 */
struct map_info microdev_flash_map = {
	.name		= "MicroDev Flash",
	.size		= 0x02000000,
	.bankwidth	= 4,
};

struct map_info microdev_eeprom_map = {
	.name		= "MicroDev EEPROM",
	.size		= 0x00100000,
	.bankwidth	= 1,
};

static const char *probes[] = { "RedBoot", "cmdlinepart", NULL };
static struct mtd_partition *parsed_parts;

/*
 * Default partition map.
 */
static struct mtd_partition microdev_partitions[] = {
#ifdef CONFIG_MTD_PARTITIONS
	{
		.name		= "bootloader",
		.size		= 0x00080000,
		.mask_flags	= MTD_WRITEABLE, /* force read-only */
	}, {
		.name		= "User FS",
		.offset		= MTDPART_OFS_APPEND,
		.size		= MTDPART_SIZ_FULL,
	},
#endif
};

static struct mtd_info *microdev_flash;
static struct mtd_info *microdev_eeprom;
static struct mtd_info *microdev_mtd;

static void show_map_info(struct map_info *info)
{
	unsigned long addr;

	if (!info)
		return;

	addr = info->phys & 0x1fffffff;

	printk(KERN_NOTICE "%s at 0x%08lx:0x%08lx, using a %d-bit bankwidth.\n",
	       info->name, addr, addr + info->size, info->bankwidth << 3);
}

static int __init microdev_map_init(void)
{
	struct mtd_partition *parts;
	int nr_parts, parsed_nr_parts;

	/* Flash mapped at FEMI area 0 */
	microdev_flash_map.phys  = 0;
	microdev_flash_map.virt  = (void __iomem *)P2SEGADDR(0);

	/* EEPROM mapped in the same place */
	microdev_eeprom_map.phys = 0;
	microdev_eeprom_map.virt = (void __iomem *)P2SEGADDR(0);

	simple_map_init(&microdev_flash_map);
	simple_map_init(&microdev_eeprom_map);

	/* Try the flash first */
	printk(KERN_NOTICE "MicroDev flash: probing for flash chips at 0x00000000:\n");
	microdev_flash = do_map_probe("cfi_probe", &microdev_flash_map);
	if (!microdev_flash) {
		printk(KERN_NOTICE "Flash chips not detected, probing for EEPROM\n");

		/* No luck, try EEPROM */
		microdev_eeprom = do_map_probe("map_rom", &microdev_eeprom_map);
		if (!microdev_eeprom) {
			/* Nope.. */
			printk(KERN_ERR "nothing found\n");
			return -ENXIO;
		}
	}

	if (microdev_flash) {
		microdev_mtd = microdev_flash;
		show_map_info(&microdev_flash_map);
	} else {
		microdev_mtd = microdev_eeprom;
		show_map_info(&microdev_eeprom_map);
	}

	microdev_mtd->owner = THIS_MODULE;

	/* Start out with a static map.. */
	parts = microdev_partitions;
	nr_parts = ARRAY_SIZE(microdev_partitions);

#ifdef CONFIG_MTD_PARTITIONS
	/* Try to parse the partitions */
	parsed_nr_parts = parse_mtd_partitions(microdev_mtd, probes, &parsed_parts, 0);
	if (parsed_nr_parts > 0) {
		parts = parsed_parts;
		nr_parts = parsed_nr_parts;
	}
#endif

	if (nr_parts > 0) {
		add_mtd_partitions(microdev_mtd, parts, nr_parts);
	} else {
		add_mtd_device(microdev_mtd);
	}

	return 0;
}

static void __exit microdev_map_exit(void)
{
	if (parsed_parts) {
		del_mtd_partitions(microdev_mtd);
	} else {
		del_mtd_device(microdev_mtd);
	}

	map_destroy(microdev_mtd);
}

module_init(microdev_map_init);
module_exit(microdev_map_exit);

MODULE_AUTHOR("Paul Mundt <lethal@linux-sh.org>");
MODULE_DESCRIPTION("MTD map driver for SuperH SH4-202 MicroDev");
MODULE_LICENSE("GPL");

