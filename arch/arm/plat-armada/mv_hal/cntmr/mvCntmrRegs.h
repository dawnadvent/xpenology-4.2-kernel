/*******************************************************************************
Copyright (C) Marvell International Ltd. and its affiliates

This software file (the "File") is owned and distributed by Marvell
International Ltd. and/or its affiliates ("Marvell") under the following
alternative licensing terms.  Once you have made an election to distribute the
File under one of the following license alternatives, please (i) delete this
introductory statement regarding license alternatives, (ii) delete the two
license alternatives that you have not elected to use and (iii) preserve the
Marvell copyright notice above.

********************************************************************************
Marvell Commercial License Option

If you received this File from Marvell and you have entered into a commercial
license agreement (a "Commercial License") with Marvell, the File is licensed
to you under the terms of the applicable Commercial License.

********************************************************************************
Marvell GPL License Option

If you received this File from Marvell, you may opt to use, redistribute and/or
modify this File in accordance with the terms and conditions of the General
Public License Version 2, June 1991 (the "GPL License"), a copy of which is
available along with the File in the license.txt file or by writing to the Free
Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 or
on the worldwide web at http://www.gnu.org/licenses/gpl.txt.

THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE IMPLIED
WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE ARE EXPRESSLY
DISCLAIMED.  The GPL License provides additional details about this warranty
disclaimer.
********************************************************************************
Marvell BSD License Option

If you received this File from Marvell, you may opt to use, redistribute and/or
modify this File under the following licensing terms.
Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    *   Redistributions of source code must retain the above copyright notice,
	this list of conditions and the following disclaimer.

    *   Redistributions in binary form must reproduce the above copyright
	notice, this list of conditions and the following disclaimer in the
	documentation and/or other materials provided with the distribution.

    *   Neither the name of Marvell nor the names of its contributors may be
	used to endorse or promote products derived from this software without
	specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*******************************************************************************/

#ifndef __INCmvTmrwtdgRegsh
#define __INCmvTmrwtdgRegsh

#ifdef __cplusplus
extern "C" {
#endif

#include "mvCntmr.h"
#include "mvSysCntmrConfig.h"

/*******************************************/
/* ARM Timers Registers Map                */
/*******************************************/
/*
	TIMER0  = Global counter 0
	TIMER1  = Global counter 1
	TIMER2  = Global counter 2
	TIMER3  = Global counter 3
	TIMER4  = Global Watchdog 0
	TIMER5  = CPU0 Timer 0
	TIMER6  = CPU0 Timer 1
	TIMER7  = CPU0 Watchdog
	TIMER8  = CPU1 Timer 0
	TIMER9  = CPU1 Timer 1
	TIMER10 = CPU1 Watchdog
	TIMER11 = CPU2 Timer 0
	TIMER12 = CPU2 Timer 1
	TIMER13 = CPU2 Watchdog
	TIMER14 = CPU73 Timer 0
	TIMER15 = CPU73 Timer 1
	TIMER16 = CPU73 Watchdog
*/
#define INVALID_CNTMR(cntmrNum)    	((cntmrNum) >= MV_CNTMR_MAX_COUNTER)

#ifdef MV88F78X60_Z1
#define TIMER_TO_CPU(t)			((t < TIMER8) ? 0 :					\
											((t < TIMER11) ? 1 : 	\
											((t < TIMER14) ? 2 : 3)))
#define CPU_TIMER(t)			((t < TIMER8) ? (t-TIMER5) :					\
											((t < TIMER11) ? (t-TIMER8) :	\
											((t < TIMER14) ? (t-TIMER11) : (t-TIMER14))))
#else
#define CPU_TIMER(t)			(t-TIMER5)
#endif


#ifdef MV88F78X60_Z1
#define CNTMR_BASE(tmrNum)		((tmrNum <= MAX_GLOBAL_TIMER) ? (MV_CNTMR_REGS_OFFSET) : \
								(MV_CPUIF_REGS_OFFSET(TIMER_TO_CPU(tmrNum)) + 0x40))

#define CNTMR_RELOAD_REG(tmrNum)	 ((tmrNum <= MAX_GLOBAL_TIMER) ? 				 \
										(CNTMR_BASE(tmrNum)  + 0x10 + (tmrNum * 8)) : \
										((MV_CPUIF_REGS_OFFSET(TIMER_TO_CPU(tmrNum)) + 0x50+CPU_TIMER(tmrNum)*8)))

#define CNTMR_VAL_REG(tmrNum)		((tmrNum <= MAX_GLOBAL_TIMER) ? 					 \
										(CNTMR_BASE(tmrNum)  + 0x14 + (tmrNum * 8)) : \
										((MV_CPUIF_REGS_OFFSET(TIMER_TO_CPU(tmrNum)) + 0x54+CPU_TIMER(tmrNum)*8)))


#else
#define CNTMR_BASE(tmrNum)		((tmrNum <= MAX_GLOBAL_TIMER) ? (MV_CNTMR_REGS_OFFSET) : \
																(MV_CPUIF_LOCAL_REGS_OFFSET + 0x40))

