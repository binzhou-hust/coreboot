/*
 * mtrr.c: setting MTRR to decent values for cache initialization on P6
 *
 * Derived from intel_set_mtrr in intel_subr.c and mtrr.c in linux kernel
 *
 * Copyright 2000 Silicon Integrated System Corporation
 * Copyright 2013 Google Inc.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *
 * Reference: Intel Architecture Software Developer's Manual, Volume 3: System Programming
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <bootstate.h>
#include <console/console.h>
#include <device/device.h>
#include <device/pci_ids.h>
#include <cpu/cpu.h>
#include <cpu/x86/msr.h>
#include <cpu/x86/mtrr.h>
#include <cpu/x86/cache.h>
#include <cpu/x86/lapic.h>
#include <arch/cpu.h>
#include <arch/acpi.h>
#include <memrange.h>
#if CONFIG_X86_AMD_FIXED_MTRRS
#include <cpu/amd/mtrr.h>
#define MTRR_FIXED_WRBACK_BITS (MTRR_READ_MEM | MTRR_WRITE_MEM)
#else
#define MTRR_FIXED_WRBACK_BITS 0
#endif

/* 2 MTRRS are reserved for the operating system */
#define BIOS_MTRRS 6
#define OS_MTRRS   2
#define MTRRS      (BIOS_MTRRS + OS_MTRRS)
/*
 * Static storage size for variable MTRRs. It's sized sufficiently large to
 * handle different types of CPUs. Empirically, 16 variable MTRRs has not
 * yet been observed.
 */
#define NUM_MTRR_STATIC_STORAGE 16

static int total_mtrrs = MTRRS;
static int bios_mtrrs = BIOS_MTRRS;

static void detect_var_mtrrs(void)
{
	msr_t msr;

	msr = rdmsr(MTRR_CAP_MSR);

	total_mtrrs = msr.lo & 0xff;

	if (total_mtrrs > NUM_MTRR_STATIC_STORAGE) {
		printk(BIOS_WARNING,
			"MTRRs detected (%d) > NUM_MTRR_STATIC_STORAGE (%d)\n",
			total_mtrrs, NUM_MTRR_STATIC_STORAGE);
		total_mtrrs = NUM_MTRR_STATIC_STORAGE;
	}
	bios_mtrrs = total_mtrrs - OS_MTRRS;
}

void enable_fixed_mtrr(void)
{
	msr_t msr;

	msr = rdmsr(MTRR_DEF_TYPE_MSR);
	msr.lo |= MTRR_DEF_TYPE_EN | MTRR_DEF_TYPE_FIX_EN;
	wrmsr(MTRR_DEF_TYPE_MSR, msr);
}

static void enable_var_mtrr(unsigned char deftype)
{
	msr_t msr;

	msr = rdmsr(MTRR_DEF_TYPE_MSR);
	msr.lo &= ~0xff;
	msr.lo |= MTRR_DEF_TYPE_EN | deftype;
	wrmsr(MTRR_DEF_TYPE_MSR, msr);
}

/* fms: find most significant bit set, stolen from Linux Kernel Source. */
static inline unsigned int fms(unsigned int x)
{
	int r;

	__asm__("bsrl %1,%0\n\t"
	        "jnz 1f\n\t"
	        "movl $0,%0\n"
	        "1:" : "=r" (r) : "g" (x));
	return r;
}

/* fls: find least significant bit set */
static inline unsigned int fls(unsigned int x)
{
	int r;

	__asm__("bsfl %1,%0\n\t"
	        "jnz 1f\n\t"
	        "movl $32,%0\n"
	        "1:" : "=r" (r) : "g" (x));
	return r;
}

#define MTRR_VERBOSE_LEVEL BIOS_NEVER

/* MTRRs are at a 4KiB granularity. Therefore all address calculations can
 * be done with 32-bit numbers. This allows for the MTRR code to handle
 * up to 2^44 bytes (16 TiB) of address space. */
