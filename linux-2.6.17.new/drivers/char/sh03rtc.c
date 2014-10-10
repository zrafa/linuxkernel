/*
 *	Real Time Clock interface for Linux	
 *
 *	Copyright (C) 2004 Interface Corporation Saito.K
 *
 *	Based on skelrton from the drivers/char/rtc.c
 *
 *	This driver allows use of the real time clock (built into
 *	nearly all computers) from user space. It exports the /dev/rtc
 *	interface supporting various ioctl() and also the
 *	/proc/driver/rtc pseudo-file for status information.
 *
 *	The ioctls can be used to set the interrupt behaviour and
 *	generation rate from the RTC via IRQ 8. Then the /dev/rtc
 *	interface can be used to make use of these timer interrupts,
 *	be they interval or alarm based.
 *
 *	The /dev/rtc interface will block on reads until an interrupt
 *	has been received. If a RTC interrupt has already happened,
 *	it will output an unsigned long and then block. The output value
 *	contains the interrupt status in the low byte and the number of
 *	interrupts since the last read in the remaining high bytes. The 
 *	/dev/rtc interface can also be used with the select(2) call.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 *	Based on other minimal char device drivers, like Alan's
 *	watchdog, Ted's random, etc. etc.
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

#define SH03_RTC_VERSION	"0.09"

#define RTC_IO_EXTENT	0x8

/*
 *	Note that *all* calls to CMOS_READ and CMOS_WRITE are done with
 *	interrupts disabled. Due to the index-port/data-port (0x70/0x71)
 *	design of the RTC, we don't want two different things trying to
 *	get to it at once. (e.g. the periodic 11 min sync from time.c vs.
 *	this driver.)
 */

#include <linux/config.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>

// @@@@@ 16.09.29 #include <linux/mc146818rtc.h>
#include <linux/rtc.h>

#include <linux/init.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/sysctl.h>
#include <linux/wait.h>
#include <linux/bcd.h>

#include <asm/current.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/io.h>

extern spinlock_t	rtc_lock;

/*
TODO: Timer
*/
#define	SH03_RTC_IRQ	11

#define RTC_BASE	0xb0000000
#define RTC_SEC1	(RTC_BASE + 0)
#define RTC_SEC10	(RTC_BASE + 1)
#define RTC_MIN1	(RTC_BASE + 2)
#define RTC_MIN10	(RTC_BASE + 3)
#define RTC_HOU1	(RTC_BASE + 4)
#define RTC_HOU10	(RTC_BASE + 5)
#define RTC_WEE1	(RTC_BASE + 6)
#define RTC_DAY1	(RTC_BASE + 7)
#define RTC_DAY10	(RTC_BASE + 8)
#define RTC_MON1	(RTC_BASE + 9)
#define RTC_MON10	(RTC_BASE + 10)
#define RTC_YEA1	(RTC_BASE + 11)
#define RTC_YEA10	(RTC_BASE + 12)
#define RTC_YEA100	(RTC_BASE + 13)
#define RTC_YEA1000	(RTC_BASE + 14)
#define RTC_CTL		(RTC_BASE + 15)
#define RTC_BUSY	1
#define RTC_STOP	2
#define RTC_BANK0	(0 << 2)
#define RTC_BANK1	(1 << 2)
#define RTC_BANK2	(2 << 2)

#define RTC_ATCTL	(RTC_BASE + 14)
#define RTC_AIE		1
#define RTC_AF		2

#define RTC_TE_CLOCK	(RTC_BASE +  8)
#define RTC_TMCTL	(RTC_BASE + 14)
#define RTC_TIE		1
#define RTC_TF		2

#define SH03_RTC_PORT(n)	((RTC_BASE) + n)
#define SH03_RTC_IO_EXTENT	0x10

#ifndef BIN_TO_BCD
#define BIN_TO_BCD(val)	((val)=(((val)/10)<<4) + (val)%10)
#endif

#define SELECT_BANK(n)	ctrl_outb((n), RTC_CTL)

#define READ_SECONDS	((ctrl_inb(RTC_SEC1) & 15) + (ctrl_inb(RTC_SEC10) &  7) * 10)
#define READ_MINUTES	((ctrl_inb(RTC_MIN1) & 15) + (ctrl_inb(RTC_MIN10) &  7) * 10)
#define READ_HOURS	((ctrl_inb(RTC_HOU1) & 15) + (ctrl_inb(RTC_HOU10) &  3) * 10)
#define READ_WEEK	 (ctrl_inb(RTC_WEE1) &  7)
#define READ_DAY	((ctrl_inb(RTC_DAY1) & 15) + (ctrl_inb(RTC_DAY10) &  3) * 10)
#define READ_MONTH	((ctrl_inb(RTC_MON1) & 15) + (ctrl_inb(RTC_MON10) &  1) * 10)
#define READ_YEAR	((ctrl_inb(RTC_YEA1) & 15) + (ctrl_inb(RTC_YEA10) & 15) * 10 \
		       + (ctrl_inb(RTC_YEA100) & 15) * 100 + (ctrl_inb(RTC_YEA1000) & 3) * 1000)

