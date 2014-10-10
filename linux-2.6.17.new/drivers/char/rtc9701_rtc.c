/*
 * linux/drivers/char/rtc9701_rtc.c
 *
 * Real Time Clock interface for Linux
 * EPSON RTC-9701JE support
 *
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/delay.h>
#include <asm/delay.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/poll.h>
#ifdef CONFIG_PROC_FS
#include <linux/proc_fs.h>
#endif

#include <asm/io.h>
#include <asm/rts7751r2d/rts7751r2d.h>

#include <linux/rtc.h>

/* define to 1 enable copious debugging info */
#undef RTC9701_DEBUG
#undef RTC9701_DEBUG_IO
#undef RTC9701_DEBUG_INTR

#ifndef BCD_TO_BIN
#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)
#endif

#ifndef BIN_TO_BCD
#define BIN_TO_BCD(val) ((val)=(((val)/10)<<4) + (val)%10)
#endif

#define	DRIVER_VERSION	"0.02"

#define RSECCNT	0x00	/* Second Counter */
#define	RMINCNT	0x01	/* Minute Counter */
#define	RHRCNT	0x02	/* Hour Counter */
#define	RWKCNT	0x03	/* Week Counter */
#define	RDAYCNT	0x04	/* Day Counter */
#define	RMONCNT	0x05	/* Month Counter */
#define	RYRCNT	0x06	/* Year Counter */
#define R100CNT	0x07	/* Y100 Counter */
#define	RMINAR	0x08	/* Minute Alarm */
#define	RHRAR	0x09	/* Hour Alarm */
#define	RWKAR	0x0a	/* Week/Day Alarm */
#define	RTIMCNT	0x0c	/* Interval Timer */
#define REXT	0x0d	/* Extension Register */
#define	RFLAG	0x0e	/* RTC Flag Register */
#define	RCR	0x0f	/* RTC Control Register */

#define WRITE_CMD	0x00	/* Write Command */
#define	READ_CMD	0x08	/* Read Command */

#define SCSMR1	0xffe00000	/* Serial Mode Register(SCI) */
#define SCSCR1	0xffe00008	/* Serial Control Register(SCI) */
#define SCSPTR1	0xffe0001c	/* Serial Port Register(SCI) */

static int rtc_usage;
static int rtc_irq_data;

static struct fasync_struct* rtc_async_queue;

static DECLARE_WAIT_QUEUE_HEAD(rtc_wait);
extern spinlock_t rtc_lock;
static unsigned int epoch = 1900;      /* year corresponding to 0x00   */

