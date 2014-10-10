/*
 *  linux/drivers/sound/voyagergx_sound.c -- voyager sound driver
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
#include <linux/version.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/sound.h>
#include <linux/slab.h>
#include <linux/soundcard.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/pci.h>
#include <linux/bitops.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>
#include <linux/ac97_codec.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/hardirq.h>
#include <asm/rts7751r2d/voyagergx_reg.h>
#include <asm/irq.h>
#include "voyagergx_sound.h"

static DEFINE_SPINLOCK(voyagergx_sound_lock);

/* --------------------------------------------------------------------- */
#undef OSS_DOCUMENTED_MIXER_SEMANTICS

#define VOYAGERGX_MODULE_NAME "Voyagergx audio"
#define PFX VOYAGERGX_MODULE_NAME

#define err(format, arg...) printk(KERN_ERR PFX ": " format "\n" , ## arg)
#define info(format, arg...) printk(KERN_INFO PFX ": " format "\n" , ## arg)
//#define info(format, arg...) printk(": " format "\n" , ## arg)
//#define warn(format, arg...) printk(KERN_WARNING PFX ": " format "\n" , ## arg)

// buffer size
#define	VOYAGER_SOUND_SIZE	0x100000
// data buffer 1 - vram upper
#define	VOYAGER_SOUND_BUF1	0xb0200000
// data buffer 2 - vram upper
#define	VOYAGER_SOUND_BUF2	VOYAGER_SOUND_BUF1+VOYAGER_SOUND_SIZE
//WAIT TIME OUT
//#define	WAIT_TIMEOUT	((VOYAGER_SOUND_SIZE / 48000) * HZ + 10)
#define	WAIT_TIMEOUT	1100

DECLARE_WAIT_QUEUE_HEAD(int_wq);

/* Boot options */
static int      vra = 0;	// 0 = no VRA, 1 = use VRA if codec supports it
MODULE_PARM(vra, "i");
MODULE_PARM_DESC(vra, "if 1 use VRA if codec supports it");

struct voyager_setting {
	int             ch;
	int		fmt;
	int		rate;
} setting;

int	init_8051(void);
int	command_8051(int com,int *data,int *data2);

// DMA used channel - set DMA used channel(fixed at 0)
static	int	sh7751_dmasound_play_irq = 0;
// buffer 1 used flag - transfersize is set when used
static	int	buff_flg1 = 0;
// buffer 2 used flag - transfersize is set when used
static	int	buff_flg2 = 0;
// buffer judgement flag - which buffer is used by DMA
static	int	proc_flg = 0;
// first judgement flag - data transfer is first time?
static	int	first_flg = 0;
// last judgement flag - data transfer is last?
static	int	last_flg = 0;
// interrupt flag - 0 cleard if interrupt occured
static	int	wari_flg;
// break flag - to cancel or done of sound play
static	int	break_flg;
// play cancel flag - performance problem or end of play
static	int	abnml_flg;
// number of write buffer - which buffer will be used
static	int	next_write;
// remained buffer size
static	int	next_size;
// DMA transfer size
static	int	dma_req[2];
// DMA address table - address per channel
static	int	dma_tbl[] = {
	0xffa00000, 0xffa00010, 0xffa00020, 0xffa00030,
	0xffa00040, 0xffa00050, 0xffa00060, 0xffa00070
};

/* --------------------------------------------------------------------- */
// DMA start
// enable selected dma channel
void	dma2_start(int irq)
{
int	base;

	base = dma_tbl[irq];
	*(volatile unsigned long *)(base + 0xc) |= 0x00000001;
}


// DMA stop
// disable selected dma channel
void	dma2_stop(int irq)
{
int	base;

	base = dma_tbl[irq];
	*(volatile unsigned long *)(base + 0xc) &= 0xfffffffc;
}


// prepare DMA transfer
// set transfer src address and transfer size
void	dma2_queue_buffer(int irq,int src, int cnt)
{
int	base;

	base = dma_tbl[irq];
	*(volatile unsigned long *)(base + 0x0) = src & 0x1fffffff;
	// change transfer unit for mono or stereo
	if(setting.ch == 1) {
		// 4 byte for stereo
		*(volatile unsigned long *)(base + 0x8) = cnt;
	}
	else {
		// 2 byte for mono
		*(volatile unsigned long *)(base + 0x8) = cnt * 2;
	}
}


