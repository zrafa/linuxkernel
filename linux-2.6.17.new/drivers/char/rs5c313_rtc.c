/*
 * RICHO RS5C313 Real Time Clock interface for Linux 
 *
 *  2005-09-19 modifed by kogiidena  
 *
 *  Copyright (C) 2000 Philipp Rumpf <prumpf@tux.org>
 *  Copyright (C) 1999 Tetsuya Okada & Niibe Yutaka
 *
 * Based on code written by Paul Gortmaker.
 *  Copyright (C) 1996 Paul Gortmaker
 *	
 *  This driver allows use of the real time clock (built into
 *  nearly all computers) from user space. It exports the /dev/rtc
 *  interface supporting various ioctl() and also the
 *  /proc/driver/rtc pseudo-file for status information.
 *
 *  The ioctls can be used to set the interrupt behaviour and
 *  generation rate from the RTC via IRQ 8. Then the /dev/rtc
 *  interface can be used to make use of these timer interrupts,
 *  be they interval or alarm based.
 *
 *  The /dev/rtc interface will block on reads until an interrupt
 *  has been received. If a RTC interrupt has already happened,
 *  it will output an unsigned long and then block. The output value
 *  contains the interrupt status in the low byte and the number of
 *  interrupts since the last read in the remaining high bytes. The 
 *  /dev/rtc interface can also be used with the select(2) call.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 *  Based on other minimal char device drivers, like Alan's
 *  watchdog, Ted's random, etc. etc.
 *
 *	1.07	Paul Gortmaker.
 *	1.08	Miquel van Smoorenburg: disallow certain things on the
 *		DEC Alpha as the CMOS clock is also used for other things.
 *	1.09	Nikita Schmidt: epoch support and some Alpha cleanup.
 *	1.09a	Pete Zaitcev: Sun SPARC
 *	1.09b	Jeff Garzik: Modularize, init cleanup
 *	1.09c	Jeff Garzik: SMP cleanup
 *	1.10    Paul Barton-Davis: add support for async I/O
 *	1.10a	Andrea Arcangeli: Alpha updates
 *	1.10b	Andrew Morton: SMP lock fix
 *	1.10c	Cesar Barros: SMP locking fixes and cleanup
 *	1.10d	Paul Gortmaker: delete paranoia check in rtc_exit
 *	1.10e	Maciej W. Rozycki: Handle DECstation's year weirdness.
 *      1.11    Takashi Iwai: Kernel access functions
 *			      rtc_register/rtc_unregister/rtc_control
 *      1.11a   Daniele Bellucci: Audit create_proc_read_entry in rtc_init
 *	1.12	Venkatesh Pallipadi: Hooks for emulating rtc on HPET base-timer
 *		CONFIG_HPET_EMULATE_RTC
 *
 */

#define RTC_VERSION		"1.12"

#include <linux/config.h>
#include <linux/delay.h>
#include <linux/bcd.h> 
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>
#include <linux/mc146818rtc.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/sysctl.h>
#include <linux/wait.h>
#include <asm/current.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/io.h>


#ifndef CONFIG_SH_LANDISK
#error  rs5c313 read and write function not defined
#endif

#ifdef CONFIG_SH_LANDISK
/*****************************************************/
/* LANDISK dependence part of RS5C313                */
/*****************************************************/

#define SCSMR1	0xFFE00000
#define SCSCR1	0xFFE00008
#define SCSMR1_CA	0x80
#define SCSCR1_CKE	0x03
#define SCSPTR1	0xFFE0001C
#define SCSPTR1_EIO	0x80
#define SCSPTR1_SPB1IO	0x08
#define SCSPTR1_SPB1DT	0x04
#define SCSPTR1_SPB0IO	0x02
#define SCSPTR1_SPB0DT	0x01

#define SDA_OEN	SCSPTR1_SPB1IO
#define SDA	SCSPTR1_SPB1DT
#define SCL_OEN	SCSPTR1_SPB0IO
#define SCL	SCSPTR1_SPB0DT

/* RICOH RS5C313 CE port */
#define RS5C313_CE		0xB0000003

/* RICOH RS5C313 CE port bit */
#define RS5C313_CE_RTCCE	0x02