#define RANGE_SHIFT 12
#define ADDR_SHIFT_TO_RANGE_SHIFT(x) \
	(((x) > RANGE_SHIFT) ? ((x) - RANGE_SHIFT) : RANGE_SHIFT)
#define PHYS_TO_RANGE_ADDR(x) ((x) >> RANGE_SHIFT)
#define RANGE_TO_PHYS_ADDR(x) (((resource_t)(x)) << RANGE_SHIFT)
#define NUM_FIXED_MTRRS (NUM_FIXED_RANGES / RANGES_PER_FIXED_MTRR)

/* The minimum alignment while handling variable MTRR ranges is 64MiB. */
#define MTRR_MIN_ALIGN PHYS_TO_RANGE_ADDR(64 << 20)
/* Helpful constants. */
#define RANGE_1MB PHYS_TO_RANGE_ADDR(1 << 20)
#define RANGE_4GB (1 << (ADDR_SHIFT_TO_RANGE_SHIFT(32)))

/*
 * The default MTRR type selection uses 3 approaches for selecting the
 * optimal number of variable MTRRs.  For each range do 3 calculations:
 *   1. UC as default type with no holes at top of range.
 *   2. UC as default using holes at top of range.
 *   3. WB as default.
 * If using holes is optimal for a range when UC is the default type the
 * tag is updated to direct the commit routine to use a hole at the top
 * of a range.
 */
#define MTRR_ALGO_SHIFT (8)
#define MTRR_TAG_MASK ((1 << MTRR_ALGO_SHIFT) - 1)
/* If the default type is UC use the hole carving algorithm for a range. */
#define MTRR_RANGE_UC_USE_HOLE (1 << MTRR_ALGO_SHIFT)

static inline uint32_t range_entry_base_mtrr_addr(struct range_entry *r)
{
	return PHYS_TO_RANGE_ADDR(range_entry_base(r));
}

static inline uint32_t range_entry_end_mtrr_addr(struct range_entry *r)
{
	return PHYS_TO_RANGE_ADDR(range_entry_end(r));
}

static inline int range_entry_mtrr_type(struct range_entry *r)
{
	return range_entry_tag(r) & MTRR_TAG_MASK;
}

static int filter_vga_wrcomb(struct device *dev, struct resource *res)
{
	/* Only handle PCI devices. */
	if (dev->path.type != DEVICE_PATH_PCI)
		return 0;

	/* Only handle VGA class devices. */
	if (((dev->class >> 8) != PCI_CLASS_DISPLAY_VGA))
		return 0;

	/* Add resource as write-combining in the address space. */
	return 1;
}

static struct memranges *get_physical_address_space(void)
{
	static struct memranges *addr_space;
	static struct memranges addr_space_storage;

	/* In order to handle some chipsets not being able to pre-determine
	 *  uncacheable ranges, such as graphics memory, at resource insertion
	 * time remove uncacheable regions from the cacheable ones. */
	if (addr_space == NULL) {
		struct range_entry *r;
		unsigned long mask;
		unsigned long match;

		addr_space = &addr_space_storage;

		mask = IORESOURCE_CACHEABLE;
		/* Collect cacheable and uncacheable address ranges. The
		 * uncacheable regions take precedence over the  cacheable
		 * regions. */
		memranges_init(addr_space, mask, mask, MTRR_TYPE_WRBACK);
		memranges_add_resources(addr_space, mask, 0,
		                        MTRR_TYPE_UNCACHEABLE);

		/* Handle any write combining resources. Only prefetchable
		 * resources are appropriate for this MTRR type. */
		match = IORESOURCE_PREFETCH;
		mask |= match;
		memranges_add_resources_filter(addr_space, mask, match, MTRR_TYPE_WRCOMB,
		                               filter_vga_wrcomb);

		/* The address space below 4GiB is special. It needs to be
		 * covered entirely by range entries so that MTRR calculations
		 * can be properly done for the full 32-bit address space.
		 * Therefore, ensure holes are filled up to 4GiB as
		 * uncacheable */
		memranges_fill_holes_up_to(addr_space,
		                           RANGE_TO_PHYS_ADDR(RANGE_4GB),
		                           MTRR_TYPE_UNCACHEABLE);

		printk(BIOS_DEBUG, "MTRR: Physical address space:\n");
		memranges_each_entry(r, addr_space)
			printk(BIOS_DEBUG,
			       "0x%016llx - 0x%016llx size 0x%08llx type %ld\n",
			       range_entry_base(r), range_entry_end(r),
			       range_entry_size(r), range_entry_tag(r));
	}

