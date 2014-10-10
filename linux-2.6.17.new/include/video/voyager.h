#define	VOY_REG_TOP	0xb3e00000	//voyager register
#define	VOY_VRAM_TOP0	0xb0400000	//PANEL PLANE
#define	VOY_VRAM_TOP1	0xb04a0000	//VIDEO PLANE 0
#define	VOY_VRAM_TOP2	0xb0540000	//VIDEO PLANE 1
#define	VOY_VRAM_TOP3	0xb05e0000	//VIDEO ALPHA PLANE
#define	VOY_VRAM_TOP4	0xb0680000	//ALPHA PLANE
#define	VOY_VRAM_TOP5	0xb0720000	//PANEL CURSOR PLANE
#define	VOY_VRAM_TOP6	0xb0730000	//CRT PLANE
#define	VOY_VRAM_TOP7	0xb07d0000	//CRT CURSOR PLANE

#define XRES 640
#define YRES 480
#define BPP  16

#define CSR_XRES 64
#define CSR_YRES 64
#define CSR_BPP  2

#define MAX_PIXEL_MEM_SIZE ((XRES * YRES * BPP) / 8)
#define LINE_LENGTH        ((XRES * BPP) / 8)
#define MAX_FRAMEBUFFER_MEM_SIZE (MAX_PIXEL_MEM_SIZE)
#define MAX_HWC_MEM_SIZE ((CSR_XRES * CSR_YRES * CSR_BPP) / 8)
#define ALLOCATED_FB_MEM_SIZE \
	(PAGE_ALIGN(MAX_FRAMEBUFFER_MEM_SIZE + PAGE_SIZE))

#define	VOYAGER_IOCTL_DEBUG_ADD		0x00
#define	VOYAGER_IOCTL_DEBUG_GET		0x01
#define	VOYAGER_IOCTL_DEBUG_PUT		0x02
#define	VOYAGER_IOCTL_ENABLE		0x10
#define	VOYAGER_IOCTL_ENABLE_CK		0x11
#define	VOYAGER_IOCTL_ENABLE_CP		0x12
#define	VOYAGER_IOCTL_ENABLE_AL		0x13
#define	VOYAGER_IOCTL_SCALE		0x20
#define	VOYAGER_IOCTL_CHKEY		0x30
#define	VOYAGER_IOCTL_COLOR_1		0x40
#define	VOYAGER_IOCTL_COLOR_2		0x41
#define	VOYAGER_IOCTL_TYPE		0x50
#define	VOYAGER_IOCTL_SELECT		0x51
#define	VOYAGER_IOCTL_ALPHA		0x60