/* SCSPTR1 data */
unsigned char scsptr1_data;

#define RS5C313_CEENABLE    ctrl_outb(RS5C313_CE_RTCCE, RS5C313_CE);
#define RS5C313_CEDISABLE   ctrl_outb(0x00, RS5C313_CE)
#define RS5C313_MISCOP      ctrl_outb(0x02, 0xB0000008)

static void rs5c313_initialize(void)
{
	/* Set SCK as I/O port and Initialize SCSPTR1 data & I/O port. */
	ctrl_outb(ctrl_inb(SCSMR1) & ~SCSMR1_CA, SCSMR1);
	ctrl_outb(ctrl_inb(SCSCR1) & ~SCSCR1_CKE, SCSCR1);

	/* And Initialize SCL for RS5C313 clock */
	scsptr1_data = ctrl_inb(SCSPTR1) | SCL;	/* SCL:H */
	ctrl_outb(scsptr1_data, SCSPTR1);
	scsptr1_data = ctrl_inb(SCSPTR1) | SCL_OEN;	/* SCL output enable */
	ctrl_outb(scsptr1_data, SCSPTR1);
	RS5C313_CEDISABLE;	/* CE:L */
}

static void rs5c313_write(unsigned char data)
{
	int i;

	for (i = 0; i < 8; i++) {
		/* SDA:Write Data */
		scsptr1_data = (scsptr1_data & ~SDA) 
		             | ((((0x80 >> i) & data) >> (7 - i)) << 2);  
		ctrl_outb(scsptr1_data, SCSPTR1);
		if (i == 0) {
			scsptr1_data |= SDA_OEN;	/* SDA:output enable */
			ctrl_outb(scsptr1_data, SCSPTR1);
		}
		ndelay(700);
		scsptr1_data &= ~SCL;	/* SCL:L */
		ctrl_outb(scsptr1_data, SCSPTR1);
		ndelay(700);
		scsptr1_data |= SCL;	/* SCL:H */
		ctrl_outb(scsptr1_data, SCSPTR1);
	}

	scsptr1_data &= ~SDA_OEN;	/* SDA:output disable */
	ctrl_outb(scsptr1_data, SCSPTR1);

}

static unsigned char rs5c313_read_data(void)
{
	int i;
	unsigned char data = 0;

	for (i = 0; i < 8; i++) {
		ndelay(700);
		/* SDA:Read Data */
		data |= ((ctrl_inb(SCSPTR1) & SDA) >> 2) << (7 - i);
		scsptr1_data &= ~SCL;	/* SCL:L */
		ctrl_outb(scsptr1_data, SCSPTR1);
		ndelay(700);
		scsptr1_data |= SCL;	/* SCL:H */
		ctrl_outb(scsptr1_data, SCSPTR1);
	}
	return data & 0x0F;
}

#endif  /* CONFIG_SH_LANDISK */


/*****************************************************/
/* machine independence part of RS5C313              */
/*****************************************************/

/* RICOH RS5C313 address */
#define RS5C313_ADDR_SEC	0x00
#define RS5C313_ADDR_SEC10	0x01
#define RS5C313_ADDR_MIN	0x02
#define RS5C313_ADDR_MIN10	0x03
#define RS5C313_ADDR_HOUR	0x04
#define RS5C313_ADDR_HOUR10	0x05
#define RS5C313_ADDR_WEEK	0x06
#define RS5C313_ADDR_INTINTVREG	0x07
#define RS5C313_ADDR_DAY	0x08
#define RS5C313_ADDR_DAY10	0x09
#define RS5C313_ADDR_MON	0x0A
#define RS5C313_ADDR_MON10	0x0B
#define RS5C313_ADDR_YEAR	0x0C
#define RS5C313_ADDR_YEAR10	0x0D
#define RS5C313_ADDR_CNTREG	0x0E
#define RS5C313_ADDR_TESTREG	0x0F

/* RICOH RS5C313 control register */
#define RS5C313_CNTREG_ADJ_BSY	0x01
#define RS5C313_CNTREG_WTEN_XSTP	0x02
#define RS5C313_CNTREG_12_24	0x04
#define RS5C313_CNTREG_CTFG	0x08