	return addr_space;
}

/* Fixed MTRR descriptor. This structure defines the step size and begin
 * and end (exclusive) address covered by a set of fixed MTRR MSRs.
 * It also describes the offset in byte intervals to store the calculated MTRR
 * type in an array. */
struct fixed_mtrr_desc {
	uint32_t begin;
	uint32_t end;
	uint32_t step;
	int range_index;
	int msr_index_base;
};

/* Shared MTRR calculations. Can be reused by APs. */
static uint8_t fixed_mtrr_types[NUM_FIXED_RANGES];

/* Fixed MTRR descriptors. */
static const struct fixed_mtrr_desc fixed_mtrr_desc[] = {
	{ PHYS_TO_RANGE_ADDR(0x000000), PHYS_TO_RANGE_ADDR(0x080000),
	  PHYS_TO_RANGE_ADDR(64 * 1024), 0, MTRR_FIX_64K_00000 },
	{ PHYS_TO_RANGE_ADDR(0x080000), PHYS_TO_RANGE_ADDR(0x0C0000),
	  PHYS_TO_RANGE_ADDR(16 * 1024), 8, MTRR_FIX_16K_80000 },
	{ PHYS_TO_RANGE_ADDR(0x0C0000), PHYS_TO_RANGE_ADDR(0x100000),
	  PHYS_TO_RANGE_ADDR(4 * 1024), 24, MTRR_FIX_4K_C0000 },
};

static void calc_fixed_mtrrs(void)
{
	static int fixed_mtrr_types_initialized;
	struct memranges *phys_addr_space;
	struct range_entry *r;
	const struct fixed_mtrr_desc *desc;
	const struct fixed_mtrr_desc *last_desc;
	uint32_t begin;
	uint32_t end;
	int type_index;

	if (fixed_mtrr_types_initialized)
		return;

	phys_addr_space = get_physical_address_space();

	/* Set all fixed ranges to uncacheable first. */
	memset(&fixed_mtrr_types[0], MTRR_TYPE_UNCACHEABLE, NUM_FIXED_RANGES);

	desc = &fixed_mtrr_desc[0];
	last_desc = &fixed_mtrr_desc[ARRAY_SIZE(fixed_mtrr_desc) - 1];

	memranges_each_entry(r, phys_addr_space) {
		begin = range_entry_base_mtrr_addr(r);
		end = range_entry_end_mtrr_addr(r);

		if (begin >= last_desc->end)
			break;

		if (end > last_desc->end)
			end = last_desc->end;

		/* Get to the correct fixed mtrr descriptor. */
		while (begin >= desc->end)
			desc++;

		type_index = desc->range_index;
		type_index += (begin - desc->begin) / desc->step;

		while (begin != end) {
			unsigned char type;

			type = range_entry_tag(r);
			printk(MTRR_VERBOSE_LEVEL,
			       "MTRR addr 0x%x-0x%x set to %d type @ %d\n",
			       begin, begin + desc->step, type, type_index);
			if (type == MTRR_TYPE_WRBACK)
				type |= MTRR_FIXED_WRBACK_BITS;
			fixed_mtrr_types[type_index] = type;
			type_index++;
			begin += desc->step;
			if (begin == desc->end)
				desc++;
		}
	}
	fixed_mtrr_types_initialized = 1;
}