// DMA registration
// register DMA interrupt
int	request_dma2(int irq,char *str,irqreturn_t (*callback)(int, void *, struct pt_regs *))
{
int	ret;

	make_ipr_irq(DMTE0_IRQ+irq, DMA_IPR_ADDR, DMA_IPR_POS,DMA_PRIORITY);
	ret = request_irq(DMTE0_IRQ+irq,callback,SA_INTERRUPT,str,0);
	return(ret);
}


// preparation for DMA use
// set transfer dst address and transfer unit, transfer condition, etc
void	dma2_set_device(int irq)
{
int	base;

	//DMA initialize
	base = dma_tbl[irq];
	*(volatile unsigned long *)(base + 0x0) = 0;
	*(volatile unsigned long *)(base + 0x4) = VOYAGER_8051_FIFO & 0x1fffffff;
	*(volatile unsigned long *)(base + 0x8) = 0;
	// change transfer unit for mono or stereo
	if(setting.ch == 1) {
		// 4 byte for stereo
		*(volatile unsigned long *)(base + 0xc) = 0xb1034;
	}
	else {
		// 2 byte for mono
		*(volatile unsigned long *)(base + 0xc) = 0xb1024;
	}
	*(volatile unsigned long *)(base + 0x40) = 0x01;
}

/* --------------------------------------------------------------------- */


// cancel sound play(done)
//
static	void	voy_break(void)
{
int	data,data2;

	// cancel for 8051
	data = 0x00;
	command_8051(0x07,&data,&data2);
	// stop DMA
	dma2_stop(sh7751_dmasound_play_irq);
	break_flg = 1;
}
/* --------------------------------------------------------------------- */
// ISR for DMA
// called after DMA transfer is done
// next request is issued here in the case of sequential transfer
static	irqreturn_t do_irq(int irq, void *dev_id, struct pt_regs *regs)
{
	spin_lock_irq(&voyagergx_sound_lock);

	// stop DMA
	dma2_stop(sh7751_dmasound_play_irq);
	// clear interrupt flag
	wari_flg = 0;
	// play of buffer 1 is done?
	if((proc_flg == 1)&&(buff_flg1 == 1)) {
		// buffer 1 is free
		buff_flg1 = 0;
		// buffer 2 is being played
		proc_flg = 2;
		// next data is ready?
		if(buff_flg2 == 1) {
			// request DMA
			dma2_queue_buffer(sh7751_dmasound_play_irq,VOYAGER_SOUND_BUF2,dma_req[1]);
			dma2_start(sh7751_dmasound_play_irq);
		}
		else {
			// set stop flag if no data
			abnml_flg = 1;
		}
	}
	// play of buffer 2 is done?
	else if((proc_flg == 2)&&(buff_flg2 == 1)) {
		// buffer 2 is free
		buff_flg2 = 0;
		// buffer 1 is being played
		proc_flg = 1;
		// next data is ready?
		if(buff_flg1 == 1) {
			// request DMA
			dma2_queue_buffer(sh7751_dmasound_play_irq,VOYAGER_SOUND_BUF1,dma_req[0]);
			dma2_start(sh7751_dmasound_play_irq);
		}
		else {
			// set stop flag if no data
			abnml_flg = 1;
		}
	}
	// play remained data(last data or small one)
	else {
		//last transfer is done
		last_flg = 0;
	}
	//wake up sleeping write routine
	wake_up_interruptible(&int_wq);
	spin_unlock_irq(&voyagergx_sound_lock);

	return IRQ_HANDLED;
}

/* --------------------------------------------------------------------- */
// 32 bit memory read
static inline u32 voyager_readl(u32 addr)
{
	return *(volatile unsigned long *)(addr);
}

// 32 bit memory write
static inline void voyager_writel(u32 val,u32 addr)
{
	*(volatile unsigned long *)(addr) = val;
}

/* --------------------------------------------------------------------- */

struct voyagergx_state {
	/* soundcore stuff */
	int             dev_audio;

	struct ac97_codec codec;
	unsigned        codec_base_caps;	// AC'97 reg 00h, "Reset Register"
	unsigned        codec_ext_caps;		// AC'97 reg 28h, "Extended Audio ID"
	int             no_vra;			// do not use VRA