/* RICOH RS5C313 test register */
#define RS5C313_TESTREG_TEST	0x01

/* RICOH RS5C313 control bit */
#define RS5C313_CNTBIT_READ	0x40
#define RS5C313_CNTBIT_AD	0x20
#define RS5C313_CNTBIT_DT	0x10

static unsigned char rs5c313_read_reg(unsigned char addr)
{

	rs5c313_write(addr | RS5C313_CNTBIT_READ | RS5C313_CNTBIT_AD);
	return rs5c313_read_data();

}

static void rs5c313_write_reg(unsigned char addr, unsigned char data)
{
        data &= 0x0f;
	rs5c313_write(addr | RS5C313_CNTBIT_AD);
	rs5c313_write(data | RS5C313_CNTBIT_DT);
	return;
}

#define rs5c313_read_cntreg()       rs5c313_read_reg(RS5C313_ADDR_CNTREG)
#define rs5c313_write_cntreg(data)  rs5c313_write_reg(RS5C313_ADDR_CNTREG,data)
#define rs5c313_write_intintvreg(data) rs5c313_write_reg(RS5C313_ADDR_INTINTVREG,data)


static void rs5c313_get_cur_time(unsigned char *sec, unsigned char *min,
				 unsigned char *hr,
				 unsigned char *day, unsigned char *mon,
				 unsigned char *yr)
{

	while (1) {
	        RS5C313_CEENABLE;	/* CE:H */

		/* Initialize control reg. 24 hour */
		rs5c313_write_cntreg(0x04);

		if (!(rs5c313_read_cntreg() & RS5C313_CNTREG_ADJ_BSY))
			break;
		RS5C313_MISCOP;
		RS5C313_CEDISABLE;
		ndelay(700);	/* CE:L */
	}

	*sec = rs5c313_read_reg(RS5C313_ADDR_SEC);
	*sec |= (rs5c313_read_reg(RS5C313_ADDR_SEC10) << 4);

	*min = rs5c313_read_reg(RS5C313_ADDR_MIN);
	*min |= (rs5c313_read_reg(RS5C313_ADDR_MIN10) << 4);

	*hr = rs5c313_read_reg(RS5C313_ADDR_HOUR);
	*hr |= (rs5c313_read_reg(RS5C313_ADDR_HOUR10) << 4);

	*day = rs5c313_read_reg(RS5C313_ADDR_DAY);
	*day |= (rs5c313_read_reg(RS5C313_ADDR_DAY10) << 4);

	*mon = rs5c313_read_reg(RS5C313_ADDR_MON);
	*mon |= (rs5c313_read_reg(RS5C313_ADDR_MON10) << 4);

	*yr = rs5c313_read_reg(RS5C313_ADDR_YEAR);
	*yr |= (rs5c313_read_reg(RS5C313_ADDR_YEAR10) << 4);

	RS5C313_CEDISABLE;
	ndelay(700);		/* CE:L */

}

static void rs5c313_set_cur_time(unsigned char sec, unsigned char min,
				 unsigned char hr,
				 unsigned char day, unsigned char mon,
				 unsigned char yr)
{

	/* bysy check. */
	while (1) {
	        RS5C313_CEENABLE;  /* CE:H */

		/* Initialize control reg. 24 hour */
		rs5c313_write_cntreg(0x04);

		if (!(rs5c313_read_cntreg() & RS5C313_CNTREG_ADJ_BSY))
			break;
		RS5C313_MISCOP;
		RS5C313_CEDISABLE;
		ndelay(700);	/* CE:L */
	}

	rs5c313_write_reg(RS5C313_ADDR_SEC, sec);
	rs5c313_write_reg(RS5C313_ADDR_SEC10, (sec >> 4));

	rs5c313_write_reg(RS5C313_ADDR_MIN, min);
	rs5c313_write_reg(RS5C313_ADDR_MIN10, (min >> 4));

	rs5c313_write_reg(RS5C313_ADDR_HOUR, hr);
	rs5c313_write_reg(RS5C313_ADDR_HOUR10, (hr >> 4));

	rs5c313_write_reg(RS5C313_ADDR_DAY, day);
	rs5c313_write_reg(RS5C313_ADDR_DAY10,(day >> 4));

	rs5c313_write_reg(RS5C313_ADDR_MON, mon);
	rs5c313_write_reg(RS5C313_ADDR_MON10, (mon >> 4));

	rs5c313_write_reg(RS5C313_ADDR_YEAR, yr);
	rs5c313_write_reg(RS5C313_ADDR_YEAR10, (yr >> 4));

	RS5C313_CEDISABLE;
	ndelay(700);		/* CE:H */

}