static void commit_fixed_mtrrs(void)
{
	int i;
	int j;
	int msr_num;
	int type_index;
	/* 8 ranges per msr. */
	msr_t fixed_msrs[NUM_FIXED_MTRRS];
	unsigned long msr_index[NUM_FIXED_MTRRS];

	memset(&fixed_msrs, 0, sizeof(fixed_msrs));

	msr_num = 0;
	type_index = 0;
	for (i = 0; i < ARRAY_SIZE(fixed_mtrr_desc); i++) {
		const struct fixed_mtrr_desc *desc;
		int num_ranges;

		desc = &fixed_mtrr_desc[i];
		num_ranges = (desc->end - desc->begin) / desc->step;
		for (j = 0; j < num_ranges; j += RANGES_PER_FIXED_MTRR) {
			msr_index[msr_num] = desc->msr_index_base +
				(j / RANGES_PER_FIXED_MTRR);
			fixed_msrs[msr_num].lo |=
				fixed_mtrr_types[type_index++] << 0;
			fixed_msrs[msr_num].lo |=
				fixed_mtrr_types[type_index++] << 8;
			fixed_msrs[msr_num].lo |=
				fixed_mtrr_types[type_index++] << 16;
			fixed_msrs[msr_num].lo |=
				fixed_mtrr_types[type_index++] << 24;
			fixed_msrs[msr_num].hi |=
				fixed_mtrr_types[type_index++] << 0;
			fixed_msrs[msr_num].hi |=
				fixed_mtrr_types[type_index++] << 8;
			fixed_msrs[msr_num].hi |=
				fixed_mtrr_types[type_index++] << 16;
			fixed_msrs[msr_num].hi |=
				fixed_mtrr_types[type_index++] << 24;
			msr_num++;
		}
	}

	for (i = 0; i < ARRAY_SIZE(fixed_msrs); i++)
		printk(BIOS_DEBUG, "MTRR: Fixed MSR 0x%lx 0x%08x%08x\n",
		       msr_index[i], fixed_msrs[i].hi, fixed_msrs[i].lo);

	disable_cache();
	for (i = 0; i < ARRAY_SIZE(fixed_msrs); i++)
		wrmsr(msr_index[i], fixed_msrs[i]);
	enable_cache();
}

void x86_setup_fixed_mtrrs_no_enable(void)
{
	calc_fixed_mtrrs();
	commit_fixed_mtrrs();
}

void x86_setup_fixed_mtrrs(void)
{
	x86_setup_fixed_mtrrs_no_enable();

	printk(BIOS_SPEW, "call enable_fixed_mtrr()\n");
	enable_fixed_mtrr();
}

struct var_mtrr_regs {
	msr_t base;
	msr_t mask;
};

struct var_mtrr_solution {
	int mtrr_default_type;
	int num_used;
	struct var_mtrr_regs regs[NUM_MTRR_STATIC_STORAGE];
};

/* Global storage for variable MTRR solution. */
static struct var_mtrr_solution mtrr_global_solution;

struct var_mtrr_state {
	struct memranges *addr_space;
	int above4gb;
	int address_bits;
	int prepare_msrs;
	int mtrr_index;
	int def_mtrr_type;
	struct var_mtrr_regs *regs;
};

static void clear_var_mtrr(int index)
{
	msr_t msr_val;

	msr_val = rdmsr(MTRR_PHYS_MASK(index));
	msr_val.lo &= ~MTRR_PHYS_MASK_VALID;
	wrmsr(MTRR_PHYS_MASK(index), msr_val);
}