	spinlock_t      lock;
	struct semaphore open_sem;
	mode_t          open_mode;

} voyagergx_state;


/* --------------------------------------------------------------------- */
// read codec
static	u16 rdcodec(struct ac97_codec *codec, u8 addr)
{
	u32             cmd,ret;
	u16             data;


	// set register address for reading
	cmd = (u32) (addr & AC97C_INDEX_MASK) << 12;
	cmd |= AC97C_READ;	// read command
	command_8051(1,&cmd,&ret);
	mdelay(1);
	// read register
	command_8051(4,&cmd,&ret);
	mdelay(1);
	// convert it as it was shifted
	data = (cmd >> 4) & 0xffff;
	return data;
}


// write codec
static	void wrcodec(struct ac97_codec *codec, u8 addr, u16 data)
{
	u32             cmd,ret;
	int	i;

	//  reset?
	if(addr == 0) {
		// try again if failed
		for(i=0;i<10;i++) {
			// set write data
			cmd = (u32) data << AC97C_WD_BIT;	// OR in the data word
			command_8051(3,&cmd,&ret);
			// set write address
			cmd = (u32) (addr & AC97C_INDEX_MASK) << 12;
			cmd &= ~AC97C_READ;	// write command
			command_8051(1,&cmd,&ret);
			// check whether reset succeed
			ret = rdcodec(codec,0);
			if((ret & 0x8000) == 0) {
				break;
			}
		}
	}
	// instead of reset
	else {
		// set write data
		cmd = (u32) data << AC97C_WD_BIT;	// OR in the data word
		command_8051(3,&cmd,&ret);
		// set write address
		cmd = (u32) (addr & AC97C_INDEX_MASK) << 12;
		cmd &= ~AC97C_READ;	// write command
		command_8051(1,&cmd,&ret);
		mdelay(1);
		// read variables of volume for preservation
		rdcodec(codec,2);
		rdcodec(codec,24);
		rdcodec(codec,22);
		rdcodec(codec,0x2c);
	}
}

/* --------------------------------------------------------------------- */

static loff_t voyagergx_llseek(struct file *file, loff_t offset, int origin)
{
	return -ESPIPE;
}


static int voyagergx_open_mixdev(struct inode *inode, struct file *file)
{
	file->private_data = &voyagergx_state;
	return 0;
}

static int voyagergx_release_mixdev(struct inode *inode, struct file *file)
{
	return 0;
}

static int mixdev_ioctl(struct ac97_codec *codec, unsigned int cmd,
                        unsigned long arg)
{
	return codec->mixer_ioctl(codec, cmd, arg);
}

static int voyagergx_ioctl_mixdev(struct inode *inode, struct file *file,
			       unsigned int cmd, unsigned long arg)
{
	struct voyagergx_state *s = (struct voyagergx_state *)file->private_data;
	struct ac97_codec *codec = &s->codec;

	return mixdev_ioctl(codec, cmd, arg);
}

static /*const */ struct file_operations voyagergx_mixer_fops = {
	owner:THIS_MODULE,
	llseek:voyagergx_llseek,
	ioctl:voyagergx_ioctl_mixdev,
	open:voyagergx_open_mixdev,
	release:voyagergx_release_mixdev,
};

/* --------------------------------------------------------------------- */
// read routine
// record(sampling) should be used originally
// do nothing without record(sampling) now
static ssize_t voyagergx_read(struct file *file, char *buffer,
			   size_t count, loff_t *ppos)
{
	if (ppos != &file->f_pos)
		return -ESPIPE;
	if (!access_ok(VERIFY_WRITE, buffer, count))
		return -EFAULT;
	return 0;
}

