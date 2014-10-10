/*
 * snd_sh_dac_audio.c - SuperH DAC audio driver for ALSA
 *
 * Copyright (c) 2007 by Rafael Ignacio Zurita <rizurita@yahoo.com>
 *
 *
 * Based completely on sh_dac_audio.c (Copyright (C) 2004,2005 by Andriy Skulysh)
 * and "Writing an ALSA driver" (Copyright (c) 2002-2005 
 * Takashi Iwai <tiwai@suse.de>)
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <sound/driver.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/info.h>
#include <asm/io.h>

/* from sh_dac_audio.c */
#include <asm/irq.h>
#include <asm/delay.h>
#include <asm/clock.h>
#include <asm/cpu/dac.h>
#include <asm/cpu/timer.h>
#include <asm/machvec.h>
#include <asm/hp6xx/hp6xx.h>
#include <asm/hd64461.h>


MODULE_AUTHOR("Rafael Ignacio Zurita <rizurita@yahoo.com>");
MODULE_DESCRIPTION("SuperH DAC audio driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{SuperH DAC audio support}}");


/* Module Parameters. I am not completely sure of these values */
static int index = -1;
static char *id;
static int enable = 1;
module_param(index, int, 0444);
MODULE_PARM_DESC(index, "Index value for SuperH DAC audio.");
module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for SuperH DAC audio.");
module_param(enable, bool, 0644);
MODULE_PARM_DESC(enable, "Enable SuperH DAC audio.");

/* Simple platform device */
static struct platform_device *pd;
#define SND_SH_DAC_DRIVER "SH_DAC"

#define BUFFER_SIZE 64000
#define TMU_TOCR_INIT   0x00
#define TMU1_TCR_INIT   0x0020  /* Clock/4, rising edge; interrupt on */
#define TMU1_TSTR_INIT  0x02    /* Bit to turn on TMU1 */

#define CONFIG_SOUND_SH_DAC_AUDIO_CHANNEL 1
#define DEBUG 1

/* main struct */
struct snd_sh_dac {
    struct snd_card *card;
    struct snd_pcm_substream *substream;
    int irq;

    /* from sh_dac_audio.c */
    int rate;
    int empty;
    char *data_buffer, *buffer_begin, *buffer_end;
    int in_use, device_major;
    int processed; /* bytes proccesed, to compare with period_size */
    int buffer_size;

};


/* FUNCIONES QUE PROVIENEN DE sh_dac_audio.c */
static void dac_audio_start_timer(void)
{
        u8 tstr;
	#ifdef DEBUG
		printk("start_timer\n");
	#endif
        tstr = ctrl_inb(TMU_TSTR);
        tstr |= TMU1_TSTR_INIT;
        ctrl_outb(tstr, TMU_TSTR);
}

static void dac_audio_stop_timer(void)
{
        u8 tstr;
	#ifdef DEBUG
		printk("stop_timer\n");
	#endif

        tstr = ctrl_inb(TMU_TSTR);
        tstr &= ~TMU1_TSTR_INIT;
        ctrl_outb(tstr, TMU_TSTR);
}

static void dac_audio_reset(struct snd_sh_dac *chip)
{
	#ifdef DEBUG
		printk("reset\n");
	#endif
        dac_audio_stop_timer();

        chip->buffer_begin = chip->buffer_end = chip->data_buffer;
	chip->processed = 0;
        chip->empty = 1;
}

static void dac_audio_sync(struct snd_sh_dac *chip)
{
	#ifdef DEBUG
		printk("sync\n");
	#endif
        while (!chip->empty)
                schedule();
}

static void dac_audio_start(void)
{
	#ifdef DEBUG
		printk("start\n");
	#endif
        if (mach_is_hp6xx()) {
                u16 v = inw(HD64461_GPADR);
                v &= ~HD64461_GPADR_SPEAKER;
                outw(v, HD64461_GPADR);
        }

        sh_dac_enable(CONFIG_SOUND_SH_DAC_AUDIO_CHANNEL);
        ctrl_outw(TMU1_TCR_INIT, TMU1_TCR);
}

static void dac_audio_stop(void)
{
	#ifdef DEBUG
		printk("stop\n");
	#endif
        dac_audio_stop_timer();

        if (mach_is_hp6xx()) {
                u16 v = inw(HD64461_GPADR);
                v |= HD64461_GPADR_SPEAKER;
                outw(v, HD64461_GPADR);
        }

        sh_dac_output(0, CONFIG_SOUND_SH_DAC_AUDIO_CHANNEL);
        sh_dac_disable(CONFIG_SOUND_SH_DAC_AUDIO_CHANNEL);
}

