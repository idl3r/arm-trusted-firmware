/*
 * Copyright (c) 2013-2015, ARM Limited and Contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of ARM nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <arch.h>
#include <asm_macros.S>
#include <gic_v2.h>
#include <platform_def.h>
#include <v2m_def.h>
#include "../drivers/pwrc/fvp_pwrc.h"
#include "../fvp_def.h"

	.globl	plat_secondary_cold_boot_setup
	.globl	plat_get_my_entrypoint
	.globl	plat_is_my_cpu_primary

	.macro	fvp_choose_gicmmap  param1, param2, x_tmp, w_tmp, res
	ldr	\x_tmp, =V2M_SYSREGS_BASE + V2M_SYS_ID
	ldr	\w_tmp, [\x_tmp]
	ubfx	\w_tmp, \w_tmp, #V2M_SYS_ID_BLD_SHIFT, #V2M_SYS_ID_BLD_LENGTH
	cmp	\w_tmp, #BLD_GIC_VE_MMAP
	csel	\res, \param1, \param2, eq
	.endm

	/* -----------------------------------------------------
	 * void plat_secondary_cold_boot_setup (void);
	 *
	 * This function performs any platform specific actions
	 * needed for a secondary cpu after a cold reset e.g
	 * mark the cpu's presence, mechanism to place it in a
	 * holding pen etc.
	 * TODO: Should we read the PSYS register to make sure
	 * that the request has gone through.
	 * -----------------------------------------------------
	 */
func plat_secondary_cold_boot_setup
	/* ---------------------------------------------
	 * Power down this cpu.
	 * TODO: Do we need to worry about powering the
	 * cluster down as well here. That will need
	 * locks which we won't have unless an elf-
	 * loader zeroes out the zi section.
	 * ---------------------------------------------
	 */
	mrs	x0, mpidr_el1
	ldr	x1, =PWRC_BASE
	str	w0, [x1, #PPOFFR_OFF]

	/* ---------------------------------------------
	 * Deactivate the gic cpu interface as well
	 * ---------------------------------------------
	 */
	ldr	x0, =VE_GICC_BASE
	ldr	x1, =BASE_GICC_BASE
	fvp_choose_gicmmap	x0, x1, x2, w2, x1
	mov	w0, #(IRQ_BYP_DIS_GRP1 | FIQ_BYP_DIS_GRP1)
	orr	w0, w0, #(IRQ_BYP_DIS_GRP0 | FIQ_BYP_DIS_GRP0)
	str	w0, [x1, #GICC_CTLR]

	/* ---------------------------------------------
	 * There is no sane reason to come out of this
	 * wfi so panic if we do. This cpu will be pow-
	 * ered on and reset by the cpu_on pm api
	 * ---------------------------------------------
	 */
	dsb	sy
	wfi
cb_panic:
	b	cb_panic
endfunc plat_secondary_cold_boot_setup

	/* ---------------------------------------------------------------------
	 * unsigned long plat_get_my_entrypoint (void);
	 *
	 * Main job of this routine is to distinguish between a cold and warm
	 * boot. On FVP, this information can be queried from the power
	 * controller. The Power Control SYS Status Register (PSYSR) indicates
	 * the wake-up reason for the CPU.
	 *
	 * For a cold boot, return 0.
	 * For a warm boot, read the mailbox and return the address it contains.
	 *
	 * TODO: PSYSR is a common register and should be
	 * 	accessed using locks. Since its not possible
	 * 	to use locks immediately after a cold reset
	 * 	we are relying on the fact that after a cold
	 * 	reset all cpus will read the same WK field
	 * ---------------------------------------------------------------------
	 */
func plat_get_my_entrypoint
	/* ---------------------------------------------------------------------
	 * When bit PSYSR.WK indicates either "Wake by PPONR" or "Wake by GIC
	 * WakeRequest signal" then it is a warm boot.
	 * ---------------------------------------------------------------------
	 */
	mrs	x2, mpidr_el1
	ldr	x1, =PWRC_BASE
	str	w2, [x1, #PSYSR_OFF]
	ldr	w2, [x1, #PSYSR_OFF]
	ubfx	w2, w2, #PSYSR_WK_SHIFT, #PSYSR_WK_WIDTH
	cmp	w2, #WKUP_PPONR
	beq	warm_reset
	cmp	w2, #WKUP_GICREQ
	beq	warm_reset

	/* Cold reset */
	mov	x0, #0
	ret

warm_reset:
	/* ---------------------------------------------------------------------
	 * A mailbox is maintained in the trusted SRAM. It is flushed out of the
	 * caches after every update using normal memory so it is safe to read
	 * it here with SO attributes.
	 * ---------------------------------------------------------------------
	 */
	mov_imm	x0, MBOX_BASE
	ldr	x0, [x0]
	cbz	x0, _panic
	ret

	/* ---------------------------------------------------------------------
	 * The power controller indicates this is a warm reset but the mailbox
	 * is empty. This should never happen!
	 * ---------------------------------------------------------------------
	 */
_panic:
	b	_panic
endfunc plat_get_my_entrypoint

	/* -----------------------------------------------------
	 * unsigned int plat_is_my_cpu_primary (void);
	 *
	 * Find out whether the current cpu is the primary
	 * cpu.
	 * -----------------------------------------------------
	 */
func plat_is_my_cpu_primary
	mrs	x0, mpidr_el1
	and	x0, x0, #(MPIDR_CLUSTER_MASK | MPIDR_CPU_MASK)
	cmp	x0, #FVP_PRIMARY_CPU
	cset	w0, eq
	ret
endfunc plat_is_my_cpu_primary
