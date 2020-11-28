#include <errno.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>

#include "asm.h"
#include "mem.h"
#include "pte.h"
#include "queue.h"
#include "tramp.h"

#undef current_task

#define XNUSPY_INSTALL_HOOK         (0)
#define XNUSPY_UNINSTALL_HOOK       (1)
#define XNUSPY_CHECK_IF_PATCHED     (2)
#define XNUSPY_GET_FUNCTION         (3)
#define XNUSPY_MAX_FLAVOR           XNUSPY_GET_FUNCTION

/* values for XNUSPY_GET_FUNCTION */
#define KPROTECT                    (0)
#define COPYOUT                     (1)
#define MAX_FUNCTION                COPYOUT

#define MARK_AS_KERNEL_OFFSET __attribute__((section("__DATA,__koff")))

/* XXX For debugging only */
MARK_AS_KERNEL_OFFSET void (*kprintf)(const char *fmt, ...);
MARK_AS_KERNEL_OFFSET void (*IOSleep)(unsigned int millis);

MARK_AS_KERNEL_OFFSET uint64_t iOS_version = 0;
MARK_AS_KERNEL_OFFSET void *(*kalloc_canblock)(vm_size_t *sizep, bool canblock,
        void *site);
MARK_AS_KERNEL_OFFSET void *(*kalloc_external)(vm_size_t sz);
MARK_AS_KERNEL_OFFSET void (*kfree_addr)(void *addr);
MARK_AS_KERNEL_OFFSET void (*kfree_ext)(void *addr, vm_size_t sz);
MARK_AS_KERNEL_OFFSET void (*lck_rw_lock_shared)(void *lock);
MARK_AS_KERNEL_OFFSET uint32_t (*lck_rw_done)(void *lock);
MARK_AS_KERNEL_OFFSET void *(*lck_grp_alloc_init)(const char *grp_name,
        void *attr);
MARK_AS_KERNEL_OFFSET void *(*lck_rw_alloc_init)(void *grp, void *attr);
MARK_AS_KERNEL_OFFSET void (*bcopy_phys)(uint64_t src, uint64_t dst,
        vm_size_t bytes);
MARK_AS_KERNEL_OFFSET uint64_t (*phystokv)(uint64_t pa);
MARK_AS_KERNEL_OFFSET int (*copyin)(const uint64_t uaddr, void *kaddr,
        vm_size_t nbytes);
MARK_AS_KERNEL_OFFSET int (*copyout)(const void *kaddr, uint64_t uaddr,
        vm_size_t nbytes);
MARK_AS_KERNEL_OFFSET uint32_t *ncpusp;
MARK_AS_KERNEL_OFFSET struct cpu_data_entry *CpuDataEntriesp;
MARK_AS_KERNEL_OFFSET vm_offset_t (*ml_io_map)(vm_offset_t phys_addr,
        vm_size_t size);
MARK_AS_KERNEL_OFFSET void *mh_execute_header;
MARK_AS_KERNEL_OFFSET uint64_t kernel_slide;

MARK_AS_KERNEL_OFFSET void (*flush_mmu_tlb_region)(uint64_t va, uint32_t len);
MARK_AS_KERNEL_OFFSET void (*flush_mmu_tlb_region_asid_async)(uint64_t va,
        uint32_t len, void *pmap);
MARK_AS_KERNEL_OFFSET void (*InvalidatePoU_IcacheRegion)(uint64_t va, uint32_t len);
MARK_AS_KERNEL_OFFSET void *(*current_task)(void);

struct pmap_statistics {
    integer_t	resident_count;	/* # of pages mapped (total)*/
    integer_t	resident_max;	/* # of pages mapped (peak) */
    integer_t	wired_count;	/* # of pages wired */
    integer_t	device;
    integer_t	device_peak;
    integer_t	internal;
    integer_t	internal_peak;
    integer_t	external;
    integer_t	external_peak;
    integer_t	reusable;
    integer_t	reusable_peak;
    uint64_t	compressed __attribute__((aligned(8)));
    uint64_t	compressed_peak __attribute__((aligned(8)));
    uint64_t	compressed_lifetime __attribute__((aligned(8)));
};