// write routine
// used for sound play
// sound play is being done by 8051 and data itsself is transfered by DMA
// the preparation for the above is done here
static ssize_t voyagergx_write(struct file *file, const char *buffer,
	     		    size_t count, loff_t * ppos)
{
int	i,data,data2,data_size;
int	ret;


	if (ppos != &file->f_pos)
		return -ESPIPE;
	if (!access_ok(VERIFY_READ, buffer, count))
		return -EFAULT;
	// exit if break flag is set
	if(break_flg) {
		return count;
	}
	// wait until buffers are not free
	if((buff_flg1 == 1)&&(buff_flg2 == 1)) {
		wari_flg = 1;
		i = 0;
		while(wari_flg) {
			// sound play already done?
			if(abnml_flg) {
				break;
			}
			// sleep until buffer is not full
			ret = interruptible_sleep_on_timeout(&int_wq,WAIT_TIMEOUT);
			if(ret == 0) {
				break;
			}
			// check Ctrl-C
			if (signal_pending(current)) {
				voy_break();
				return -ERESTARTSYS;
			}
			i++;
			if(i > 10000000) {
				printk("DMA endcheck-1 error\n");
				voy_break();
				return -EFAULT;
			}
		}
	}
	data_size = count;
	// multiple transfer
	if(data_size >= next_size) {
		// until play data is larger than buffer
		while(data_size >= next_size) {
			// buffer 1 is free
			if((next_write == 1)&&(buff_flg1 == 0)) {
				// copy data to VRAM
				copy_from_user((long *)(VOYAGER_SOUND_BUF1+VOYAGER_SOUND_SIZE-next_size),
					(long *)buffer,next_size);
				// buffer1 used
				buff_flg1 = 1;
				next_write = 2;
				// update pointer
				buffer += next_size;
				// decrease remained play data
				data_size -= next_size;
				// set remained buffer size
				next_size = VOYAGER_SOUND_SIZE;
				// kick DMA again when data is not enough
				if(abnml_flg == 1) {
					dma2_queue_buffer(sh7751_dmasound_play_irq,
							VOYAGER_SOUND_BUF1,dma_req[0]);
					dma2_start(sh7751_dmasound_play_irq);
					abnml_flg = 0;
				}
			}
			// buffer 2 is free
			else if((next_write == 2)&&(buff_flg2 == 0)) {
				// copy data to VRAM
				copy_from_user((long *)(VOYAGER_SOUND_BUF2+VOYAGER_SOUND_SIZE-next_size),
					(long *)buffer,next_size);
				//buffer 2 used
				buff_flg2 = 1;
				next_write = 1;
				// update pointer
				buffer += next_size;
				// decrease remained play data
				data_size -= next_size;
				// set remained buffer size
				next_size = VOYAGER_SOUND_SIZE;
				// kick DMA again when data is not enough
				if(abnml_flg == 1) {
					dma2_queue_buffer(sh7751_dmasound_play_irq,
						VOYAGER_SOUND_BUF2,dma_req[1]);
					dma2_start(sh7751_dmasound_play_irq);
					abnml_flg = 0;
				}
			}
			// first time?
			if(first_flg == 0) {
				//buffer 1 is full?(buffer 1 is used at first)
				if(buff_flg1 == 1) {
					// request DMA
					dma2_queue_buffer(sh7751_dmasound_play_irq,
       							VOYAGER_SOUND_BUF1,
       							VOYAGER_SOUND_SIZE/4);
					// start play on 8051
					data = 0x01;
					command_8051(0x07,&data,&data2);
					// start DMA transfer
					dma2_start(sh7751_dmasound_play_irq);
					// set first flag
					first_flg = 1;
				}
			}
			// wait until 2 buffer are full
			if((buff_flg1 == 1)&&(buff_flg2 == 1)) {
				wari_flg = 1;
				i = 0;
				while(wari_flg) {
					// sound play already done?
					if(abnml_flg) {
						break;
					}
					// sleep until buffer is not full
					ret = interruptible_sleep_on_timeout(&int_wq,WAIT_TIMEOUT);
					if(ret == 0) {
						break;
					}
					// check Ctrl-C
					if (signal_pending(current)) {
						voy_break();
						return -ERESTARTSYS;
					}
					i++;
					if(i > 10000000) {
						printk("DMA endcheck-2 error\n");
						voy_break();
						return -EFAULT;
					}
				}
			}
		}
	}
	// play data size is less than remained buffer size
	if((data_size != 0)&&(data_size < next_size)) {
		// buffer 1 is free
		if((next_write == 1)&&(buff_flg1 == 0)) {
			// copy data to VRAM
			copy_from_user((long *)(VOYAGER_SOUND_BUF1+VOYAGER_SOUND_SIZE-next_size),
				(void *)buffer,data_size);
			// reset buffer remain size
			next_size -= data_size;
			// buffer is full?
			if(next_size == 0) {
				//buffer1 used
				buff_flg1 = 1;
				next_write = 2;
				// reset buffer remain size
				next_size = VOYAGER_SOUND_SIZE;
				// kick DMA again when data is not enough
				if(abnml_flg == 1) {
					dma2_queue_buffer(sh7751_dmasound_play_irq,
						VOYAGER_SOUND_BUF1,dma_req[0]);
					dma2_start(sh7751_dmasound_play_irq);
					abnml_flg = 0;
				}
			}
		}
		//buffer 2 is free
		else if((next_write == 2)&&(buff_flg2 == 0)) {
			// copy data to VRAM
			copy_from_user((long *)(VOYAGER_SOUND_BUF2+VOYAGER_SOUND_SIZE-next_size),
				(void *)buffer,data_size);
			// reset buffer remained size
			next_size -= data_size;
			// buffer is full?
			if(next_size == 0) {
				//buffer2 used
				buff_flg2 = 1;
				next_write = 1;
				// reset buffer remained size
				next_size = VOYAGER_SOUND_SIZE;
				// kick DMA again when data is not enough
				if(abnml_flg == 1) {
					dma2_queue_buffer(sh7751_dmasound_play_irq,
						VOYAGER_SOUND_BUF2,dma_req[1]);
					dma2_start(sh7751_dmasound_play_irq);
					abnml_flg = 0;
				}
			}
		}
	}
	// is it first time?
	if(first_flg == 0) {
		// buffer 1 is full(buffer1 is used first at first time)
		if(buff_flg1 == 1) {
			// DMA request
			dma2_queue_buffer(sh7751_dmasound_play_irq,
       					VOYAGER_SOUND_BUF1,
       					VOYAGER_SOUND_SIZE/4);
			// start play on 8051
			data = 0x01;
			command_8051(0x07,&data,&data2);
			dma2_start(sh7751_dmasound_play_irq);
			// set first flag
			first_flg = 1;
		}
	}
	return(count);
}

