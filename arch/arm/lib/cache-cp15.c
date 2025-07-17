// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2002
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

#define DEBUG

#include <common.h>
#include <cpu_func.h>
#include <log.h>
#include <asm/global_data.h>
#include <asm/system.h>
#include <asm/cache.h>
#include <linux/compiler.h>
#include <asm/armv7_mpu.h>

#if !(CONFIG_IS_ENABLED(SYS_ICACHE_OFF) && CONFIG_IS_ENABLED(SYS_DCACHE_OFF))

DECLARE_GLOBAL_DATA_PTR;

#ifdef CONFIG_SYS_ARM_MMU
__weak void arm_init_before_mmu(void)
{		
	u32 reg;
	asm volatile("mcr p15, 0, %0, c0, c1, 1"
		: : "r" (reg) : "memory");

#define CPUID_ARM_VIRT_MASK		(0xF << CPUID_ARM_VIRT_SHIFT)
#define CPUID_ARM_VIRT_SHIFT		12
	if ((reg & CPUID_ARM_VIRT_MASK) >> 12 == 1)
		printf("mb: Virt supported\n");
	else
		printf("mb: Virt not supported\n");

	if (is_hyp())
		printf("mb: Processor in HYP mode\n");
}

static int count = 0;
static void set_section_phys(int section, phys_addr_t phys,
			     enum dcache_option option)
{
#ifdef CONFIG_ARMV7_LPAE
	u64 *page_table = (u64 *)gd->arch.tlb_addr;
	/* Need to set the access flag to not fault */
	u64 value = TTB_SECT_AP | TTB_SECT_AF;
#else
	u32 *page_table = (u32 *)gd->arch.tlb_addr;
	u32 value = TTB_SECT_AP;
#endif

	/* Add the page offset */
	value |= phys;

	/* Add caching bits */
	value |= option;

	/* Set PTE */
	page_table[section] = value;

	/* Print only N pages tables */
	if (count++ <= 3) {
		printf("mb: %s(): addr %p: section %d, pa 0x%lx, dcache_option (0x%16llx), descriptor 0x%016llx\n",
			__func__, page_table + section, section, phys,
			(u64)option, value);
		printf("mb: %s(): We have HMAIR[0] or [1], each having 4x blocks [7:0]. Block descriptor points to HMAIR[%llx] and block %llx within it. Read it out\n",
			__func__, (((value >> 2) & 0x7) & 0x4) >> 2, (value >> 2) & 0x3);
		
	}
}

void set_section_dcache(int section, enum dcache_option option)
{
	set_section_phys(section, (u32)section << MMU_SECTION_SHIFT, option);
}

__weak void mmu_page_table_flush(unsigned long start, unsigned long stop)
{
	debug("%s: Warning: not implemented\n", __func__);
}

void mmu_set_region_dcache_behaviour_phys(phys_addr_t start, phys_addr_t phys,
					size_t size, enum dcache_option option)
{
#ifdef CONFIG_ARMV7_LPAE
	u64 *page_table = (u64 *)gd->arch.tlb_addr;
#else
	u32 *page_table = (u32 *)gd->arch.tlb_addr;
#endif
	unsigned long startpt, stoppt;
	unsigned long upto, end;

	/* div by 2 before start + size to avoid phys_addr_t overflow */
	end = ALIGN((start / 2) + (size / 2), MMU_SECTION_SIZE / 2)
	      >> (MMU_SECTION_SHIFT - 1);
	start = start >> MMU_SECTION_SHIFT;

#ifdef CONFIG_ARMV7_LPAE
	debug("%s: start=%pa, size=%zu, option=%llx\n", __func__, &start, size,
	      option);
#else
	debug("%s: start=%pa, size=%zu, option=0x%x\n", __func__, &start, size,
	      option);
#endif
	for (upto = start; upto < end; upto++, phys += MMU_SECTION_SIZE)
		set_section_phys(upto, phys, option);

	/*
	 * Make sure range is cache line aligned
	 * Only CPU maintains page tables, hence it is safe to always
	 * flush complete cache lines...
	 */

	startpt = (unsigned long)&page_table[start];
	startpt &= ~(CONFIG_SYS_CACHELINE_SIZE - 1);
	stoppt = (unsigned long)&page_table[end];
	stoppt = ALIGN(stoppt, CONFIG_SYS_CACHELINE_SIZE);
	mmu_page_table_flush(startpt, stoppt);
}