#define WRITE_SECONDS(d)	ctrl_outb((d) & 15, RTC_SEC1); ctrl_outb((d) >> 4, RTC_SEC10)
#define WRITE_MINUTES(d)	ctrl_outb((d) & 15, RTC_MIN1); ctrl_outb((d) >> 4, RTC_MIN10)
#define WRITE_HOURS(d)		ctrl_outb((d) & 15, RTC_HOU1); ctrl_outb((d) >> 4, RTC_HOU10)
#define WRITE_WEEK(d)		ctrl_outb((d) & 15, RTC_WEE1)
#define WRITE_DAY(d)		ctrl_outb((d) & 15, RTC_DAY1); ctrl_outb((d) >> 4, RTC_DAY10)
#define WRITE_MONTH(d)		ctrl_outb((d) & 15, RTC_MON1); ctrl_outb((d) >> 4, RTC_MON10)
#define WRITE_YEAR_LOW(d)	ctrl_outb((d) & 15, RTC_YEA1); ctrl_outb((d) >> 4, RTC_YEA10)
#define WRITE_YEAR_HIGH(d)	ctrl_outb((d) & 15, RTC_YEA100); ctrl_outb((d) >> 4, RTC_YEA1000)

#define READ_ATCTL		(ctrl_inb(RTC_ATCTL))
#define WRITE_ATCTL(d)		ctrl_outb((d), RTC_ATCTL)

#define READ_TMCTL		(ctrl_inb(RTC_TMCTL))
#define WRITE_TMCTL(d)		ctrl_outb((d), RTC_TMCTL)

#define READ_CLOCK		(ctrl_inb(RTC_TE_CLOCK))
#define WRITE_CLOCK(d)		ctrl_outb((d) & 15, RTC_TE_CLOCK)



#ifdef SH03_RTC_IRQ
static int sh03_rtc_has_irq = SH03_RTC_IRQ;
#endif

/*
 *	We sponge a minor off of the misc major. No need slurping
 *	up another valuable major dev number for this. If you add
 *	an ioctl, make sure you don't conflict with SPARC's RTC
 *	ioctls.
 */

static struct fasync_struct *sh03_rtc_async_queue;

static DECLARE_WAIT_QUEUE_HEAD(sh03_rtc_wait);

#ifdef SH03_RTC_IRQ
static struct timer_list sh03_rtc_irq_timer;
#endif

static ssize_t sh03_rtc_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos);

static int sh03_rtc_ioctl(struct inode *inode, struct file *file,
		     unsigned int cmd, unsigned long arg);

#ifdef SH03_RTC_IRQ
static unsigned int sh03_rtc_poll(struct file *file, poll_table *wait);
#endif

void sh03_rtc_get_rtc_time(struct rtc_time *rtc_tm);
static void sh03_get_rtc_alm_time (struct rtc_time *alm_tm);
#ifdef SH03_RTC_IRQ
static void sh03_rtc_dropped_irq(unsigned long data);

static void sh03_set_rtc_irq_bit(unsigned int bit);
static void sh03_mask_rtc_irq_bit(unsigned int bit);
#endif

static int sh03_rtc_read_proc(char *page, char **start, off_t off,
                         int count, int *eof, void *data);

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

#ifdef SH03_RTC_IRQ
/*
 * rtc_task_lock nests inside rtc_lock.
 */
static spinlock_t rtc_task_lock = SPIN_LOCK_UNLOCKED;
static rtc_task_t *rtc_callback = NULL;
#endif

/*
 *	If this driver ever becomes modularised, it will be really nice
 *	to make the epoch retain its value across module reload...
 */

static unsigned long epoch = 1900;	/* year corresponding to 0x00	*/