unsigned long rs5c313_get_cmos_time(
        unsigned int *BCD_yr,  unsigned int *BCD_mon,
	unsigned int *BCD_day, unsigned int *BCD_hr,
	unsigned int *BCD_min, unsigned int *BCD_sec)
{

	unsigned int sec128, sec, min, hr, day, mon, yr, yr100;
	int clkstop = 0;

	/* Set SCK as I/O port and Initialize SCSPTR1 data & I/O port. */
	/* And Initialize SCL for RS5C313 clock */
	rs5c313_initialize();

      again:
	/* check XSTP bit for clock stoped */
	RS5C313_CEENABLE;	/* CE:H */
	if (rs5c313_read_cntreg() & RS5C313_CNTREG_WTEN_XSTP) {
		/* INT interval reg. OFF */
		rs5c313_write_intintvreg(0x00);
		/* Initialize control reg. 24 hour & adjust */
		rs5c313_write_cntreg(0x07);
		/* bysy check. */
		while (rs5c313_read_cntreg() & RS5C313_CNTREG_ADJ_BSY)
			RS5C313_MISCOP;
		/* Initialize control reg. 24 hour */
		rs5c313_write_cntreg(0x04);
		clkstop = 1;
	} else {
		clkstop = 0;
	}
	RS5C313_CEDISABLE;
	ndelay(700);		/* CE:L */

	/* Get current time. */
	sec = 0;
	min = 0;
	hr = 0;
	day = 0;
	mon = 0;
	yr = 0;
	rs5c313_get_cur_time((unsigned char *)&sec,
			     (unsigned char *)&min, (unsigned char *)&hr,
			     (unsigned char *)&day,
			     (unsigned char *)&mon, (unsigned char *)&yr);

	/* S-3531A count year from 2000 to 2099. */
	yr100 = 0x20;
	/* S-3531A can't get sec128. */
	sec128 = 0;

	*BCD_yr = yr;
	*BCD_mon = mon;
	*BCD_day = day;
	*BCD_hr = hr;
	*BCD_min = min;
	*BCD_sec = sec;

	yr100 = BCD2BIN(yr100);
	yr    = BCD2BIN(yr);
	mon   = BCD2BIN(mon);
	day   = BCD2BIN(day);
	hr    = BCD2BIN(hr);
	min   = BCD2BIN(min);
	sec   = BCD2BIN(sec);

	if (yr > 99 || mon < 1 || mon > 12 || day > 31 || day < 1 ||
	    hr > 23 || min > 59 || sec > 59 || clkstop) {
		printk(KERN_ERR
		       "RICHO RS5C313: invalid value, resetting to 1 Jan 2000\n");
		/* Reset S-3531A set (20)00year/01month/01day  */
		/*                    00hour 00minute 00second */
		sec = 0;
		min = 0;
		hr  = 0;
		day = 1;
		mon = 1;
		yr = 00;
		rs5c313_set_cur_time((unsigned char)sec,
				     (unsigned char)min, (unsigned char)hr,
				     (unsigned char)day,
				     (unsigned char)mon, (unsigned char)yr);

		goto again;
	}

	return mktime(yr100 * 100 + yr, mon, day, hr, min, sec);
}

void rs5c313_set_cmos_time(unsigned int BCD_yr,  unsigned int BCD_mon,
			   unsigned int BCD_day, unsigned int BCD_hr,
			   unsigned int BCD_min, unsigned int BCD_sec)
{

	rs5c313_set_cur_time((unsigned char)BCD_sec,
			     (unsigned char)BCD_min,
			     (unsigned char)BCD_hr,
			     (unsigned char)BCD_day,
			     (unsigned char)BCD_mon, (unsigned char)BCD_yr);

}

