/*
 *  i386 emulator main execution loop
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"
#include "cpu.h"
#include "disas.h"
#include "tcg.h"
#include "qemu-barrier.h"


// TRL 0810 record replay stuff 
#include "rr_log.h"
#include <signal.h>

//mz need this here because CPU_LOG_RR constant is not available in rr_log.[ch]
int is_cpu_log_rr_set(void) {
    return (loglevel & CPU_LOG_RR);
}

extern FILE *stderr;

int tb_invalidated_flag;

//#define CONFIG_DEBUG_EXEC

bool qemu_cpu_has_work(CPUState *env)
{
    return cpu_has_work(env);
}

void cpu_loop_exit(CPUState *env, char *file, int line)
{
    if (rr_debug_whisper()) {
      qemu_log_mask(CPU_LOG_RR, 
		    "tp (icount=%llu, eip=0x%08x, ecx=0x%08x) called cpu_loop_exit from %s:%d.\n", 
		    (unsigned long long)rr_prog_point.guest_instr_count,
		    rr_prog_point.eip, rr_prog_point.ecx,
            file, line);
    }
    env->current_tb = NULL;
    longjmp(env->jmp_env, 1);
}

/* exit the current TB from a signal handler. The host registers are
   restored in a state compatible with the CPU emulator
 */
#if defined(CONFIG_SOFTMMU)
void cpu_resume_from_signal(CPUState *env, void *puc)
{
    /* XXX: restore cpu registers saved in host registers */

    env->exception_index = -1;

    qemu_log_mask(CPU_LOG_RR, "calling longjmp in cpu_resume_from_signal\n");

    //mz Record & Replay NOTE:
    //mz we're not in the middle of recording any more...
    //mz 08.2010 I don't think this ever gets called.
    extern volatile sig_atomic_t rr_record_in_progress;
    rr_record_in_progress = 0;

    longjmp(env->jmp_env, 1);
}
#endif

/* Execute the code without caching the generated code. An interpreter
   could be used if available. */
static void cpu_exec_nocache(CPUState *env, int max_cycles,
                             TranslationBlock *orig_tb)
{
    unsigned long next_tb;
    TranslationBlock *tb;

    /* Should never happen.
       We only end up here when an existing TB is too long.  */
    if (max_cycles > CF_COUNT_MASK)
        max_cycles = CF_COUNT_MASK;

    tb = tb_gen_code(env, orig_tb->pc, orig_tb->cs_base, orig_tb->flags,
                     max_cycles);
    env->current_tb = tb;
    /* execute the generated code */
    next_tb = tcg_qemu_tb_exec(env, tb->tc_ptr);
    env->current_tb = NULL;

    if ((next_tb & 3) == 2) {
        /* Restore PC.  This may happen if async event occurs before
           the TB starts executing.  */
        cpu_pc_from_tb(env, tb);
    }
    tb_phys_invalidate(tb, -1);
    tb_free(tb);
}

static TranslationBlock *tb_find_slow(CPUState *env,
                                      target_ulong pc,
                                      target_ulong cs_base,
                                      uint64_t flags)
{
    TranslationBlock *tb, **ptb1;
    unsigned int h;
    tb_page_addr_t phys_pc, phys_page1;
    target_ulong virt_page2;

    tb_invalidated_flag = 0;

    /* find translated block using physical mappings */
    phys_pc = get_page_addr_code(env, pc);
    phys_page1 = phys_pc & TARGET_PAGE_MASK;
    h = tb_phys_hash_func(phys_pc);
    ptb1 = &tb_phys_hash[h];
    for(;;) {
        tb = *ptb1;
        if (!tb)
            goto not_found;
        if (tb->pc == pc &&
            tb->page_addr[0] == phys_page1 &&
            tb->cs_base == cs_base &&
            tb->flags == flags) {
            /* check next page if needed */
            if (tb->page_addr[1] != -1) {
                tb_page_addr_t phys_page2;

                virt_page2 = (pc & TARGET_PAGE_MASK) +
                    TARGET_PAGE_SIZE;
                phys_page2 = get_page_addr_code(env, virt_page2);
                if (tb->page_addr[1] == phys_page2)
                    goto found;
            } else {
                goto found;
            }
        }
        ptb1 = &tb->phys_hash_next;
    }
 not_found:
   /* if no translated code available, then translate it now */
    tb = tb_gen_code(env, pc, cs_base, flags, 0);

 found:
    /* Move the last found TB to the head of the list */
    if (likely(*ptb1)) {
        *ptb1 = tb->phys_hash_next;
        tb->phys_hash_next = tb_phys_hash[h];
        tb_phys_hash[h] = tb;
    }
    /* we add the TB in the virtual pc hash table */
    env->tb_jmp_cache[tb_jmp_cache_hash_func(pc)] = tb;
    return tb;
}