static const unsigned char days_in_mo[] = 
{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

#if 0 /* @@@@@ 16.09.29 TODO */
/*
 * Returns true if a clock update is in progress
 */
static inline unsigned char rtc_is_updating(void)
{
	unsigned char uip;

	spin_lock_irq(&rtc_lock);
	uip = (CMOS_READ(RTC_FREQ_SELECT) & RTC_UIP);
	spin_unlock_irq(&rtc_lock);
	return uip;
}
#endif

#ifdef SH03_RTC_IRQ
/*
 *	A very tiny interrupt handler. It runs with SA_INTERRUPT set,
 *	but there is possibility of conflicting with the set_rtc_mmss()
 *	call (the rtc irq and the timer irq can easily run at the same
 *	time in two different CPUs). So we need to serialize
 *	accesses to the chip with the rtc_lock spinlock that each
 *	architecture should implement in the timer code.
 *	(See ./arch/XXXX/kernel/time.c for the set_rtc_mmss() function.)
 */

irqreturn_t sh03_rtc_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	/*
	 *	Can be an alarm interrupt, update complete interrupt,
	 *	or a periodic interrupt. We store the status in the
	 *	low byte and the number of interrupts received since
	 *	the last read in the remainder of rtc_irq_data.
	 */

	spin_lock (&rtc_lock);
	rtc_irq_data += 0x100;
	rtc_irq_data &= ~0xff;
	SELECT_BANK(RTC_BANK1);
	rtc_irq_data |= (READ_ATCTL & RTC_AF) >> 1;
	WRITE_ATCTL(READ_ATCTL & ~ RTC_AF);
	SELECT_BANK(RTC_BANK2);
	rtc_irq_data |= (READ_TMCTL & RTC_TF);
	WRITE_TMCTL(READ_TMCTL & ~ RTC_TF);
	SELECT_BANK(RTC_BANK0);

	if (rtc_status & RTC_TIMER_ON)
		mod_timer(&sh03_rtc_irq_timer, jiffies + HZ/rtc_freq + 2*HZ/100);

	spin_unlock (&rtc_lock);

	/* Now do the rest of the actions */
	spin_lock(&rtc_task_lock);
	if (rtc_callback)
		rtc_callback->func(rtc_callback->private_data);
	spin_unlock(&rtc_task_lock);
	wake_up_interruptible(&sh03_rtc_wait);	

	kill_fasync (&sh03_rtc_async_queue, SIGIO, POLL_IN);

	return IRQ_HANDLED;
}
#endif

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

static ssize_t sh03_rtc_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
#ifndef SH03_RTC_IRQ
	return -EIO;
#else
	DECLARE_WAITQUEUE(wait, current);
	unsigned long data;
	ssize_t retval;
	
	if (sh03_rtc_has_irq == 0)
		return -EIO;

	if (count < sizeof(unsigned))
		return -EINVAL;

	add_wait_queue(&sh03_rtc_wait, &wait);

	do {
		/* First make it right. Then make it fast. Putting this whole
		 * block within the parentheses of a while would be too
		 * confusing. And no, xchg() is not the answer. */

		__set_current_state(TASK_INTERRUPTIBLE);
		
		spin_lock_irq (&rtc_lock);
		data = rtc_irq_data;
		rtc_irq_data = 0;
		spin_unlock_irq (&rtc_lock);

		if (data != 0)
			break;

		if (file->f_flags & O_NONBLOCK) {
			retval = -EAGAIN;
			goto out;
		}
		if (signal_pending(current)) {
			retval = -ERESTARTSYS;
			goto out;
		}
		schedule();
	} while (1);

	if (count < sizeof(unsigned long))
		retval = put_user(data, (unsigned int __user *)buf) ?: sizeof(int); 
	else
		retval = put_user(data, (unsigned long __user *)buf) ?: sizeof(long);
 out:
	current->state = TASK_RUNNING;
	remove_wait_queue(&sh03_rtc_wait, &wait);

	return retval;
#endif
}

