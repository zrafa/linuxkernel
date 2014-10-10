/* -------------------------------------------------------------------- */
/* rts7751r2d-flash.c:                                                     */
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
*/
/* -------------------------------------------------------------------- */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>
#include <linux/mtd/partitions.h>

#include <asm/io.h>

#undef FLASH4M_16BIT
#define FLASH16M_16BIT

#if defined(FLASH4M_16BIT)
#define RTS7751R2D_FLASH_SIZE 0x00100000
#else
#define RTS7751R2D_FLASH_SIZE 0x01000000
#endif

static struct map_info rts7751r2d_map = {
	.name		= "SH-Graphic flash",
	.bankwidth	= 2,
	.size		= RTS7751R2D_FLASH_SIZE,
};

/*
 * Here are partition information for all known SH-Graphic based devices.
 * See include/linux/mtd/partitions.h for definition of the mtd_partition
 * structure.
 *
 * The *_max_flash_size is the maximum possible mapped flash size which
 * is not necessarily the actual flash size.  It must correspond to the
 * value specified in the mapping definition defined by the
 * "struct map_desc *_io_desc" for the corresponding machine.
 */

#if defined(FLASH4M_16BIT)
static struct mtd_partition rts7751r2d_partitions[] = {
	{
		.name		= "bootloader",
		.size		= 0x00080000,
		.offset		= 0xa0000000,
		.mask_flags	= MTD_WRITEABLE, /* force read-only */
	},{
		.name		= "SH-Graphic jffs",
		.size		= 0x00080000,
		.offset		= 0xa0080000,
	}
};
#else
static struct mtd_partition rts7751r2d_partitions[] = {
	{
		.name		= "bootloader",
		.size		= 0x00020000,
		.offset		= 0x00000000,
		.mask_flags	= MTD_WRITEABLE, /* force read-only */
	},{
		.name		= "mtdblock1",
		.size		= 0x00300000,
		.offset		= 0x00020000,
	},{
		.name		= "mtdblock2",
		.size		= 0x004e0000,
		.offset		= 0x00320000,
	},{
		.name		= "mtdblock3",
		.size		= 0x00800000,
		.offset		= 0x00800000,
	}
};
#endif

static struct mtd_partition *parsed_parts;
static struct mtd_info *mymtd;

int __init rts7751r2d_mtd_init(void)
{
	struct mtd_partition *parts;
	int nb_parts = 0;
	int parsed_nr_parts = 0;
	char *part_type;

	/* Default flash buswidth */

	/*
	 * Static partition definition selection
	 */
	part_type = "static";
	parts = rts7751r2d_partitions;

	nb_parts = ARRAY_SIZE(rts7751r2d_partitions);
	rts7751r2d_map.phys = 0;
	rts7751r2d_map.virt = P2SEGADDR(0);

	/*
	 * Now let's probe for the actual flash.  Do it here since
	 * specific machine settings might have been set above.
	 */
	printk(KERN_NOTICE "RTS7751R2D flash: probing %d-bit flash bus\n",
			rts7751r2d_map.bankwidth*8);
	simple_map_init(&rts7751r2d_map);

	mymtd = do_map_probe("cfi_probe", &rts7751r2d_map);
	if (!mymtd)
		return -ENXIO;

	mymtd->owner = THIS_MODULE;
	mymtd->erasesize = 0x10000;

	/*
	 * Dynamic partition selection stuff (might override the static ones)
	 */

	if (parsed_nr_parts > 0) {
		parts = parsed_parts;
		nb_parts = parsed_nr_parts;
	}

	if (nb_parts == 0) {
		printk(KERN_NOTICE "RTS7751R2D partition info available, registering whole flash at once\n");
		add_mtd_device(mymtd);
	} else {
		printk(KERN_NOTICE "Using %s partition definition\n", part_type);
		add_mtd_partitions(mymtd, parts, nb_parts);
	}
	return 0;
}

static void __exit rts7751r2d_mtd_cleanup(void)
{
	if (mymtd) {
		del_mtd_partitions(mymtd);
		map_destroy(mymtd);
		if (parsed_parts)
			kfree(parsed_parts);
	}
}

module_init(rts7751r2d_mtd_init);
module_exit(rts7751r2d_mtd_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lineo uSolutions,Inc.");
MODULE_DESCRIPTION("MTD map driver for RTS7751R2D base board");