static void prep_var_mtrr(struct var_mtrr_state *var_state,
                          uint32_t base, uint32_t size, int mtrr_type)
{
	struct var_mtrr_regs *regs;
	resource_t rbase;
	resource_t rsize;
	resource_t mask;

	/* Some variable MTRRs are attempted to be saved for the OS use.
	 * However, it's more important to try to map the full address space
	 * properly. */
	if (var_state->mtrr_index >= bios_mtrrs)
		printk(BIOS_WARNING, "Taking a reserved OS MTRR.\n");
	if (var_state->mtrr_index >= total_mtrrs) {
		printk(BIOS_ERR, "ERROR: Not enough MTRRs available! MTRR index"
		       "is %d with %d MTTRs in total.\n",
		       var_state->mtrr_index, total_mtrrs);
		return;
	}

	rbase = base;
	rsize = size;

	rbase = RANGE_TO_PHYS_ADDR(rbase);
	rsize = RANGE_TO_PHYS_ADDR(rsize);
	rsize = -rsize;

	mask = (1ULL << var_state->address_bits) - 1;
	rsize = rsize & mask;

	printk(BIOS_DEBUG, "MTRR: %d base 0x%016llx mask 0x%016llx type %d\n",
	       var_state->mtrr_index, rbase, rsize, mtrr_type);

	regs = &var_state->regs[var_state->mtrr_index];

	regs->base.lo = rbase;
	regs->base.lo |= mtrr_type;
	regs->base.hi = rbase >> 32;

	regs->mask.lo = rsize;
	regs->mask.lo |= MTRR_PHYS_MASK_VALID;
	regs->mask.hi = rsize >> 32;
}

static void calc_var_mtrr_range(struct var_mtrr_state *var_state,
                                uint32_t base, uint32_t size, int mtrr_type)
{
	while (size != 0) {
		uint32_t addr_lsb;
		uint32_t size_msb;
		uint32_t mtrr_size;

		addr_lsb = fls(base);
		size_msb = fms(size);

		/* All MTRR entries need to have their base aligned to the mask
		 * size. The maximum size is calculated by a function of the
		 * min base bit set and maximum size bit set. */
		if (addr_lsb > size_msb)
			mtrr_size = 1 << size_msb;
		else
			mtrr_size = 1 << addr_lsb;

		if (var_state->prepare_msrs)
			prep_var_mtrr(var_state, base, mtrr_size, mtrr_type);

		size -= mtrr_size;
		base += mtrr_size;
		var_state->mtrr_index++;
	}
}

static void calc_var_mtrrs_with_hole(struct var_mtrr_state *var_state,
                                     struct range_entry *r)
{
	uint32_t a1, a2, b1, b2;
	int mtrr_type;
	struct range_entry *next;

	/*
	 * Determine MTRRs based on the following algorithm for the given entry:
	 * +------------------+ b2 = ALIGN_UP(end)
	 * |  0 or more bytes | <-- hole is carved out between b1 and b2
	 * +------------------+ a2 = b1 = end
	 * |                  |
	 * +------------------+ a1 = begin
	 *
	 * Thus, there are 3 sub-ranges to configure variable MTRRs for.
	 */
	mtrr_type = range_entry_mtrr_type(r);

	a1 = range_entry_base_mtrr_addr(r);
	a2 = range_entry_end_mtrr_addr(r);

	/* The end address is under 1MiB. The fixed MTRRs take
	 * precedence over the variable ones. Therefore this range
	 * can be ignored. */
	if (a2 < RANGE_1MB)
		return;

	/* Again, the fixed MTRRs take precedence so the beginning
	 * of the range can be set to 0 if it starts below 1MiB. */
	if (a1 < RANGE_1MB)
		a1 = 0;

	/* If the range starts above 4GiB the processing is done. */
	if (!var_state->above4gb && a1 >= RANGE_4GB)
		return;

	/* Clip the upper address to 4GiB if addresses above 4GiB
	 * are not being processed. */
	if (!var_state->above4gb && a2 > RANGE_4GB)
		a2 = RANGE_4GB;

	next = memranges_next_entry(var_state->addr_space, r);

	b1 = a2;