/*****************************************************/
/* machine independence part of RTC driver           */
/*****************************************************/

#define RTC_IO_EXTENT	0x8

static struct fasync_struct *rtc_async_queue;
static DECLARE_WAIT_QUEUE_HEAD(rtc_wait);
static ssize_t rtc_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos);
static int rtc_ioctl(struct inode *inode, struct file *file,
		     unsigned int cmd, unsigned long arg);
static int rtc_proc_open(struct inode *inode, struct file *file);

/*
 *	Bits in rtc_status. (6 bits of room for future expansion)
 */

#define RTC_IS_OPEN		0x01	/* means /dev/rtc is in use	*/
#define RTC_TIMER_ON		0x02	/* missed irq timer active	*/

/*
 * rtc_status is never changed by rtc_interrupt, and ioctl/open/close is
 * protected by the big kernel lock. However, ioctl can still disable the timer
 * in rtc_status and then with del_timer after the interrupt has read
 * rtc_status but before mod_timer is called, which would then reenable the
 * timer (but you would need to have an awful timing before you'd trip on it)
 */
static unsigned long rtc_status = 0;	/* bitmapped status byte.	*/
static unsigned long rtc_freq = 0;	/* Current periodic IRQ rate	*/
static unsigned long rtc_irq_data = 0;	/* our output to the world	*/
static unsigned long rtc_max_user_freq = 64; /* > this, need CAP_SYS_RESOURCE */

/*
 *	If this driver ever becomes modularised, it will be really nice
 *	to make the epoch retain its value across module reload...
 */

static unsigned long epoch = 1900;	/* year corresponding to 0x00	*/

static const unsigned char days_in_mo[] = 
{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

/*
 * sysctl-tuning infrastructure.
 */
static ctl_table rtc_table[] = {
	{
		.ctl_name	= 1,
		.procname	= "max-user-freq",
		.data		= &rtc_max_user_freq,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec,
	},
	{ .ctl_name = 0 }
};

static ctl_table rtc_root[] = {
	{
		.ctl_name	= 1,
		.procname	= "rtc",
		.maxlen		= 0,
		.mode		= 0555,
		.child		= rtc_table,
	},
	{ .ctl_name = 0 }
};

static ctl_table dev_root[] = {
	{
		.ctl_name	= CTL_DEV,
		.procname	= "dev",
		.maxlen		= 0,
		.mode		= 0555,
		.child		= rtc_root,
	},
	{ .ctl_name = 0 }
};

static struct ctl_table_header *sysctl_header;

static int __init init_sysctl(void)
{
    sysctl_header = register_sysctl_table(dev_root, 0);
    return 0;
}

static void __exit cleanup_sysctl(void)
{
    unregister_sysctl_table(sysctl_header);
}

/*
 *	Now all the various file operations that we export.
 */

static ssize_t rtc_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	return -EIO;
}