static int sh03_rtc_do_ioctl(unsigned int cmd, unsigned long arg, int kernel)
{
	struct rtc_time wtime; 

#ifdef SH03_RTC_IRQ
	if (sh03_rtc_has_irq == 0) {
		switch (cmd) {
		case RTC_AIE_OFF:
		case RTC_AIE_ON:
		case RTC_PIE_OFF:
		case RTC_PIE_ON:
		case RTC_UIE_OFF:
		case RTC_UIE_ON:
		case RTC_IRQP_READ:
		case RTC_IRQP_SET:
			return -EINVAL;
		};
	}
#endif

	switch (cmd) {
#ifdef SH03_RTC_IRQ
	case RTC_AIE_OFF:	/* Mask alarm int. enab. bit	*/
	{
		sh03_mask_rtc_irq_bit(RTC_AIE);
		return 0;
	}
	case RTC_AIE_ON:	/* Allow alarm interrupts.	*/
	{
		sh03_set_rtc_irq_bit(RTC_AIE);
		return 0;
	}
	case RTC_PIE_OFF:	/* Mask periodic int. enab. bit	*/
	{
		sh03_mask_rtc_irq_bit(RTC_PIE_OFF);
		if (rtc_status & RTC_TIMER_ON) {
			spin_lock_irq (&rtc_lock);
			rtc_status &= ~RTC_TIMER_ON;
			del_timer(&sh03_rtc_irq_timer);
			spin_unlock_irq (&rtc_lock);
		}
		return 0;
	}
	case RTC_PIE_ON:	/* Allow periodic ints		*/
	{

		/*
		 * We don't really want Joe User enabling more
		 * than 64Hz of interrupts on a multi-user machine.
		 */
		if (!kernel && (rtc_freq > rtc_max_user_freq) &&
			(!capable(CAP_SYS_RESOURCE)))
			return -EACCES;

		if (!(rtc_status & RTC_TIMER_ON)) {
			spin_lock_irq (&rtc_lock);
			sh03_rtc_irq_timer.expires = jiffies + HZ/rtc_freq + 2*HZ/100;
			add_timer(&sh03_rtc_irq_timer);
			rtc_status |= RTC_TIMER_ON;
			spin_unlock_irq (&rtc_lock);
		}
		sh03_set_rtc_irq_bit(RTC_PIE_ON);
		return 0;
	}
#if 1 /* @@@@@ 16.09.29 TODO */
	case RTC_UIE_OFF:	/* Mask ints from RTC updates.	*/
	case RTC_UIE_ON:	/* Allow ints for RTC updates.	*/
		return -EINVAL;
#else
	case RTC_UIE_OFF:	/* Mask ints from RTC updates.	*/
	{
		sh03_mask_rtc_irq_bit(RTC_UIE);
		return 0;
	}
	case RTC_UIE_ON:	/* Allow ints for RTC updates.	*/
	{
		sh03_set_rtc_irq_bit(RTC_UIE);
		return 0;
	}
#endif
#endif
	case RTC_ALM_READ:	/* Read the present alarm time */
	{
		/*
		 * This returns a struct rtc_time. Reading >= 0xc0
		 * means "don't care" or "match all". Only the tm_hour,
		 * tm_min, and tm_sec values are filled in.
		 */
		memset(&wtime, 0, sizeof(struct rtc_time));
		sh03_get_rtc_alm_time(&wtime);
		break; 
	}
	case RTC_ALM_SET:	/* Store a time into the alarm */
	{
		/*
		 * This expects a struct rtc_time. Writing 0xff means
		 * "don't care" or "match all". Only the tm_hour,
		 * tm_min and tm_sec are used.
		 */
		unsigned char day, week, hrs, min, sec;
		struct rtc_time alm_tm;

		if (copy_from_user(&alm_tm, (struct rtc_time __user *)arg,
				   sizeof(struct rtc_time)))
			return -EFAULT;

		day  = alm_tm.tm_mday;
		week = alm_tm.tm_wday;
		hrs  = alm_tm.tm_hour;
		min  = alm_tm.tm_min;
		sec  = alm_tm.tm_sec;
		if (day >= 31)
			day = 99;

		if (week >= 7)
			week = 9;

		if (hrs >= 24)
			hrs = 99;

		if (min >= 60)
			min = 99;

		if (sec >= 60)
			sec = 99;

		spin_lock_irq(&rtc_lock);
		BIN_TO_BCD(sec);
		BIN_TO_BCD(min);
		BIN_TO_BCD(hrs);
		BIN_TO_BCD(day);
		SELECT_BANK(RTC_BANK1);
		WRITE_DAY(day);
		WRITE_WEEK(week);
		WRITE_HOURS(hrs);
		WRITE_MINUTES(min);
		WRITE_SECONDS(sec);
		SELECT_BANK(RTC_BANK0);
		spin_unlock_irq(&rtc_lock);

		return 0;
	}
	case RTC_RD_TIME:	/* Read the time/date from RTC	*/
	{
		memset(&wtime, 0, sizeof(struct rtc_time));
		sh03_rtc_get_rtc_time(&wtime);
		break;
	}
	case RTC_SET_TIME:	/* Set the RTC */
	{
		struct rtc_time rtc_tm;
		unsigned char mon, day, hrs, min, sec, leap_yr;
		unsigned int yrs, yrs_low, yrs_high;

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

		spin_lock_irq(&rtc_lock);

		/* These limits and adjustments are independent of
		 * whether the chip is in binary mode or not.
		 */
		if (yrs > 169) {
			spin_unlock_irq(&rtc_lock);
			return -EINVAL;
		}
		if (yrs >= 100)
			yrs -= 100;

		yrs += epoch;
		BIN_TO_BCD(sec);
		BIN_TO_BCD(min);
		BIN_TO_BCD(hrs);
		BIN_TO_BCD(day);
		BIN_TO_BCD(mon);
		yrs_low = yrs % 100;
		yrs_high = yrs / 100;
		BIN_TO_BCD(yrs_low);
		BIN_TO_BCD(yrs_high);

		WRITE_YEAR_HIGH(yrs_high);
		WRITE_YEAR_LOW(yrs_low);
		WRITE_MONTH(mon);
		WRITE_DAY(day);
		WRITE_HOURS(hrs);
		WRITE_MINUTES(min);
		WRITE_SECONDS(sec);

		spin_unlock_irq(&rtc_lock);
		return 0;
	}
#ifdef SH03_RTC_IRQ
	case RTC_IRQP_READ:	/* Read the periodic IRQ rate.	*/
	{
		return put_user(rtc_freq, (unsigned long __user *)arg);
	}
	case RTC_IRQP_SET:	/* Set periodic IRQ rate.	*/
	{
		int tmp = 0;

		/*
		 * We don't really want Joe User generating more
		 * than 64Hz of interrupts on a multi-user machine.
		 */
		if (!kernel && (arg > rtc_max_user_freq) && (!capable(CAP_SYS_RESOURCE)))
			return -EACCES;

		switch (arg) {
		case    1: tmp = 2; break;
		case   64: tmp = 1; break;
		case 4096: tmp = 0; break;
		default:   return -EINVAL;
		}

		spin_lock_irq(&rtc_lock);
		rtc_freq = arg;
		SELECT_BANK(RTC_BANK2);
		WRITE_CLOCK((READ_CLOCK & ~3) | tmp);
		SELECT_BANK(RTC_BANK0);
		spin_unlock_irq(&rtc_lock);
		return 0;
	}
#endif
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

static int sh03_rtc_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
		     unsigned long arg)
{
	return sh03_rtc_do_ioctl(cmd, arg, 0);
}

/*
 *	We enforce only one user at a time here with the open/close.
 *	Also clear the previous interrupt data on an open, and clean
 *	up things on a close.
 */

/* We use rtc_lock to protect against concurrent opens. So the BKL is not
 * needed here. Or anywhere else in this driver. */
static int sh03_rtc_open(struct inode *inode, struct file *file)
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

static int sh03_rtc_fasync (int fd, struct file *filp, int on)

{
	return fasync_helper (fd, filp, on, &sh03_rtc_async_queue);
}

static int sh03_rtc_release(struct inode *inode, struct file *file)
{
#ifdef SH03_RTC_IRQ
	if (sh03_rtc_has_irq == 0)
		goto no_irq;

	/*
	 * Turn off all interrupts once the device is no longer
	 * in use, and clear the data.
	 */

	spin_lock_irq(&rtc_lock);
	SELECT_BANK(RTC_BANK1);
	ctrl_outb(0, RTC_ATCTL);
	SELECT_BANK(RTC_BANK2);
	ctrl_outb(0, RTC_TMCTL);
	SELECT_BANK(RTC_BANK0);

	if (rtc_status & RTC_TIMER_ON) {
		rtc_status &= ~RTC_TIMER_ON;
		del_timer(&sh03_rtc_irq_timer);
	}
	spin_unlock_irq(&rtc_lock);

	if (file->f_flags & FASYNC) {
		sh03_rtc_fasync (-1, file, 0);
	}
no_irq:
#endif

	spin_lock_irq (&rtc_lock);
	rtc_irq_data = 0;
	rtc_status &= ~RTC_IS_OPEN;
	spin_unlock_irq (&rtc_lock);
	return 0;
}

#ifdef SH03_RTC_IRQ
/* Called without the kernel lock - fine */
static unsigned int sh03_rtc_poll(struct file *file, poll_table *wait)
{
	unsigned long l;

	if (sh03_rtc_has_irq == 0)
		return 0;

	poll_wait(file, &sh03_rtc_wait, wait);

	spin_lock_irq (&rtc_lock);
	l = rtc_irq_data;
	spin_unlock_irq (&rtc_lock);

	if (l != 0)
		return POLLIN | POLLRDNORM;
	return 0;
}
#endif

int rtc_control(rtc_task_t *task, unsigned int cmd, unsigned long arg)
{
#ifndef SH03_RTC_IRQ
	return -EIO;
#else
	spin_lock_irq(&rtc_task_lock);
	if (rtc_callback != task) {
		spin_unlock_irq(&rtc_task_lock);
		return -ENXIO;
	}
	spin_unlock_irq(&rtc_task_lock);
	return sh03_rtc_do_ioctl(cmd, arg, 1);
#endif
}


/*
 *	The various file operations we support.
 */

static struct file_operations sh03_rtc_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.read		= sh03_rtc_read,
#ifdef SH03_RTC_IRQ
	.poll		= sh03_rtc_poll,
#endif
	.ioctl		= sh03_rtc_ioctl,
	.open		= sh03_rtc_open,
	.release	= sh03_rtc_release,
	.fasync		= sh03_rtc_fasync,
};