// polling - not used
static unsigned int voyagergx_poll(struct file *file,
				struct poll_table_struct *wait)
{
	return 0;
}

// mmap - not used
static int voyagergx_mmap(struct file *file, struct vm_area_struct *vma)
{
	return 0;
}

// I/O control
// operation for change data type and volume
static int voyagergx_ioctl(struct inode *inode, struct file *file,
                        unsigned int cmd, unsigned long arg)
{
	struct voyagergx_state *s = (struct voyagergx_state *)file->private_data;
	int	data,data2,rate;


	switch (cmd) {
//Sound Stop control
	case -1:
		voy_break();
		return 0;
	case OSS_GETVERSION:
		return put_user(SOUND_VERSION, (int *) arg);

	case SNDCTL_DSP_SYNC:
		return 0;

	case SNDCTL_DSP_SETDUPLEX:
		return 0;

	case SNDCTL_DSP_GETCAPS:
		return put_user(DSP_CAP_DUPLEX | DSP_CAP_REALTIME |
				DSP_CAP_TRIGGER | DSP_CAP_MMAP, (int *)arg);

	case SNDCTL_DSP_RESET:
		return 0;

	case SNDCTL_DSP_SPEED:		// set sampling rate
		if((*(int *)arg <= 0xbb80)&&(*(int *)arg >= 0x1b80)) {
			setting.rate = *(int *)arg;
		}
		if(setting.ch == 0) {
			rate = setting.rate / 2;
		}
		else {
			rate = setting.rate;
		}
		return setting.rate;

	case SNDCTL_DSP_STEREO:		//set # of play channel(stereo or mono)
		setting.ch = *(int *)arg;
		// set # of channel for 8051
		data = setting.ch;
		command_8051(0x05,&data,&data2);
        	dma2_set_device(sh7751_dmasound_play_irq);
		return(setting.ch);

	case SNDCTL_DSP_CHANNELS:
		return 0;

	case SNDCTL_DSP_GETFMTS:	// set play data format(8bit or 16bit)
		if(setting.fmt == 16) {
			arg = (unsigned long)AFMT_S16_LE;
		}
		else {
			arg = (unsigned long)AFMT_U8;
		}
		return 0;

	case SNDCTL_DSP_SETFMT:		// set play data format(8bit or 16bit)
		if(*(int *)arg == AFMT_S16_LE) {
			setting.fmt = 16;
		}
		else {
			setting.fmt = 8;
		}
		return 0;

	case SNDCTL_DSP_POST:
		return 0;

	case SNDCTL_DSP_GETTRIGGER:
		return 0;

	case SNDCTL_DSP_SETTRIGGER:
		return 0;

	case SNDCTL_DSP_GETOSPACE:
		return 0;

	case SNDCTL_DSP_GETISPACE:
		return 0;

	case SNDCTL_DSP_NONBLOCK:
		return 0;

	case SNDCTL_DSP_GETODELAY:
		return 0;

	case SNDCTL_DSP_GETIPTR:
		return 0;

	case SNDCTL_DSP_GETOPTR:
		return 0;

	case SNDCTL_DSP_GETBLKSIZE:
		return 0;

	case SNDCTL_DSP_SETFRAGMENT:
		return 0;

	case SNDCTL_DSP_SUBDIVIDE:
		return 0;

	case SOUND_PCM_READ_RATE:
		return 0;

	case SOUND_PCM_READ_CHANNELS:
		return 0;

	case SOUND_PCM_READ_BITS:
		return 0;

	case SOUND_PCM_WRITE_FILTER:
	case SNDCTL_DSP_SETSYNCRO:
	case SOUND_PCM_READ_FILTER:
		return -EINVAL;
	}

	return mixdev_ioctl(&s->codec, cmd, arg);
}