static const unsigned char days_in_mo[] =
{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static __inline__ unsigned char rtc9701_inb(unsigned long addr)
{
	unsigned char data, retval;
	int i;

	ctrl_outw(0x0001, PA_RTCCE);		/* CE=1 */
	ndelay(170);				/* 170ns delay (tZR) */
	ctrl_outb(0x8c, SCSPTR1);
	for (i=0 ; i<18 ; i++)
		if (ctrl_inb(SCSPTR1) & 0x01)	/* Check ready */
			break;
		else
			mdelay(1);		/* 1ms delay */
//#ifdef RTC9701_DEBUG_IO
	if (i >= 18)
		printk("RTC-9701JE Read Time out ready wait\n");
//#endif
	ndelay(65);				/* 65ns delay (tRDY) */

	for (i=0 ; i<4 ; i++) {			/* Command Set */
		if ((READ_CMD << i) & 0x08)
			data = 0x8b;		/* DATA=1 */
		else
			data = 0x8a;		/* DATA=0 */
		ctrl_outb(data, SCSPTR1);	/* CLK=0, DATA */
		ndelay(250);			/* 250ns delay (tWL) */
		data |= 0x04;
		ctrl_outb(data, SCSPTR1);	/* CLK=1 */
		ndelay(250);			/* 250ns delay (tWH) */
	}

	for (i=0 ; i<4 ; i++) {			/* Address Set */
		if ((addr << i) & 0x08)
			data = 0x8b;		/* DATA=1 */
		else
			data = 0x8a;		/* DATA=0 */
		ctrl_outb(data, SCSPTR1);	/* CLK=0, DATA */
		ndelay(250);			/* 250ns delay (tWL) */
		data |= 0x04;
		ctrl_outb(data, SCSPTR1);	/* CLK=1 */
		ndelay(250);			/* 250ns delay (tWH) */
	}

#if 0
	/* Dummy clock */
	ctrl_outb(0x88, SCSPTR1);		/* CLK=0 */
	ndelay(250);				/* 250ns delay (tWL) */
	ctrl_inb(SCSPTR1);			/* Dummy read */
	ctrl_outb(0x8c, SCSPTR1);		/* CLK=1 */
	ndelay(250);				/* 250ns delay (tWH) */
#endif

	retval = 0;
	for (i=0 ; i<8 ; i++) {			/* DATA Read */
		ctrl_outb(0x88, SCSPTR1);	/* CLK=0 */
#if 0
		if (i == 0)
			ndelay(200);		/* 200ns delay (tZR) */
#endif
		ndelay(250);			/* 250ns delay (tWL) */
		retval <<= 1;
		retval &= 0xfe;
		if (ctrl_inb(SCSPTR1) & 0x01)
			retval |= 0x01;
		ctrl_outb(0x8c, SCSPTR1);	/* CLK=1 */
		ndelay(250);			/* 250ns delay (tWH) */
	}

	ctrl_outw(0x0000, PA_RTCCE);		/* CE=0 */
#if 0
	mdelay(1);				/* 1ms delay (tCR) */
#endif

#ifdef RTC9701_DEBUG_IO
	printk("rtc9701_inb addr=%x value=%x\n", (unsigned int)addr, retval);
#endif
	return retval;
}

static __inline__ void rtc9701_outb(unsigned char b, unsigned long addr)
{
	int i;
	unsigned char data;

#ifdef RTC9701_DEBUG_IO
	printk("rtc9701_outb addr=%x value=%x\n", (unsigned int)addr, b);
#endif
	ctrl_outw(0x0001, PA_RTCCE);		/* CE=1 */
	ndelay(170);				/* 170ns delay (tZR) */
	ctrl_outb(0x8c, SCSPTR1);
	for (i=0 ; i<18 ; i++)
		if (ctrl_inb(SCSPTR1) & 0x01)	/* Check ready */
			break;
		else
			mdelay(1);		/* 1ms delay */
//#ifdef RTC9701_DEBUG_IO
	if (i >= 18)
		printk("RTC-9701JE Write Time out ready wait\n");
//#endif
	ndelay(65);				/* 65ns delay (tRDY) */

	for (i=0 ; i<4 ; i++) {			/* Command Set */
		if ((WRITE_CMD << i) & 0x08)
			data = 0x8b;		/* DATA=1 */
		else
			data = 0x8a;		/* DATA=0 */
		ctrl_outb(data, SCSPTR1);	/* CLK=0, DATA */
		ndelay(250);			/* 250ns delay (tWL) */
		data |= 0x04;
		ctrl_outb(data, SCSPTR1);	/* CLK=1 */
		ndelay(250);			/* 250ns delay (tWH) */
	}

	for (i=0 ; i<4 ; i++) {			/* Address Set */
		if ((addr << i) & 0x08)
			data = 0x8b;		/* DATA=1 */
		else
			data = 0x8a;		/* DATA=0 */
		ctrl_outb(data, SCSPTR1);	/* CLK=0, DATA */
		ndelay(250);			/* 250ns delay (tWL) */
		data |= 0x04;
		ctrl_outb(data, SCSPTR1);	/* CLK=1 */
		ndelay(250);			/* 250ns delay (tWH) */
	}

	for (i=0 ; i<8 ; i++) {			/* DATA Write */
		if ((b << i ) & 0x80)
			data = 0x8b;		/* DATA=1 */
		else
			data = 0x8a;		/* DATA=0 */
		ctrl_outb(data, SCSPTR1);	/* CLK=0, DATA */
		ndelay(250);			/* 250ns delay (tWL) */
		data |= 0x04;
		ctrl_outb(data, SCSPTR1);	/* CLK=1 */
		ndelay(250);			/* 250ns delay (tWH) */
	}

	ctrl_outw(0x0000, PA_RTCCE);		/* CE=0 */
#if 0
	mdelay(1);				/* 1ms delay (tCR) */
#endif
}

static irqreturn_t rtc9701_rtc_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned char	wk;

#ifdef RTC9701_DEBUG
	printk("RTC-9701JE Interrupt irq=%d\n", irq);
#endif
	spin_lock(&rtc_lock);

	if (irq == IRQ_RTCALM) {
		wk = rtc9701_inb(RFLAG);
#ifdef RTC9701_DEBUG_INTR
		printk("RTC-9701JE Interrupt alarm flag=%02x\n", wk);
#endif
		if (wk & 0x08) {
			wk &= 0xb6;
			rtc9701_outb(wk, RFLAG);
			rtc_irq_data = 1;
		}
	} else if (irq == IRQ_RTCTIME) {
		wk = rtc9701_inb(RFLAG);
#ifdef RTC9701_DEBUG_INTR
		printk("RTC-9701JE Interrupt timer flag=%02x\n", wk);
#endif
		if (wk & 0x10) {	/* Interval timer */
			wk &= 0xae;
			rtc9701_outb(wk, RFLAG);
			rtc_irq_data = 2;
		}
		if (wk & 0x20) {	/* Update time */
			wk &= 0x9e;
			rtc9701_outb(wk, RFLAG);
			rtc_irq_data = 2;
		}
	}

	spin_unlock(&rtc_lock);
#if 0
	mdelay(20);
#endif
	wake_up_interruptible(&rtc_wait);

	kill_fasync(&rtc_async_queue, SIGIO, POLL_IN);

	return IRQ_HANDLED;
}