static void dac_audio_set_rate(int rate)
{
        unsigned long interval;
        struct clk *clk;

	#ifdef DEBUG
		printk("set_rate\n");
	#endif
        clk = clk_get("module_clk");
        interval = (clk_get_rate(clk) / 4) / rate;
        clk_put(clk);
        ctrl_outl(interval, TMU1_TCOR);
        ctrl_outl(interval, TMU1_TCNT);
}

/* FIN DE LAS FUNCIONES QUE PROVIENEN DE sh_dac_audio.c */



/* PCM INTERFACE */

static struct snd_pcm_hardware snd_sh_dac_pcm_hw =
{
        .info                   = (SNDRV_PCM_INFO_MMAP |
                                   SNDRV_PCM_INFO_MMAP_VALID |
                                   SNDRV_PCM_INFO_INTERLEAVED | 
                                   SNDRV_PCM_INFO_HALF_DUPLEX),
                                   //SNDRV_PCM_INFO_BLOCK_TRANSFER |
        //.formats                = SNDRV_PCM_FMTBIT_MU_LAW | SNDRV_PCM_FMTBIT_A_LAW,
        //.formats                = SNDRV_PCM_FMTBIT_S8,
        //.formats                = SNDRV_PCM_FORMAT_U8SNDRV_PCM_FMTBIT_U8,
        .formats                = SNDRV_PCM_FMTBIT_U8,
        .rates                  = SNDRV_PCM_RATE_8000,
        .rate_min               = 8000,
        .rate_max               = 8000,
        .channels_min           = 1,
        .channels_max           = 1,
        .buffer_bytes_max       = (48*1024),
        .period_bytes_min       = 1,
        .period_bytes_max       = (48*1024),
        .periods_min            = 1,
        .periods_max            = 1024,
};

/*
 * en copy callback recibo como argumentos a substream -el cual tiene private_data (el cual
 * es la tarjeta, con su irq, etc)-,
 * src y count. Entonces debo tomar del substream el private_data. Ahi voy a tener como componente
 * tal vez el buffer intermedio. A ese buffer intermedio (sea cual sea y venga de donde venga)
 * debo colocarle la data que la aplicacion en espacio de usuario necesita enviar a la
 * tarjeta de sonido. La data esta en src y la cantidad a copiar al buffer intermedio es count.
 * Una funcion util para hacer esto es copy_to_user_fromio()
 *
 * buffer_size = frames_to_bytes(runtime, runtime->buffer_size);
 * period_bytes = frames_to_bytes(runtime, runtime->period_size);
 * 
 * en el interrupt handler debo llamar a snd_pcm_period_elapsed()
 * en copy y silence debo llenar el buffer de acuerdo a la posicion pos
 * en el interrupt handler debo llevar el puntero de lo consumido en el buffer
 * cuando llega al limite el puntero debe inicializarse con el puntero buffer
 */


static int snd_sh_dac_pcm_open(struct snd_pcm_substream *substream)
{
        struct snd_sh_dac *chip = snd_pcm_substream_chip(substream);
        struct snd_pcm_runtime *runtime = substream->runtime;
        int err;

	#ifdef DEBUG
		printk("open\n");
	#endif
        runtime->hw = snd_sh_dac_pcm_hw;
        //spu_enable();

        chip->substream = substream;

        chip->buffer_begin = chip->buffer_end = chip->data_buffer;
	chip->processed = 0;
        chip->empty = 1;
	dac_audio_start();

        return 0;

}

static int snd_sh_dac_pcm_close(struct snd_pcm_substream *substream)
{
        struct snd_sh_dac *chip = snd_pcm_substream_chip(substream);
        struct snd_pcm_runtime *runtime = substream->runtime;

	#ifdef DEBUG
		printk("close\n");
	#endif
	
	dac_audio_sync(chip);
	dac_audio_stop();

        return 0;
}