__weak void dram_bank_mmu_setup(int bank)
{
	struct bd_info *bd = gd->bd;
	int	i;

	/* bd->bi_dram is available only after relocation */
	if ((gd->flags & GD_FLG_RELOC) == 0)
		return;

	debug("%s: bank: %d\n", __func__, bank);
	for (i = bd->bi_dram[bank].start >> MMU_SECTION_SHIFT;
	     i < (bd->bi_dram[bank].start >> MMU_SECTION_SHIFT) +
		 (bd->bi_dram[bank].size >> MMU_SECTION_SHIFT);
	     i++)
		set_section_dcache(i, DCACHE_DEFAULT_OPTION);
}

/* to activate the MMU we need to set up virtual memory: use 1M areas */
static inline void mmu_setup(void)
{
	int i;
	u32 reg = 0;

	/* clear VM bit (aka disable stage-2 translation) */
	{
		u32 hcr;
		asm volatile("mrc p15, 4, %0, c1, c1, 0" : "=r" (hcr));
		hcr &= ~(1<<0);
		asm volatile("mrc p15, 4, %0, c1, c1, 0" : : "r" (hcr));
	}

	printf("mb: %s(): page_tables @ %p\n", __func__, (u64 *)gd->arch.tlb_addr);
	{		
		u32 reg;
		asm volatile("mrc p15, 0, %0, c0, c1, 1"
			: "=r" (reg) : : "memory");

#define CPUID_ARM_VIRT_MASK		(0xF << CPUID_ARM_VIRT_SHIFT)
#define CPUID_ARM_VIRT_SHIFT		12
		if ((reg & CPUID_ARM_VIRT_MASK) >> 12 == 1)
			printf("mb: Virt supported\n");
		else
			printf("mb: Virt not supported\n");

		if (is_hyp())
			printf("mb: Processor in HYP mode\n");
	}

	{
		#define D_MAR1 0x34
		/*#define D_MAR2 (1ULL << 54)*/
		enum marek_type {
			mar1 = D_MAR1,
			mar2 = D_MAR1,
		};
		
		enum marek_type m1 = mar2;

		printf("mb: size of enum-type %d, size of enum-var %d, mar1 0x%x, mar2 0x%016llx\n",
			sizeof(enum marek_type),sizeof(m1),mar1,(unsigned long long)mar2);
	}

	printf("mb: %s():\n"
	      "\t DCACHE_OFF_DEVICE 0x%x\n"
	      "\t DCACHE_WRITEALLOC 0x%x\n"
	      "\t DCACHE_DEFAULT_OPTION 0x%x\n",
	      __func__,
	      DCACHE_OFF_DEVICE, DCACHE_WRITEALLOC, DCACHE_DEFAULT_OPTION);

	printf("mb: %s(): Set up an identity-mapping for all 4GB, DCACHE_OFF, rw for everyone\n", __func__);

	/* Set up an identity-mapping for all 4GB, rw for everyone */
	for (i = 0; i < ((4096ULL * 1024 * 1024) >> MMU_SECTION_SHIFT); i++)
		set_section_dcache(i, DCACHE_OFF_DEVICE);
	count = 0;

	printf("mb: %s(): mmu mapping for CONFIG_NR_DRAM_BANKS\n", __func__);
	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		dram_bank_mmu_setup(i);
		count = 0;
	}