static inline TranslationBlock *tb_find_fast(CPUState *env)
{
    TranslationBlock *tb;
    target_ulong cs_base, pc;
    int flags;

    /* we record a subset of the CPU state. It will
       always be the same before a given translated block
       is executed. */
    cpu_get_tb_cpu_state(env, &pc, &cs_base, &flags);
    tb = env->tb_jmp_cache[tb_jmp_cache_hash_func(pc)];
    if (unlikely(!tb || tb->pc != pc || tb->cs_base != cs_base ||
                 tb->flags != flags)) {
        tb = tb_find_slow(env, pc, cs_base, flags);
    }
    return tb;
}

static CPUDebugExcpHandler *debug_excp_handler;

CPUDebugExcpHandler *cpu_set_debug_excp_handler(CPUDebugExcpHandler *handler)
{
    CPUDebugExcpHandler *old_handler = debug_excp_handler;

    debug_excp_handler = handler;
    return old_handler;
}

static void cpu_handle_debug_exception(CPUState *env)
{
    CPUWatchpoint *wp;

    if (!env->watchpoint_hit) {
        QTAILQ_FOREACH(wp, &env->watchpoints, entry) {
            wp->flags &= ~BP_WATCHPOINT_HIT;
        }
    }
    if (debug_excp_handler) {
        debug_excp_handler(env);
    }
}



void rr_set_program_point(void) {
    if (cpu_single_env) {
        rr_set_prog_point(cpu_single_env->eip, cpu_single_env->regs[R_ECX], GUEST_ICOUNT);
    }
}

void rr_quit_cpu_loop(void) {
    if (cpu_single_env) {
        cpu_single_env->exception_index = EXCP_INTERRUPT;
        cpu_loop_exit(cpu_single_env, __FILE__, __LINE__);
    }
}


void rr_clear_rr_guest_instr_count(CPUState *cpu_state) {
  cpu_state->rr_guest_instr_count = 0;
}



/* main execution loop */

volatile sig_atomic_t exit_request;