static int snd_sh_dac_pcm_hw_params(struct snd_pcm_substream
                                     *substream, struct snd_pcm_hw_params
                                     *hw_params)
{
	#ifdef DEBUG
		printk("hw_params\n");
	#endif
        /* Allocate the buffers here using ALSA built-ins perhaps */
        return
            snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int snd_sh_dac_pcm_hw_free(struct snd_pcm_substream
                                   *substream)
{
	#ifdef DEBUG
		printk("hw_free\n");
	#endif
        /* Free the buffer */
        return snd_pcm_lib_free_pages(substream);
}

static int snd_sh_dac_pcm_prepare(struct snd_pcm_substream
                                   *substream)
{
  	struct snd_sh_dac *chip = snd_pcm_substream_chip(substream);
        struct snd_pcm_runtime *runtime = chip->substream->runtime;
	#ifdef DEBUG
		printk("pcm_prepare\n");
	  	printk ("period_size=%i\nperiods=%i\nbuffer_size=%i\n",runtime->period_size,runtime->periods,runtime->buffer_size);
	#endif
	/* I need the buffer_size in bytes, but runtime->buffer_size is in frames.
	 * aqui tal vez deberia inicializar algunos buffers.
         * el problema es que este callback es llamado varias veces 
         * en pausas.
         *
	 * I need the buffer_size in bytes, but runtime->buffer_size is in frames.
	 * Anyway, because in our pocket pc we will use 1 frame = 1 bytes the
	 * initialization below is ok.
	 *
	 * We use always 1 frame = 8 bits * 1 channel = 1 byte
	 * period_size = 4096 frames = 4096 bytes
	 * periods = 8 (periods in a buffer)
	 * period_bytes = 4096 bytes = period_size * bytes_per_frame 
	 * bytes_per_frame = 1 channel * 8 bits (bytes_per_sample) = 1 byte
	 */
	chip->buffer_size=runtime->buffer_size;
        return 0;
}

static int snd_sh_dac_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
  struct snd_sh_dac *chip = snd_pcm_substream_chip(substream);

	#ifdef DEBUG
		printk("pcm_trigger\n");
	#endif
        switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
        	dac_audio_start_timer();
                break;
        case SNDRV_PCM_TRIGGER_STOP:
        	//dac_audio_stop_timer();
                break;
        default:
                return -EINVAL;
        }
        return 0;
}

static int snd_sh_dac_pcm_copy(struct snd_pcm_substream *substream, int channel, snd_pcm_uframes_t pos, void __user *src, snd_pcm_uframes_t count)   /* channel not used (interleaved data) */
{
        struct snd_sh_dac *chip = snd_pcm_substream_chip(substream);
        struct snd_pcm_runtime *runtime = substream->runtime;
	#ifdef DEBUG
		//printk("pcm_copy\n");
	#endif

	if (count < 0)
		return -EINVAL;

	if (!count) {
		dac_audio_sync(chip);
		return 0;
	}

//        if (copy_from_user_toio(chip->data_buffer + pos, src, count))
 //               return -EFAULT;

	memcpy_toio(chip->data_buffer + frames_to_bytes(runtime, pos), src, frames_to_bytes(runtime , count));
	
	/* perhaps i must do buffer_end=0, after buffer_end+=byes(count), after etc.. 
	 * . Or buffer_end=get_hw_ptr(chip)
	 */
	 chip->buffer_end = chip->data_buffer + frames_to_bytes(runtime, pos) + frames_to_bytes(runtime, count);

        if (chip->empty) {
                chip->empty = 0;
                dac_audio_start_timer();
        }

        return 0;
}

static int snd_sh_dac_pcm_silence(struct snd_pcm_substream *substream, int channel, snd_pcm_uframes_t pos, snd_pcm_uframes_t count) /* channel not used (interleaved data) */
{
        struct snd_sh_dac *chip = snd_pcm_substream_chip(substream);
        struct snd_pcm_runtime *runtime = substream->runtime;
	#ifdef DEBUG
		printk("pcm_silence\n");
	#endif

	if (count < 0)
		return -EINVAL;

	if (!count) {
		dac_audio_sync(chip);
		return 0;
	}
	//memset_io(chip->data_buffer + pos, 0, count);
	memset_io(chip->data_buffer + frames_to_bytes(runtime, pos), 0, frames_to_bytes(runtime , count));

	chip->buffer_end = chip->data_buffer + frames_to_bytes(runtime, pos) + frames_to_bytes(runtime, count);

        if (chip->empty) {
                chip->empty = 0;
                dac_audio_start_timer();
        }
	return 0;
}

static int snd_sh_dac_pcm_pointer(struct snd_pcm_substream *substream)
{
  	struct snd_sh_dac *chip = snd_pcm_substream_chip(substream);
  	int pointer = chip->buffer_begin - chip->data_buffer;
	#ifdef DEBUG
		//printk("pcm_pointer. data_buffer=%i  buffer_begin=%i  buffer_end=%i\n",chip->data_buffer, chip->buffer_begin, chip->buffer_end);
	#endif

  	return pointer;
}

