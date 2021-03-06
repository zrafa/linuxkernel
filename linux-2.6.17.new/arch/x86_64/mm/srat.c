/*
 * ACPI 3.0 based NUMA setup
 * Copyright 2004 Andi Kleen, SuSE Labs.
 *
 * Reads the ACPI SRAT table to figure out what memory belongs to which CPUs.
 *
 * Called from acpi_numa_init while reading the SRAT and SLIT tables.
 * Assumes all memory regions belonging to a single proximity domain
 * are in one chunk. Holes between them will be included in the node.
 */

#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/mmzone.h>
#include <linux/bitmap.h>
#include <linux/module.h>
#include <linux/topology.h>
#include <linux/bootmem.h>
#include <linux/mm.h>
#include <asm/proto.h>
#include <asm/numa.h>
#include <asm/e820.h>

#if (defined(CONFIG_ACPI_HOTPLUG_MEMORY) || \
	defined(CONFIG_ACPI_HOTPLUG_MEMORY_MODULE)) \
		&& !defined(CONFIG_MEMORY_HOTPLUG)
#define RESERVE_HOTADD 1
#endif

static struct acpi_table_slit *acpi_slit;

static nodemask_t nodes_parsed __initdata;
static nodemask_t nodes_found __initdata;
static struct bootnode nodes[MAX_NUMNODES] __initdata;
static struct bootnode nodes_add[MAX_NUMNODES] __initdata;
static int found_add_area __initdata;
int hotadd_percent __initdata = 0;
#ifndef RESERVE_HOTADD
#define hotadd_percent 0	/* Ignore all settings */
#endif
static u8 pxm2node[256] = { [0 ... 255] = 0xff };

/* Too small nodes confuse the VM badly. Usually they result
   from BIOS bugs. */
#define NODE_MIN_SIZE (4*1024*1024)

static int node_to_pxm(int n);

int pxm_to_node(int pxm)
{
	if ((unsigned)pxm >= 256)
		return -1;
	/* Extend 0xff to (int)-1 */
	return (signed char)pxm2node[pxm];
}

static __init int setup_node(int pxm)
{
	unsigned node = pxm2node[pxm];
	if (node == 0xff) {
		if (nodes_weight(nodes_found) >= MAX_NUMNODES)
			return -1;
		node = first_unset_node(nodes_found); 
		node_set(node, nodes_found);
		pxm2node[pxm] = node;
	}
	return pxm2node[pxm];
}

static __init int conflicting_nodes(unsigned long start, unsigned long end)
{
	int i;
	for_each_node_mask(i, nodes_parsed) {
		struct bootnode *nd = &nodes[i];
		if (nd->start == nd->end)
			continue;
		if (nd->end > start && nd->start < end)
			return i;
		if (nd->end == end && nd->start == start)
			return i;
	}
	return -1;
}

static __init void cutoff_node(int i, unsigned long start, unsigned long end)
{
	struct bootnode *nd = &nodes[i];

	if (found_add_area)
		return;

	if (nd->start < start) {
		nd->start = start;
		if (nd->end < nd->start)
			nd->start = nd->end;
	}
	if (nd->end > end) {
		nd->end = end;
		if (nd->start > nd->end)
			nd->start = nd->end;
	}
}

static __init void bad_srat(void)
{
	int i;
	printk(KERN_ERR "SRAT: SRAT not used.\n");
	acpi_numa = -1;
	found_add_area = 0;
	for (i = 0; i < MAX_LOCAL_APIC; i++)
		apicid_to_node[i] = NUMA_NO_NODE;
	for (i = 0; i < MAX_NUMNODES; i++)
		nodes_add[i].start = nodes[i].end = 0;
}

static __init inline int srat_disabled(void)
{
	return numa_off || acpi_numa < 0;
}

/*
 * A lot of BIOS fill in 10 (= no distance) everywhere. This messes
 * up the NUMA heuristics which wants the local node to have a smaller
 * distance than the others.
 * Do some quick checks here and only use the SLIT if it passes.
 */