static int rtc_do_ioctl(unsigned int cmd, unsigned long arg, int kernel)
{
	struct rtc_time wtime; 

	switch (cmd) {
	case RTC_RD_TIME:	/* Read the time/date from RTC	*/
	{
		memset(&wtime, 0, sizeof(struct rtc_time));
		rtc_get_rtc_time(&wtime);
		break;
	}
	case RTC_SET_TIME:	/* Set the RTC */
	{
		struct rtc_time rtc_tm;
		unsigned char mon, day, hrs, min, sec, leap_yr;
		unsigned int yrs;

		if (!capable(CAP_SYS_TIME))
			return -EACCES;

		if (copy_from_user(&rtc_tm, (struct rtc_time __user *)arg,
				   sizeof(struct rtc_time)))
			return -EFAULT;

		yrs = rtc_tm.tm_year + 1900;
		mon = rtc_tm.tm_mon + 1;   /* tm_mon starts at zero */
		day = rtc_tm.tm_mday;
		hrs = rtc_tm.tm_hour;
		min = rtc_tm.tm_min;
		sec = rtc_tm.tm_sec;

		if (yrs < 1970)
			return -EINVAL;

		leap_yr = ((!(yrs % 4) && (yrs % 100)) || !(yrs % 400));

		if ((mon > 12) || (day == 0))
			return -EINVAL;

		if (day > (days_in_mo[mon] + ((mon == 2) && leap_yr)))
			return -EINVAL;
			
		if ((hrs >= 24) || (min >= 60) || (sec >= 60))
			return -EINVAL;

		if ((yrs -= epoch) > 255)    /* They are unsigned */
			return -EINVAL;

		/* These limits and adjustments are independent of
		 * whether the chip is in binary mode or not.
		 */
		if (yrs > 169) {
			return -EINVAL;
		}
		if (yrs >= 100)
			yrs -= 100;

		sec = BIN2BCD(sec);
		min = BIN2BCD(min);
		hrs = BIN2BCD(hrs);
		day = BIN2BCD(day);
		mon = BIN2BCD(mon);
		yrs = BIN2BCD(yrs);

		spin_lock_irq(&rtc_lock);
		rs5c313_set_cmos_time(yrs, mon, day, hrs, min, sec);
		spin_unlock_irq(&rtc_lock);

		return 0;
	}
	case RTC_EPOCH_READ:	/* Read the epoch.	*/
	{
		return put_user (epoch, (unsigned long __user *)arg);
	}
	case RTC_EPOCH_SET:	/* Set the epoch.	*/
	{
		/* 
		 * There were no RTC clocks before 1900.
		 */
		if (arg < 1900)
			return -EINVAL;

		if (!capable(CAP_SYS_TIME))
			return -EACCES;

		epoch = arg;
		return 0;
	}
	default:
		return -ENOTTY;
	}
	return copy_to_user((void __user *)arg, &wtime, sizeof wtime) ? -EFAULT : 0;
}

static int rtc_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
		     unsigned long arg)
{
	return rtc_do_ioctl(cmd, arg, 0);
}

/*
 *	We enforce only one user at a time here with the open/close.
 *	Also clear the previous interrupt data on an open, and clean
 *	up things on a close.
 */

/* We use rtc_lock to protect against concurrent opens. So the BKL is not
 * needed here. Or anywhere else in this driver. */
static int rtc_open(struct inode *inode, struct file *file)
{
	spin_lock_irq (&rtc_lock);

	if(rtc_status & RTC_IS_OPEN)
		goto out_busy;

	rtc_status |= RTC_IS_OPEN;

	rtc_irq_data = 0;
	spin_unlock_irq (&rtc_lock);
	return 0;

out_busy:
	spin_unlock_irq (&rtc_lock);
	return -EBUSY;
}

static int rtc_fasync (int fd, struct file *filp, int on)

{
	return fasync_helper (fd, filp, on, &rtc_async_queue);
}

static int rtc_release(struct inode *inode, struct file *file)
{
	spin_lock_irq (&rtc_lock);
	rtc_irq_data = 0;
	rtc_status &= ~RTC_IS_OPEN;
	spin_unlock_irq (&rtc_lock);
	return 0;
}


/*
 * exported stuffs
 */

EXPORT_SYMBOL(rtc_register);
EXPORT_SYMBOL(rtc_unregister);
EXPORT_SYMBOL(rtc_control);

int rtc_register(rtc_task_t *task)
{
	return -EIO;
}

int rtc_unregister(rtc_task_t *task)
{
	return -EIO;
}

int rtc_control(rtc_task_t *task, unsigned int cmd, unsigned long arg)
{
	return -EIO;
}


/*
 *	The various file operations we support.
 */

static struct file_operations rtc_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.read		= rtc_read,
	.ioctl		= rtc_ioctl,
	.open		= rtc_open,
	.release	= rtc_release,
	.fasync		= rtc_fasync,
};

static struct miscdevice rtc_dev = {
	.minor		= RTC_MINOR,
	.name		= "rtc",
	.fops		= &rtc_fops,
};