struct queue_entry {
    struct queue_entry *next;
    struct queue_entry *prev;
};

typedef struct queue_entry queue_chain_t;
typedef struct queue_entry queue_head_t;

struct pmap {
    uint64_t *tte;
    uint64_t ttep;
    uint64_t min;
    uint64_t max;
    void *ledger;
    struct {
        uint64_t lock_data;
        uint64_t type;
    } lock;
    struct pmap_statistics stats;
    queue_chain_t pmaps;

};

MARK_AS_KERNEL_OFFSET struct pmap *(*get_task_pmap)(void *task);
MARK_AS_KERNEL_OFFSET queue_head_t *map_pmap_list;

#define tt1_index(pmap, addr)								\
	(((addr) & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT)
#define tt2_index(pmap, addr)								\
	(((addr) & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT)
#define tt3_index(pmap, addr)								\
	(((addr) & ARM_TT_L3_INDEX_MASK) >> ARM_TT_L3_SHIFT)

/* shorter macros so I can stay under 80 column lines */
#define DIST_FROM_REFCNT_TO(x) __builtin_offsetof(struct xnuspy_tramp, x) - \
    __builtin_offsetof(struct xnuspy_tramp, refcnt)

/* This structure represents a function hook. Every xnuspy_tramp struct resides
 * on writeable, executable memory. */
struct xnuspy_tramp {
    /* Address of userland replacement */
    uint64_t replacement;
    _Atomic uint32_t refcnt;
    /* The trampoline for a hooked function. When the user installs a hook
     * on a function, the first instruction of that function is replaced
     * with a branch to here. An xnuspy trampoline looks like this:
     *  tramp[0]    ADR X16, <refcntp>
     *  tramp[1]    B _save_original_state0
     *  tramp[2]    B _reftramp0
     *  tramp[3]    B _swap_ttbr0
     *  tramp[4]    ADR X16, <replacementp>
     *  tramp[5]    LDR X16, [X16]
     *  tramp[6]    BR X16
     */
    uint32_t tramp[7];
    /* An abstraction that represents the original function. It's just another
     * trampoline, but it can take on one of five forms. Every form starts
     * with this header:
     *  orig[0]     B _save_original_state1
     *  orig[1]     B _reftramp1
     *  orig[2]     B _restore_ttbr0
     *
     * Continuing from that header, the most common form is:
     *  orig[3]     <original first instruction of the hooked function>
     *  orig[4]     ADR X16, #0xc
     *  orig[5]     LDR X16, [X16]
     *  orig[6]     BR X16
     *  orig[7]     <address of second instruction of the hooked function>[31:0]
     *  orig[8]     <address of second instruction of the hooked function>[63:32]
     *
     * The above form is taken when the original first instruction of the hooked
     * function is not an immediate conditional branch (b.cond), an immediate
     * compare and branch (cbz/cbnz), an immediate test and branch (tbz/tbnz),
     * or an ADR.
     * These are special cases because the immediates do not contain enough
     * bits for me to just "fix up", so I need to emit an equivalent sequence
     * of instructions.
     *
     * If the first instruction was B.cond <label>
     *  orig[3]     ADR X16, #0x14
     *  orig[4]     ADR X17, #0x18
     *  orig[5]     CSEL X16, X16, X17, <cond>
     *  orig[6]     LDR X16, [X16]
     *  orig[7]     BR X16
     *  orig[8]     <destination if condition holds>[31:0]
     *  orig[9]     <destination if condition holds>[63:32]
     *  orig[10]     <address of second instruction of the hooked function>[31:0]
     *  orig[11]    <address of second instruction of the hooked function>[63:32]
     *
     * If the first instruction was CBZ Rn, <label> or CBNZ Rn, <label>
     *  orig[3]     ADR X16, #0x18
     *  orig[4]     ADR X17, #0x1c
     *  orig[5]     CMP Rn, #0
     *  orig[6]     CSEL X16, X16, X17, <if CBZ, eq, if CBNZ, ne>
     *  orig[7]     LDR X16, [X16]
     *  orig[8]     BR X16
     *  orig[9]     <destination if condition holds>[31:0]
     *  orig[10]     <destination if condition holds>[63:32]
     *  orig[11]    <address of second instruction of the hooked function>[31:0]
     *  orig[12]    <address of second instruction of the hooked function>[63:32]
     *
     * If the first instruction was TBZ Rn, #n, <label> or TBNZ Rn, #n, <label>
     *  orig[3]     ADR X16, #0x18
     *  orig[4]     ADR X17, #0x1c
     *  orig[5]     TST Rn, #(1 << n)
     *  orig[6]     CSEL X16, X16, X17, <if TBZ, eq, if TBNZ, ne>
     *  orig[7]     LDR X16, [X16]
     *  orig[8]     BR X16
     *  orig[9]     <destination if condition holds>[31:0]
     *  orig[10]     <destination if condition holds>[63:32]
     *  orig[11]    <address of second instruction of the hooked function>[31:0]
     *  orig[12]    <address of second instruction of the hooked function>[63:32]
     *
     * If the first instruction was ADR Rn, #n
     *  orig[3]     ADRP Rn, #n@PAGE
     *  orig[4]     ADD Rn, Rn, #n@PAGEOFF
     *  orig[5]     ADR X16, #0xc
     *  orig[6]     LDR X16, [X16]
     *  orig[7]     BR X16
     *  orig[8]     <address of second instruction of the hooked function>[31:0]
     *  orig[9]     <address of second instruction of the hooked function>[63:32]
     */
    uint32_t orig[13];
    /* This will be set to whatever TTBR0_EL1 is from the process that
     * installed this hook. Before we call into the user replacement, we need
     * to swap TTBR0_EL1 with this one. */
    uint64_t ttbr0_el1;
};

static void desc_xnuspy_tramp(struct xnuspy_tramp *t, uint32_t orig_tramp_len){
    kprintf("This xnuspy_tramp is @ %#llx\n", (uint64_t)t);
    kprintf("Replacement: %#llx\n", t->replacement);
    kprintf("Refcount:    %d\n", t->refcnt);
    
    kprintf("Replacement trampoline:\n");
    for(int i=0; i<5; i++)
        kprintf("\ttramp[%d]    %#x\n", i, t->tramp[i]);

    kprintf("Original trampoline:\n");
    for(int i=0; i<orig_tramp_len; i++)
        kprintf("\ttramp[%d]    %#x\n", i, t->orig[i]);

    kprintf("TTBR0_EL1 of process which made this hook: %#llx\n", t->ttbr0_el1);
}

MARK_AS_KERNEL_OFFSET struct xnuspy_tramp *xnuspy_tramp_page;
MARK_AS_KERNEL_OFFSET uint8_t *xnuspy_tramp_page_end;

static int xnuspy_init_flag = 0;

static void xnuspy_init(void){
    /* Mark the xnuspy_tramp page as writeable/executable */
    vm_prot_t prot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    kprotect((uint64_t)xnuspy_tramp_page, 0x4000, prot);

    /* Zero out PAN in case no instruction did it before us. After our kernel
     * patches, the PAN bit cannot be set to 1 again.
     *
     * msr pan, #0
     *
     * XXX XXX NEED TO CHECK IF THE HARDWARE SUPPORTS THIS BIT
     */
    asm volatile(".long 0xd500409f");
    asm volatile("isb sy");

    /* combat short read of this image */
    /* asm volatile(".align 14"); */
    /* asm volatile(".align 14"); */

    xnuspy_init_flag = 1;
}

/* If you decide to edit the functions marked as naked, you need to make
 * sure clang isn't clobbering registers. */

/* swap_ttbr0: set this CPU's TTBR0_EL1 to the one in the current xnuspy_tramp
 * struct. This is meant to be called only from a trampoline inside the
 * current xnuspy_tramp struct since we have to manually branch back to that
 * trampoline.
 */
__attribute__ ((naked)) void swap_ttbr0(void){
    asm volatile(""
            "mrs x16, dbgbvr3_el1\n"
            "add x17, x16, %[ttbr0_el1_dist]\n"
            "ldr x17, [x17]\n"
            "msr ttbr0_el1, x17\n"
            "isb\n"
            "dsb ish\n"
            "tlbi vmalle1\n"
            "dsb ish\n"
            "isb\n"
            /* branch back to tramp+4 */
            "add x16, x16, %[tramp_plus_4_dist]\n"
            "br x16\n"
            : : [ttbr0_el1_dist] "r" (DIST_FROM_REFCNT_TO(ttbr0_el1)),
            [tramp_plus_4_dist] "r" (DIST_FROM_REFCNT_TO(tramp[4]))
            );
}

/* restore_ttbr0: restore this CPU's original TTBR0_EL1. Again, this is only
 * meant to be called from a trampoline from the current xnuspy_tramp struct.
 */
__attribute__ ((naked)) void restore_ttbr0(void){
    asm volatile(""
            "mrs x16, dbgbvr0_el1\n"
            "msr ttbr0_el1, x16\n"
            "isb\n"
            "dsb ish\n"
            "tlbi vmalle1\n"
            "dsb ish\n"
            "isb\n"
            "mrs x16, dbgbvr3_el1\n"
            /* branch back to orig+3 */
            "add x16, x16, %[orig_plus_3_dist]\n"
            "br x16\n"
            : : [orig_plus_3_dist] "r" (DIST_FROM_REFCNT_TO(orig[3]))
            );
}

/* reletramp: release a reference, using the reference count pointer held
 * inside DBGBVR3_EL1.
 *
 * TODO actually sever the branch from the hooked function to the tramp
 * when the count hits zero
 *
 * reletramp0 and reletramp1 will call reletramp_common to drop a reference,
 * and then they'll branch back to their original callers. Because we're
 * restoring LR in both those functions, using BL is safe.
 */
__attribute__ ((naked)) void reletramp_common(void){
    asm volatile(""
            "mrs x16, dbgbvr3_el1\n"
            "1:\n"
            "ldaxr w14, [x16]\n"
            "mov x15, x14\n"
            "sub w14, w15, #1\n"
            "stlxr w15, w14, [x16]\n"
            "cbnz w15, 1b\n"
            "ret\n"
            );
}

/* This function is only called when the replacement code returns back to
 * its caller. We are always returning back to the kernel, so we need to
 * restore the original TTBR0_EL1.
 */ 
__attribute__ ((naked)) void reletramp0(void){
    asm volatile(""
            "bl _reletramp_common\n"
            "mrs x9, dbgbvr0_el1\n"
            "msr ttbr0_el1, x9\n"
            "isb\n"
            "dsb ish\n"
            "tlbi vmalle1\n"
            "dsb ish\n"
            "isb\n"
            "mrs x29, dbgbvr2_el1\n"
            "mrs x30, dbgbvr1_el1\n"
            "ret"
            );
}

/* This function is only called when the original function returns back
 * to the user's replacement code. We are always returning back to the user's
 * code, so we need to swap TTBR0_EL1 back to the one in this xnuspy_tramp
 * struct.
 */ 
__attribute__ ((naked)) void reletramp1(void){
    asm volatile(""
            "bl _reletramp_common\n"
            "mrs x16, dbgbvr3_el1\n"
            "add x16, x16, %[ttbr0_el1_dist]\n"
            "ldr x16, [x16]\n"
            "msr ttbr0_el1, x16\n"
            "isb\n"
            "dsb ish\n"
            "tlbi vmalle1\n"
            "dsb ish\n"
            "isb\n"
            "mrs x29, dbgbvr5_el1\n"
            "mrs x30, dbgbvr4_el1\n"
            "ret"
            : : [ttbr0_el1_dist] "r" (DIST_FROM_REFCNT_TO(ttbr0_el1))
            );
}

/* save_original_state0: save this CPU's original TTBR0_EL1, FP, LR, and
 * X16, and set LR to the appropriate 'reletramp' routine. This is only called
 * when the kernel calls the hooked function. X16 holds a pointer to the refcnt
 * of an xnuspy_tramp when this is called.
 *
 * For both save_original_state functions, I need a way to persist data. For
 * save_original_state0, I need to save the original TTBR0_EL1 and a pointer
 * to the reference count of whatever xnuspy_tramp struct X16 holds. For
 * both, I also need to save the current stack frame because I set LR to
 * 'reletramp' and have to be able to branch back to the original caller.
 *
 * Normally, I would just use the stack, but functions like
 * kprintf rely on some arguments being passed on the stack. If I were
 * to modify it, the parameters would be incorrect inside of the user's
 * replacement code. Instead of using the stack, I will use the first six
 * debug breakpoint value registers in the following way:
 *  
 * DBGBVR0_EL1: This CPU's original TTBR0_EL1.
 * DBGBVR1_EL1: Original link register when the kernel calls the hooked function.
 * DBGBVR2_EL1: Original frame pointer when the kernel calls the hooked function.
 * DBGBVR3_EL1: A pointer to the current xnuspy_tramp's reference count.
 * DBGBVR4_EL1: Original link register when the user calls the original function.
 * DBGBVR5_EL1: Original frame pointer when the user calls the original function.
 *
 * Because I am using these registers, you CANNOT set any hardware breakpoints
 * if you're debugging something while xnuspy is doing its thing. You can set
 * software breakpoints, though. You're able to specify whether you want a
 * software breakpoint or a hardware breakpoint inside of LLDB.
 */
__attribute__ ((naked)) void save_original_state0(void){
    asm volatile(""
            "mrs x9, ttbr0_el1\n"
            "msr dbgbvr0_el1, x9\n"
            "msr dbgbvr1_el1, x30\n"
            "msr dbgbvr2_el1, x29\n"
            "msr dbgbvr3_el1, x16\n"
            "mov x30, %[reletramp0]\n"
            /* branch back to tramp+2 */
            "add x16, x16, %[tramp_plus_2_dist]\n"
            "br x16\n"
            : : [reletramp0] "r" (reletramp0),
            [tramp_plus_2_dist] "r" (DIST_FROM_REFCNT_TO(tramp[2]))
            );
}

/* save_original_state1: save this CPU's FP and LR. This is only called when
 * the user calls the original function from their replacement code. */
__attribute__ ((naked)) void save_original_state1(void){
    asm volatile(""
            "msr dbgbvr4_el1, x30\n"
            "msr dbgbvr5_el1, x29\n"
            "mov x30, %[reletramp1]\n"
            "mrs x16, dbgbvr3_el1\n"
            /* branch back to orig+1 */
            "add x16, x16, %[orig_plus_1_dist]\n"
            "br x16\n"
            : : [reletramp1] "r" (reletramp1),
            [orig_plus_1_dist] "r" (DIST_FROM_REFCNT_TO(orig[1]))
            );
}

/* reftramp0 and reftramp1: take a reference on an xnuspy_tramp.
 *
 * reftramp0 is called when the kernel calls the hooked function.
 *
 * reftramp1 is called when the original function, called through the 'orig'
 * trampoline, is called by the user.
 *
 * Sadly, these can't be merged into one function because we cannot modify
 * LR and we have no way of knowing what context (tramp or orig) it would be
 * called from.
 */
__attribute__ ((naked)) void reftramp0(void){
    asm volatile(""
            "mrs x16, dbgbvr3_el1\n"
            "1:\n"
            "ldaxr w14, [x16]\n"
            "mov x15, x14\n"
            "add w14, w15, #1\n"
            "stlxr w15, w14, [x16]\n"
            "cbnz w15, 1b\n"
            /* branch back to tramp+3 */
            "add x16, x16, %[tramp_plus_3_dist]\n"
            "br x16\n"
            : : [tramp_plus_3_dist] "r" (DIST_FROM_REFCNT_TO(tramp[3]))
            );
}

__attribute__ ((naked)) void reftramp1(void){
    asm volatile(""
            "mrs x16, dbgbvr3_el1\n"
            "1:\n"
            "ldaxr w14, [x16]\n"
            "mov x15, x14\n"
            "add w14, w15, #1\n"
            "stlxr w15, w14, [x16]\n"
            "cbnz w15, 1b\n"
            /* branch back to orig+2 */
            "add x16, x16, %[orig_plus_2_dist]\n"
            "br x16\n"
            : : [orig_plus_2_dist] "r" (DIST_FROM_REFCNT_TO(orig[2]))
            );
}

static int xnuspy_install_hook(uint64_t target, uint64_t replacement,
        uint64_t /* __user */ origp){
    kprintf("%s: called with target %#llx replacement %#llx origp %#llx\n",
            __func__, target, replacement, origp);

    /* slide target */
    target += kernel_slide;

    /* Find a free xnuspy_tramp inside the trampoline page */
    struct xnuspy_tramp *tramp = xnuspy_tramp_page;

    while((uint8_t *)tramp < xnuspy_tramp_page_end){
        if(!tramp->refcnt)
            break;

        tramp++;
    }

    if(!tramp){
        kprintf("%s: no free xnuspy_tramp structs\n", __func__);
        return ENOSPC;
    }

    kprintf("%s: got free xnuspy_ctl struct @ %#llx\n", __func__, tramp);

    /* +1 for creation */
    tramp->refcnt = 1;
    tramp->replacement = replacement;

    /* Build the trampoline to the replacement as well as the trampoline
     * that represents the original function */

    uint32_t orig_tramp_len = 0;

    generate_replacement_tramp(save_original_state0, reftramp0, swap_ttbr0,
            tramp->tramp);

    generate_original_tramp(target + 4, save_original_state1, reftramp1,
            restore_ttbr0, tramp->orig, &orig_tramp_len);

    /* copyout the original function trampoline before the replacement
     * is called */
    uint32_t *orig_tramp = tramp->orig;
    int err = copyout(&orig_tramp, origp, sizeof(origp));

    /* XXX do something if copyout fails */

    uint64_t ttbr0_el1;
    asm volatile("mrs %0, ttbr0_el1" : "=r" (ttbr0_el1));
    tramp->ttbr0_el1 = ttbr0_el1;

    desc_xnuspy_tramp(tramp, orig_tramp_len);

    kprintf("%s: on this CPU, ttbr0_el1 == %#llx baddr phys %#llx baddr kva %#llx\n",
            __func__, ttbr0_el1, ttbr0_el1 & 0xfffffffffffe,
            phystokv(ttbr0_el1 & 0xfffffffffffe));

    /* XXX we don't need to unset nG bit in user pte if we are just swapping ttbr0? */
    
    /* Mark the user replacement as executable from EL1. This function
     * will clear NX as well as PXN. */
    /* TODO We need to mark the entirety of the calling processes' __text
     * segment as executable from EL1 so the user can call other functions
     * they write inside their program from their kernel hook. */
    // XXX something like get_calling_process_text_segment
    uprotect(tramp->replacement, 0x4000, VM_PROT_READ | VM_PROT_EXECUTE);

    /* All the trampolines are set up, write the branch */
    kprotect(target, 0x4000, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);

    *(uint32_t *)target = assemble_b(target, (uint64_t)tramp->tramp);

    asm volatile("dc cvau, %0" : : "r" (target));
    asm volatile("dsb ish");
    asm volatile("ic ivau, %0" : : "r" (target));
    asm volatile("dsb ish");
    asm volatile("isb sy");

    return 0;
}

static int xnuspy_uninstall_hook(uint64_t target){
    kprintf("%s: XNUSPY_UNINSTALL_HOOK is not implemented yet\n", __func__);
    return ENOSYS;
}

static int xnuspy_get_function(uint64_t which, uint64_t /* __user */ outp){
    kprintf("%s: XNUSPY_GET_FUNCTION called with which %lld origp %#llx\n",
            __func__, which, outp);

    if(which > MAX_FUNCTION)
        return ENOENT;

    switch(which){
        case KPROTECT:
            which = (uint64_t)kprotect;
            break;
        case COPYOUT:
            which = (uint64_t)copyout;
            break;
        default:
            break;
    };

    return copyout(&which, outp, sizeof(outp));
}

static int xnuspy_make_callable(uint64_t target, uint64_t /* __user */ origp){
    kprintf("%s: called with target %#llx origp %#llx\n", __func__, target, origp);

    /* slide target */
    target += kernel_slide;

    /* The problem we have is the following: we are swapping the current CPU's
     * TTBR0_EL1 with the TTBR0_EL1 from the CPU which installed a hook. The
     * user is allowed to call other kernel functions from within their
     * userland replacement. So what happens if some kernel function the user
     * calls within their replacement relies on seeing the original TTBR0_EL1?
     *
     * To solve this problem, an external kernel function will be represented
     * as an xnuspy_wrapper struct. Every xnuspy_wrapper struct resides on
     * writable, executable memory. The wrapper will contain a small trampoline
     * to restore the original TTBR0, call the actual kernel function, swap
     * back the TTBR0 of the CPU that installed the hook, and then return
     * back to the user's replacement code.
     *
     * Again, we cannot use the stack to persist data across function calls.
     * Since I'm already using most of the debug breakpoint value registers,
     * I'll just use callee-saved registers instead.
     *
     * An xnuspy_wrapper trampoline looks like this:
     *  wtramp[0]   MOV X28, X29
     *  wtramp[1]   MOV X27, X30
     *  wtramp[2]   MOV X26, X19
     *  wtramp[3]   MOV X25, X20
     *  wtramp[4]   MOV X24, X21
     *  wtramp[5]   MOV X23, X22
     *
     *  XXX here we have x19-x22 to work with
     *  
     *  ; FAR_EL1 is the register we're persisting the original value
     *  ; of TTBR0_EL1 with.
     *  wtramp[6]   MRS X19, FAR_EL1
     *
     *  ; Back up current TTBR0
     *  wtramp[7]   MRS X20, TTBR0_EL1
     *
     *  ; Set original TTBR0_EL1
     *  wtramp[8]   MSR TTBR0_EL1, X19
     *
     *  wtramp[9,13] barriers, invalidate TLBs
     *
     *  XXX stick a pointer to the kernel function in some register
     *
     *  wtramp[.]   BLR <kernel_functionp>
     *
     *  wtramp[.]   MSR TTBR0_EL1, X20
     *
     *  wtramp[...] barriers, invalidate TLBs
     *
     *  wtramp[.]   MOV X22, X23
     *  wtramp[.]   MOV X21, X24
     *  wtramp[.]   MOV X20, X25
     *  wtramp[.]   MOV X19, X26
     *  wtramp[.]   MOV X30, X27
     *  wtramp[.]   MOV X29, X28
     *  wtramp[.]   RET
     */

    return 0;
}

struct xnuspy_ctl_args {
    uint64_t flavor;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
};

int xnuspy_ctl(void *p, struct xnuspy_ctl_args *uap, int *retval){
    uint64_t flavor = uap->flavor;

    if(flavor > XNUSPY_MAX_FLAVOR){
        kprintf("%s: bad flavor %d\n", __func__, flavor);
        *retval = -1;
        return EINVAL;
    }

    kprintf("%s: got flavor %d\n", __func__, flavor);
    kprintf("%s: kslide %#llx\n", __func__, kernel_slide);
    kprintf("%s: xnuspy_ctl @ %#llx (unslid)\n", __func__,
            (uint64_t)xnuspy_ctl - kernel_slide);
    kprintf("%s: xnuspy_ctl tramp page @ [%#llx,%#llx] (unslid)\n", __func__,
            (uint64_t)xnuspy_tramp_page - kernel_slide,
            (uint64_t)xnuspy_tramp_page_end - kernel_slide);

    if(!xnuspy_init_flag)
        xnuspy_init();

    int res;

    switch(flavor){
        case XNUSPY_CHECK_IF_PATCHED:
            *retval = 999;
            return 0;
        case XNUSPY_INSTALL_HOOK:
            res = xnuspy_install_hook(uap->arg1, uap->arg2, uap->arg3);
            break;
        case XNUSPY_UNINSTALL_HOOK:
            res = xnuspy_uninstall_hook(uap->arg1);
            break;
            /* XXX below will be replaced with XNUSPY_MAKE_CALLABLE */
        case XNUSPY_GET_FUNCTION:
            res = xnuspy_get_function(uap->arg1, uap->arg2);
            break;
        default:
            *retval = -1;
            return EINVAL;
    };

    if(res)
        *retval = -1;

    return res;
}