static __init int slit_valid(struct acpi_table_slit *slit)
{
	int i, j;
	int d = slit->localities;
	for (i = 0; i < d; i++) {
		for (j = 0; j < d; j++)  {
			u8 val = slit->entry[d*i + j];
			if (i == j) {
				if (val != 10)
					return 0;
			} else if (val <= 10)
				return 0;
		}
	}
	return 1;
}

/* Callback for SLIT parsing */
void __init acpi_numa_slit_init(struct acpi_table_slit *slit)
{
	if (!slit_valid(slit)) {
		printk(KERN_INFO "ACPI: SLIT table looks invalid. Not used.\n");
		return;
	}
	acpi_slit = slit;
}

/* Callback for Proximity Domain -> LAPIC mapping */
void __init
acpi_numa_processor_affinity_init(struct acpi_table_processor_affinity *pa)
{
	int pxm, node;
	if (srat_disabled())
		return;
	if (pa->header.length != sizeof(struct acpi_table_processor_affinity)) {
		bad_srat();
		return;
	}
	if (pa->flags.enabled == 0)
		return;
	pxm = pa->proximity_domain;
	node = setup_node(pxm);
	if (node < 0) {
		printk(KERN_ERR "SRAT: Too many proximity domains %x\n", pxm);
		bad_srat();
		return;
	}
	apicid_to_node[pa->apic_id] = node;
	acpi_numa = 1;
	printk(KERN_INFO "SRAT: PXM %u -> APIC %u -> Node %u\n",
	       pxm, pa->apic_id, node);
}

#ifdef RESERVE_HOTADD
/*
 * Protect against too large hotadd areas that would fill up memory.
 */
static int hotadd_enough_memory(struct bootnode *nd)
{
	static unsigned long allocated;
	static unsigned long last_area_end;
	unsigned long pages = (nd->end - nd->start) >> PAGE_SHIFT;
	long mem = pages * sizeof(struct page);
	unsigned long addr;
	unsigned long allowed;
	unsigned long oldpages = pages;

	if (mem < 0)
		return 0;
	allowed = (end_pfn - e820_hole_size(0, end_pfn)) * PAGE_SIZE;
	allowed = (allowed / 100) * hotadd_percent;
	if (allocated + mem > allowed) {
		unsigned long range;
		/* Give them at least part of their hotadd memory upto hotadd_percent
		   It would be better to spread the limit out
		   over multiple hotplug areas, but that is too complicated
		   right now */
		if (allocated >= allowed)
			return 0;
		range = allowed - allocated;
		pages = (range / PAGE_SIZE);
		mem = pages * sizeof(struct page);
		nd->end = nd->start + range;
	}
	/* Not completely fool proof, but a good sanity check */
	addr = find_e820_area(last_area_end, end_pfn<<PAGE_SHIFT, mem);
	if (addr == -1UL)
		return 0;
	if (pages != oldpages)
		printk(KERN_NOTICE "SRAT: Hotadd area limited to %lu bytes\n",
			pages << PAGE_SHIFT);
	last_area_end = addr + mem;
	allocated += mem;
	return 1;
}

/*
 * It is fine to add this area to the nodes data it will be used later
 * This code supports one contigious hot add area per node.
 */
