#
# Makefile for the VoyagerGX framebuffer driver
#

my-obj-$(CONFIG_FB_VOYAGER_GX_MP)	+= voyager_alphafb.o \
					   voyager_crtcsrfb.o \
					   voyager_crtfb.o \
					   voyager_panelcsrfb.o \
					   voyager_valphafb.o \
					   voyager_videofb.o

obj-$(CONFIG_FB_VOYAGER_GX)		+= voyager_gxfb.o $(my-obj-y)