static int rtc9701_rtc_open(struct inode *minode, struct file *mfile)
{
	if (rtc_usage != 0)
		return -EBUSY;

	rtc_usage = 1;

        return 0;
}

static int rtc9701_rtc_release(struct inode *minode, struct file *mfile)
{
	rtc_usage = 0;

        return 0;
}

static int rtc9701_rtc_fasync(int fd, struct file *filp, int on)
{
        return fasync_helper(fd, filp, on, &rtc_async_queue);
}

static loff_t rtc9701_rtc_llseek(struct file *file, loff_t offset, int origin)
{
        return -ESPIPE;
}

static ssize_t rtc9701_rtc_read(struct file* file,
			      char*        buf,
			      size_t       count,
			      loff_t*      ppos)
{
        DECLARE_WAITQUEUE(wait, current);
	unsigned long data = 1;
        ssize_t retval;

	if (count < sizeof(unsigned long))
		return -EINVAL;

	add_wait_queue(&rtc_wait, &wait);

	current->state = TASK_INTERRUPTIBLE;

	do {
		spin_lock_irq (&rtc_lock);
		data = rtc_irq_data;
		spin_unlock_irq (&rtc_lock);

		if (data != 0) {
			rtc_irq_data = 0;
			break;
		}

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

	retval = put_user(data, (unsigned long *)buf);
	if (!retval)
		retval = sizeof(unsigned long);
out:
	current->state = TASK_RUNNING;
	remove_wait_queue(&rtc_wait, &wait);

        return retval;
}

static void rtc_set_timer(unsigned long value)
{
	unsigned char time;

	time = (unsigned char)value | 0x80;
	rtc9701_outb(time, RTIMCNT);
#ifdef RTC9701_DEBUG
	time = rtc9701_inb(RTIMCNT);
	printk("RTC-9701JE Set timer value=%x\n", time);
#endif
}

static unsigned long rtc_read_timer(void)
{
	unsigned char time;

	time = rtc9701_inb(RTIMCNT);
#ifdef RTC9701_DEBUG
	printk("RTC-9701JE Read timer value=%x\n", time);
#endif
	return (unsigned long)time;
}

static void control_periodic_irq(int mode)
{
	unsigned char wk;
	unsigned char rcr;

	if (mode == 0) {	/* OFF */
		rcr	= rtc9701_inb(RCR);
		wk	= rcr & 0x2e;
		rtc9701_outb(wk, RCR);
	} else {		/* ON */
		rcr	= rtc9701_inb(RCR);
		wk	= rcr | 0x10;
		rtc9701_outb(wk, RCR);
	}

	rtc_irq_data = 0;
}

static void control_alarm_irq(int mode)
{
	unsigned char rcr;

	if (mode == 0) {	/* AIE = OFF */
		rcr = rtc9701_inb(RCR);
		rcr &= 0x36;
		rtc9701_outb(rcr, RCR);
	} else {		/* AIE = ON */
		rcr = rtc9701_inb(RCR);
		rcr |= 0x08;
		rtc9701_outb(rcr, RCR);
	}

	rtc_irq_data = 0;
}

static void get_rtc_data(struct rtc_time *tm)
{
	unsigned char wk;

	tm->tm_sec	= 0;
	tm->tm_min	= 0;
	tm->tm_hour	= 0;
	tm->tm_mday	= 0;
	tm->tm_mon	= 0;
	tm->tm_year	= 0;
	tm->tm_wday	= 0;
	tm->tm_yday	= 0;
	tm->tm_isdst	= 0;

	wk = rtc9701_inb(RSECCNT);
	wk &= 0x7f;
	BCD_TO_BIN(wk);
	tm->tm_sec	= wk;

	wk = rtc9701_inb(RMINCNT);
	wk &= 0x7f;
	BCD_TO_BIN(wk);
	tm->tm_min	= wk;

	wk = rtc9701_inb(RHRCNT);
	wk &= 0x7f;
	BCD_TO_BIN(wk);
	tm->tm_hour	= wk;

	wk = rtc9701_inb(RWKCNT);
	wk &= 0x7f;
	tm->tm_wday	= wk - 1;

	wk = rtc9701_inb(RDAYCNT);
	wk &= 0x7f;
	BCD_TO_BIN(wk);
	tm->tm_mday	= wk;

	wk = rtc9701_inb(RMONCNT);
	wk &= 0x7f;
	BCD_TO_BIN(wk);
	tm->tm_mon	= wk - 1;

	wk = rtc9701_inb(RYRCNT);
	BCD_TO_BIN(wk);
	tm->tm_year	= wk + 100;
}

static void get_rtc_alarm_data(struct rtc_time *tm)
{
	unsigned char wk;
	unsigned char rflag, rcr;

	tm->tm_sec	= 0;
	tm->tm_min	= 0;
	tm->tm_hour	= 0;
	tm->tm_mday	= 0;
	tm->tm_mon	= 0;
	tm->tm_year	= 0;
	tm->tm_wday	= 0;
	tm->tm_yday	= 0;
	tm->tm_isdst	= 0;

	rflag = rtc9701_inb(RFLAG);
	wk = rflag & 0xf7;
	rtc9701_outb(wk, RFLAG);	/* AF=0 */
	rcr = rtc9701_inb(RCR);
	wk = rcr & 0xf7;
	rtc9701_outb(wk, RCR);		/* AIE=0 */

	wk = rtc9701_inb(RMINAR);
	wk &= 0x7f;
	BCD_TO_BIN(wk);
	tm->tm_min	= wk;

	wk = rtc9701_inb(RHRAR);
	wk &= 0x7f;
	BCD_TO_BIN(wk);
	tm->tm_hour	= wk;

	rtc9701_outb(rflag, RFLAG);
	rtc9701_outb(rcr, RCR);
#ifdef RTC9701_DEBUG
	printk("get_rtc_alarm_data: hour:%x min:%x\n", tm->tm_hour, tm->tm_min);
#endif
}

static void set_rtc_data(struct rtc_time *tm)
{
	unsigned char sec, min, hour, mday, wday, mon, year;

#ifdef RTC9701_DEBUG
	printk("set_rtc_data:%d/%d/%d %d:%d:%d\n", tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
#endif
	sec = tm->tm_sec;
	min = tm->tm_min;
	hour = tm->tm_hour;
	mday = tm->tm_mday;
	wday = tm->tm_wday;
	mon = tm->tm_mon;

	BIN_TO_BCD(sec);
	rtc9701_outb(sec, RSECCNT);

	BIN_TO_BCD(min);
	rtc9701_outb(min, RMINCNT);

	BIN_TO_BCD(hour);
	rtc9701_outb(hour, RHRCNT);

	rtc9701_outb(wday, RWKCNT);

	BIN_TO_BCD(mday);
	rtc9701_outb(mday, RDAYCNT);

	BIN_TO_BCD(mon);
	rtc9701_outb(mon, RMONCNT);

	if (tm->tm_year > 100)
		tm->tm_year -= 100;
	year = tm->tm_year;
	BIN_TO_BCD(year);
	rtc9701_outb(year, RYRCNT);
}

static void set_rtc_alarm_data(struct rtc_time *tm)
{
	unsigned char wk;
	unsigned char min, hour;

#ifdef RTC9701_DEBUG
	printk("set_rtc_alarm_data: hour:%x min:%x\n", tm->tm_hour, tm->tm_min);
#endif
	wk = rtc9701_inb(RFLAG);
	wk &= 0xf7;
	rtc9701_outb(wk, RFLAG);	/* AF=0 */
	wk = rtc9701_inb(RCR);
	wk &= 0xf7;
	rtc9701_outb(wk, RCR);		/* AIE=0 */

	min = tm->tm_min;
	BIN_TO_BCD(min);
	rtc9701_outb(min, RMINAR);

	hour = tm->tm_hour;
	BIN_TO_BCD(hour);
	rtc9701_outb(hour, RHRAR);

	wk = rtc9701_inb(RFLAG);
	wk &= 0xf7;
	rtc9701_outb(wk, RFLAG);	/* AF=0 */
}

static int rtc9701_rtc_ioctl(struct inode* inode,
			   struct file*  file,
			   unsigned int  cmd,
			   unsigned long arg)
{
	struct rtc_time	wtime, rtc_tm;
	unsigned char	mon, day, hrs, min, sec, week, leap_yr;
	unsigned int	yrs;
	unsigned long	value;

	wtime.tm_sec	= 0;
	wtime.tm_min	= 0;
	wtime.tm_hour	= 0;
	wtime.tm_mday	= 0;
	wtime.tm_mon	= 0;
	wtime.tm_year	= 0;
	wtime.tm_wday	= 0;
	wtime.tm_yday	= 0;
	wtime.tm_isdst	= 0;

	switch (cmd) {
	case RTC_AIE_OFF:			/* =2:Alarm int. disable */
		spin_lock_irq(&rtc_lock);
		control_alarm_irq(0);
		spin_unlock_irq(&rtc_lock);
		mdelay(20);
		return 0;

	case RTC_AIE_ON:			/* =1:Alarm int. enable */
		spin_lock_irq(&rtc_lock);
		control_alarm_irq(1);
		spin_unlock_irq(&rtc_lock);
		mdelay(20);
		return 0;

	case RTC_PIE_OFF:			/* =6:Periodic int. disable */
		spin_lock_irq(&rtc_lock);
		control_periodic_irq(0);
		spin_unlock_irq(&rtc_lock);
		mdelay(20);
		return 0;

	case RTC_PIE_ON:			/* =5:Periodic int. enable */
		spin_lock_irq(&rtc_lock);
		control_periodic_irq(1);
		spin_unlock_irq(&rtc_lock);
		mdelay(20);
		return 0;

	case RTC_IRQP_SET:
		if (copy_from_user(&value, (unsigned long *)arg, sizeof(value)))
			return -EFAULT;
		rtc_set_timer(value);
		mdelay(20);
		return 0;

	case RTC_IRQP_READ:
		value = rtc_read_timer();
		mdelay(20);
		return put_user(value, (unsigned long *)arg);

	case RTC_ALM_READ:			/* =8:Read alarm time */
		get_rtc_alarm_data(&wtime);
		mdelay(20);
		break;

	case RTC_ALM_SET:			/* =7:Set alarm time */
		if (copy_from_user(&rtc_tm, (struct rtc_time*)arg,
                                   sizeof(struct rtc_time)))
			return -EFAULT;

		hrs = rtc_tm.tm_hour;
		min = rtc_tm.tm_min;

		if ((hrs >= 24) || (min >= 60)) {
			return -EINVAL;
		}

		/* update the alarm register */
		spin_lock_irq(&rtc_lock);
		wtime.tm_min	= min;
		wtime.tm_hour	= hrs;
		set_rtc_alarm_data(&wtime);
		spin_unlock_irq(&rtc_lock);
		mdelay(20);
		return 0;

	case RTC_RD_TIME:			/* =9:Read RTC time */
		spin_lock_irq(&rtc_lock);
		get_rtc_data(&wtime);
		spin_unlock_irq(&rtc_lock);
		mdelay(20);
		break;

	case RTC_SET_TIME:			/* =10:Set RTC time */
		if (copy_from_user(&rtc_tm, (struct rtc_time*)arg,
                                   sizeof(struct rtc_time)))
			return -EFAULT;

		yrs = rtc_tm.tm_year + epoch;
		mon = rtc_tm.tm_mon+1;
		day = rtc_tm.tm_mday;
		hrs = rtc_tm.tm_hour;
		min = rtc_tm.tm_min;
		sec = rtc_tm.tm_sec;
		week = rtc_tm.tm_wday+1;

		if (yrs < epoch){
			return -EINVAL;
		}
		leap_yr = ((!(yrs % 4) && (yrs % 100)) || !(yrs % 400));
		if ((mon > 12) || (day == 0)){
			return -EINVAL;
		}
		if (day > (days_in_mo[mon] + ((mon == 2) && leap_yr))){
			return -EINVAL;
		}
		if ((hrs >= 24) || (min >= 60) || (sec >= 60) || (week > 64)){
			return -EINVAL;
		}
		if ((yrs - epoch) > 255){
			return -EINVAL;
		}

		spin_lock_irq(&rtc_lock);
		wtime.tm_sec	= sec;
		wtime.tm_min	= min;
		wtime.tm_hour	= hrs;
		wtime.tm_mday	= day;
		wtime.tm_mon	= mon;
		wtime.tm_wday	= week;
		wtime.tm_year	= yrs - epoch;
		set_rtc_data(&wtime);
		spin_unlock_irq(&rtc_lock);
		mdelay(20);
		return 0;

	default:
		return -EINVAL;
	}

        return copy_to_user((void*)arg, &wtime, sizeof(wtime)) ? -EFAULT : 0;
}

static unsigned int rtc9701_rtc_poll(struct file* file, poll_table* wait)
{
	unsigned long l;

	poll_wait(file, &rtc_wait, wait);

	spin_lock_irq(&rtc_lock);
	l = rtc_irq_data;
	spin_unlock_irq(&rtc_lock);

	if (l != 0)
		return POLLIN | POLLRDNORM;
	else
        	return 0;
}

#ifdef CONFIG_PROC_FS

static int rtc9701_rtc_proc_output(char *buf)
{
        char		*p;
        struct rtc_time	tm;

	get_rtc_data(&tm);
	mdelay(20);

	p = buf;
	p += sprintf(p,
                     "rtc_time\t: %02d:%02d:%02d\n"
                     "rtc_date\t: %04d-%02d-%02d\n"
                     "rtc_epoch\t: %04lu\n",
                     tm.tm_hour, tm.tm_min, tm.tm_sec,
                     tm.tm_year + epoch, tm.tm_mon+1, tm.tm_mday,
		     (unsigned long)epoch);

	get_rtc_alarm_data(&tm);
	mdelay(20);

	p += sprintf(p,
		     "alrm_time\t: %02d:%02d\n",
                     tm.tm_hour, tm.tm_min);

	p += sprintf(p,"alarm_IRQ\t: %s\n",
		     (rtc9701_inb(RCR) & 0x08) ? "yes" : "no" );
	mdelay(20);
	p += sprintf(p,"periodic_IRQ\t: %s\n",
		     (rtc9701_inb(RCR) & 0x10) ? "yes" : "no" );
	mdelay(20);

	p += sprintf(p,"periodic_freq\t: 1\n");
	p += sprintf(p,"batt_status\t: unsupported\n");

        return (p - buf);
}

static int rtc9701_rtc_read_proc(char*  page,
			       char** start,
			       off_t  off,
			       int    count,
			       int*   eof,
			       void*  data)
{
        int len = rtc9701_rtc_proc_output(page);

	if (len <= off + count) { *eof = 1; }
	*start = page + off;
	len -= off;
	if (len > count) { len = count; }
	if (len < 0)     { len = 0; }

        return len;
}

#endif

static void rtc9701_initial_check(void)
{
	unsigned int sec, min, hr, day, mon, yr;

	sec = rtc9701_inb(RSECCNT) & 0x7f;
	min = rtc9701_inb(RMINCNT) & 0x7f;
	hr  = rtc9701_inb(RHRCNT) & 0x7f;
	day = rtc9701_inb(RDAYCNT) & 0x7f;
	mon = rtc9701_inb(RMONCNT) & 0x7f;
	yr  = rtc9701_inb(RYRCNT) & 0x7f;

	BCD_TO_BIN(sec);
	BCD_TO_BIN(min);
	BCD_TO_BIN(hr);
	BCD_TO_BIN(day);
	BCD_TO_BIN(mon);
	BCD_TO_BIN(yr);

	if (yr > 99 || mon < 1 || mon > 12 || day > 31 || day < 1 ||
	    hr > 23 || min > 59 || sec > 59) {
		printk("Current RTC Time:%d-%d-%d %d:%d:%d\n", yr, mon, day, hr, min, sec);
		printk(KERN_ERR "RTC-9701: invalid value, resetting to 1 Jan 2000\n");
		rtc9701_outb(0, RSECCNT);
		rtc9701_outb(0, RMINCNT);
		rtc9701_outb(0, RHRCNT);
		rtc9701_outb(0x40, RWKCNT);
		rtc9701_outb(1, RDAYCNT);
		rtc9701_outb(1, RMONCNT);
		rtc9701_outb(0, RYRCNT);
	}
}

static struct file_operations rtc_fops = {
	.owner		= THIS_MODULE,
	.llseek		= rtc9701_rtc_llseek,
	.read		= rtc9701_rtc_read,
	.poll		= rtc9701_rtc_poll,
	.ioctl		= rtc9701_rtc_ioctl,
	.open		= rtc9701_rtc_open,
	.release	= rtc9701_rtc_release,
	.fasync		= rtc9701_rtc_fasync,
};


static struct miscdevice rts7751r2drtc_miscdev = {
	RTC_MINOR,
	"rtc",
	&rtc_fops
};

static void rtc9701_rtc_exit(void)
{
	spin_lock_irq(&rtc_lock);
	rtc9701_outb(0x00, RCR);
	spin_unlock_irq(&rtc_lock);
	mdelay(20);

	free_irq(IRQ_RTCALM, NULL);
	free_irq(IRQ_RTCTIME, NULL);
#ifdef CONFIG_PROC_FS
	remove_proc_entry("driver/rtc", NULL);
#endif

	misc_deregister(&rts7751r2drtc_miscdev);
}

static int __init rtc9701_rtc_init(void)
{
	unsigned char val;

	misc_register(&rts7751r2drtc_miscdev);

#ifdef CONFIG_PROC_FS
	create_proc_read_entry("driver/rtc", 0, 0, rtc9701_rtc_read_proc, NULL);
#endif
	ctrl_outb((ctrl_inb(SCSMR1) & 0x7f), SCSMR1);
	ctrl_outb((ctrl_inb(SCSCR1) & 0x9c), SCSCR1);
	ctrl_outw(0x0000, PA_RTCCE);	/* CE=0 */
	ctrl_outb(0x8c, SCSPTR1);	/* EIO=1, SPB1IO=1, SPB1DT=1, SPB0IO=0 */

	rtc9701_initial_check();	/* RTC Data Initial check */
	mdelay(20);

	rtc9701_outb(0x02, REXT);	/* WADA=0, UDUTY=0, USEL=0, TSEL1=1, TSEL0=0 */
	mdelay(20);
	rtc9701_outb(0x00, RCR);	/* UIE=0, TIE=0, AIE=0, EXIE=0, VLIE=0 */
	mdelay(20);
	rtc9701_outb(0x00, RFLAG);
	mdelay(20);
#if 0
	val = rtc9701_inb(REXT);
	val &= 0x33;
	rtc9701_outb(val, REXT);	/* WADA=0 */
#endif
	val = 0xff;
	rtc9701_outb(val, RWKAR);
	mdelay(20);

	rtc9701_outb(0x00, RTIMCNT);
	mdelay(20);

	if (request_irq(IRQ_RTCALM, rtc9701_rtc_interrupt, SA_INTERRUPT, "rtc_alarm", NULL)) {
		printk(KERN_ERR "rtc: IRQ %d already in use.\n", IRQ_RTCALM);
		return -EIO;
	}

	if (request_irq(IRQ_RTCTIME, rtc9701_rtc_interrupt, SA_INTERRUPT, "rtc_timer", NULL)) {
		printk(KERN_ERR "rtc: IRQ %d already in use.\n", IRQ_RTCTIME);
		return -EIO;
	}

	printk(KERN_INFO "RTC-9701JE Real Time Clock Driver v" DRIVER_VERSION "\n");

        return 0;
}

module_init(rtc9701_rtc_init);
module_exit(rtc9701_rtc_exit);
