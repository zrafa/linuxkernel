linuxkernel
===========

Linux kernel drivers and patches I wrote for Jlime and mainline kernel

I was maintaining our best Linux kernels for the HP Palmtop 620lx and 660lx machines.
Some patches were to mainline Linux kernel, so live in www.kernel.org, but some not.

The Linux kernel for these machines are under these directories :
  - jlime-stable.best_so_far/ (it is 2.6.24)
  - linux-2.6.17.new/

Both kernels have the drivers we wrote for those machines, but 2.6.17 is more stable than 2.6.24.

So if you want to build these
kernels you need a sh3 crosscompiler (We used to use gcc-3.4.4-glibc-2.3.5/sh3-unknown-linux-gnu)
These were hp6x SuperH3 architecture, 