#define CNTMR_RELOAD_REG(tmrNum)	 ((tmrNum <= MAX_GLOBAL_TIMER) ? 				 \
										(CNTMR_BASE(tmrNum)  + 0x10 + (tmrNum * 8)) : \
										(MV_CPUIF_LOCAL_REGS_OFFSET + 0x50 + ((tmrNum-5) * 8)))

#define CNTMR_VAL_REG(tmrNum)		((tmrNum <= MAX_GLOBAL_TIMER) ? 					 \
										(CNTMR_BASE(tmrNum)  + 0x14 + (tmrNum * 8)) : \
										(MV_CPUIF_LOCAL_REGS_OFFSET + 0x54 + ((tmrNum-5) * 8)))

#endif

/* #define CNTMR_CTRL_REG(tmrNum)	(tmrNum <=MAX_GLOBAL_TIMER) ? (MV_CNTMR_REGS_OFFSET) :
						 (MV_CPUIF_REGS_OFFSET(0) + 0x84) */
#define CNTMR_CTRL_REG(tmrNum)		CNTMR_BASE(tmrNum)

/* ARM Timers Registers Map                */
/*******************************************/

/* ARM Timers Control Register */
/* CPU_TIMERS_CTRL_REG (CTCR) */

#define CTCR_ARM_TIMER_EN_OFFS(timer)	((timer <= MAX_GLOBAL_TIMER) ? (timer * 2) : ((CPU_TIMER(timer))*2))

#define CTCR_ARM_TIMER_EN_MASK(cntr)	(1 << CTCR_ARM_TIMER_EN_OFFS(cntr))
#define CTCR_ARM_TIMER_EN(cntr)		(1 << CTCR_ARM_TIMER_EN_OFFS(cntr))
#define CTCR_ARM_TIMER_DIS(cntr)	(0 << CTCR_ARM_TIMER_EN_OFFS(cntr))

#if defined (CONFIG_ARCH_ARMADA370)
#define CTCR_ARM_TIMER_AUTO_OFFS(timer) ((timer <= MAX_GLOBAL_TIMER) ? (1 + (timer * 2)) : \
										(1 + (timer-FIRST_PRIVATE_TIMER) * 2))
#else
#define CTCR_ARM_TIMER_AUTO_OFFS(timer)	((timer <= MAX_GLOBAL_TIMER) ? (1 + (timer * 2)) : \
										(1 + ((CPU_TIMER(timer))) * 2))
#endif

#define CTCR_ARM_TIMER_AUTO_MASK(cntr)	(1 << CTCR_ARM_TIMER_EN_OFFS(cntr))
#define CTCR_ARM_TIMER_AUTO_EN(cntr)	(1 << CTCR_ARM_TIMER_AUTO_OFFS(cntr))
#define CTCR_ARM_TIMER_AUTO_DIS(cntr)	(0 << CTCR_ARM_TIMER_AUTO_OFFS(cntr))


#define CTCR_ARM_TIMER_RATIO_OFFS(timer) ((timer < TIMER4) ? (19 + timer*3) :	\
										 ((timer == TIMER4) ? 16 :				\
										 ((CPU_TIMER(timer) == 0) ? 19 :      \
										 ((CPU_TIMER(timer) == 1) ? 22 : 16))))

#define CTCR_ARM_TIMER_RATIO_MASK(cntr)	(0x7 << CTCR_ARM_TIMER_RATIO_OFFS(cntr))

#define CTCR_ARM_TIMER_25MhzFRQ_ENABLE_OFFS(timer) ((timer < TIMER4) ? (11 + timer) :		\
													 ((timer == TIMER4) ? 10 :				\
													 ((CPU_TIMER(timer) == 0) ? 11 :	\
													 ((CPU_TIMER(timer) == 1) ? 12 : 10))))

#define CTCR_ARM_TIMER_25MhzFRQ_MASK(cntr)	(1 << CTCR_ARM_TIMER_25MhzFRQ_ENABLE_OFFS(cntr))
#define CTCR_ARM_TIMER_25MhzFRQ_EN(cntr)	(1 << CTCR_ARM_TIMER_25MhzFRQ_ENABLE_OFFS(cntr))
#define CTCR_ARM_TIMER_25MhzFRQ_DIS(cntr)	(0 << CTCR_ARM_TIMER_25MhzFRQ_ENABLE_OFFS(cntr))


/* ARM Timer\Watchdog Reload Register */
/* CNTMR_RELOAD_REG (TRR) */

#define TRG_ARM_TIMER_REL_OFFS			0
#define TRG_ARM_TIMER_REL_MASK			0xffffffff

/* ARM Timer\Watchdog Register */
/* CNTMR_VAL_REG (TVRG) */

#define TVR_ARM_TIMER_OFFS			0
#define TVR_ARM_TIMER_MASK			0xffffffff
#define TVR_ARM_TIMER_MAX			0xffffffff

#ifdef __cplusplus
}
#endif
#endif				/* __INCmvTmrwtdgRegsh */
