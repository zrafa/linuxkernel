/*
 * drivers/pcmcia/hd64461_ss.c
 *
 * PCMCIA support for Hitachi HD64461 companion chip
 * by Andriy Skulysh <askulysh@image.kiev.ua> 2002, 2003, 2004
 *
 *	based on hd64461_ss.c by Greg Banks <gbanks@pocketpenguins.com>
 *
 */
#include <linux/config.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/ss.h>
#include <pcmcia/bulkmem.h>
#include <pcmcia/cistpl.h>
#include "cs_internal.h"
#include <asm/io.h>
#include <asm/hd64461.h>
#include <asm/hp6xx/hp6xx.h>

#define MODNAME "HD64461_ss"

#ifdef DEBUG
static int hd64461_pc_debug = 2;

module_param_named(pc_debug, hd64461_pc_debug, int, 0644);

#define DPRINTK(n, args...)		\
do {					\
	if (hd64461_pc_debug >= (n))	\
		printk(args);		\
} while (0)
#else
#define DPRINTK(n, args...)	do { } while (0)
#endif

#define HD64461_PCC(s,reg)						\
	( CONFIG_HD64461_IOBASE-HD64461_STBCR+ ( (s) ? HD64461_PCC1##reg : \
	HD64461_PCC0##reg ) )

typedef struct hd64461_socket_t {
	unsigned int irq;
	unsigned long mem_base;
	socket_state_t state;
	pccard_mem_map mem_maps[MAX_WIN];
	unsigned char IC_memory;
	struct pcmcia_socket socket;
 	u8 cscier;
} hd64461_socket_t;

static hd64461_socket_t hd64461_sockets[CONFIG_HD64461_PCMCIA_SOCKETS];

#define hd64461_sockno(sp)	(sp - hd64461_sockets)

static void hd64461_enable_int(unsigned int irq)
{
	u8 cscier;
	u32 cscier_reg = HD64461_PCC(0, CSCIER);

	cscier = ctrl_inb(cscier_reg);
	cscier &= ~HD64461_PCCCSCIER_IREQE_MASK;
	cscier |= HD64461_PCCCSCIER_IREQE_LEVEL;
	ctrl_outb(cscier, cscier_reg);
}

static void hd64461_disable_int(unsigned int irq)
{
	u8 cscier;
	u32 cscier_reg = HD64461_PCC(0, CSCIER);

	cscier = ctrl_inb(cscier_reg);
	cscier &= ~HD64461_PCCCSCIER_IREQE_MASK;
	ctrl_outb(cscier, cscier_reg);
}

static void hd64461_enable_irq(unsigned int irq)
{
	DPRINTK(3, "hd64461_enable_irq(irq=%d)\n", irq);
	hd64461_enable_int(irq);
}

static void hd64461_disable_irq(unsigned int irq)
{
	DPRINTK(3, "hd64461_disable_irq(irq=%d)\n", irq);
	hd64461_disable_int(irq);
}

static unsigned int hd64461_startup_irq(unsigned int irq)
{
	DPRINTK(3, "hd64461_startup_irq(irq=%d)\n", irq);
	hd64461_enable_irq(irq);
	return 0;
}

static void hd64461_shutdown_irq(unsigned int irq)
{
	DPRINTK(3, "hd64461_shutdown_irq(irq=%d)\n", irq);
	hd64461_disable_irq(irq);
}

static void hd64461_mask_and_ack_irq(unsigned int irq)
{
	DPRINTK(3, "hd64461_mask_and_ack_irq(irq=%d)\n", irq);
	hd64461_disable_irq(irq);
}

static void hd64461_end_irq(unsigned int irq)
{
	DPRINTK(3, "hd64461_end_irq(irq=%d)\n", irq);
	hd64461_enable_irq(irq);
}

static struct hw_interrupt_type hd64461_ss_irq_type = {
	.typename	= "HD64461_SS-IRQ",
	.startup	= hd64461_startup_irq,
	.shutdown	= hd64461_shutdown_irq,
	.enable		= hd64461_enable_irq,
	.disable	= hd64461_disable_irq,
	.ack		= hd64461_mask_and_ack_irq,
	.end		= hd64461_end_irq
};

static int hd64461_set_voltage(int sock, int Vcc, int Vpp)
{
	u8 gcr, scr;
	u16 stbcr;
	u32 gcr_reg = HD64461_PCC(sock, GCR);
	u32 scr_reg = HD64461_PCC(sock, SCR);
	DPRINTK(2, "hd64461_set_voltage(%d, %d, %d)\n", sock, Vcc, Vpp);

	gcr = ctrl_inb(gcr_reg);
	scr = ctrl_inb(scr_reg);

	switch (Vcc) {
	case 0:
		gcr |= HD64461_PCCGCR_VCC0;
		scr |= HD64461_PCCSCR_VCC1;
		break;
	case 33:
		if (sock == 1) {
			gcr &= ~HD64461_PCCGCR_VCC0;
			scr &= ~HD64461_PCCSCR_VCC1;
		} else {
			gcr |= HD64461_PCCGCR_VCC0;
			scr &= ~HD64461_PCCSCR_VCC1;
		}
		break;
	case 50:
		gcr &= ~HD64461_PCCGCR_VCC0;
		scr &= ~HD64461_PCCSCR_VCC1;
		break;
	}

	ctrl_outb(gcr, gcr_reg);
	ctrl_outb(scr, scr_reg);

	stbcr = inw(HD64461_STBCR);

	if (Vcc > 0) {
		stbcr &= ~(sock == 0 ? HD64461_STBCR_SPC0ST :
			   HD64461_STBCR_SPC1ST);
	} else {
		stbcr |= (sock == 0 ? HD64461_STBCR_SPC0ST :
			  HD64461_STBCR_SPC1ST);
	}

	outw(stbcr, HD64461_STBCR);

	return 1;
}

static int hd64461_init(struct pcmcia_socket *s)
{
	u16 gpadr;
	hd64461_socket_t *sp = container_of(s, struct hd64461_socket_t, socket);

	DPRINTK(0, "hd64461_init(%d)\n", s->sock);

	sp->state.Vcc = 0;
	sp->state.Vpp = 0;
	hd64461_set_voltage(s->sock, 0, 0);

	if (mach_is_hp6xx() && s->sock == 0) {
		gpadr = inw(HD64461_GPADR);
		gpadr &= ~HD64461_GPADR_PCMCIA0;
		outw(gpadr, HD64461_GPADR);
	}

	return 0;
}

static int hd64461_suspend(struct pcmcia_socket *s)
{
	u16 gpadr;
	u8 gcr;
	u32 gcr_reg = HD64461_PCC(s->sock, GCR);

	DPRINTK(0, "hd64461_suspend(%d)\n", s->sock);

	gcr = ctrl_inb(gcr_reg);
	gcr &= ~HD64461_PCCGCR_DRVE;
	ctrl_outb(gcr, gcr_reg);
	hd64461_set_voltage(s->sock, 0, 0);

	if ((mach_is_hp6xx())&&(s->sock == 0)) {
		gpadr = inw(HD64461_GPADR);
		gpadr |= HD64461_GPADR_PCMCIA0;
		outw(gpadr, HD64461_GPADR);
	}

	return 0;
}

static int hd64461_get_status(struct pcmcia_socket *s, u32 * value)
{
	u8 isr;
	u32 status = 0;
	hd64461_socket_t *sp = container_of(s, struct hd64461_socket_t, socket);

	isr = ctrl_inb(HD64461_PCC(s->sock, ISR));

	if ((isr & HD64461_PCCISR_PCD_MASK) == 0) {
		status |= SS_DETECT;

		if (sp->IC_memory) {
			switch (isr & HD64461_PCCISR_BVD_MASK) {
			case HD64461_PCCISR_BVD_BATGOOD:
				break;
			case HD64461_PCCISR_BVD_BATWARN:
				status |= SS_BATWARN;
				break;
			default:
				status |= SS_BATDEAD;
				break;
			}

			if (isr & HD64461_PCCISR_READY)
				status |= SS_READY;
			if (isr & HD64461_PCCISR_MWP)
				status |= SS_WRPROT;
		} else {
			if (isr & HD64461_PCCISR_BVD1)
				status |= SS_STSCHG;
		}

		switch (isr & (HD64461_PCCISR_VS2 | HD64461_PCCISR_VS1)) {
		case HD64461_PCCISR_VS1:
			printk(KERN_NOTICE MODNAME
			       ": cannot handle X.XV card, ignored\n");
			status = 0;
			break;
		case 0:
		case HD64461_PCCISR_VS2:
			status |= SS_3VCARD;
			break;
		case HD64461_PCCISR_VS2 | HD64461_PCCISR_VS1:
			break;
		}

		if ((sp->state.Vcc != 0) || (sp->state.Vpp != 0))
			status |= SS_POWERON;
	}
	DPRINTK(0, "hd64461_get_status(%d) = %x\n", s->sock, status);

	*value = status;
	return 0;
}

static int hd64461_set_socket(struct pcmcia_socket *s, socket_state_t * state)
{
	u32 flags;
	u32 changed;
	u8 gcr, cscier;
	hd64461_socket_t *sp = container_of(s, struct hd64461_socket_t, socket);
	u32 gcr_reg = HD64461_PCC(s->sock, GCR);
	u32 cscier_reg = HD64461_PCC(s->sock, CSCIER);

	DPRINTK(0, "%s(sock=%d, flags=%x, csc_mask=%x, Vcc=%d, Vpp=%d, io_irq=%d)\n",
		__FUNCTION__, s->sock, state->flags, state->csc_mask, state->Vcc,
		state->Vpp, state->io_irq);

	local_irq_save(flags);

	if (state->Vpp != sp->state.Vpp || state->Vcc != sp->state.Vcc) {
		if (!hd64461_set_voltage(s->sock, state->Vcc, state->Vpp)) {
    			local_irq_restore(flags);
			return -EINVAL;
		}
	}

	changed = sp->state.csc_mask ^ state->csc_mask;
	cscier = ctrl_inb(cscier_reg);

	if (changed & SS_DETECT) {
		if (state->csc_mask & SS_DETECT)
			cscier |= HD64461_PCCCSCIER_CDE;
		else
			cscier &= ~HD64461_PCCCSCIER_CDE;
	}

	if (changed & SS_READY) {
		if (state->csc_mask & SS_READY)
			cscier |= HD64461_PCCCSCIER_RE;
		else
			cscier &= ~HD64461_PCCCSCIER_RE;
	}

	if (changed & SS_BATDEAD) {
		if (state->csc_mask & SS_BATDEAD)
			cscier |= HD64461_PCCCSCIER_BDE;
		else
			cscier &= ~HD64461_PCCCSCIER_BDE;
	}

	if (changed & SS_BATWARN) {
		if (state->csc_mask & SS_BATWARN)
			cscier |= HD64461_PCCCSCIER_BWE;
		else
			cscier &= ~HD64461_PCCCSCIER_BWE;
	}

	if (changed & SS_STSCHG) {
		if (state->csc_mask & SS_STSCHG)
			cscier |= HD64461_PCCCSCIER_SCE;
		else
			cscier &= ~HD64461_PCCCSCIER_SCE;
	}

	ctrl_outb(cscier, cscier_reg);

	changed = sp->state.flags ^ state->flags;

	gcr = ctrl_inb(gcr_reg);

	if (changed & SS_IOCARD) {
		DPRINTK(0, "card type: %s\n",
			(state->flags & SS_IOCARD ? "i/o" : "memory"));
		if (state->flags & SS_IOCARD) {
			if (s->sock == 1) {
				printk(KERN_ERR
				       "socket 1 can be only IC Memory card\n");
			} else {
				gcr |= HD64461_PCCGCR_PCCT;
				sp->IC_memory = 0;
			}
		} else {
			gcr &= ~HD64461_PCCGCR_PCCT;
			sp->IC_memory = 1;
		}
	}

	if (changed & SS_RESET) {
		DPRINTK(0, "%s reset card\n",
			(state->flags & SS_RESET ? "start" : "stop"));
		if (state->flags & SS_RESET)
			gcr |= HD64461_PCCGCR_PCCR;
		else
			gcr &= ~HD64461_PCCGCR_PCCR;
	}

	if (changed & SS_OUTPUT_ENA) {
		DPRINTK(0, "%sabling card output\n",
			(state->flags & SS_OUTPUT_ENA ? "en" : "dis"));
		if (state->flags & SS_OUTPUT_ENA)
			gcr |= HD64461_PCCGCR_DRVE;
		else
			gcr &= ~HD64461_PCCGCR_DRVE;
	}

	DPRINTK(2, "cscier=%02x ", cscier);
	DPRINTK(2, "gcr=%02x\n", gcr);
	ctrl_outb(gcr, gcr_reg);

	sp->state = *state;

	local_irq_restore(flags);

	return 0;
}

static int hd64461_set_io_map(struct pcmcia_socket *s, struct pccard_io_map *io)
{
	/* this is not needed due to static mappings */
	DPRINTK(0, "hd64461_set_io_map(%d)\n", s->sock);

	return 0;
}

static int hd64461_set_mem_map(struct pcmcia_socket *s,
			       struct pccard_mem_map *mem)
{
	hd64461_socket_t *sp = container_of(s, struct hd64461_socket_t, socket);
	struct pccard_mem_map *smem;
	int map = mem->map;
	unsigned long saddr;

	DPRINTK(0, "%s(sock=%d, map=%d, flags=0x%x,static_start=0x%08lx, card_start=0x%08x)\n",
		__FUNCTION__, s->sock, map, mem->flags, mem->static_start, mem->card_start);

	if (map >= MAX_WIN)
		return -EINVAL;

	smem = &sp->mem_maps[map];
	saddr = sp->mem_base + mem->card_start;

	if (!(mem->flags & MAP_ATTRIB))
		saddr += HD64461_PCC_WINDOW;

	mem->static_start = saddr;
	*smem = *mem;

	return 0;
}

static int hd64461_pcmcia_irq_demux(int irq, void *dev)
{
	hd64461_socket_t *sp = (hd64461_socket_t *) dev;
	unsigned char cscr;
	unsigned cscr_reg = HD64461_PCC(0, CSCR);

	DPRINTK(3, "hd64461_pcmcia_irq_demux(irq= %d - ", irq);

	cscr = ctrl_inb(cscr_reg);
	if (cscr & HD64461_PCCCSCR_IREQ) {
		cscr &= ~HD64461_PCCCSCR_IREQ;
		ctrl_outb(cscr, cscr_reg);
		irq = sp->socket.pci_irq;
	}

	DPRINTK(3, "%d)\n", irq);

	return irq;
}

static irqreturn_t hd64461_interrupt(int irq, void *dev, struct pt_regs *regs)
{
	hd64461_socket_t *sp = (hd64461_socket_t *) dev;
	unsigned events = 0;
	unsigned char cscr;
	unsigned cscr_reg = HD64461_PCC(hd64461_sockno(sp), CSCR);

	cscr = ctrl_inb(cscr_reg);

	DPRINTK(3, "hd64461_interrupt: cscr=%04x irq=%d\n", cscr, irq);

	if (cscr & HD64461_PCCCSCR_CDC) {
		cscr &= ~HD64461_PCCCSCR_CDC;
		events |= SS_DETECT;

		if ((ctrl_inb(HD64461_PCC(hd64461_sockno(sp), ISR)) &
		     HD64461_PCCISR_PCD_MASK) != 0) {
			cscr &= ~(HD64461_PCCCSCR_RC | HD64461_PCCCSCR_BW |
				  HD64461_PCCCSCR_BD | HD64461_PCCCSCR_SC);
		}
	}

	if (sp->IC_memory) {
		if (cscr & HD64461_PCCCSCR_RC) {
			cscr &= ~HD64461_PCCCSCR_RC;
			events |= SS_READY;
		}

		if (cscr & HD64461_PCCCSCR_BW) {
			cscr &= ~HD64461_PCCCSCR_BW;
			events |= SS_BATWARN;
		}

		if (cscr & HD64461_PCCCSCR_BD) {
			cscr &= ~HD64461_PCCCSCR_BD;
			events |= SS_BATDEAD;
		}
	} else {
		if (cscr & HD64461_PCCCSCR_SC) {
			cscr &= ~HD64461_PCCCSCR_SC;
			events |= SS_STSCHG;
		}
	}

	ctrl_outb(cscr, cscr_reg);

	if (events)
		pcmcia_parse_events(&sp->socket, events);

	return IRQ_HANDLED;
}

static struct pccard_operations hd64461_operations = {
	.init = hd64461_init,
	.suspend = hd64461_suspend,
	.get_status = hd64461_get_status,
	.set_socket = hd64461_set_socket,
	.set_io_map = hd64461_set_io_map,
	.set_mem_map = hd64461_set_mem_map,
};

int hd64461_init_socket(int sock, int irq, int io_irq, unsigned long mem_base,
			unsigned short io_offset)
{
	hd64461_socket_t *sp = &hd64461_sockets[sock];
	unsigned gcr_reg = HD64461_PCC(sock, GCR);
	int irq_flags = (sock == 0) ? SA_INTERRUPT : SA_SHIRQ;
	u8 gcr;
	int i, err;

	ctrl_outb(0, HD64461_PCC(sock, CSCIER));

	memset(sp, 0, sizeof(*sp));
	sp->IC_memory = 1;
	sp->irq = irq;
	sp->mem_base = mem_base;
	sp->socket.features =
	    SS_CAP_PCCARD | SS_CAP_STATIC_MAP | SS_CAP_PAGE_REGS;
	sp->socket.resource_ops = &pccard_static_ops;
	sp->socket.map_size = HD64461_PCC_WINDOW;	/* 16MB fixed window size */
	sp->socket.pci_irq = io_irq;
	sp->socket.io_offset = io_offset;
	sp->socket.owner = THIS_MODULE;
	sp->socket.ops = &hd64461_operations;

	for (i = 0; i != MAX_WIN; i++)
		sp->mem_maps[i].map = i;

	if ((err =
	     request_irq(irq, hd64461_interrupt, irq_flags, MODNAME, sp)) < 0) {
		printk(KERN_ERR
		       "HD64461 PCMCIA socket %d: can't request irq %d\n", sock,
		       sp->irq);
		return err;
	}

	if (sock == 0) {
		irq_desc[io_irq].handler = &hd64461_ss_irq_type;
		hd64461_register_irq_demux(sp->irq, hd64461_pcmcia_irq_demux,
					   sp);
	}

	gcr = ctrl_inb(gcr_reg);
	gcr |= HD64461_PCCGCR_PMMOD;	/* 16MB mapping mode */
	gcr &= ~(HD64461_PCCGCR_PA25 | HD64461_PCCGCR_PA24);	/* lowest 16MB of Common */
	ctrl_outb(gcr, gcr_reg);

	return 0;
}

void hd64461_exit_socket(int sock)
{
	hd64461_socket_t *sp = &hd64461_sockets[sock];
	unsigned cscier_reg = HD64461_PCC(sock, CSCIER);

	ctrl_outb(0, cscier_reg);
	hd64461_suspend(&sp->socket);

	if (sp->irq) {
		if (sock == 0)
			hd64461_unregister_irq_demux(sp->irq);
		free_irq(sp->irq, sp);
		if (sock == 0)
			irq_desc[sp->socket.pci_irq].handler = &no_irq_type;
	}
}

static int __devexit hd64461_pcmcia_drv_remove(struct platform_device *dev)
{
	int i;

	for (i = 0; i != CONFIG_HD64461_PCMCIA_SOCKETS; i++) {
		pcmcia_unregister_socket(&hd64461_sockets[i].socket);
		hd64461_exit_socket(i);
	}
	
	return 0;
}

#ifdef CONFIG_PM
static int hd64461_pcmcia_drv_suspend(struct platform_device *dev, pm_message_t state)
{
	int ret = 0;
	int i;
	
	for (i = 0; i != CONFIG_HD64461_PCMCIA_SOCKETS; i++) {
		u32 cscier_reg = HD64461_PCC(i, CSCIER);
		hd64461_sockets[i].cscier = ctrl_inb(cscier_reg);
		ctrl_outb(0, cscier_reg);
		ret = pcmcia_socket_dev_suspend(&dev->dev, state);
	}
 	return ret;
 }
 
static int hd64461_pcmcia_drv_resume(struct platform_device *dev)
{
	int ret = 0;
	int i;
	
	for (i = 0; i != CONFIG_HD64461_PCMCIA_SOCKETS; i++) {
		u32 cscier_reg = HD64461_PCC(i, CSCIER);
		ctrl_outb(hd64461_sockets[i].cscier, cscier_reg);
		ret = pcmcia_socket_dev_resume(&dev->dev);
	}
 	
	return ret;
}
#endif

static struct platform_driver hd64461_pcmcia_driver = {
	.remove = __devexit_p(hd64461_pcmcia_drv_remove),
#ifdef CONFIG_PM
	.suspend = hd64461_pcmcia_drv_suspend,
	.resume = hd64461_pcmcia_drv_resume,
#endif
	.driver		= {
		.name	= "hd64461-pcmcia",
	},
};

static struct platform_device *hd64461_pcmcia_device;

static int __init init_hd64461_ss(void)
{
	int i;

	printk(KERN_INFO "HD64461 PCMCIA bridge.\n");
	if (platform_driver_register(&hd64461_pcmcia_driver))
		return -EINVAL;

	i = hd64461_init_socket(0, HD64461_IRQ_PCC0, HD64461_IRQ_PCC0 + 1,
				HD64461_PCC0_BASE, 0xf000);
	if (i < 0) {
		platform_driver_unregister(&hd64461_pcmcia_driver);
		return i;
	}
#if CONFIG_HD64461_PCMCIA_SOCKETS==2
	i = hd64461_init_socket(1, HD64461_IRQ_PCC1, HD64461_IRQ_PCC1,
				HD64461_PCC1_BASE, 0);
	if (i < 0) {
		platform_driver_unregister(&hd64461_pcmcia_driver);
		return i;
	}
#endif

	hd64461_pcmcia_device = platform_device_register_simple("hd64461-pcmcia", -1, NULL, 0);
	if (IS_ERR(hd64461_pcmcia_device)) {
		platform_driver_unregister(&hd64461_pcmcia_driver);
		return PTR_ERR(hd64461_pcmcia_device);
	}

	for (i = 0; i != CONFIG_HD64461_PCMCIA_SOCKETS; i++) {
		unsigned int ret;
		hd64461_sockets[i].socket.dev.dev = &hd64461_pcmcia_device->dev;
		ret = pcmcia_register_socket(&hd64461_sockets[i].socket);
		if (ret && i)
			pcmcia_unregister_socket(&hd64461_sockets[0].socket);
	}

	return 0;
}

static void __exit exit_hd64461_ss(void)
{
	platform_device_unregister(hd64461_pcmcia_device);
	platform_driver_unregister(&hd64461_pcmcia_driver);
}

module_init(init_hd64461_ss);
module_exit(exit_hd64461_ss);

MODULE_AUTHOR("Andriy Skulysh <askulysh@gmail.com>");
MODULE_DESCRIPTION("PCMCIA driver for Hitachi HD64461 companion chip");
MODULE_LICENSE("GPL");