static struct miscdevice sh03_rtc_dev=
{
	RTC_MINOR,
	"sh03-rtc",
	&sh03_rtc_fops
};

#ifdef SH03_RTC_IRQ
static irqreturn_t (*rtc_int_handler_ptr)(int irq, void *dev_id, struct pt_regs *regs);
#endif

static int __init sh03_rtc_init(void)
{
	if (!request_region(SH03_RTC_PORT(0), SH03_RTC_IO_EXTENT, "sh03-rtc")) {
		printk(KERN_ERR "sh03-rtc: I/O port %d is not free.\n", SH03_RTC_PORT (0));
		return -EIO;
	}

#ifdef SH03_RTC_IRQ
	rtc_int_handler_ptr = sh03_rtc_interrupt;
	if(request_irq(SH03_RTC_IRQ, rtc_int_handler_ptr, SA_INTERRUPT, "sh03-rtc", NULL)) {
		/* Yeah right, seeing as irq 8 doesn't even hit the bus. */
		printk(KERN_ERR "sh03-rtc: IRQ %d is not free.\n", RTC_IRQ);
		release_region(SH03_RTC_PORT(0), SH03_RTC_IO_EXTENT);
		return -EIO;
	}
#endif

	if (misc_register(&sh03_rtc_dev)) {
#ifdef SH03_RTC_IRQ
		free_irq(SH03_RTC_IRQ, NULL);
#endif
		release_region(SH03_RTC_PORT(0), SH03_RTC_IO_EXTENT);
		return -ENODEV;
	}
	if (!create_proc_read_entry ("driver/sh03-rtc", 0, NULL, sh03_rtc_read_proc, NULL)) {
#ifdef SH03_RTC_IRQ
		free_irq(SH03_RTC_IRQ, NULL);
#endif
		release_region(SH03_RTC_PORT(0), SH03_RTC_IO_EXTENT);
		misc_deregister(&sh03_rtc_dev);
		return -ENOMEM;
	}

#ifdef SH03_RTC_IRQ
	if (sh03_rtc_has_irq == 0)
		goto no_irq2;

	init_timer(&sh03_rtc_irq_timer);
	sh03_rtc_irq_timer.function = sh03_rtc_dropped_irq;
	spin_lock_irq(&rtc_lock);
	SELECT_BANK(RTC_BANK2);
	WRITE_CLOCK(2);
	SELECT_BANK(RTC_BANK0);
	rtc_freq = 1;
	spin_unlock_irq(&rtc_lock);
no_irq2:
#endif

	(void) init_sysctl();

	printk(KERN_INFO "CTP/PCI-SH03 Real Time Clock Driver v" SH03_RTC_VERSION "\n");

	return 0;
}