/* pcm ops */
static struct snd_pcm_ops snd_sh_dac_pcm_ops = {
        .open =         snd_sh_dac_pcm_open,
        .close =        snd_sh_dac_pcm_close,
        .ioctl =        snd_pcm_lib_ioctl,
        .hw_params =    snd_sh_dac_pcm_hw_params,
        .hw_free =      snd_sh_dac_pcm_hw_free,
        .prepare =      snd_sh_dac_pcm_prepare,
        .trigger =      snd_sh_dac_pcm_trigger,
        .pointer =      snd_sh_dac_pcm_pointer,
        .copy =         snd_sh_dac_pcm_copy,
        .silence =      snd_sh_dac_pcm_silence,
        // .mmap =         snd_sh_dac_pcm_mmap_iomem,    // We perhaps will not use this ops
	.mmap =		snd_pcm_lib_mmap_iomem,
};

static int __devinit snd_sh_dac_pcm(struct snd_sh_dac *chip, int device)
{
    int err;
    struct snd_pcm *pcm;
    #ifdef DEBUG
	printk("pcm\n");
    #endif
    /* device should be always 0 for us */
    err = snd_pcm_new(chip->card, "SH_DAC PCM", device, 1, 0, &pcm);
    if (err < 0)
        return err;
    pcm->private_data = chip;
    strcpy(pcm->name, "SH_DAC PCM");
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_sh_dac_pcm_ops);

    /* buffer size=48K ? */
    snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					  snd_dma_continuous_data(GFP_KERNEL),
							48 * 1024,
							48 * 1024);
    return 0;
}

/* END OF PCM INTERFACE */



/* driver .remove  --  destructor */
static int snd_sh_dac_remove(struct platform_device *devptr)
{
    #ifdef DEBUG
	printk("remove\n");
    #endif
    snd_card_free(platform_get_drvdata(devptr));    
    platform_set_drvdata(devptr, NULL);
    return 0;
}

/* free -- it has been defined by create */
static int snd_sh_dac_dev_free(struct snd_sh_dac *chip)
{
    #ifdef DEBUG
	printk("dev_free\n");
    #endif
    /* disable hardware here if any */

    /* release the irq */
    if (chip->irq >= 0)
            free_irq(chip->irq, (void *)chip);
    /* release the data */
    /* snd_magic_kfree(chip)   // does not work yet */
    kfree(chip);
    return 0;
}

static irqreturn_t snd_sh_dac_interrupt(int irq, void *dev, struct pt_regs *regs)
{
        unsigned long timer_status;
	struct snd_sh_dac *chip = (struct snd_sh_dac *) dev;
        struct snd_pcm_runtime *runtime = chip->substream->runtime;

        timer_status = ctrl_inw(TMU1_TCR);
        timer_status &= ~0x100;
        ctrl_outw(timer_status, TMU1_TCR);

	//udelay(50);

        if (!chip->empty) {

                sh_dac_output(*chip->buffer_begin, CONFIG_SOUND_SH_DAC_AUDIO_CHANNEL);
                chip->buffer_begin++;
		chip->processed++;

		if (chip->processed >= frames_to_bytes(runtime, runtime->period_size)){
			 chip->processed -= frames_to_bytes(runtime, runtime->period_size);
			 snd_pcm_period_elapsed(chip->substream);
			 #ifdef DEBUG
			 	printk("estoy actualizando el buffer pointer\n");
			 #endif
		}	

                if (chip->buffer_begin == (chip->data_buffer + chip->buffer_size - 1)){
                        chip->buffer_begin = chip->data_buffer;
    #ifdef DEBUG
	printk("fin del buffer, volviendo al principio ..interrupt handler\n");
    #endif
    		}
                if (chip->buffer_begin == chip->buffer_end) {
                        chip->empty = 1;
                        dac_audio_stop_timer(); 
		}

        }
	/*else {
			 snd_pcm_period_elapsed(chip->substream);
			 printk("elapsed!");
	} */

        return IRQ_HANDLED;
}


