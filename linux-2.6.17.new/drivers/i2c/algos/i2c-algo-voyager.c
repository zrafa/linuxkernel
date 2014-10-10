/* -------------------------------------------------------------------- */
/* i2c-algo-voyagergx.c:                                                */
/* -------------------------------------------------------------------- */
/*
    This program is free software; you can redistribute it and/or modify
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
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <linux/ioport.h>
#include <linux/errno.h>
#include <linux/sched.h>

#include <linux/i2c.h>
#include "i2c-algo-voyager.h"

/* ----- global defines ----------------------------------------------- */
#define DEB(x) if (i2c_debug>=1) x
#define DEB2(x) if (i2c_debug>=2) x
#define DEB3(x) if (i2c_debug>=3) x /* print several statistical values*/
#define DEBPROTO(x) if (i2c_debug>=9) x;
 	/* debug the protocol by showing transferred bits */
#define DEF_TIMEOUT 16

/* debugging - slow down transfer to have a look at the data .. 	*/
/* I use this with two leds&resistors, each one connected to sda,scl 	*/
/* respectively. This makes sure that the algorithm works. Some chips   */
/* might not like this, as they have an internal timeout of some mils	*/


/* ----- global variables ---------------------------------------------	*/

/* module parameters:
 */
static int i2c_debug=0;

/* --- setting states on the bus with the right timing: ---------------	*/

#define iic_outb(adap, reg, val) adap->setiic(adap->data, reg, val)
#define iic_inb(adap, reg) adap->getiic(adap->data, reg)

/* --- other auxiliary functions --------------------------------------	*/

static void iic_start(struct i2c_algo_iic_data *adap)
{
	unsigned char ctl;
 	ctl = iic_inb(adap, I2C_CONTROL);
	ctl = (ctl | I2C_CONTROL_STATUS);
	DEB(printk("iic_start I2C_CONTROL = 0x%x\n", ctl));
	iic_outb(adap,I2C_CONTROL,ctl);
}

static void iic_stop(struct i2c_algo_iic_data *adap)
{
	unsigned char ctl;
	ctl = iic_inb(adap, I2C_CONTROL);
	ctl = (ctl & ~I2C_CONTROL_STATUS);
	DEB(printk("iic_stop I2C_CONTROL = 0x%x\n", ctl));
	iic_outb(adap,I2C_CONTROL,ctl);
}

static void iic_reset(struct i2c_algo_iic_data *adap)
{
	unsigned char ctl;
	ctl = iic_inb(adap, I2C_RESET);
	ctl = (ctl & ~I2C_RESET_ERROR);
	DEB(printk("iic_reset I2C_CONTROL = 0x%x\n", ctl));
	iic_outb(adap,I2C_RESET,ctl);
}


static int wait_for_bb(struct i2c_algo_iic_data *adap)
{
	int timeout = DEF_TIMEOUT;
	char status;

	status = iic_inb(adap, I2C_STATUS);
#ifndef STUB_I2C
	while (timeout-- && (status & I2C_STATUS_BUSY)) {
		udelay(1000); /* How much is this? */
		status = iic_inb(adap, I2C_STATUS);
	}
#endif
	if (timeout<=0) {
		printk(KERN_ERR "Timeout, host is busy (%d)\n",timeout);
		iic_reset(adap);
	}
	return(timeout<=0);
}

/*
 * Puts this process to sleep for a period equal to timeout 
 */
static inline void iic_sleep(unsigned long timeout)
{
	schedule_timeout( timeout * HZ);
}

static int wait_for_pin(struct i2c_algo_iic_data *adap, char *status)
{
	int timeout = DEF_TIMEOUT;

	timeout = wait_for_bb(adap);
	if (timeout) {
  		DEB2(printk("Timeout waiting for host not busy\n");)
  		return -EIO;
	}

	timeout = DEF_TIMEOUT;

	*status = iic_inb(adap, I2C_STATUS);
	while (timeout-- && !(*status & I2C_STATUS_ACK)) {
	   adap->waitforpin();
	   *status = iic_inb(adap, I2C_STATUS);
	}
	if (timeout <= 0)
		return(-1);
	else
		return(0);
}