// open
// flag initialization
// stop 8051 to play(it will be started by first write)
static int  voyagergx_open(struct inode *inode, struct file *file)
{
struct voyagergx_state *s = &voyagergx_state;
int	data,data2;

	file->private_data = s;

	s->open_mode |= file->f_mode & (FMODE_READ | FMODE_WRITE);
	//head transferflag clear
	first_flg = 0;
	//last transfer flag clear
	last_flg = 0;
	//buffer flag clear
	buff_flg1 = buff_flg2 = 0;
	// buffer 1 is first
	proc_flg = 1;
	// reset buffer remained size
	next_size = VOYAGER_SOUND_SIZE;
	// initialize DMA transfer size
	dma_req[0] = VOYAGER_SOUND_SIZE/4;
	dma_req[1] = VOYAGER_SOUND_SIZE/4;
	// clear break flag
	break_flg = 0;
	// clear cancel play flag
	abnml_flg = 0;
	// initialize # of write buffer
	next_write = 1;
	// start play on 8051
	data = 0x00;
	command_8051(0x07,&data,&data2);

	return 0;
}

// release(close)
// play again if data remains in buffer
// stop DMA
// stop 8051
static int voyagergx_release(struct inode *inode, struct file *file)
{
int	data,data2,i;
int	ret;

	// stop if break flag is set
	if(break_flg) {
		return 0;
	}
	// wait until buffer is not full
	if((buff_flg1 == 1)&&(buff_flg2 == 1)) {
		wari_flg = 1;
		i = 0;
		while(wari_flg) {
			// sound play already done?
			if(abnml_flg) {
				break;
			}
			// sleep until buffer is not full
			ret = interruptible_sleep_on_timeout(&int_wq,WAIT_TIMEOUT);
			if(ret == 0) {
				break;
			}
			// check Ctrl-C
			if (signal_pending(current)) {
				voy_break();
				return -ERESTARTSYS;
			}
			i++;
			if(i > 10000000) {
				printk("DMA endcheck-3 error\n");
				voy_break();
				return -EFAULT;
			}
		}
	}
	// exit if size is less than 4
	if((VOYAGER_SOUND_SIZE - next_size) < 4) {
		voy_break();
		return 0;
	}
	// buffer is not full?
	if(next_size != 0) {
		
		// first time?
		if(first_flg == 0) {
			// no data
			if(next_size == VOYAGER_SOUND_SIZE) {
				// exit
				voy_break();
				return 0;
			}
			// buffer 1 used?
			if(proc_flg == 1) {
      				dma2_queue_buffer(sh7751_dmasound_play_irq,
       						VOYAGER_SOUND_BUF1,
       						(VOYAGER_SOUND_SIZE-next_size)/4);
				// set last judgement flag
				last_flg = 1;
			}
			// start play on 8051
			data = 0x01;
			command_8051(0x07,&data,&data2);
			dma2_start(sh7751_dmasound_play_irq);
		}
		// sound play in case that data is less than a buffer
		else {
			if(proc_flg == 2) {
				dma_req[0] = (VOYAGER_SOUND_SIZE-next_size)/4;
				buff_flg1 = 1;
				// set last judgement flag
				last_flg = 1;
				// kick dma again if data is not enough
				if(abnml_flg == 1) {
					dma2_queue_buffer(sh7751_dmasound_play_irq,
						VOYAGER_SOUND_BUF1,dma_req[0]);
					dma2_start(sh7751_dmasound_play_irq);
					abnml_flg = 0;
				}
			}
			else {
				// DMA request
				dma_req[1] = (VOYAGER_SOUND_SIZE-next_size)/4;
				buff_flg2 = 1;
				// set last  judgement flag
				last_flg = 1;
				// kick dma again if data is not enough
				if(abnml_flg == 1) {
					dma2_queue_buffer(sh7751_dmasound_play_irq,
						VOYAGER_SOUND_BUF2,dma_req[1]);
					dma2_start(sh7751_dmasound_play_irq);
					abnml_flg = 0;
				}
			}
		}
	}
	// wait for all sound play(transfer)
	while(1) {
		if(abnml_flg) {
			break;
		}
		// exit if last judgement flag is 0
		if(last_flg == 0) {
			break;
		}
		wari_flg = 1;
		i = 0;
		while(wari_flg) {
			// sound play already done?
			if(abnml_flg) {
				break;
			}
			// exit if last judgement flag is 0
			if(last_flg == 0) {
				break;
			}
			// sleep until buffer is not full
			ret = interruptible_sleep_on_timeout(&int_wq,WAIT_TIMEOUT);
			if(ret == 0) {
				voy_break();
				return -EFAULT;
			}
			// check Ctrl-C
			if (signal_pending(current)) {
				voy_break();
				return -ERESTARTSYS;
			}
			i++;
			if(i > 10000000) {
				printk("DMA endcheck-last error\n");
				voy_break();
				return -EFAULT;
			}
		}
	}
	// end
	voy_break();

	return 0;
}