static int reserve_hotadd(int node, unsigned long start, unsigned long end)
{
	unsigned long s_pfn = start >> PAGE_SHIFT;
	unsigned long e_pfn = end >> PAGE_SHIFT;
	int changed = 0;
	struct bootnode *nd = &nodes_add[node];

	/* I had some trouble with strange memory hotadd regions breaking
	   the boot. Be very strict here and reject anything unexpected.
	   If you want working memory hotadd write correct SRATs.

	   The node size check is a basic sanity check to guard against
	   mistakes */
	if ((signed long)(end - start) < NODE_MIN_SIZE) {
		printk(KERN_ERR "SRAT: Hotplug area too small\n");
		return -1;
	}

	/* This check might be a bit too strict, but I'm keeping it for now. */
	if (e820_hole_size(s_pfn, e_pfn) != e_pfn - s_pfn) {
		printk(KERN_ERR "SRAT: Hotplug area has existing memory\n");
		return -1;
	}

	if (!hotadd_enough_memory(&nodes_add[node]))  {
		printk(KERN_ERR "SRAT: Hotplug area too large\n");
		return -1;
	}

	/* Looks good */

 	found_add_area = 1;
	if (nd->start == nd->end) {
 		nd->start = start;
 		nd->end = end;
		changed = 1;
 	} else {
 		if (nd->start == end) {
 			nd->start = start;
			changed = 1;
		}
 		if (nd->end == start) {
 			nd->end = end;
			changed = 1;
		}
		if (!changed)
			printk(KERN_ERR "SRAT: Hotplug zone not continuous. Partly ignored\n");
 	}

 	if ((nd->end >> PAGE_SHIFT) > end_pfn)
 		end_pfn = nd->end >> PAGE_SHIFT;

	if (changed)
	 	printk(KERN_INFO "SRAT: hot plug zone found %Lx - %Lx\n", nd->start, nd->end);
	return 0;
}
#endif

/* Callback for parsing of the Proximity Domain <-> Memory Area mappings */
void __init
acpi_numa_memory_affinity_init(struct acpi_table_memory_affinity *ma)
{
	struct bootnode *nd, oldnode;
	unsigned long start, end;
	int node, pxm;
	int i;

	if (srat_disabled())
		return;
	if (ma->header.length != sizeof(struct acpi_table_memory_affinity)) {
		bad_srat();
		return;
	}
	if (ma->flags.enabled == 0)
		return;
 	if (ma->flags.hot_pluggable && hotadd_percent == 0)
		return;
	start = ma->base_addr_lo | ((u64)ma->base_addr_hi << 32);
	end = start + (ma->length_lo | ((u64)ma->length_hi << 32));
	pxm = ma->proximity_domain;
	node = setup_node(pxm);
	if (node < 0) {
		printk(KERN_ERR "SRAT: Too many proximity domains.\n");
		bad_srat();
		return;
	}
	i = conflicting_nodes(start, end);
	if (i == node) {
		printk(KERN_WARNING
		"SRAT: Warning: PXM %d (%lx-%lx) overlaps with itself (%Lx-%Lx)\n",
			pxm, start, end, nodes[i].start, nodes[i].end);
	} else if (i >= 0) {
		printk(KERN_ERR
		       "SRAT: PXM %d (%lx-%lx) overlaps with PXM %d (%Lx-%Lx)\n",
		       pxm, start, end, node_to_pxm(i),
			nodes[i].start, nodes[i].end);
		bad_srat();
		return;
	}
	nd = &nodes[node];
	oldnode = *nd;
	if (!node_test_and_set(node, nodes_parsed)) {
		nd->start = start;
		nd->end = end;
	} else {
		if (start < nd->start)
			nd->start = start;
		if (nd->end < end)
			nd->end = end;
	}

	printk(KERN_INFO "SRAT: Node %u PXM %u %Lx-%Lx\n", node, pxm,
	       nd->start, nd->end);

#ifdef RESERVE_HOTADD
 	if (ma->flags.hot_pluggable && reserve_hotadd(node, start, end) < 0) {
		/* Ignore hotadd region. Undo damage */
		printk(KERN_NOTICE "SRAT: Hotplug region ignored\n");
		*nd = oldnode;
		if ((nd->start | nd->end) == 0)
			node_clear(node, nodes_parsed);
	}
#endif
}

/* Sanity check to catch more bad SRATs (they are amazingly common).
   Make sure the PXMs cover all memory. */