	/* First check if a1 is >= 4GiB and the current entry is the last
	 * entry. If so perform an optimization of covering a larger range
	 * defined by the base address' alignment. */
	if (a1 >= RANGE_4GB && next == NULL) {
		uint32_t addr_lsb;

		addr_lsb = fls(a1);
		b2 = (1 << addr_lsb) + a1;
		if (b2 >= a2) {
			calc_var_mtrr_range(var_state, a1, b2 - a1, mtrr_type);
			return;
		}
	}

	/* Handle the min alignment roundup case. */
	b2 = ALIGN_UP(a2, MTRR_MIN_ALIGN);

	/* Check against the next range. If the current range_entry is the
	 * last entry then carving a hole is no problem. If the current entry
	 * isn't the last entry then check that the last entry covers the
	 * entire hole range with the default mtrr type. */
	if (next != NULL &&
	    (range_entry_mtrr_type(next) != var_state->def_mtrr_type ||
	     range_entry_end_mtrr_addr(next) < b2)) {
		calc_var_mtrr_range(var_state, a1, a2 - a1, mtrr_type);
		return;
	}

	calc_var_mtrr_range(var_state, a1, b2 - a1, mtrr_type);
	calc_var_mtrr_range(var_state, b1, b2 - b1, var_state->def_mtrr_type);
}

static void calc_var_mtrrs_without_hole(struct var_mtrr_state *var_state,
                                         struct range_entry *r)
{
	uint32_t a1, a2, b1, b2, c1, c2;
	int mtrr_type;

	/*
	 * For each range that meets the non-default type process it in the
	 * following manner:
	 * +------------------+ c2 = end
	 * |  0 or more bytes |
	 * +------------------+ b2 = c1 = ALIGN_DOWN(end)
	 * |                  |
	 * +------------------+ b1 = a2 = ALIGN_UP(begin)
	 * |  0 or more bytes |
	 * +------------------+ a1 = begin
	 *
	 * Thus, there are 3 sub-ranges to configure variable MTRRs for.
	 */
	mtrr_type = range_entry_mtrr_type(r);

	a1 = range_entry_base_mtrr_addr(r);
	c2 = range_entry_end_mtrr_addr(r);

	/* The end address is under 1MiB. The fixed MTRRs take
	 * precedence over the variable ones. Therefore this range
	 * can be ignored. */
	if (c2 < RANGE_1MB)
		return;

	/* Again, the fixed MTRRs take precedence so the beginning
	 * of the range can be set to 0 if it starts below 1MiB. */
	if (a1 < RANGE_1MB)
		a1 = 0;

	/* If the range starts above 4GiB the processing is done. */
	if (!var_state->above4gb && a1 >= RANGE_4GB)
		return;

	/* Clip the upper address to 4GiB if addresses above 4GiB
	 * are not being processed. */
	if (!var_state->above4gb && c2 > RANGE_4GB)
		c2 = RANGE_4GB;

	/* Don't align up or down on the range if it is smaller
	 * than the minimum granularity. */
	if ((c2 - a1) < MTRR_MIN_ALIGN) {
		calc_var_mtrr_range(var_state, a1, c2 - a1, mtrr_type);
		return;
	}

	b1 = a2 = ALIGN_UP(a1, MTRR_MIN_ALIGN);
	b2 = c1 = ALIGN_DOWN(c2, MTRR_MIN_ALIGN);

	calc_var_mtrr_range(var_state, a1, a2 - a1, mtrr_type);
	calc_var_mtrr_range(var_state, b1, b2 - b1, mtrr_type);
	calc_var_mtrr_range(var_state, c1, c2 - c1, mtrr_type);
}