#if defined(CONFIG_ARMV7_LPAE) && __LINUX_ARM_ARCH__ != 4
	/* mb: LPAE
	 * For ours in 2nd level table each entry/block maps 2M. Therefore to map 4G we need
	 * 2048 entries/blocks. However each block is 8 bytes so all together
	 * the page tables occupy 16,384 bytes (=16K). So far tlb_addr points to it.
	 *
	 * However it is 2nd level table (holding 2M blocks) that needs to be pointed
	 * from 1st level table entries that are tables ths time. We need four of them
	 * each pointing to our 4 1G page tables. So what addr to place it? Possibly
	 * just right after the 2nd levl page tables (=tlb_addr + 16K) calculated before.
	 */ 

	/* Set up 4 PTE entries pointing to our 4 1GB page tables */
	for (i = 0; i < 4; i++) {
		u64 *page_table = (u64 *)(gd->arch.tlb_addr + (4096 * 4));
		u64 tpt = gd->arch.tlb_addr + (4096 * i);
		page_table[i] = tpt | TTB_PAGETABLE;
	}
	printf("mb: %s(): we set page_tables 2048x entries (each 8 bytes) over tlb_addr @ %p\n", __func__, (u64 *)(gd->arch.tlb_addr + (4096 * 4)));
	for (i = 0; i < 4; i++)
		printf("0x%016llx ", *(u64 *)(gd->arch.tlb_addr + ((4096 * 4) + i * 8)));
	printf("\n");

	/*reg = TTBCR_EAE;*/
#if defined(CONFIG_SYS_ARM_CACHE_WRITETHROUGH)
	printf("mb: %s(): CONFIG_SYS_ARM_CACHE_WRITETHROUGH\n", __func__);
	reg |= TTBCR_ORGN0_WT | TTBCR_IRGN0_WT;
#elif defined(CONFIG_SYS_ARM_CACHE_WRITEALLOC)
	printf("mb: %s(): CONFIG_SYS_ARM_CACHE_WRITEALLOC\n", __func__);
	reg |= TTBCR_ORGN0_WBWA | TTBCR_IRGN0_WBWA;
#else
#endif

	/*reg |= TTBCR_ORGN0_WBNWA | TTBCR_IRGN0_WBNWA;*/
#define TTBCR_EAE_BIT		(1 << 31)
#define HTCR_RES1			(1 << 31) | (1 << 23)
#define HTCR_SH0_INNER_SHAREABLE	(0x3 << 12)
#define HTCR_RGN0_OUTER_WBA	(0x1 << 10)
#define HTCR_RGN0_INNER_WBA	(0x1 << 8)
	reg = TTBCR_EAE_BIT | HTCR_RES1 | HTCR_SH0_INNER_SHAREABLE | HTCR_RGN0_OUTER_WBA | HTCR_RGN0_INNER_WBA;

	if (!is_hyp()) {
		/* Set HTCR to enable LPAE */
		printf("mb: %s(): set htcr to 0x%x\n", __func__, reg);
		asm volatile("mcr p15, 4, %0, c2, c0, 2"
			: : "r" (reg) : "memory");

		/* Set HTTBR */
		printf("mb: %s(): set httbr to 0x%x\n", __func__, gd->arch.tlb_addr + (4096 * 4));
		asm volatile("mcrr p15, 4, %0, %1, c2"
			: : "r"(gd->arch.tlb_addr + (4096 * 4)), "r"(0)
			: "memory");

		/* Set HMAIR0 and 1*/
		printf("mb: %s(): set hmair0 to 0x%x hmair1 to 0x%x\n",
			__func__, MEMORY_ATTRIBUTES, 0);
		asm volatile("mcr p15, 4, %0, c10, c2, 0"
			: : "r" (MEMORY_ATTRIBUTES) : "memory");
		asm volatile("mcr p15, 4, %0, c10, c2, 1"
			: : "r" (0) : "memory");
		/*cp_delay*/
		{
			volatile int i;
#define nop() __asm__ __volatile__("mov\tr0,r0\t@ nop\n\t");

			/* copro seems to need some delay between reading and writing */
			for (i = 0; i < 100; i++)
				nop();
			asm volatile("" : : : "memory");
		}
		/*read it back*/
		{
			uint32_t hmair0;
			asm volatile("mrc p15, 4, %0, c10, c2, 0" : "=r"(hmair0));
			printf("HMAIR0 = 0x%08x\n", hmair0);
		}
	} else {
		/* Set TTBCR to enable LPAE */
		asm volatile("mcr p15, 0, %0, c2, c0, 2"
			: : "r" (reg) : "memory");
		/* Set 64-bit TTBR0 */
		asm volatile("mcrr p15, 0, %0, %1, c2"
			:
			: "r"(gd->arch.tlb_addr + (4096 * 4)), "r"(0)
			: "memory");
		/* Set MAIR */
		asm volatile("mcr p15, 0, %0, c10, c2, 0"
			: : "r" (MEMORY_ATTRIBUTES) : "memory");
		asm volatile("mcr p15, 0, %0, c10, c2, 1"
			: : "r" (MEMORY_ATTRIBUTES) : "memory");
	}
	/* Copy the page table address to cp15 */
	asm volatile("mcr p15, 0, %0, c2, c0, 0"
		     : : "r" (gd->arch.tlb_addr) : "memory");
