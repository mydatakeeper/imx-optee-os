// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2017 NXP
 *
 */
#include <arm.h>
#include <console.h>
#include <drivers/imx_uart.h>
#include <io.h>
#include <imx_pm.h>
#include <imx.h>
#include <kernel/generic_boot.h>
#include <kernel/interrupt.h>
#include <kernel/misc.h>
#include <kernel/panic.h>
#include <kernel/pm_stubs.h>
#include <kernel/spinlock.h>
#include <kernel/cache_helpers.h>
#include <mm/core_mmu.h>
#include <mm/core_memprot.h>
#include <platform_config.h>
#include <stdint.h>
#include <sm/optee_smc.h>
#include <sm/psci.h>
#include <sm/sm.h>
#include <sm/pm.h>
#include <string.h>
#include <tee/entry_std.h>
#include <tee/entry_fast.h>
#include <util.h>

/*
 * cpuidle and suspend use the same one,
 * because lowpower idle and suspend can not reach at the same time
 */

int imx7d_cpuidle_init(void)
{
	uint32_t lowpower_idle_ocram_base = (uint32_t)phys_to_virt(
			imx_get_ocram_tz_start_addr() +
			LOWPOWER_IDLE_OCRAM_OFFSET, MEM_AREA_TEE_COHERENT);
	struct imx7_cpuidle_pm_info *p =
		(struct imx7_cpuidle_pm_info *)lowpower_idle_ocram_base;

	dcache_op_level1(DCACHE_OP_CLEAN_INV);

	p->va_base = lowpower_idle_ocram_base;
	p->pa_base = imx_get_ocram_tz_start_addr() + LOWPOWER_IDLE_OCRAM_OFFSET;
	p->tee_resume = (paddr_t)virt_to_phys((void *)(vaddr_t)v7_cpu_resume);
	p->pm_info_size = sizeof(*p);
	p->ddrc_va_base = core_mmu_get_va(DDRC_BASE, MEM_AREA_IO_SEC);
	p->ddrc_pa_base = DDRC_BASE;
	p->ccm_va_base = core_mmu_get_va(CCM_BASE, MEM_AREA_IO_SEC);
	p->ccm_pa_base = CCM_BASE;
	p->anatop_va_base = core_mmu_get_va(ANATOP_BASE, MEM_AREA_IO_SEC);
	p->anatop_pa_base = ANATOP_BASE;
	p->src_va_base = core_mmu_get_va(SRC_BASE, MEM_AREA_IO_SEC);
	p->src_pa_base = SRC_BASE;
	p->iomuxc_gpr_va_base = core_mmu_get_va(IOMUXC_GPR_BASE,
					MEM_AREA_IO_SEC);
	p->iomuxc_gpr_pa_base = IOMUXC_GPR_BASE;
	p->gpc_va_base = core_mmu_get_va(GPC_BASE, MEM_AREA_IO_SEC);
	p->gpc_pa_base = GPC_BASE;
	p->gic_va_base = core_mmu_get_va(GIC_BASE, MEM_AREA_IO_SEC);
	p->gic_pa_base = GIC_BASE;

	p->num_lpi_cpus = 0;
	p->num_online_cpus = -1;

	memcpy((void *)(lowpower_idle_ocram_base + sizeof(*p)),
		(void *)(vaddr_t)imx7d_low_power_idle,
	       LOWPOWER_IDLE_OCRAM_SIZE - sizeof(*p));

	dcache_clean_range((void *)lowpower_idle_ocram_base,
			   LOWPOWER_IDLE_OCRAM_SIZE);
	/*
	 * Note that IRAM IOSEC map, if changed to MEM map,
	 * need to flush cache
	 */
	icache_inv_all();

	return 0;
}

static int lowpoweridle_init;

static void atomic_set(volatile uint32_t *p, uint32_t v)
{
	dmb();
	*p = v;
}

static uint32_t atomic_read(volatile uint32_t *p)
{
	uint32_t v;

	v = *p;
	dmb();
	return v;
}