static void __calc_var_mtrrs(struct memranges *addr_space,
                             int above4gb, int address_bits,
                             int *num_def_wb_mtrrs, int *num_def_uc_mtrrs)
{
	int wb_deftype_count;
	int uc_deftype_count;
	struct range_entry *r;
	struct var_mtrr_state var_state;

	/* The default MTRR cacheability type is determined by calculating
	 * the number of MTRRs required for each MTRR type as if it was the
	 * default. */
	var_state.addr_space = addr_space;
	var_state.above4gb = above4gb;
	var_state.address_bits = address_bits;
	var_state.prepare_msrs = 0;

	wb_deftype_count = 0;
	uc_deftype_count = 0;

	/*
	 * For each range do 3 calculations:
	 *   1. UC as default type with no holes at top of range.
	 *   2. UC as default using holes at top of range.
	 *   3. WB as default.
	 * The lowest count is then used as default after totaling all
	 * MTRRs. Note that the optimal algorithm for UC default is marked in
	 * the tag of each range regardless of final decision.  UC takes
	 * precedence in the MTRR architecture. Therefore, only holes can be
	 * used when the type of the region is MTRR_TYPE_WRBACK with
	 * MTRR_TYPE_UNCACHEABLE as the default type.
	 */
	memranges_each_entry(r, var_state.addr_space) {
		int mtrr_type;

		mtrr_type = range_entry_mtrr_type(r);

		if (mtrr_type != MTRR_TYPE_UNCACHEABLE) {
			int uc_hole_count;
			int uc_no_hole_count;

			var_state.def_mtrr_type = MTRR_TYPE_UNCACHEABLE;
			var_state.mtrr_index = 0;

			/* No hole calculation. */
			calc_var_mtrrs_without_hole(&var_state, r);
			uc_no_hole_count = var_state.mtrr_index;

			/* Hole calculation only if type is WB. The 64 number
			 * is a count that is unachievable, thus making it
			 * a default large number in the case of not doing
			 * the hole calculation. */
			uc_hole_count = 64;
			if (mtrr_type == MTRR_TYPE_WRBACK) {
				var_state.mtrr_index = 0;
				calc_var_mtrrs_with_hole(&var_state, r);
				uc_hole_count = var_state.mtrr_index;
			}

			/* Mark the entry with the optimal algorithm. */
			if (uc_no_hole_count < uc_hole_count) {
				uc_deftype_count += uc_no_hole_count;
			} else {
				unsigned long new_tag;

				new_tag = mtrr_type | MTRR_RANGE_UC_USE_HOLE;
				range_entry_update_tag(r, new_tag);
				uc_deftype_count += uc_hole_count;
			}
		}

		if (mtrr_type != MTRR_TYPE_WRBACK) {
			var_state.mtrr_index = 0;
			var_state.def_mtrr_type = MTRR_TYPE_WRBACK;
			calc_var_mtrrs_without_hole(&var_state, r);
			wb_deftype_count += var_state.mtrr_index;
		}
	}
	*num_def_wb_mtrrs = wb_deftype_count;
	*num_def_uc_mtrrs = uc_deftype_count;
}

static int calc_var_mtrrs(struct memranges *addr_space,
                          int above4gb, int address_bits)
{
	int wb_deftype_count = 0;
	int uc_deftype_count = 0;

	__calc_var_mtrrs(addr_space, above4gb, address_bits, &wb_deftype_count,
	                 &uc_deftype_count);

	if (wb_deftype_count > bios_mtrrs && uc_deftype_count > bios_mtrrs) {
		printk(BIOS_DEBUG, "MTRR: Removing WRCOMB type. "
		       "WB/UC MTRR counts: %d/%d > %d.\n",
		       wb_deftype_count, uc_deftype_count, bios_mtrrs);
		memranges_update_tag(addr_space, MTRR_TYPE_WRCOMB,
		                     MTRR_TYPE_UNCACHEABLE);
		__calc_var_mtrrs(addr_space, above4gb, address_bits,
		                 &wb_deftype_count, &uc_deftype_count);
	}

	printk(BIOS_DEBUG, "MTRR: default type WB/UC MTRR counts: %d/%d.\n",
	       wb_deftype_count, uc_deftype_count);

	if (wb_deftype_count < uc_deftype_count) {
		printk(BIOS_DEBUG, "MTRR: WB selected as default type.\n");
		return MTRR_TYPE_WRBACK;
	}
	printk(BIOS_DEBUG, "MTRR: UC selected as default type.\n");
	return MTRR_TYPE_UNCACHEABLE;
}