/* Verify the device we want to talk to on the IIC bus really exists. */
static inline int try_address(struct i2c_algo_iic_data *adap,
		       unsigned int addr, int retries)
{
	int i, ret = -1;
	unsigned char status;

	for (i=0;i<retries;i++) {
		iic_outb(adap, I2C_SADDRESS, addr);
		iic_start(adap);
		if (wait_for_pin(adap, &status) == 0) {
			ret=1;
			break;	/* success! */
		}
		iic_stop(adap);
		udelay(adap->udelay);
	}
	DEB2(if (i) printk("try_address: needed %d retries for 0x%x\n",i,
	                   addr));
	return ret;
}

int iic_sendbytes(struct i2c_adapter *i2c_adap,const char *buf,
                         int count)
{
	struct i2c_algo_iic_data	*adap = i2c_adap->algo_data;
	int				wrcount,timeout,i;
	unsigned char			*addr,status;

	if (count != PACKET_SIZE)
		return -EPROTO;

	iic_outb(adap,I2C_BYTECOUNT,(unsigned char)(count - 1));
	iic_outb(adap,I2C_SADDRESS,(unsigned char)SERIAL_WRITE_ADDR);

	timeout = wait_for_bb(adap);
	if (timeout)
		return -ETIMEDOUT;

	wrcount	= 0;
	addr	= (unsigned char *)I2C_DATA;

	for (i = 0;i < count;i++){
		iic_outb(adap,(int)addr++,buf[wrcount++]);
	}

	iic_start(adap);
		
	/* Wait for transmission to complete */
	timeout = wait_for_pin(adap,&status);
	if (timeout){
		iic_stop(adap);
		printk("iic_sendbytes: %s write timeout.\n", i2c_adap->name);
		return -EREMOTEIO; /* got a better one ?? */
     	}

	iic_stop(adap);

	return wrcount;
}

static int iic_readbytes(struct i2c_adapter *i2c_adap, char *buf, int count,
	int sread)
{
	struct i2c_algo_iic_data	*adap = i2c_adap->algo_data;
	int				rdcount,timeout,i;
	unsigned char			*addr,wk,status;

	iic_outb(adap,I2C_BYTECOUNT,(unsigned char)(PACKET_SIZE - 1));
	iic_outb(adap,I2C_SADDRESS,(unsigned char)SERIAL_READ_ADDR);

	rdcount	= 0;
	addr	= (unsigned char *)I2C_DATA;

	iic_start(adap);

	timeout = wait_for_pin(adap,&status);
	if (timeout){
		iic_stop(adap);
		printk("iic_readbytes: %s read timeout.\n", i2c_adap->name);
		return -EREMOTEIO; /* got a better one ?? */
     	}

	for (i = 0;i < PACKET_SIZE;i++){
		wk = iic_inb(adap,(int)addr++);
		buf[rdcount++] = wk;
	}

	iic_stop(adap);

	return rdcount;
}

/* Whenever we initiate a transaction, the first byte clocked
 * onto the bus after the start condition is the address (7 bit) of the
 * device we want to talk to.  This function manipulates the address specified
 * so that it makes sense to the hardware when written to the IIC peripheral.
 *
 * Note: 10 bit addresses are not supported in this driver, although they are
 * supported by the hardware.  This functionality needs to be implemented.
 */
static inline int iic_doAddress(struct i2c_algo_iic_data *adap,
                                struct i2c_msg *msg, int retries) 
{
	unsigned int addr;
	int ret;

	addr = ( msg->addr << 1 );

	if (iic_inb(adap, I2C_SADDRESS) != addr) {
		iic_outb(adap, I2C_SADDRESS, addr);
		ret = try_address(adap, addr, retries);
		if (ret!=1) {
			printk("iic_doAddress: died at address code.\n");
				return -EREMOTEIO;
		}
	}

	return 0;
}


/* Description: Prepares the controller for a transaction (clearing status
 * registers, data buffers, etc), and then calls either iic_readbytes or
 * iic_sendbytes to do the actual transaction.
 *
 * still to be done: Before we issue a transaction, we should
 * verify that the bus is not busy or in some unknown state.
 */