static void __exit sh03_rtc_exit (void)
{
	cleanup_sysctl();
	remove_proc_entry ("driver/sh03-rtc", NULL);
	misc_deregister(&sh03_rtc_dev);
	release_region (SH03_RTC_PORT (0), SH03_RTC_IO_EXTENT);
#ifdef SH03_RTC_IRQ
	if (sh03_rtc_has_irq)
		free_irq (SH03_RTC_IRQ, NULL);
#endif
}

module_init(sh03_rtc_init);
module_exit(sh03_rtc_exit);

#ifdef SH03_RTC_IRQ
/*
 * 	At IRQ rates >= 4096Hz, an interrupt may get lost altogether.
 *	(usually during an IDE disk interrupt, with IRQ unmasking off)
 *	Since the interrupt handler doesn't get called, the IRQ status
 *	byte doesn't get read, and the RTC stops generating interrupts.
 *	A timer is set, and will call this function if/when that happens.
 *	To get it out of this stalled state, we just read the status.
 *	At least a jiffy of interrupts (rtc_freq/HZ) will have been lost.
 *	(You *really* shouldn't be trying to use a non-realtime system 
 *	for something that requires a steady > 1KHz signal anyways.)
 */

static void sh03_rtc_dropped_irq(unsigned long data)
{
	unsigned long freq;

	spin_lock_irq (&rtc_lock);

	/* Just in case someone disabled the timer from behind our back... */
	if (rtc_status & RTC_TIMER_ON)
		mod_timer(&sh03_rtc_irq_timer, jiffies + HZ/rtc_freq + 2*HZ/100);

	rtc_irq_data += ((rtc_freq/HZ)<<8);
	rtc_irq_data &= ~0xff;
	SELECT_BANK(RTC_BANK1);
	rtc_irq_data |= (READ_ATCTL & RTC_AF) >> 1;
	SELECT_BANK(RTC_BANK2);
	rtc_irq_data |= (READ_TMCTL & RTC_TF);
	SELECT_BANK(RTC_BANK0);

	freq = rtc_freq;

	spin_unlock_irq(&rtc_lock);

	printk(KERN_WARNING "sh03-rtc: lost some interrupts at %ldHz.\n", freq);

	/* Now we have new data */
	wake_up_interruptible(&sh03_rtc_wait);

	kill_fasync (&sh03_rtc_async_queue, SIGIO, POLL_IN);
}
#endif