static void prepare_var_mtrrs(struct memranges *addr_space, int def_type,
				int above4gb, int address_bits,
				struct var_mtrr_solution *sol)
{
	struct range_entry *r;
	struct var_mtrr_state var_state;

	var_state.addr_space = addr_space;
	var_state.above4gb = above4gb;
	var_state.address_bits = address_bits;
	/* Prepare the MSRs. */
	var_state.prepare_msrs = 1;
	var_state.mtrr_index = 0;
	var_state.def_mtrr_type = def_type;
	var_state.regs = &sol->regs[0];

	memranges_each_entry(r, var_state.addr_space) {
		if (range_entry_mtrr_type(r) == def_type)
			continue;

		if (def_type == MTRR_TYPE_UNCACHEABLE &&
		    (range_entry_tag(r) & MTRR_RANGE_UC_USE_HOLE))
			calc_var_mtrrs_with_hole(&var_state, r);
		else
			calc_var_mtrrs_without_hole(&var_state, r);
	}

	/* Update the solution. */
	sol->num_used = var_state.mtrr_index;
}

static void commit_var_mtrrs(const struct var_mtrr_solution *sol)
{
	int i;

	/* Write out the variable MTRRs. */
	disable_cache();
	for (i = 0; i < sol->num_used; i++) {
		wrmsr(MTRR_PHYS_BASE(i), sol->regs[i].base);
		wrmsr(MTRR_PHYS_MASK(i), sol->regs[i].mask);
	}
	/* Clear the ones that are unused. */
	for (; i < total_mtrrs; i++)
		clear_var_mtrr(i);
	enable_var_mtrr(sol->mtrr_default_type);
	enable_cache();

}

void x86_setup_var_mtrrs(unsigned int address_bits, unsigned int above4gb)
{
	static struct var_mtrr_solution *sol = NULL;
	struct memranges *addr_space;

	addr_space = get_physical_address_space();

	if (sol == NULL) {
		sol = &mtrr_global_solution;
		sol->mtrr_default_type =
			calc_var_mtrrs(addr_space, !!above4gb, address_bits);
		prepare_var_mtrrs(addr_space, sol->mtrr_default_type,
				  !!above4gb, address_bits, sol);
	}

	commit_var_mtrrs(sol);
}

void x86_setup_mtrrs(void)
{
	int address_size;

	x86_setup_fixed_mtrrs();
	address_size = cpu_phys_address_size();
	printk(BIOS_DEBUG, "CPU physical address size: %d bits\n",
		address_size);
	/* Always handle addresses above 4GiB. */
	x86_setup_var_mtrrs(address_size, 1);
}

void x86_setup_mtrrs_with_detect(void)
{
	detect_var_mtrrs();
	x86_setup_mtrrs();
}

void x86_mtrr_check(void)
{
	/* Only Pentium Pro and later have MTRR */
	msr_t msr;
	printk(BIOS_DEBUG, "\nMTRR check\n");

	msr = rdmsr(MTRR_DEF_TYPE_MSR);

	printk(BIOS_DEBUG, "Fixed MTRRs   : ");
	if (msr.lo & MTRR_DEF_TYPE_FIX_EN)
		printk(BIOS_DEBUG, "Enabled\n");
	else
		printk(BIOS_DEBUG, "Disabled\n");

	printk(BIOS_DEBUG, "Variable MTRRs: ");
	if (msr.lo & MTRR_DEF_TYPE_EN)
		printk(BIOS_DEBUG, "Enabled\n");
	else
		printk(BIOS_DEBUG, "Disabled\n");

	printk(BIOS_DEBUG, "\n");

	post_code(0x93);
}