static /*const */ struct file_operations voyagergx_audio_fops = {
	owner:		THIS_MODULE,
	llseek:		voyagergx_llseek,
	read:		voyagergx_read,
	write:		voyagergx_write,
	poll:		voyagergx_poll,
	ioctl:		voyagergx_ioctl,
	mmap:		voyagergx_mmap,
	open:		voyagergx_open,
	release:	voyagergx_release,
};


/* --------------------------------------------------------------------- */
MODULE_AUTHOR("Atom Create Engineering Co.,Ltd.");
MODULE_DESCRIPTION("DSP audio and mixer driver for Silicon Motion VoyagerGX audio device"); 
  
/* --------------------------------------------------------------------- */

// probe
// driver initialization
// codec initialization
// check ac97 chip#
static int __devinit voyagergx_probe(void)
{
struct voyagergx_state *s = &voyagergx_state;
int             val;

	memset(s, 0, sizeof(struct voyagergx_state));

	init_MUTEX(&s->open_sem);
	s->codec.private_data = s;
	s->codec.id = 0;
	s->codec.codec_read = rdcodec;
	s->codec.codec_write = wrcodec;
	s->codec.codec_wait = NULL;

	/* register devices */

	if ((s->dev_audio = register_sound_dsp(&voyagergx_audio_fops, -1)) < 0)
		goto err_dev1;
	if ((s->codec.dev_mixer =
	     register_sound_mixer(&voyagergx_mixer_fops, -1)) < 0)
		goto err_dev2;


	/* codec init */
	if (!ac97_probe_codec(&s->codec))
		goto err_dev3;

	s->codec_base_caps = rdcodec(&s->codec, AC97_RESET);
	s->codec_ext_caps = rdcodec(&s->codec, AC97_EXTENDED_ID);
	info("AC'97 Base/Extended ID = %04x/%04x",
	     s->codec_base_caps, s->codec_ext_caps);

	s->codec.supported_mixers |= SOUND_MASK_ALTPCM;
	val = 0x4343;
	mixdev_ioctl(&s->codec, SOUND_MIXER_WRITE_ALTPCM,
		     (unsigned long) &val);
	
	if (!(s->codec_ext_caps & AC97_EXTID_VRA)) {
		// codec does not support VRA
		s->no_vra = 1;
	} else if (!vra) {
		// Boot option says disable VRA
		u16 ac97_extstat = rdcodec(&s->codec, AC97_EXTENDED_STATUS);
		wrcodec(&s->codec, AC97_EXTENDED_STATUS,
			ac97_extstat & ~AC97_EXTSTAT_VRA);
		s->no_vra = 1;
	}
	if (s->no_vra)
		info("no VRA, interpolating and decimating");

	// set 48k for sampling rate
	setting.rate  = 48000;
	wrcodec(&s->codec, 0x2a, 1);
	wrcodec(&s->codec, 0x2c, setting.rate);
	//volume set
	wrcodec(&s->codec, 2, 0);
	wrcodec(&s->codec, 24, 0);
	wrcodec(&s->codec, 22, 0);

	return 0;

 err_dev3:
	unregister_sound_mixer(s->codec.dev_mixer);
 err_dev2:
	unregister_sound_dsp(s->dev_audio);
 err_dev1:
	return -1;
}

