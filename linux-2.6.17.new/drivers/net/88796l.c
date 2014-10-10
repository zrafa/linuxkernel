#define	NE_EEPROM	0x14

static inline void delay(void);
static unsigned char asix88796_eeprom_getbit(int ioaddr);
static void asix88796_eeprom_send(int ioaddr, int value);
static unsigned short asix88796_eeprom_get(int ioaddr);
static void asix88796_eeprom_setaddr(int ioaddr, int addr);
static void asix88796_eeprom_command(int ioaddr);
static void asix88796_eeprom_read(int ioaddr, unsigned char *buff);

static inline void delay(void)
{
	ctrl_inw(0xa0000000);
}

static unsigned char asix88796_eeprom_getbit(int ioaddr)
{
	unsigned char value;

	outb(inb(ioaddr + NE_EEPROM) & 0x7f, ioaddr + NE_EEPROM);
	delay();
	value = (inb(ioaddr + NE_EEPROM) & 0x40) >> 6;
	outb(inb(ioaddr + NE_EEPROM) | 0x80, ioaddr + NE_EEPROM);
	delay();

	return value;
}

static void asix88796_eeprom_send(int ioaddr, int value)
{
	if (value)
		outb(inb(ioaddr + NE_EEPROM) | 0x20, ioaddr + NE_EEPROM);
	else
		outb(inb(ioaddr + NE_EEPROM) & 0xdf, ioaddr + NE_EEPROM);
	outb(inb(ioaddr + NE_EEPROM) & 0x7f, ioaddr + NE_EEPROM);
	delay();
	outb(inb(ioaddr + NE_EEPROM) | 0x80, ioaddr + NE_EEPROM);
	delay();
	outb(inb(ioaddr + NE_EEPROM) & 0xdf, ioaddr + NE_EEPROM);
}

static unsigned short asix88796_eeprom_get(int ioaddr)
{
	unsigned short value = 0;
	int i;

	asix88796_eeprom_getbit(ioaddr);
	for (i=0; i<16; i++) {
		value <<= 1;
		value |= asix88796_eeprom_getbit(ioaddr);
	}

	return value;
}

static void asix88796_eeprom_setaddr(int ioaddr, int addr)
{
	asix88796_eeprom_send(ioaddr, addr & 0x0080);
	asix88796_eeprom_send(ioaddr, addr & 0x0040);
	asix88796_eeprom_send(ioaddr, addr & 0x0020);
	asix88796_eeprom_send(ioaddr, addr & 0x0010);
	asix88796_eeprom_send(ioaddr, addr & 0x0008);
	asix88796_eeprom_send(ioaddr, addr & 0x0004);
	asix88796_eeprom_send(ioaddr, addr & 0x0002);
	asix88796_eeprom_send(ioaddr, addr & 0x0001);
}

static void asix88796_eeprom_command(int ioaddr)
{
	asix88796_eeprom_send(ioaddr, 0);
	asix88796_eeprom_send(ioaddr, 1);
	asix88796_eeprom_send(ioaddr, 1);
	asix88796_eeprom_send(ioaddr, 0);
}

static void asix88796_eeprom_read(int ioaddr, unsigned char *buff)
{
	int i;
	int addr = 0;
	unsigned short value;

	for (i=0; i<3; i++) {
		outb(inb(ioaddr + NE_EEPROM) | 0x10, ioaddr + NE_EEPROM);
		outb(inb(ioaddr + NE_EEPROM) & 0xdf, ioaddr + NE_EEPROM);
		delay();
		asix88796_eeprom_command(ioaddr);
		asix88796_eeprom_setaddr(ioaddr, addr++);
		value = asix88796_eeprom_get(ioaddr);
		*buff++ = (unsigned char)(value & 0xff);
		*buff++ = (unsigned char)((value >> 8) & 0xff);
		outb(inb(ioaddr + NE_EEPROM) & 0x7f, ioaddr + NE_EEPROM);
		outb(inb(ioaddr + NE_EEPROM) & 0xdf, ioaddr + NE_EEPROM);
		outb(inb(ioaddr + NE_EEPROM) & 0xef, ioaddr + NE_EEPROM);
	}
}