/*
 *	Info exported via "/proc/driver/sh03-rtc".
 */

static int sh03_rtc_proc_output (char *buf)
{
#define YN(bit) ((bit) ? "yes" : "no")
#define NY(bit) ((bit) ? "no" : "yes")
	char *p;
	struct rtc_time tm;
	unsigned char aie, tie, af, tf;
	unsigned long freq;
	int sec_ae, min_ae, hour_ae, week_ae, day_ae;

	spin_lock_irq(&rtc_lock);
	SELECT_BANK(RTC_BANK1);
	aie = READ_ATCTL & RTC_AIE;
	af  = READ_ATCTL & 2;
	sec_ae  = ctrl_inb(RTC_SEC10) & 8;
	min_ae  = ctrl_inb(RTC_MIN10) & 8;
	hour_ae = ctrl_inb(RTC_HOU10) & 8;
	week_ae = ctrl_inb(RTC_WEE1 ) & 8;
	day_ae  = ctrl_inb(RTC_DAY10) & 8;
	SELECT_BANK(RTC_BANK2);
	tie = READ_TMCTL & RTC_TIE;
	tf  = READ_TMCTL & 2;
	SELECT_BANK(RTC_BANK0);
	freq = rtc_freq;
	spin_unlock_irq(&rtc_lock);

	p = buf;

	sh03_rtc_get_rtc_time(&tm);

	/*
	 * There is no way to tell if the luser has the RTC set for local
	 * time or for Universal Standard Time (GMT). Probably local though.
	 */
	p += sprintf(p,
		     "rtc_time\t: %02d:%02d:%02d\n"
		     "rtc_date\t: %04d-%02d-%02d\n"
	 	     "rtc_epoch\t: %04lu\n",
		     tm.tm_hour, tm.tm_min, tm.tm_sec,
		     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, epoch);

	sh03_get_rtc_alm_time(&tm);

	/*
	 * We implicitly assume 24hr mode here. Alarm values >= 0xc0 will
	 * match any value for that particular field. Values that are
	 * greater than a valid time, but less than 0xc0 shouldn't appear.
	 */
	p += sprintf(p, "alarm\t\t: ");
	if (!day_ae)
		p += sprintf(p, "%02d ", tm.tm_mday);
	else
		p += sprintf(p, "** ");
	if (!week_ae)
		p += sprintf(p, "%01d ", tm.tm_wday);
	else
		p += sprintf(p, "* ");
	if (!hour_ae)
		p += sprintf(p, "%02d:", tm.tm_hour);
	else
		p += sprintf(p, "**:");

	if (!min_ae)
		p += sprintf(p, "%02d:", tm.tm_min);
	else
		p += sprintf(p, "**:");

	if (!sec_ae)
		p += sprintf(p, "%02d\n", tm.tm_sec);
	else
		p += sprintf(p, "**\n");

	p += sprintf(p,
		     "alarm_IRQ\t: %s-%s\n"
		     "periodic_IRQ\t: %s-%s\n"
		     "periodic_freq\t: %ld\n"
		     "FOS\t\t: %s\n"
		     "Control Reg\t: %x\n",
		     YN(aie),YN(af),
		     YN(tie),YN(tf),
		     freq,
		     ctrl_inb(RTC_SEC10) & 8 ? "on" : "off",
		     ctrl_inb(RTC_CTL) & 15);

	return  p - buf;
#undef YN
#undef NY
}

static int sh03_rtc_read_proc(char *page, char **start, off_t off,
                         int count, int *eof, void *data)
{
        int len = sh03_rtc_proc_output (page);
        if (len <= off+count) *eof = 1;
        *start = page + off;
        len -= off;
        if (len>count) len = count;
        if (len<0) len = 0;
        return len;
}