/* create  --  chip-specific constructor for the cards components */
static int __devinit snd_sh_dac_create(struct snd_card *card, struct platform_device *devptr, struct snd_sh_dac **rchip)
{
    struct snd_sh_dac *chip;		
    int err;
    #ifdef DEBUG
	printk("create\n");
    #endif
    static struct snd_device_ops ops = {
           .dev_free = snd_sh_dac_dev_free,
    };

    *rchip = NULL;

    /* check PCI availability (28bit DMA)
     * not implemented because we don't have a pci bus
     */

    /* chip = snd_magic_kcalloc(snd_sh_dac, 0, GFP_KERNEL);   // this code doesn't work :( */
    chip = kzalloc(sizeof(*chip), GFP_KERNEL);
    if (chip == NULL)
            return -ENOMEM;

    /* initialize the stuff */
    chip->card = card;
    chip->irq = -1;

	/* based in sh_dac_audio.c. the lines below sets the hardware rate
         * interrupt. In the old driver, "data_buffer" was a global var. 
         * Now it is a var in the snd_sh_dac struct
         */
        chip->data_buffer = kmalloc(BUFFER_SIZE, GFP_KERNEL);
        if (chip->data_buffer == NULL)
                return -ENOMEM;
        dac_audio_reset(chip);    // was dac_audio_reset();
        chip->rate = 8000;    // was rate = 8000;
        dac_audio_set_rate(chip->rate); // was dac_audio_set_rate(); 

    if (request_irq(TIMER1_IRQ, snd_sh_dac_interrupt, SA_INTERRUPT, "snd_sh_dac", (void *)chip)) {
            snd_sh_dac_dev_free(chip);
            printk(KERN_ERR "cannot grab irq\n");
            return -EBUSY;
    }
    chip->irq = TIMER1_IRQ;

    /* (2) initialization of the chip h/w */
    if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops)) < 0) {
            snd_sh_dac_dev_free(chip);
            return err;
    }
    *rchip = chip;

    return 0;
}       

/* driver .probe  --  constructor */
static int __devinit snd_sh_dac_probe(struct platform_device *devptr){
    /* static int dev;            // we don't need it in our sh3 jornadas i think */
    struct snd_sh_dac *chip;   /* should i change this name? (chip) */
    struct snd_card *card;
    int err;
    #ifdef DEBUG
	printk("probe\n");
    #endif

    /* (1) Check and increment the device index 
     * if (dev >= SNDRV_CARDS)
     *        return -ENODEV;
     * if (!enable[dev]) {
     */
    if (!enable) {
            /* dev++; */
            return -ENOENT;
    }

    /* (2) Create a card instance 
     * card = snd_card_new(index[dev], id[dev], THIS_MODULE, 0);
     */
    card = snd_card_new(index, id, THIS_MODULE, 0);
    if (card == NULL)
            return -ENOMEM;

    /* (3) Create a main component */
    if ((err = snd_sh_dac_create(card, devptr, &chip)) < 0) 
            goto probe_error;

    /* (4) Create other components, (such as mixer, MIDI, etc) */
    if ((err = snd_sh_dac_pcm(chip, 0)) < 0)
            goto probe_error;

    /* (5) Set the driver ID and name strings */
    strcpy(card->driver, "snd_sh_dac");
    strcpy(card->shortname, "SuperH DAC audio driver");
    sprintf(card->longname, "%s at HD64461 irq %i", card->shortname, chip->irq);

    /* (6) Register the card instance */
    if ((err = snd_card_register(card)) < 0) 
            goto probe_error;

    snd_printk("ALSA driver for SuperH DAC audio");

    /* (7) Set the platform driver data and return zero */
    platform_set_drvdata(devptr, card);
    /* dev++; */
    return 0;

probe_error:
            snd_card_free(card);
            return err;
}

/* "driver" definition 
 * I use "platform_driver" 
 */
static struct platform_driver driver = {   
    .probe = snd_sh_dac_probe,
    .remove = snd_sh_dac_remove,
    .driver = {
               .name = SND_SH_DAC_DRIVER,
     },
};

/* clean up the module */
static void __exit sh_dac_exit(void){
    #ifdef DEBUG
	printk("exit\n");
    #endif
    platform_device_unregister(pd);
    platform_driver_unregister(&driver);

    /* the next exits comes from sh_dac_audio.c */
    free_irq(TIMER1_IRQ, 0);
    /* ESTE BUFFER HAY QUE LIBERARLO SI O SI !!! es chip->data_buffer!!
     * o tal vez se libera automaticamente en el remove?
     * kfree((void *)data_buffer);
     */
}

 
static int __init sh_dac_init(void) {
    int err;
    #ifdef DEBUG
	printk("init\n");
    #endif
    err = platform_driver_register(&driver);
    if (unlikely(err < 0)) return err;

    /* pd = platform_device_register(...  Si debo agregar a "pd"
     * entonces debo agregarlo en sh_dac_exit como 
     * platform_device_unregister. Also, i must check the correct
     * values for the arguments in platform_device_register_simple
     */
    pd = platform_device_register_simple(SND_SH_DAC_DRIVER, -1, NULL, 0);
    if (unlikely(IS_ERR(pd))) {
        platform_driver_unregister(&driver);
        return PTR_ERR(pd);
    }

    return 0;
}

module_init(sh_dac_init);
module_exit(sh_dac_exit);