static int nodes_cover_memory(void)
{
	int i;
	unsigned long pxmram, e820ram;

	pxmram = 0;
	for_each_node_mask(i, nodes_parsed) {
		unsigned long s = nodes[i].start >> PAGE_SHIFT;
		unsigned long e = nodes[i].end >> PAGE_SHIFT;
		pxmram += e - s;
		pxmram -= e820_hole_size(s, e);
		pxmram -= nodes_add[i].end - nodes_add[i].start;
		if ((long)pxmram < 0)
			pxmram = 0;
	}

	e820ram = end_pfn - e820_hole_size(0, end_pfn);
	/* We seem to lose 3 pages somewhere. Allow a bit of slack. */
	if ((long)(e820ram - pxmram) >= 1*1024*1024) {
		printk(KERN_ERR
	"SRAT: PXMs only cover %luMB of your %luMB e820 RAM. Not used.\n",
			(pxmram << PAGE_SHIFT) >> 20,
			(e820ram << PAGE_SHIFT) >> 20);
		return 0;
	}
	return 1;
}

static void unparse_node(int node)
{
	int i;
	node_clear(node, nodes_parsed);
	for (i = 0; i < MAX_LOCAL_APIC; i++) {
		if (apicid_to_node[i] == node)
			apicid_to_node[i] = NUMA_NO_NODE;
	}
}

void __init acpi_numa_arch_fixup(void) {}

/* Use the information discovered above to actually set up the nodes. */
int __init acpi_scan_nodes(unsigned long start, unsigned long end)
{
	int i;

	/* First clean up the node list */
	for (i = 0; i < MAX_NUMNODES; i++) {
 		cutoff_node(i, start, end);
		if ((nodes[i].end - nodes[i].start) < NODE_MIN_SIZE) {
			unparse_node(i);
			node_set_offline(i);
		}
	}

	if (acpi_numa <= 0)
		return -1;

	if (!nodes_cover_memory()) {
		bad_srat();
		return -1;
	}

	memnode_shift = compute_hash_shift(nodes, MAX_NUMNODES);
	if (memnode_shift < 0) {
		printk(KERN_ERR
		     "SRAT: No NUMA node hash function found. Contact maintainer\n");
		bad_srat();
		return -1;
	}

	/* Finally register nodes */
	for_each_node_mask(i, nodes_parsed)
		setup_node_bootmem(i, nodes[i].start, nodes[i].end);
	/* Try again in case setup_node_bootmem missed one due
	   to missing bootmem */
	for_each_node_mask(i, nodes_parsed)
		if (!node_online(i))
			setup_node_bootmem(i, nodes[i].start, nodes[i].end);

	for (i = 0; i < NR_CPUS; i++) { 
		if (cpu_to_node[i] == NUMA_NO_NODE)
			continue;
		if (!node_isset(cpu_to_node[i], nodes_parsed))
			numa_set_node(i, NUMA_NO_NODE);
	}
	numa_init_array();
	return 0;
}

static int node_to_pxm(int n)
{
       int i;
       if (pxm2node[n] == n)
               return n;
       for (i = 0; i < 256; i++)
               if (pxm2node[i] == n)
                       return i;
       return 0;
}

void __init srat_reserve_add_area(int nodeid)
{
	if (found_add_area && nodes_add[nodeid].end) {
		u64 total_mb;

		printk(KERN_INFO "SRAT: Reserving hot-add memory space "
				"for node %d at %Lx-%Lx\n",
			nodeid, nodes_add[nodeid].start, nodes_add[nodeid].end);
		total_mb = (nodes_add[nodeid].end - nodes_add[nodeid].start)
					>> PAGE_SHIFT;
		total_mb *= sizeof(struct page);
		total_mb >>= 20;
		printk(KERN_INFO "SRAT: This will cost you %Lu MB of "
				"pre-allocated memory.\n", (unsigned long long)total_mb);
		reserve_bootmem_node(NODE_DATA(nodeid), nodes_add[nodeid].start,
			       nodes_add[nodeid].end - nodes_add[nodeid].start);
	}
}

int __node_distance(int a, int b)
{
	int index;

	if (!acpi_slit)
		return a == b ? 10 : 20;
	index = acpi_slit->localities * node_to_pxm(a);
	return acpi_slit->entry[index + node_to_pxm(b)];
}

EXPORT_SYMBOL(__node_distance);