static struct file_operations rtc_proc_fops = {
	.owner = THIS_MODULE,
	.open = rtc_proc_open,
	.read  = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


static int __init rtc_init(void)
{
	struct proc_dir_entry *ent;

	if (!request_region(RTC_PORT(0), RTC_IO_EXTENT, "rtc")) {
		printk(KERN_ERR "rtc: I/O port %d is not free.\n", RTC_PORT (0));
		return -EIO;
	}

	if (misc_register(&rtc_dev)) {
		release_region(RTC_PORT(0), RTC_IO_EXTENT);
		return -ENODEV;
	}

	ent = create_proc_entry("driver/rtc", 0, NULL);
	if (!ent) {
		release_region(RTC_PORT(0), RTC_IO_EXTENT);
		misc_deregister(&rtc_dev);
		return -ENOMEM;
	}
	ent->proc_fops = &rtc_proc_fops;
	(void) init_sysctl();

	printk(KERN_INFO "RICHO RS5C313 Real Time Clock Driver v" RTC_VERSION "\n");

	return 0;
}

static void __exit rtc_exit (void)
{
	cleanup_sysctl();
	remove_proc_entry ("driver/rtc", NULL);
	misc_deregister(&rtc_dev);
	release_region (RTC_PORT (0), RTC_IO_EXTENT);
}

module_init(rtc_init);
module_exit(rtc_exit);


/*
 *	Info exported via "/proc/driver/rtc".
 */

static int rtc_proc_show(struct seq_file *seq, void *v)
{
#define YN(bit) ((ctrl & bit) ? "yes" : "no")
#define NY(bit) ((ctrl & bit) ? "no" : "yes")
	struct rtc_time tm;
	unsigned char batt, ctrl;
	unsigned long freq;

	batt = 1;
	freq = rtc_freq;
	ctrl = RTC_24H; 

	rtc_get_rtc_time(&tm);

	/*
	 * There is no way to tell if the luser has the RTC set for local
	 * time or for Universal Standard Time (GMT). Probably local though.
	 */
	seq_printf(seq,
		   "rtc_time\t: %02d:%02d:%02d\n"
		   "rtc_date\t: %04d-%02d-%02d\n"
		   "rtc_epoch\t: %04lu\n",
		   tm.tm_hour, tm.tm_min, tm.tm_sec,
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, epoch);

	seq_puts(seq, "alarm\t\t: **:**:**\n");

	seq_printf(seq,
		   "DST_enable\t: %s\n"
		   "BCD\t\t: %s\n"
		   "24hr\t\t: %s\n"
		   "square_wave\t: %s\n"
		   "alarm_IRQ\t: %s\n"
		   "update_IRQ\t: %s\n"
		   "periodic_IRQ\t: %s\n"
		   "periodic_freq\t: %ld\n"
		   "batt_status\t: %s\n",
		   YN(RTC_DST_EN),
		   NY(RTC_DM_BINARY),
		   YN(RTC_24H),
		   YN(RTC_SQWE),
		   YN(RTC_AIE),
		   YN(RTC_UIE),
		   YN(RTC_PIE),
		   freq,
		   batt ? "okay" : "dead");

	return  0;
#undef YN
#undef NY
}

static int rtc_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtc_proc_show, NULL);
}

void rtc_get_rtc_time(struct rtc_time *rtc_tm)
{
	unsigned int BCD_yr, BCD_mon, BCD_day, BCD_hr, BCD_min, BCD_sec;

	spin_lock_irq(&rtc_lock);
	rs5c313_get_cmos_time(&BCD_yr, &BCD_mon, &BCD_day, 
			      &BCD_hr, &BCD_min, &BCD_sec);
	spin_unlock_irq(&rtc_lock);

	rtc_tm->tm_sec  = BCD2BIN(BCD_sec);
	rtc_tm->tm_min  = BCD2BIN(BCD_min);
	rtc_tm->tm_hour = BCD2BIN(BCD_hr);
	rtc_tm->tm_mday = BCD2BIN(BCD_day);
	rtc_tm->tm_mon  = BCD2BIN(BCD_mon);
	rtc_tm->tm_year = BCD2BIN(BCD_yr);

	/*
	 * Account for differences between how the RTC uses the values
	 * and how they are defined in a struct rtc_time;
	 */
	if ((rtc_tm->tm_year += (epoch - 1900)) <= 69)
		rtc_tm->tm_year += 100;

	rtc_tm->tm_mon--;
}


MODULE_AUTHOR("kogiidena");
MODULE_LICENSE("GPL");
MODULE_ALIAS_MISCDEV(RTC_MINOR);