static int iic_xfer(struct i2c_adapter *i2c_adap,
		    struct i2c_msg msgs[], 
		    int num)
{
	struct i2c_algo_iic_data *adap = i2c_adap->algo_data;
	struct i2c_msg *pmsg;
	int i = 0;
	int ret, timeout;
    
	pmsg = &msgs[i];

	if(!pmsg->len) {
		DEB2(printk("iic_xfer: read/write length is 0\n");)
		return -EIO;
	}

	/* Wait for any pending transfers to complete */
	timeout = wait_for_bb(adap);
	if (timeout) {
		DEB2(printk("iic_xfer: Timeout waiting for host not busy\n");)
		return -EIO;
	}

	/* Load address */
	ret = iic_doAddress(adap, pmsg, i2c_adap->retries);
	if (ret)
		return -EIO;

	DEB3(printk("iic_xfer: Msg %d, addr=0x%x, flags=0x%x, len=%d\n",
		i, msgs[i].addr, msgs[i].flags, msgs[i].len);)

	if (pmsg->flags & I2C_M_RD) {		/* Read */
		ret = iic_readbytes(i2c_adap, pmsg->buf, pmsg->len, 0);
	} else {				/* Write */ 
		udelay(1000);
		ret = iic_sendbytes(i2c_adap, pmsg->buf, pmsg->len);
	}

	if (ret != pmsg->len) {
		DEB3(printk("iic_xfer: error or fail on read/write %d bytes.\n",ret)); 
	} else {
		DEB3(printk("iic_xfer: read/write %d bytes.\n",ret));
	}

	return ret;
}


/* Implements device specific ioctls.  Higher level ioctls can
 * be found in i2c-core.c and are typical of any i2c controller (specifying
 * slave address, timeouts, etc).  These ioctls take advantage of any hardware
 * features built into the controller for which this algorithm-adapter set
 * was written.  These ioctls allow you to take control of the data and clock
 * lines and set the either high or low,
 * similar to a GPIO pin.
 */
static int algo_control(struct i2c_adapter *adapter, 
	unsigned int cmd, unsigned long arg)
{
	struct i2c_iic_msg s_msg;
	char *buf;
	int ret;

	if (cmd == I2C_SREAD) {
		if(copy_from_user(&s_msg, (struct i2c_iic_msg *)arg, 
				sizeof(struct i2c_iic_msg))) 
			return -EFAULT;
		buf = kmalloc(s_msg.len, GFP_KERNEL);
		if (buf== NULL)
			return -ENOMEM;

		ret = iic_readbytes(adapter, buf, s_msg.len, 1);
		if (ret>=0) {
			if(copy_to_user( s_msg.buf, buf, s_msg.len) ) 
				ret = -EFAULT;
		}
		kfree(buf);
	}
	return 0;
}


static u32 iic_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_EMUL | I2C_FUNC_10BIT_ADDR | 
	       I2C_FUNC_PROTOCOL_MANGLING; 
}

/* -----exported algorithm data: -------------------------------------	*/

static struct i2c_algorithm iic_algo = {
	.name		= "VoyagerGX I2C algorithm",
	.id		= I2C_ALGO_VOYAGER,
	.master_xfer	= iic_xfer,
	.algo_control	= algo_control,
	.functionality	= iic_func,
};


/* 
 * registering functions to load algorithms at runtime 
 */
int i2c_voyager_add_bus(struct i2c_adapter *adap)
{
	DEB2(printk("i2c-algo-voyagergx: hw routines for %s registered.\n",
	            adap->name));

	adap->id |= iic_algo.id;
	adap->algo = &iic_algo;

	adap->timeout = 100;	/* default values, should	*/
	adap->retries = 3;		/* be replaced by defines	*/
	adap->flags = 0;

	return i2c_add_adapter(adap);
}


int i2c_voyager_del_bus(struct i2c_adapter *adap)
{
	return i2c_del_adapter(adap);
}

int __init i2c_algo_iic_init (void)
{
	printk(KERN_INFO "VoyagerGX iic (i2c) algorithm module\n");
	return 0;
}

void i2c_algo_iic_exit(void)
{
}

MODULE_AUTHOR("Lineo uSolutions,Inc. <www.lineo.co.jp>");
MODULE_DESCRIPTION("VoyagerGX I2C algorithm");
MODULE_LICENSE("GPL");

MODULE_PARM(i2c_debug,"i");
MODULE_PARM_DESC(i2c_debug,
        "debug level - 0 off; 1 normal; 2,3 more verbose; 9 iic-protocol");

module_init(i2c_algo_iic_init);
module_exit(i2c_algo_iic_exit);