int cpu_exec(CPUState *env)
{
    int ret, interrupt_request, saved_exit_request;
    TranslationBlock *tb;
    uint8_t *tc_ptr;
    unsigned long next_tb;

/*
    sigset_t blockset, oldset;

    // create a signal set containing just ALARM and USR2
    sigemptyset(&blockset);
    sigaddset(&blockset, SIGALRM);
    sigaddset(&blockset, SIGUSR2);
    sigaddset(&blockset, SIGIO);

    if (__builtin_expect(rr_record_requested, 0)) {
        //block signals
        sigprocmask(SIG_BLOCK, &blockset, &oldset);
        rr_do_begin_record(rr_requested_name, env);
        rr_record_requested = 0;
        //unblock signals
        sigprocmask(SIG_SETMASK, &oldset, NULL);
    }
    if (__builtin_expect(rr_replay_requested, 0)) {
        extern void quit_timers(void);
        //block signals
        sigprocmask(SIG_BLOCK, &blockset, &oldset);
        rr_do_begin_replay(rr_requested_name, env);
        quit_timers();
        rr_replay_requested = 0;
        //unblock signals
        sigprocmask(SIG_SETMASK, &oldset, NULL);
    }
*/

    //mz This is done once at the start of record and once at the start of
    //replay.  So we should be ok.
    if (unlikely(rr_flush_tb())) {
      qemu_log_mask(CPU_LOG_RR, "flushing tb\n");
      tb_flush(env);
      tb_invalidated_flag = 1;
      rr_flush_tb_off();  // just the first time, eh?
    }

    //qemu_log_mask(CPU_LOG_RR, "head of cpu_exec: env1->hflags = %x\n", env->hflags);
    //    qemu_log_mask(CPU_LOG_RR, "head of cpu_exec: env1->hflags & HF_HALTED_MASK = %x\n",
    //		  env->hflags & HF_HALTED_MASK);

    if (env->halted) {
        if (!cpu_has_work(env)) {
            return EXCP_HALTED;
        }

        env->halted = 0;
    }

    cpu_single_env = env;
    
    //mz 06.2012  I *believe* this is OK.  We certainly want this to happen in
    //record, and in replay we want this to happen because this will let us
    //know that the monitor has been used and we should go service that
    //request.  The actual value of env->exit_request will be checked against
    //the RR log before it is used.
    if (unlikely(exit_request)) {
        env->exit_request = 1;
    }

#if defined(TARGET_I386)
    /* put eflags in CPU temporary format */
    CC_SRC = env->eflags & (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
    DF = 1 - (2 * ((env->eflags >> 10) & 1));
    CC_OP = CC_OP_EFLAGS;
    env->eflags &= ~(DF_MASK | CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
#elif defined(TARGET_SPARC)
#elif defined(TARGET_M68K)
    env->cc_op = CC_OP_FLAGS;
    env->cc_dest = env->sr & 0xf;
    env->cc_x = (env->sr >> 4) & 1;
#elif defined(TARGET_ALPHA)
#elif defined(TARGET_ARM)
#elif defined(TARGET_UNICORE32)
#elif defined(TARGET_PPC)
    env->reserve_addr = -1;
#elif defined(TARGET_LM32)
#elif defined(TARGET_MICROBLAZE)
#elif defined(TARGET_MIPS)
#elif defined(TARGET_SH4)
#elif defined(TARGET_CRIS)
#elif defined(TARGET_S390X)
#elif defined(TARGET_XTENSA)
    /* XXXXX */
#else
#error unsupported target CPU
#endif
    env->exception_index = -1;

    /* prepare setjmp context for exception handling */
    for(;;) {
        if (setjmp(env->jmp_env) == 0) {
	    // NB: we can only be here if we came from immediately before
	    // the if (setjmp...)  stmt.  
	    // the else block gets executed when we longjmp(env->jmp_env)
            //mz Set the program point here.
            rr_set_program_point();
            /* if an exception is pending, we execute it here */
            if (env->exception_index >= 0) {
                if (env->exception_index >= EXCP_INTERRUPT) {
                    /* exit request from the cpu execution loop */
                    ret = env->exception_index;
                    if (ret == EXCP_DEBUG) {
                        cpu_handle_debug_exception(env);
                    }
                    break;
                } else {
#if defined(CONFIG_USER_ONLY)
                    /* if user mode only, we simulate a fake exception
                       which will be handled outside the cpu execution
                       loop */
#if defined(TARGET_I386)
                    do_interrupt(env);
#endif
                    ret = env->exception_index;
                    break;
#else
                    do_interrupt(env);
                    env->exception_index = -1;
#endif
                }
            }

            next_tb = 0; /* force lookup of first TB */
            for(;;) {
                //mz Set the program point here.
                rr_set_program_point();
                // cache interrupt request value.
                interrupt_request = env->interrupt_request;
                //mz Record and Replay.
                //mz it is important to do this in the order written, as
                //during record env->interrupt_request can be changed at any
                //time via a signal.  Thus, we want to make sure that we
                //record the same value in the log as the one being used in
                //these decisions.
                rr_skipped_callsite_location = RR_CALLSITE_CPU_EXEC_1;
                rr_interrupt_request(&interrupt_request);

                // Extra debug
                if (rr_debug_whisper()) {
                      qemu_log_mask(CPU_LOG_RR, 
                          "RR_CALLSITE_CPU_EXEC_1 interrupt_request %x: env->eflags=%x env->hflags=%x env->hflags2=%x\n", 
                          interrupt_request, env->eflags, env->hflags, env->hflags2);
                }

                if (rr_in_replay()) {
                    env->interrupt_request = interrupt_request;
                }
                if (unlikely(interrupt_request)) {
                    if (unlikely(env->singlestep_enabled & SSTEP_NOIRQ)) {
                        /* Mask out external interrupts for this step. */
                        interrupt_request &= ~CPU_INTERRUPT_SSTEP_MASK;
                    }
                    if (interrupt_request & CPU_INTERRUPT_DEBUG) {
                        env->interrupt_request &= ~CPU_INTERRUPT_DEBUG;
                        env->exception_index = EXCP_DEBUG;
                        cpu_loop_exit(env, __FILE__, __LINE__);
                    }
#if defined(TARGET_ARM) || defined(TARGET_SPARC) || defined(TARGET_MIPS) || \
    defined(TARGET_PPC) || defined(TARGET_ALPHA) || defined(TARGET_CRIS) || \
    defined(TARGET_MICROBLAZE) || defined(TARGET_LM32) || defined(TARGET_UNICORE32)
                    if (interrupt_request & CPU_INTERRUPT_HALT) {
                        env->interrupt_request &= ~CPU_INTERRUPT_HALT;
                        env->halted = 1;
                        env->exception_index = EXCP_HLT;
                        cpu_loop_exit(env, __FILE__, __LINE__);
                    }
#endif
#if defined(TARGET_I386)
                    if (interrupt_request & CPU_INTERRUPT_INIT) {
                            svm_check_intercept(env, SVM_EXIT_INIT);
                            do_cpu_init(env);
                            env->exception_index = EXCP_HALTED;
                            cpu_loop_exit(env, __FILE__, __LINE__);
                    } else if (interrupt_request & CPU_INTERRUPT_SIPI) {
                            do_cpu_sipi(env);
                    } else if (env->hflags2 & HF2_GIF_MASK) {
                        if ((interrupt_request & CPU_INTERRUPT_SMI) &&
                            !(env->hflags & HF_SMM_MASK)) {
                            svm_check_intercept(env, SVM_EXIT_SMI);
                            env->interrupt_request &= ~CPU_INTERRUPT_SMI;
                            do_smm_enter(env);
                            next_tb = 0;
                        } else if ((interrupt_request & CPU_INTERRUPT_NMI) &&
                                   !(env->hflags2 & HF2_NMI_MASK)) {
                            env->interrupt_request &= ~CPU_INTERRUPT_NMI;
                            env->hflags2 |= HF2_NMI_MASK;
                            do_interrupt_x86_hardirq(env, EXCP02_NMI, 1);
                            next_tb = 0;
			} else if (interrupt_request & CPU_INTERRUPT_MCE) {
                            env->interrupt_request &= ~CPU_INTERRUPT_MCE;
                            do_interrupt_x86_hardirq(env, EXCP12_MCHK, 0);
                            next_tb = 0;
                        } else if ((interrupt_request & CPU_INTERRUPT_HARD) &&
                                   (((env->hflags2 & HF2_VINTR_MASK) && 
                                     (env->hflags2 & HF2_HIF_MASK)) ||
                                    (!(env->hflags2 & HF2_VINTR_MASK) && 
                                     (env->eflags & IF_MASK && 
                                      !(env->hflags & HF_INHIBIT_IRQ_MASK))))) {
                            int intno;
                            svm_check_intercept(env, SVM_EXIT_INTR);
                            env->interrupt_request &= ~(CPU_INTERRUPT_HARD | CPU_INTERRUPT_VIRQ);
			    // dont bother calling this if we are replaying       
			    // ... just obtain "intno" from (or record it to) 
			    // non-deterministic inputs log
			    RR_DO_RECORD_OR_REPLAY(
						   /*action=*/intno = cpu_get_pic_interrupt(env),
						   /*record=*/rr_input_4(&intno),
						   /*replay=*/rr_input_4(&intno),
						   /*location=*/RR_CALLSITE_CPU_EXEC_2);			    
			    //mz servicing hardware interrupt
                            qemu_log_mask(CPU_LOG_TB_IN_ASM, "Servicing hardware INT=0x%02x\n", intno);
                            do_interrupt_x86_hardirq(env, intno, 1);
                            /* ensure that no TB jump will be modified as
                               the program flow was changed */
                            next_tb = 0;
#if !defined(CONFIG_USER_ONLY)
                        } else if ((interrupt_request & CPU_INTERRUPT_VIRQ) &&
                                   (env->eflags & IF_MASK) && 
                                   !(env->hflags & HF_INHIBIT_IRQ_MASK)) {
                            int intno;
                            /* FIXME: this should respect TPR */
                            svm_check_intercept(env, SVM_EXIT_VINTR);
                            intno = ldl_phys(env->vm_vmcb + offsetof(struct vmcb, control.int_vector));
                            qemu_log_mask(CPU_LOG_TB_IN_ASM, "Servicing virtual hardware INT=0x%02x\n", intno);
                            do_interrupt_x86_hardirq(env, intno, 1);
                            env->interrupt_request &= ~CPU_INTERRUPT_VIRQ;
                            next_tb = 0;
#endif
                        }
                    }
#elif defined(TARGET_PPC)
#if 0
                    if ((interrupt_request & CPU_INTERRUPT_RESET)) {
                        cpu_reset(env);
                    }
#endif
                    if (interrupt_request & CPU_INTERRUPT_HARD) {
                        ppc_hw_interrupt(env);
                        if (env->pending_interrupts == 0)
                            env->interrupt_request &= ~CPU_INTERRUPT_HARD;
                        next_tb = 0;
                    }
#elif defined(TARGET_LM32)
                    if ((interrupt_request & CPU_INTERRUPT_HARD)
                        && (env->ie & IE_IE)) {
                        env->exception_index = EXCP_IRQ;
                        do_interrupt(env);
                        next_tb = 0;
                    }
#elif defined(TARGET_MICROBLAZE)
                    if ((interrupt_request & CPU_INTERRUPT_HARD)
                        && (env->sregs[SR_MSR] & MSR_IE)
                        && !(env->sregs[SR_MSR] & (MSR_EIP | MSR_BIP))
                        && !(env->iflags & (D_FLAG | IMM_FLAG))) {
                        env->exception_index = EXCP_IRQ;
                        do_interrupt(env);
                        next_tb = 0;
                    }
#elif defined(TARGET_MIPS)
                    if ((interrupt_request & CPU_INTERRUPT_HARD) &&
                        cpu_mips_hw_interrupts_pending(env)) {
                        /* Raise it */
                        env->exception_index = EXCP_EXT_INTERRUPT;
                        env->error_code = 0;
                        do_interrupt(env);
                        next_tb = 0;
                    }
#elif defined(TARGET_SPARC)
                    if (interrupt_request & CPU_INTERRUPT_HARD) {
                        if (cpu_interrupts_enabled(env) &&
                            env->interrupt_index > 0) {
                            int pil = env->interrupt_index & 0xf;
                            int type = env->interrupt_index & 0xf0;

                            if (((type == TT_EXTINT) &&
                                  cpu_pil_allowed(env, pil)) ||
                                  type != TT_EXTINT) {
                                env->exception_index = env->interrupt_index;
                                do_interrupt(env);
                                next_tb = 0;
                            }
                        }
		    }
#elif defined(TARGET_ARM)
                    if (interrupt_request & CPU_INTERRUPT_FIQ
                        && !(env->uncached_cpsr & CPSR_F)) {
                        env->exception_index = EXCP_FIQ;
                        do_interrupt(env);
                        next_tb = 0;
                    }
                    /* ARMv7-M interrupt return works by loading a magic value
                       into the PC.  On real hardware the load causes the
                       return to occur.  The qemu implementation performs the
                       jump normally, then does the exception return when the
                       CPU tries to execute code at the magic address.
                       This will cause the magic PC value to be pushed to
                       the stack if an interrupt occurred at the wrong time.
                       We avoid this by disabling interrupts when
                       pc contains a magic address.  */
                    if (interrupt_request & CPU_INTERRUPT_HARD
                        && ((IS_M(env) && env->regs[15] < 0xfffffff0)
                            || !(env->uncached_cpsr & CPSR_I))) {
                        env->exception_index = EXCP_IRQ;
                        do_interrupt(env);
                        next_tb = 0;
                    }
#elif defined(TARGET_UNICORE32)
                    if (interrupt_request & CPU_INTERRUPT_HARD
                        && !(env->uncached_asr & ASR_I)) {
                        do_interrupt(env);
                        next_tb = 0;
                    }
#elif defined(TARGET_SH4)
                    if (interrupt_request & CPU_INTERRUPT_HARD) {
                        do_interrupt(env);
                        next_tb = 0;
                    }
#elif defined(TARGET_ALPHA)
                    {
                        int idx = -1;
                        /* ??? This hard-codes the OSF/1 interrupt levels.  */
		        switch (env->pal_mode ? 7 : env->ps & PS_INT_MASK) {
                        case 0 ... 3:
                            if (interrupt_request & CPU_INTERRUPT_HARD) {
                                idx = EXCP_DEV_INTERRUPT;
                            }
                            /* FALLTHRU */
                        case 4:
                            if (interrupt_request & CPU_INTERRUPT_TIMER) {
                                idx = EXCP_CLK_INTERRUPT;
                            }
                            /* FALLTHRU */
                        case 5:
                            if (interrupt_request & CPU_INTERRUPT_SMP) {
                                idx = EXCP_SMP_INTERRUPT;
                            }
                            /* FALLTHRU */
                        case 6:
                            if (interrupt_request & CPU_INTERRUPT_MCHK) {
                                idx = EXCP_MCHK;
                            }
                        }
                        if (idx >= 0) {
                            env->exception_index = idx;
                            env->error_code = 0;
                            do_interrupt(env);
                            next_tb = 0;
                        }
                    }
#elif defined(TARGET_CRIS)
                    if (interrupt_request & CPU_INTERRUPT_HARD
                        && (env->pregs[PR_CCS] & I_FLAG)
                        && !env->locked_irq) {
                        env->exception_index = EXCP_IRQ;
                        do_interrupt(env);
                        next_tb = 0;
                    }
                    if (interrupt_request & CPU_INTERRUPT_NMI
                        && (env->pregs[PR_CCS] & M_FLAG)) {
                        env->exception_index = EXCP_NMI;
                        do_interrupt(env);
                        next_tb = 0;
                    }
#elif defined(TARGET_M68K)
                    if (interrupt_request & CPU_INTERRUPT_HARD
                        && ((env->sr & SR_I) >> SR_I_SHIFT)
                            < env->pending_level) {
                        /* Real hardware gets the interrupt vector via an
                           IACK cycle at this point.  Current emulated
                           hardware doesn't rely on this, so we
                           provide/save the vector when the interrupt is
                           first signalled.  */
                        env->exception_index = env->pending_vector;
                        do_interrupt_m68k_hardirq(env);
                        next_tb = 0;
                    }
#elif defined(TARGET_S390X) && !defined(CONFIG_USER_ONLY)
                    if ((interrupt_request & CPU_INTERRUPT_HARD) &&
                        (env->psw.mask & PSW_MASK_EXT)) {
                        do_interrupt(env);
                        next_tb = 0;
                    }
#elif defined(TARGET_XTENSA)
                    if (interrupt_request & CPU_INTERRUPT_HARD) {
                        env->exception_index = EXC_IRQ;
                        do_interrupt(env);
                        next_tb = 0;
                    }
#endif
                    //mz set program point after handling interrupts.
                    rr_set_program_point();
                    //mz record the value again in case do_interrupt has set EXITTB flag
                    rr_skipped_callsite_location = RR_CALLSITE_CPU_EXEC_4;
                    rr_interrupt_request(&env->interrupt_request);

                    if (rr_debug_whisper()) {
                          qemu_log_mask(CPU_LOG_RR, 
                              "RR_CALLSITE_CPU_EXEC_4 interrupt_request %x: env->eflags=%x env->hflags=%x env->hflags2=%x\n", 
                              interrupt_request, env->eflags, env->hflags, env->hflags2);
                    }

                   /* Don't use the cached interrupt_request value,
                      do_interrupt may have updated the EXITTB flag. */
                    if (env->interrupt_request & CPU_INTERRUPT_EXITTB) {
                        env->interrupt_request &= ~CPU_INTERRUPT_EXITTB;
                        /* ensure that no TB jump will be modified as
                           the program flow was changed */
                        next_tb = 0;
                    }
                }

                // capture exit_request as it may change
                saved_exit_request = env->exit_request;

                rr_set_program_point();
                rr_skipped_callsite_location = RR_CALLSITE_CPU_EXEC_00;
                if (rr_in_record()) {
                    rr_exit_request(&saved_exit_request);
                }
                else if (rr_in_replay()) {
                    if (!rr_use_live_exit_request) {
                        rr_exit_request(&saved_exit_request);
                    }
                }

                if (rr_debug_whisper()) {
                      qemu_log_mask(CPU_LOG_RR, 
                          "RR_CALLSITE_CPU_EXEC_00 exit_request %d: env->eflags=%x env->hflags=%x env->hflags2=%x\n", 
                          saved_exit_request, env->eflags, env->hflags, env->hflags2);
                }

                if(saved_exit_request != env->exit_request) {
                    //printf("RR_CALLSITE_CPU_EXEC_00: Ruh roh! Someone done changed env->exit_request behind our back!\n");
                    fflush(stdout);
                }

                if (unlikely(saved_exit_request)) {
                    env->exit_request = 0;
                    env->exception_index = EXCP_INTERRUPT;
                    cpu_loop_exit(env, __FILE__, __LINE__);
                }

                if (rr_prog_point.guest_instr_count == 4998583) {
                    if (rr_debug_whisper()) {
                          qemu_log_mask(CPU_LOG_RR, 
                              "At problematic log point, ECX=%08x ESI=%08x EDI=%08x EBP=%08x\n", 
                              ECX, ESI, EDI, EBP);
                    }
                }
                    

#if defined(DEBUG_DISAS) || defined(CONFIG_DEBUG_EXEC)
                if (qemu_loglevel_mask(CPU_LOG_TB_CPU)) {
                    /* restore flags in standard format */
#if defined(TARGET_I386)
                    env->eflags = env->eflags | cpu_cc_compute_all(env, CC_OP)
                        | (DF & DF_MASK);
                    log_cpu_state(env, X86_DUMP_CCOP);
                    env->eflags &= ~(DF_MASK | CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
#elif defined(TARGET_M68K)
                    cpu_m68k_flush_flags(env, env->cc_op);
                    env->cc_op = CC_OP_FLAGS;
                    env->sr = (env->sr & 0xffe0)
                              | env->cc_dest | (env->cc_x << 4);
                    log_cpu_state(env, 0);
#else
                    log_cpu_state(env, 0);
#endif
                }
#endif /* DEBUG_DISAS || CONFIG_DEBUG_EXEC */
                spin_lock(&tb_lock);

                qemu_log_mask(CPU_LOG_TB_IN_ASM, 
			      "Prog point: {guest=%llu, eip=%08x, ecx=%08x}\n",
			      (unsigned long long)rr_prog_point.guest_instr_count, rr_prog_point.eip, rr_prog_point.ecx);

                if(rr_debug_whisper()) {
                    qemu_log_mask(CPU_LOG_RR, 
                        "Register dump, EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n", 
                            EAX, EBX, ECX, EDX, ESI, EDI, EBP, ESP);
                }


                tb = tb_find_fast(env);

                if (rr_mode == RR_REPLAY)
                {
                    if (tb->num_guest_insns > rr_num_instr_before_next_interrupt) {
                        //mz invalidate current TB and retranslate
                        //printf("invalidating single TB: %llu -> %llu\n", 
                        // tb->num_guest_insns, rr_num_instr_before_next_interrupt);
                        invalidate_single_tb(env, tb->pc);
                        //mz try again.
                        tb = tb_find_fast(env);
                    }
                }

                /* Note: we do it here to avoid a gcc bug on Mac OS X when
                   doing it in tb_find_slow */
                if (tb_invalidated_flag) {
                    /* as some TB could have been invalidated because
                       of memory exceptions while generating the code, we
                       must recompute the hash index here */
                    next_tb = 0;
                    tb_invalidated_flag = 0;
                }
#ifdef CONFIG_DEBUG_EXEC
                qemu_log_mask(CPU_LOG_EXEC, "Trace 0x%08lx [" TARGET_FMT_lx "] %s\n",
                             (long)tb->tc_ptr, tb->pc,
                             lookup_symbol(tb->pc));
#endif
                /* see if we can patch the calling TB. When the TB
                   spans two pages, we cannot safely do a direct
                   jump. */
		// TRL: note that this is where the translation block chaining happens.
		// (T0 & ~3) contains pointer to previous translation block.
		// (T0 & 3) contains info about which branch we took (why 2 bits?)
		// tb is current translation block.  
                if (0 && rr_mode != RR_REPLAY)
                {		
                    if (next_tb != 0 && tb->page_addr[1] == -1) {
                        tb_add_jump((TranslationBlock *)(next_tb & ~3), next_tb & 3, tb);
                    }
		} else {
		  /*
		    TRL In 0.9.1, here, in the else branch, we BREAK_CHAIN.
		    There appears to be no equivalent in 1.0.1.  :<
		  */
		}		

		spin_unlock(&tb_lock);	       

                /* cpu_interrupt might be called while translating the
                   TB, but before it is linked into a potentially
                   infinite loop and becomes env->current_tb. Avoid
                   starting execution if there is a pending interrupt. */
                env->current_tb = tb;
                barrier();

                // Capture exit_request
                saved_exit_request = env->exit_request;

                rr_set_program_point();
                rr_skipped_callsite_location = RR_CALLSITE_CPU_EXEC_000;
                if (rr_in_record()) {
                    rr_exit_request(&saved_exit_request);
                }
                else if (rr_in_replay()) {
                    if (!rr_use_live_exit_request) {
                        rr_exit_request(&saved_exit_request);
                    }
                }

                if (rr_debug_whisper()) {
                      qemu_log_mask(CPU_LOG_RR, 
                          "RR_CALLSITE_CPU_EXEC_000 exit_request %d: env->eflags=%x env->hflags=%x env->hflags2=%x\n", 
                          saved_exit_request, env->eflags, env->hflags, env->hflags2);
                }

                if(saved_exit_request != env->exit_request) {
                    //printf("RR_CALLSITE_CPU_EXEC_000: Ruh roh! Someone done changed env->exit_request behind our back!\n");
                    fflush(stdout);
                }

                if (likely(!saved_exit_request)) {
                    tc_ptr = tb->tc_ptr;
                    //mz setting program point just before call to gen_func()
                    rr_set_program_point();
                    //mz Actually jump into the generated code
                    /* execute the generated code */
                    next_tb = tcg_qemu_tb_exec(env, tc_ptr);
                    if ((next_tb & 3) == 2) {
                        /* Instruction counter expired.  */
                        int insns_left;
                        tb = (TranslationBlock *)(long)(next_tb & ~3);
                        /* Restore PC.  */
                        cpu_pc_from_tb(env, tb);
                        insns_left = env->icount_decr.u32;
                        if (env->icount_extra && insns_left >= 0) {
                            /* Refill decrementer and continue execution.  */
                            env->icount_extra += insns_left;
                            if (env->icount_extra > 0xffff) {
                                insns_left = 0xffff;
                            } else {
                                insns_left = env->icount_extra;
                            }
                            env->icount_extra -= insns_left;
                            env->icount_decr.u16.low = insns_left;
                        } else {
                            if (insns_left > 0) {
                                /* Execute remaining instructions.  */
                                cpu_exec_nocache(env, insns_left, tb);
                            }
                            env->exception_index = EXCP_INTERRUPT;
                            next_tb = 0;
                            cpu_loop_exit(env, __FILE__, __LINE__);
                        }
                    }
                }
                env->current_tb = NULL;
                /* reset soft MMU for next block (it can currently
                   only be set by a memory fault) */
            } /* for(;;) */
        } else {
            /* Reload env after longjmp - the compiler may have smashed all
             * local variables as longjmp is marked 'noreturn'. */
            env = cpu_single_env;
        }
    } /* for(;;) */


#if defined(TARGET_I386)
    /* restore flags in standard format */
    env->eflags = env->eflags | cpu_cc_compute_all(env, CC_OP)
        | (DF & DF_MASK);
#elif defined(TARGET_ARM)
    /* XXX: Save/restore host fpu exception state?.  */
#elif defined(TARGET_UNICORE32)
#elif defined(TARGET_SPARC)
#elif defined(TARGET_PPC)
#elif defined(TARGET_LM32)
#elif defined(TARGET_M68K)
    cpu_m68k_flush_flags(env, env->cc_op);
    env->cc_op = CC_OP_FLAGS;
    env->sr = (env->sr & 0xffe0)
              | env->cc_dest | (env->cc_x << 4);
#elif defined(TARGET_MICROBLAZE)
#elif defined(TARGET_MIPS)
#elif defined(TARGET_SH4)
#elif defined(TARGET_ALPHA)
#elif defined(TARGET_CRIS)
#elif defined(TARGET_S390X)
#elif defined(TARGET_XTENSA)
    /* XXXXX */
#else
#error unsupported target CPU
#endif

    /* fail safe : never use cpu_single_env outside cpu_exec() */
    cpu_single_env = NULL;
    return ret;
}