#endif
	/*
	 * initial value of Domain Access Control Register (DACR)
	 * Set the access control to client (1U) for each of the 16 domains
	 */
	asm volatile("mcr p15, 0, %0, c3, c0, 0"
		     : : "r" (0x55555555));

	/* Invalidate TLB entries */
	asm volatile("mcr p15, 0, %0, c8, c7, 0"
		     : : "r" (0));

	/*
	 * Ensure all translation table writes have drained into memory, the TLB
	 * invalidation is complete, and translation register writes are
	 * committed before enabling the MMU
	 */
	asm volatile("dsb ish\n\t");
	asm volatile("isb\n\t");

	/* and enable the mmu */
	reg = get_cr();	/* get control reg. */

#define SCTLR_WXN_BIT		(1 << 19)
	set_cr(reg | CR_M | CR_C);
}

static int mmu_enabled(void)
{
	printf("mb: %s()\n", __func__);
	return get_cr() & CR_M;
}
#endif /* CONFIG_SYS_ARM_MMU */

/* cache_bit must be either CR_I or CR_C */
static void cache_enable(uint32_t cache_bit)
{
	uint32_t reg;

	/* The data cache is not active unless the mmu/mpu is enabled too */
#ifdef CONFIG_SYS_ARM_MMU
	if ((cache_bit == CR_C) && !mmu_enabled())
		mmu_setup();
#elif defined(CONFIG_SYS_ARM_MPU)
	if ((cache_bit == CR_C) && !mpu_enabled()) {
		printf("Consider enabling MPU before enabling caches\n");
		return;
	}
#endif
	reg = get_cr();	/* get control reg. */
	set_cr(reg | cache_bit);
}

/* cache_bit must be either CR_I or CR_C */
static void cache_disable(uint32_t cache_bit)
{
	uint32_t reg;

	reg = get_cr();

	if (cache_bit == CR_C) {
		/* if cache isn;t enabled no need to disable */
		if ((reg & CR_C) != CR_C)
			return;
#ifdef CONFIG_SYS_ARM_MMU
		/* if disabling data cache, disable mmu too */
		cache_bit |= CR_M;
#endif
	}
	reg = get_cr();

#ifdef CONFIG_SYS_ARM_MMU
	if (cache_bit == (CR_C | CR_M))
#elif defined(CONFIG_SYS_ARM_MPU)
	if (cache_bit == CR_C)
#endif
		flush_dcache_all();
	set_cr(reg & ~cache_bit);
}
#endif

#if CONFIG_IS_ENABLED(SYS_ICACHE_OFF)
void icache_enable(void)
{
	return;
}

void icache_disable(void)
{
	return;
}

int icache_status(void)
{
	return 0;					/* always off */
}
#else
void icache_enable(void)
{
	cache_enable(CR_I);
}

void icache_disable(void)
{
	cache_disable(CR_I);
}

int icache_status(void)
{
	return (get_cr() & CR_I) != 0;
}
#endif

#if CONFIG_IS_ENABLED(SYS_DCACHE_OFF)
void dcache_enable(void)
{
	return;
}

void dcache_disable(void)
{
	return;
}

int dcache_status(void)
{
	return 0;					/* always off */
}

void mmu_set_region_dcache_behaviour(phys_addr_t start, size_t size,
				     enum dcache_option option)
{
}

#else
void dcache_enable(void)
{
	cache_enable(CR_C);
}

void dcache_disable(void)
{
	cache_disable(CR_C);
}

int dcache_status(void)
{
	return (get_cr() & CR_C) != 0;
}

void mmu_set_region_dcache_behaviour(phys_addr_t start, size_t size,
				     enum dcache_option option)
{
	mmu_set_region_dcache_behaviour_phys(start, start, size, option);
}
#endif