void sh03_rtc_get_rtc_time(struct rtc_time *rtc_tm)
{
// @@@@@ 16.09.29 TODO	unsigned long uip_watchdog = jiffies;

	/*
	 * read RTC once any update in progress is done. The update
	 * can take just over 2ms. We wait 10 to 20ms. There is no need to
	 * to poll-wait (up to 1s - eeccch) for the falling edge of RTC_UIP.
	 * If you need to know *exactly* when a second has started, enable
	 * periodic update complete interrupts, (via ioctl) and then 
	 * immediately read /dev/rtc which will block until you get the IRQ.
	 * Once the read clears, read the RTC time (again via ioctl). Easy.
	 */

#if 0 /* @@@@@ 16.09.29 TODO */
	if (rtc_is_updating() != 0)
		while (jiffies - uip_watchdog < 2*HZ/100) {
			barrier();
			cpu_relax();
		}
#endif
	/*
	 * Only the values that we read from the RTC are set. We leave
	 * tm_wday, tm_yday and tm_isdst untouched. Even though the
	 * RTC has RTC_DAY_OF_WEEK, we ignore it, as it is only updated
	 * by the RTC when initially set to a non-zero value.
	 */
	spin_lock_irq(&rtc_lock);
	rtc_tm->tm_sec = READ_SECONDS;
	rtc_tm->tm_min = READ_MINUTES;
	rtc_tm->tm_hour = READ_HOURS;
	rtc_tm->tm_wday = READ_WEEK;
	rtc_tm->tm_mday = READ_DAY;
	rtc_tm->tm_mon = READ_MONTH;
	rtc_tm->tm_year = READ_YEAR - epoch;
	spin_unlock_irq(&rtc_lock);

	/*
	 * Account for differences between how the RTC uses the values
	 * and how they are defined in a struct rtc_time;
	 */
	if ((rtc_tm->tm_year += (epoch - 1900)) <= 69)
		rtc_tm->tm_year += 100;

	rtc_tm->tm_mon--;
}

static void sh03_get_rtc_alm_time(struct rtc_time *alm_tm)
{
	/*
	 * Only the values that we read from the RTC are set. That
	 * means only tm_hour, tm_min, and tm_sec.
	 */
	spin_lock_irq(&rtc_lock);
	SELECT_BANK(RTC_BANK1);
	alm_tm->tm_sec = READ_SECONDS;
	alm_tm->tm_min = READ_MINUTES;
	alm_tm->tm_hour = READ_HOURS;
	alm_tm->tm_wday = READ_WEEK;
	alm_tm->tm_mday = READ_DAY;
	SELECT_BANK(RTC_BANK0);
	spin_unlock_irq(&rtc_lock);
}

#ifdef SH03_RTC_IRQ
/*
 * Used to disable/enable interrupts for any one of UIE, AIE, PIE.
 * Rumour has it that if you frob the interrupt enable/disable
 * bits in RTC_CONTROL, you should read RTC_INTR_FLAGS, to
 * ensure you actually start getting interrupts. Probably for
 * compatibility with older/broken chipset RTC implementations.
 * We also clear out any old irq data after an ioctl() that
 * meddles with the interrupt enable/disable bits.
 */

static void sh03_mask_rtc_irq_bit(unsigned int bit)
{
	unsigned char val;

	spin_lock_irq(&rtc_lock);
	switch(bit) {
	case RTC_AIE_OFF:
	  SELECT_BANK(RTC_BANK1);
	  val = READ_ATCTL;
	  val &= ~RTC_AIE;
	  WRITE_ATCTL(val);
	  SELECT_BANK(RTC_BANK0);
	  break;
	case RTC_PIE_OFF:
	  SELECT_BANK(RTC_BANK2);
	  val = READ_TMCTL;
	  val &= ~RTC_TIE;
	  WRITE_TMCTL(val);
	  SELECT_BANK(RTC_BANK0);
	  break;
	}

	rtc_irq_data = 0;
	spin_unlock_irq(&rtc_lock);
}

static void sh03_set_rtc_irq_bit(unsigned int bit)
{
	unsigned char val;

	spin_lock_irq(&rtc_lock);
	switch(bit) {
	case RTC_AIE_ON:
	  SELECT_BANK(RTC_BANK1);
	  val = READ_ATCTL;
	  val |= RTC_AIE;
	  WRITE_ATCTL(val);
	  SELECT_BANK(RTC_BANK0);
	  break;
	case RTC_PIE_ON:
	  SELECT_BANK(RTC_BANK2);
	  val = READ_TMCTL;
	  val |= RTC_TIE;
	  WRITE_TMCTL(val);
	  SELECT_BANK(RTC_BANK0);
	  break;
	}

	rtc_irq_data = 0;
	spin_unlock_irq(&rtc_lock);
}
#endif

MODULE_AUTHOR("Saito.K Interface Corporation");
MODULE_LICENSE("GPL");
MODULE_ALIAS_MISCDEV(RTC_MINOR);