// remove procedure
static void __devinit voyagergx_remove(void)
{
	struct voyagergx_state *s = &voyagergx_state;

	if (!s)
		return;
	unregister_sound_dsp(s->dev_audio);
	unregister_sound_mixer(s->codec.dev_mixer);
}

// initilization
static	int __init init_voyagergx(void)
{
unsigned long	value;
int	err;

	info("sakuma@ace-jp.com, built " __TIME__ " on " __DATE__);

	// set GPIO for ac97 & 8051
	value = *(volatile unsigned long *)(GPIO_MUX_LOW);
	value |= GPIO_MUX_LOW_AC97 | GPIO_MUX_LOW_8051;
	*(volatile unsigned long *)(GPIO_MUX_LOW) = value;

	// stop DMA
	dma2_stop(sh7751_dmasound_play_irq);

	//DMA interrupt request
        err = request_dma2(sh7751_dmasound_play_irq, "voyager DMA",do_irq);
        if (err) {
                return 0;
        }

	// enalbe ac97 interrupt
	value = *(volatile unsigned long *)(VOYAGER_INT_MASK);
	value |= VOYAGER_INT_MASK_AC;
	*(volatile unsigned long *)(VOYAGER_INT_MASK) = value;

	// power on ac97
	value = *(volatile unsigned long *)(POWER_MODE0_GATE);
	value |= POWER_MODE0_GATE_AC;
	*(volatile unsigned long *)(POWER_MODE0_GATE) = value;

	// power on ac97
	value = *(volatile unsigned long *)(POWER_MODE1_GATE);
	value |= POWER_MODE1_GATE_AC;
	*(volatile unsigned long *)(POWER_MODE1_GATE) = value;

	// enable ac97
	value = *(volatile unsigned long *)(AC97_CONTROL_STATUS);
	value |= 0x0000000F;
	*(volatile unsigned long *)(AC97_CONTROL_STATUS) = value;
	// wait for a while
	mdelay(2);
	// exit reset
	value &= 0xFFFFFFF9;
	*(volatile unsigned long *)(AC97_CONTROL_STATUS) = value;

	// tag initialization(enable stot1-4)
	value = *(volatile unsigned long *)(AC97_TX_SLOT0);
	value |= 0x0000F800;
	*(volatile unsigned long *)(AC97_TX_SLOT0) = value;

	// mono 16bit 48k
	setting.ch = 0;
	setting.fmt = 16;
	setting.rate = 48000;

	// DMA initialization
        dma2_set_device(sh7751_dmasound_play_irq);

	// 8051 initialization
	init_8051();

	return voyagergx_probe();
}

// unload
static void __exit cleanup_voyagergx(void)
{
	info("unloading");
	voyagergx_remove();
}

module_init(init_voyagergx);
module_exit(cleanup_voyagergx);
MODULE_LICENSE("GPL");