static void imx_pen_lock(uint32_t cpu)
{
	uint32_t cpuidle_ocram_base = (uint32_t)phys_to_virt(
					imx_get_ocram_tz_start_addr() +
					LOWPOWER_IDLE_OCRAM_OFFSET,
					MEM_AREA_TEE_COHERENT);
	struct imx7_cpuidle_pm_info *p =
			(struct imx7_cpuidle_pm_info *)cpuidle_ocram_base;

	if (cpu == 0) {
		atomic_set(&p->flag0, 1);
		dsb();
		atomic_set(&p->val, cpu);
		do {
			dsb();
		} while (atomic_read(&p->flag1) == 1
			&& atomic_read(&p->val) == cpu)
			;
	} else {
		atomic_set(&p->flag1, 1);
		dsb();
		atomic_set(&p->val, cpu);
		do {
			dsb();
		} while (atomic_read(&p->flag0) == 1
			&& atomic_read(&p->val) == cpu)
			;
	}
}

static void imx_pen_unlock(int cpu)
{
	uint32_t cpuidle_ocram_base = (uint32_t)phys_to_virt(
					imx_get_ocram_tz_start_addr() +
					LOWPOWER_IDLE_OCRAM_OFFSET,
					MEM_AREA_TEE_COHERENT);
	struct imx7_cpuidle_pm_info *p =
			(struct imx7_cpuidle_pm_info *)cpuidle_ocram_base;

	dsb();
	if (cpu == 0)
		atomic_set(&p->flag0, 0);
	else
		atomic_set(&p->flag1, 0);
}

static uint32_t get_online_cpus(void)
{
	vaddr_t src_a7rcr1 = core_mmu_get_va(SRC_BASE + SRC_A7RCR1,
					MEM_AREA_IO_SEC);
	uint32_t val = read32(src_a7rcr1);

	return (val & (1 << BP_SRC_A7RCR1_A7_CORE1_ENABLE)) ? 2 : 1;
}

int imx7d_lowpower_idle(uint32_t power_state __unused,
			uintptr_t entry __unused,
			uint32_t context_id __unused,
			struct sm_nsec_ctx *nsec)
{
	int ret;
	/*
	 * TODO: move the code to a platform init place, note that
	 * need to change kernel pm-imx7.c to avoid use LPRAM.
	 */
	uint32_t cpuidle_ocram_base = (uint32_t)phys_to_virt(
					imx_get_ocram_tz_start_addr() +
					LOWPOWER_IDLE_OCRAM_OFFSET,
					MEM_AREA_TEE_COHERENT);
	struct imx7_cpuidle_pm_info *p =
			(struct imx7_cpuidle_pm_info *)cpuidle_ocram_base;
	uint32_t type = (power_state & PSCI_POWER_STATE_TYPE_MASK) >>
		PSCI_POWER_STATE_TYPE_SHIFT;
	static uint32_t gic_inited = 0;
	uint32_t cpu = get_core_pos();
	/*
	 * TODO:
	 * Check power_state?
	 */
	imx_pen_lock(cpu);
	if (!lowpoweridle_init) {
		imx7d_cpuidle_init();
		lowpoweridle_init = 1;
	}

	if (type != PSCI_POWER_STATE_TYPE_POWER_DOWN)
		panic();

	p->num_online_cpus = get_online_cpus();
	p->num_lpi_cpus++;

	/* Store non-sec ctx regs */
	sm_save_modes_regs(&nsec->mode_regs);

	ret = sm_pm_cpu_suspend((uint32_t)p, (int (*)(uint32_t))
				(cpuidle_ocram_base + sizeof(*p)));
	/*
	 * Sometimes cpu_suspend may not really suspended, we need to check
	 * it's return value to restore reg or not
	 */
	if (ret < 0) {
		p->num_lpi_cpus--;
		imx_pen_unlock(cpu);
		DMSG("=== Not suspended, GPC IRQ Pending ===\n");
		return 0;
	}

	/* Restore register of different mode in secure world */
	sm_restore_modes_regs(&nsec->mode_regs);

	p->num_lpi_cpus--;

	/* Set entry for back to Linux */
	nsec->mon_lr = (uint32_t)entry;

	if (gic_inited == 0) {
		/*
		 * Call the Wakeup Late function to restore some
		 * HW configuration (e.g. TZASC)
		 */
		plat_cpu_wakeup_late();

		main_init_gic();
		gic_inited = 1;
		DMSG("=== Back from Suspended ===\n");
	} else {
		main_secondary_init_gic();
		gic_inited = 0;
	}
	imx_pen_unlock(cpu);

	return 0;
}
