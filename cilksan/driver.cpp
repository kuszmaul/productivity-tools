#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <sys/mman.h>

#include "checking.h"
#include "cilksan_internal.h"
#include "debug_util.h"
#include "driver.h"
#include "stack.h"
#include "addrmap.h"

// Raise a link-time error if the user attempts to use Cilksan with -static.
//
// This trick is copied from AddressSanitizer.  We only use this trick on Linux,
// since static linking is not supported on MacOSX anyway.
#ifdef __linux__
#include <link.h>
void *CilksanDoesNotSupportStaticLinkage() {
  // This will fail to link with -static.
  return &_DYNAMIC;  // defined in link.h
}
#endif // __linux__

// FILE io used to print error messages
FILE *err_io = stderr;

// defined in libopencilk
extern "C" int __cilkrts_is_initialized(void);
extern "C" void __cilkrts_internal_set_nworkers(unsigned int nworkers);
extern "C" void __cilkrts_internal_set_force_reduce(unsigned int force_reduce);

// declared in cilksan.cpp
extern uintptr_t stack_low_addr;
extern uintptr_t stack_high_addr;

// Estimate on the value of the stack size, used to detect stack-switching.
static constexpr size_t DEFAULT_STACK_SIZE = 1UL << 21;

// declared in cilksan; for debugging only
#if CILKSAN_DEBUG
extern enum EventType_t last_event;
#endif

static csi_id_t total_call = 0;
static csi_id_t total_spawn = 0;
static csi_id_t total_loop = 0;
static csi_id_t total_load = 0;
static csi_id_t total_store = 0;
static csi_id_t total_alloca = 0;
static csi_id_t total_allocfn = 0;
static csi_id_t total_free = 0;

// We reserve lock_id == 0 for atomic operations.
const LockID_t atomic_lock_id = 0;

// Flag to track whether Cilksan is initialized.
bool TOOL_INITIALIZED = false;

// Flag to globally enable/disable instrumentation.
bool instrumentation = false;

// Reentrant flag for enabling/disabling instrumentation; 0 enables checking.
int checking_disabled = 0;

// Stack structure for tracking whether the current execution is parallel, i.e.,
// whether there are any unsynced spawns in the program execution.
Stack_t<uint8_t> parallel_execution;
Stack_t<bool> spbags_frame_skipped;

// Storage for old values of stack_low_addr and stack_high_addr, saved when
// entering a cilkified region.
uintptr_t uncilkified_stack_low_addr = (uintptr_t)-1;
uintptr_t uncilkified_stack_high_addr = 0;

// Stack for tracking whether a stack switch has occurred.
Stack_t<uint8_t> switched_stack;

// Stack structures for keeping track of MAAPs for pointer arguments to function
// calls.
Stack_t<std::pair<csi_id_t, MAAP_t>> MAAPs;
Stack_t<unsigned> MAAP_counts;

///////////////////////////////////////////////////////////////////////////
// Methods for enabling and disabling instrumentation

static inline void enable_instrumentation() {
  DBG_TRACE(DEBUG_BASIC, "Enable instrumentation.\n");
  instrumentation = true;
}

static inline void disable_instrumentation() {
  DBG_TRACE(DEBUG_BASIC, "Disable instrumentation.\n");
  instrumentation = false;
}

// outside world (including runtime).
// Non-inlined version for user code to use
CILKSAN_API void __cilksan_enable_checking(void) {
  checking_disabled--;
  cilksan_assert(checking_disabled >= 0);
  DBG_TRACE(DEBUG_BASIC, "External enable checking (%d).\n", checking_disabled);
}

// Non-inlined version for user code to use
CILKSAN_API void __cilksan_disable_checking(void) {
  cilksan_assert(checking_disabled >= 0);
  checking_disabled++;
  DBG_TRACE(DEBUG_BASIC, "External disable checking (%d).\n", checking_disabled);
}

// Non-inlined callback for user code to check if checking is enabled.
CILKSAN_API bool __cilksan_is_checking_enabled(void) {
  return (checking_disabled == 0);
}

///////////////////////////////////////////////////////////////////////////
// Hooks for setting and getting MAAPs.

CILKSAN_API void __csan_set_MAAP(MAAP_t val, csi_id_t id) {
  if (!should_check())
    return;

  DBG_TRACE(DEBUG_CALLBACK, "__csan_set_MAAP(%d, %ld)\n", val, id);
  MAAPs.push_back(std::make_pair(id, val));
}

CILKSAN_API void __csan_get_MAAP(MAAP_t *ptr, csi_id_t id, unsigned idx) {
  DBG_TRACE(DEBUG_CALLBACK, "__csan_get_MAAP(%x, %d, %d)\n", ptr, id, idx);
  // We presume that __csan_get_MAAP runs early in the function, so if
  // instrumentation is disabled, it's disabled for the whole function.
  if (!should_check()) {
    *ptr = MAAP_t::NoAccess;
    return;
  }

  unsigned MAAP_count = MAAP_counts.back();
  if (idx >= MAAP_count) {
    DBG_TRACE(DEBUG_CALLBACK, "  No MAAP found: idx %d >= count %d\n", idx,
              MAAP_count);
    // The stack doesn't have MAAPs for us, so assume the worst: modref with
    // aliasing.
    *ptr = MAAP_t::ModRef;
    return;
  }

  std::pair<csi_id_t, MAAP_t> MAAP = *MAAPs.ancestor(idx);
  if (MAAP.first == id) {
    DBG_TRACE(DEBUG_CALLBACK, "  Found MAAP: %d\n", MAAP.second);
    *ptr = MAAP.second;
  } else {
    DBG_TRACE(DEBUG_CALLBACK, "  No MAAP found\n");
    // The stack doesn't have MAAPs for us, so assume the worst.
    *ptr = MAAP_t::ModRef;
  }
}

///////////////////////////////////////////////////////////////////////////
// Tool initialization and deinitialization

// called upon process exit
static void csan_destroy(void) {
  disable_instrumentation();
  disable_checking();
  CilkSanImpl.deinit();
  fflush(stdout);
  if (call_pc) {
    free(call_pc);
    call_pc = nullptr;
  }
  if (spawn_pc) {
    free(spawn_pc);
    spawn_pc = nullptr;
  }
  if (loop_pc) {
    free(loop_pc);
    loop_pc = nullptr;
  }
  if (load_pc) {
    free(load_pc);
    load_pc = nullptr;
  }
  if (store_pc) {
    free(store_pc);
    store_pc = nullptr;
  }
  if (alloca_pc) {
    free(alloca_pc);
    alloca_pc = nullptr;
  }
  if (allocfn_pc) {
    free(allocfn_pc);
    allocfn_pc = nullptr;
  }
  if (allocfn_prop) {
    free(allocfn_prop);
    allocfn_prop = nullptr;
  }
  if (free_pc) {
    free(free_pc);
    free_pc = nullptr;
  }
}

CilkSanImpl_t::~CilkSanImpl_t() {
  csan_destroy();
  TOOL_INITIALIZED = false;
}

static void init_internal() {
  if (__cilkrts_is_initialized()) {
    __cilkrts_internal_set_nworkers(1);
    __cilkrts_internal_set_force_reduce(1);
  } else {
    // Force the number of Cilk workers to be 1.
    char *e = getenv("CILK_NWORKERS");
    if (!e || 0 != strcmp(e, "1")) {
      if (setenv("CILK_NWORKERS", "1", 1)) {
        fprintf(err_io, "Error setting CILK_NWORKERS to be 1\n");
        exit(1);
      }
    }

    // Force reductions.
    e = getenv("CILK_FORCE_REDUCE");
    if (!e || 0 != strcmp(e, "1")) {
      if (setenv("CILK_FORCE_REDUCE", "1", 1)) {
        fprintf(err_io, "Error setting CILKS_FORCE_REDUCE to be 1\n");
        exit(1);
      }
    }
  }
}

CILKSAN_API void __csan_init() {
  // This method should only be called once.

  // We use the automatic deallocation of the CilkSanImpl top-level tool object
  // to shutdown and cleanup the tool at program termination.

  init_internal();
}

// Helper function to grow a map from CSI ID to program counter (PC).
static void grow_pc_table(uintptr_t *&table, csi_id_t &table_cap,
                          csi_id_t extra_cap) {
  csi_id_t new_cap = table_cap + extra_cap;
  table = (uintptr_t *)realloc(table, new_cap * sizeof(uintptr_t));
  for (csi_id_t i = table_cap; i < new_cap; ++i)
    table[i] = (uintptr_t)nullptr;
  table_cap = new_cap;
}

CILKSAN_API
void __csan_unit_init(const char *const file_name,
                      const csan_instrumentation_counts_t counts) {
  // Grow the tables mapping CSI ID's to PC values.
  if (counts.num_call)
    grow_pc_table(call_pc, total_call, counts.num_call);
  if (counts.num_detach)
    grow_pc_table(spawn_pc, total_spawn, counts.num_detach);
  if (counts.num_loop)
    grow_pc_table(loop_pc, total_loop, counts.num_loop);
  if (counts.num_load)
    grow_pc_table(load_pc, total_load, counts.num_load);
  if (counts.num_store)
    grow_pc_table(store_pc, total_store, counts.num_store);
  if (counts.num_alloca)
    grow_pc_table(alloca_pc, total_alloca, counts.num_alloca);
  if (counts.num_allocfn) {
    csi_id_t new_cap = total_allocfn + counts.num_allocfn;
    allocfn_prop = (allocfn_prop_t *)realloc(allocfn_prop,
                                             new_cap * sizeof(allocfn_prop_t));
    for (csi_id_t i = total_allocfn; i < new_cap; ++i)
      allocfn_prop[i].allocfn_ty = uint8_t(-1);
    grow_pc_table(allocfn_pc, total_allocfn, counts.num_allocfn);
  }
  if (counts.num_free)
    grow_pc_table(free_pc, total_free, counts.num_free);
}

///////////////////////////////////////////////////////////////////////////
// Standard instrumentation hooks for control flow: function entry and exit,
// spawning and syncing of tasks, etc.

static inline void handle_stack_switch(uintptr_t bp, uintptr_t sp) {
  uncilkified_stack_high_addr = stack_high_addr;
  uncilkified_stack_low_addr = stack_low_addr;

  // The base pointer may still be on the old stack, in which case we use sp to
  // set both stack_high_addr and stack_low_addr.
  if (bp - sp > DEFAULT_STACK_SIZE)
    stack_high_addr = sp;
  else
    stack_high_addr = bp;
  stack_low_addr = sp;
}

// Hook called upon entering a function.
CILKSAN_API void __csan_func_entry(const csi_id_t func_id,
                                   const void *bp, const void *sp,
                                   const func_prop_t prop) {
  if (!TOOL_INITIALIZED)
    return;

  { // Handle tool initialization as a special case.
    static bool first_call = true;
    if (first_call) {
      first_call = false;
      CilkSanImpl.init();
      enable_instrumentation();
      // Note that we start executing the program in series.
      parallel_execution.push_back(0);
      // Push a default value of 0 onto the MAAP_counts stack, in case this
      // function contains get_MAAP calls.
      MAAP_counts.push_back(0);
    }
  }

  if (!should_check())
    return;

  if (((uintptr_t)bp - (uintptr_t)sp > DEFAULT_STACK_SIZE) ||
      (stack_low_addr > (uintptr_t)sp &&
       stack_low_addr - (uintptr_t)sp > DEFAULT_STACK_SIZE)) {
    // It looks like we have switched stacks to start executing Cilk code.
    handle_stack_switch((uintptr_t)bp, (uintptr_t)sp);
    switched_stack.push_back(1);
    if ((uintptr_t)bp - (uintptr_t)sp > DEFAULT_STACK_SIZE)
      bp = sp;
  } else {
    if (stack_high_addr < (uintptr_t)bp)
      stack_high_addr = (uintptr_t)bp;
    if (stack_low_addr > (uintptr_t)sp)
      stack_low_addr = (uintptr_t)sp;
    switched_stack.push_back(0);
  }

  WHEN_CILKSAN_DEBUG({
      const csan_source_loc_t *srcloc = __csan_get_func_source_loc(func_id);
      DBG_TRACE(DEBUG_CALLBACK, "__csan_func_entry(%d) at %s (%s:%d)\n",
                func_id,
                srcloc->name, srcloc->filename,
                srcloc->line_number);
    });

  // Propagate the parallel-execution state to the child.
  uint8_t current_pe = parallel_execution.back();
  // First we push the pe value on function entry.
  parallel_execution.push_back(current_pe);
  // We push a second copy to update aggressively on detaches.
  parallel_execution.push_back(current_pe);

  CilkSanImpl.push_stack_frame((uintptr_t)bp, (uintptr_t)sp);

  if (!prop.may_spawn && CilkSanImpl.is_local_synced()) {
    spbags_frame_skipped.push_back(true);
    // Ignore entry calls into non-Cilk functions when the parent frame is
    // synced.
    enable_instrumentation();
    return;
  }
  spbags_frame_skipped.push_back(false);

  // Update the tool for entering a Cilk function.
  CilkSanImpl.do_enter(prop.num_sync_reg);
  enable_instrumentation();
}

// Hook called when exiting a function.
CILKSAN_API void __csan_func_exit(const csi_id_t func_exit_id,
                                  const csi_id_t func_id,
                                  const func_exit_prop_t prop) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check())
    return;

#if CILKSAN_DEBUG
  const csan_source_loc_t *srcloc = __csan_get_func_exit_source_loc(func_exit_id);
#endif
  DBG_TRACE(DEBUG_CALLBACK, "__csan_func_exit(%ld, %ld) at %s (%s:%d)\n",
            func_exit_id, func_id,
            srcloc->name, srcloc->filename,
            srcloc->line_number);

  if (!spbags_frame_skipped.back()) {
    // Update the tool for leaving a Cilk function.
    //
    // NOTE: Technically the sync region that would synchronize any orphaned
    // child tasks is not well defined.  This case should never arise in Cilk
    // programs.
    CilkSanImpl.do_leave(0);
  }
  spbags_frame_skipped.pop();

  // Pop both local copies of the parallel-execution state.
  parallel_execution.pop();
  parallel_execution.pop();

  CilkSanImpl.pop_stack_frame();

  if (switched_stack.back()) {
    // We switched stacks upon entering this function.  Now switch back.
    stack_high_addr = uncilkified_stack_high_addr;
    stack_low_addr = uncilkified_stack_low_addr;
  }
  switched_stack.pop();
}

// Hook called just before executing a loop.
CILKSAN_API void __csan_before_loop(const csi_id_t loop_id,
                                    const int64_t trip_count,
                                    const loop_prop_t prop) {
  if (!prop.is_tapir_loop)
    return;

  if (!should_check())
    return;

  DBG_TRACE(DEBUG_CALLBACK, "__csan_before_loop(%ld)\n", loop_id);

  // Record the address of this parallel loop.
  if (__builtin_expect(!loop_pc[loop_id], false))
    loop_pc[loop_id] = CALLERPC;

  // Push the parallel loop onto the call stack.
  CilkSanImpl.record_call(loop_id, LOOP);

  // Propagate the parallel-execution state to the child.
  uint8_t current_pe = parallel_execution.back();
  // First push the pe value on function entry.
  parallel_execution.push_back(current_pe);
  // Push an extra copy to the head, to be updated aggressively due to detaches.
  parallel_execution.push_back(current_pe);

  CilkSanImpl.do_loop_begin();
}

// Hook called just after executing a loop.
CILKSAN_API void __csan_after_loop(const csi_id_t loop_id,
                                   const unsigned sync_reg,
                                   const loop_prop_t prop) {
  if (!prop.is_tapir_loop)
    return;

  if (!should_check())
    return;

  DBG_TRACE(DEBUG_CALLBACK, "__csan_after_loop(%ld)\n", loop_id);

  CilkSanImpl.do_loop_end(sync_reg);

  // Pop the parallel-execution state.
  parallel_execution.pop();
  parallel_execution.pop();

  // Pop the call off of the call stack.
  CilkSanImpl.record_call_return(loop_id, LOOP);
}

// Hook called before a function-call instruction.
CILKSAN_API void __csan_before_call(const csi_id_t call_id,
                                    const csi_id_t func_id, unsigned MAAP_count,
                                    const call_prop_t prop) {
  if (!should_check())
    return;

  DBG_TRACE(DEBUG_CALLBACK, "__csan_before_call(%ld, %ld)\n", call_id, func_id);

  // Record the address of this call site.
  if (__builtin_expect(!call_pc[call_id], false))
    call_pc[call_id] = CALLERPC;

  // Push the MAAP count onto the stack.
  MAAP_counts.push_back(MAAP_count);
  cilksan_assert(MAAP_count == MAAP_counts.back() &&
                 "Mismatched MAAP counts before call.");

  // Push the call onto the call stack.
  CilkSanImpl.record_call(call_id, CALL);
}

// Hook called upon returning from a function call.
CILKSAN_API void __csan_after_call(const csi_id_t call_id,
                                   const csi_id_t func_id,
                                   unsigned MAAP_count,
                                   const call_prop_t prop) {
  if (!should_check())
    return;

  DBG_TRACE(DEBUG_CALLBACK, "__csan_after_call(%ld, %ld)\n",
            call_id, func_id);

  // Pop any MAAPs.
  cilksan_assert(MAAP_count == MAAP_counts.back() &&
                 "Mismatched MAAP counts after call.");
  for (unsigned i = 0; i < MAAP_count; ++i)
    MAAPs.pop();
  MAAP_counts.pop();

  // Pop the call off of the call stack.
  CilkSanImpl.record_call_return(call_id, CALL);
}

// Hook called when spawning a new task.
CILKSAN_API void __csan_detach(const csi_id_t detach_id,
                               const unsigned sync_reg) {
  if (!should_check())
    return;

  DBG_TRACE(DEBUG_CALLBACK, "__csan_detach(%ld)\n",
            detach_id);
  WHEN_CILKSAN_DEBUG(cilksan_assert(last_event == NONE));
  WHEN_CILKSAN_DEBUG(last_event = NONE);

  // Record the address of this detach.
  if (__builtin_expect(!spawn_pc[detach_id], false))
    spawn_pc[detach_id] = CALLERPC;

  // Update the parallel-execution state to reflect this detach.  Essentially,
  // this notes the change of peer sets.
  parallel_execution.back() = 1;

  if (!CilkSanImpl.handle_loop())
    // Push the detach onto the call stack.
    CilkSanImpl.record_call(detach_id, SPAWN);
}

// Hook called upon entering the body of a task.
CILKSAN_API void __csan_task(const csi_id_t task_id, const csi_id_t detach_id,
                             const void *bp, const void *sp,
                             const task_prop_t prop) {
  if (!should_check())
    return;

  // Update the low address of the stack
  if (stack_low_addr > (uintptr_t)sp) {
    if (stack_low_addr - (uintptr_t)sp > DEFAULT_STACK_SIZE) {
      // It looks like we have switched stacks to start executing Cilk code.
      handle_stack_switch((uintptr_t)bp, (uintptr_t)sp);
      switched_stack.push_back(1);
      if ((uintptr_t)bp - (uintptr_t)sp > DEFAULT_STACK_SIZE)
        bp = sp;
    } else {
      stack_low_addr = (uintptr_t)sp;
      switched_stack.push_back(0);
    }
  } else {
    switched_stack.push_back(0);
  }

  DBG_TRACE(DEBUG_CALLBACK, "__csan_task(%ld, %ld, %d)\n",
            task_id, detach_id, prop.is_tapir_loop_body);
  WHEN_CILKSAN_DEBUG(last_event = NONE);

  CilkSanImpl.push_stack_frame((uintptr_t)bp, (uintptr_t)sp);

  if (prop.is_tapir_loop_body && CilkSanImpl.handle_loop()) {
    CilkSanImpl.do_loop_iteration_begin(prop.num_sync_reg);
    return;
  }

  // Propagate the parallel-execution state to the child.
  uint8_t current_pe = parallel_execution.back();
  // Push the pe value on function entry.
  parallel_execution.push_back(current_pe);
  // Push a second copy to update aggressively on detaches.
  parallel_execution.push_back(current_pe);

  // Update tool for entering detach-helper function and performing detach.
  CilkSanImpl.do_enter_helper(prop.num_sync_reg);
  CilkSanImpl.do_detach();
}

// Hook called when exiting the body of a task.
CILKSAN_API void __csan_task_exit(const csi_id_t task_exit_id,
                                  const csi_id_t task_id,
                                  const csi_id_t detach_id,
                                  const unsigned sync_reg,
                                  const task_exit_prop_t prop) {
  if (!should_check())
    return;

  DBG_TRACE(DEBUG_CALLBACK, "__csan_task_exit(%ld, %ld, %ld, %d, %d)\n",
            task_exit_id, task_id, detach_id, sync_reg,
            prop.is_tapir_loop_body);

  if (prop.is_tapir_loop_body && CilkSanImpl.handle_loop()) {
    // Update tool for leaving the parallel iteration.
    CilkSanImpl.do_loop_iteration_end();

    // The parallel-execution state will be popped when the loop terminates.
  } else {
    // Update tool for leaving a detach-helper function.
    CilkSanImpl.do_leave(sync_reg);

    // Pop the parallel-execution state.
    parallel_execution.pop();
    parallel_execution.pop();
  }

  CilkSanImpl.pop_stack_frame();

  if (switched_stack.back()) {
    // We switched stacks upon entering this function.  Now switch back.
    stack_high_addr = uncilkified_stack_high_addr;
    stack_low_addr = uncilkified_stack_low_addr;
  }
  switched_stack.pop();
}

// Hook called at the continuation of a detach, i.e., a task spawn.
CILKSAN_API void __csan_detach_continue(const csi_id_t detach_continue_id,
                                        const csi_id_t detach_id) {
  if (!should_check())
    return;

  DBG_TRACE(DEBUG_CALLBACK, "__csan_detach_continue(%ld)\n",
            detach_id);

  if (!CilkSanImpl.handle_loop()) {
    CilkSanImpl.record_call_return(detach_id, SPAWN);
    CilkSanImpl.do_detach_continue();
  }

  WHEN_CILKSAN_DEBUG(last_event = NONE);
}

// Hook called at a sync
CILKSAN_API void __csan_sync(csi_id_t sync_id, const unsigned sync_reg) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check())
    return;

  // Because this is a serial tool, we can safely perform all operations related
  // to a sync.
  CilkSanImpl.do_sync(sync_reg);

  // Restore the parallel-execution state to that of the function/task entry.
  if (CilkSanImpl.is_local_synced()) {
    parallel_execution.back() = parallel_execution.from_back(1);
  }
}

///////////////////////////////////////////////////////////////////////////
// Hooks for memory accesses, i.e., loads and stores
//
// In the user program, __csan_load/store are called right before the
// corresponding read / write in the user code.  The return addr of
// __csan_load/store is the rip for the read / write.

// Hook called for an ordinary load instruction
CILKSAN_API
void __csan_load(csi_id_t load_id, const void *addr, int32_t size,
                 load_prop_t prop) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check()) {
    DBG_TRACE(DEBUG_MEMORY, "SKIP %s read (%p, %ld)\n", __FUNCTION__, addr,
              size);
    return;
  }
  if (!is_execution_parallel()) {
    DBG_TRACE(DEBUG_MEMORY, "SKIP %s read (%p, %ld) during serial execution\n",
              __FUNCTION__, addr, size);
    return;
  }

  // Record the address of this load.
  if (__builtin_expect(!load_pc[load_id], false))
    load_pc[load_id] = CALLERPC;

  DBG_TRACE(DEBUG_MEMORY, "%s read (%p, %ld)\n", __FUNCTION__, addr, size);

  // Record this read.
  if (prop.is_atomic) {
    CilkSanImpl.do_atomic_read(load_id, (uintptr_t)addr, size, prop.alignment,
                               atomic_lock_id);
    return;
  }
  if (__builtin_expect(CilkSanImpl.locks_held(), false))
    CilkSanImpl.do_locked_read<MAType_t::RW>(load_id, (uintptr_t)addr, size,
                                             prop.alignment);
  else
    CilkSanImpl.do_read<MAType_t::RW>(load_id, (uintptr_t)addr, size,
                                      prop.alignment);
}

// Hook called for a "large" load instruction, e.g., due to a memory intrinsic.
CILKSAN_API
void __csan_large_load(csi_id_t load_id, const void *addr, size_t size,
                       load_prop_t prop) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check()) {
    DBG_TRACE(DEBUG_MEMORY, "SKIP %s read (%p, %ld)\n", __FUNCTION__, addr,
              size);
    return;
  }
  if (!is_execution_parallel()) {
    DBG_TRACE(DEBUG_MEMORY, "SKIP %s read (%p, %ld) during serial execution\n",
              __FUNCTION__, addr, size);
    return;
  }

  // Record the address of this load.
  if (__builtin_expect(!load_pc[load_id], false))
    load_pc[load_id] = CALLERPC;

  DBG_TRACE(DEBUG_MEMORY, "%s read (%p, %ld)\n", __FUNCTION__, addr, size);

  // Record this read.
  if (prop.is_atomic) {
    CilkSanImpl.do_atomic_read(load_id, (uintptr_t)addr, size, prop.alignment,
                               atomic_lock_id);
    return;
  }
  if (__builtin_expect(CilkSanImpl.locks_held(), false))
    CilkSanImpl.do_locked_read<MAType_t::RW>(load_id, (uintptr_t)addr, size,
                                             prop.alignment);
  else
    CilkSanImpl.do_read<MAType_t::RW>(load_id, (uintptr_t)addr, size,
                                      prop.alignment);
}

// Hook called for an ordinary store instruction.
CILKSAN_API
void __csan_store(csi_id_t store_id, const void *addr, int32_t size,
                  store_prop_t prop) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check()) {
    DBG_TRACE(DEBUG_MEMORY, "SKIP %s wrote (%p, %ld)\n", __FUNCTION__, addr,
              size);
    return;
  }
  if (!is_execution_parallel()) {
    DBG_TRACE(DEBUG_MEMORY, "SKIP %s wrote (%p, %ld) during serial execution\n",
              __FUNCTION__, addr, size);
    return;
  }

  // Record the address of this store.
  if (__builtin_expect(!store_pc[store_id], false))
    store_pc[store_id] = CALLERPC;

  DBG_TRACE(DEBUG_MEMORY, "%s wrote (%p, %ld)\n", __FUNCTION__, addr, size);

  // Record this write.
  if (prop.is_atomic) {
    CilkSanImpl.do_atomic_write(store_id, (uintptr_t)addr, size, prop.alignment,
                                atomic_lock_id);
    return;
  }
  if (__builtin_expect(CilkSanImpl.locks_held(), false))
    CilkSanImpl.do_locked_write<MAType_t::RW>(store_id, (uintptr_t)addr, size,
                                              prop.alignment);
  else
    CilkSanImpl.do_write<MAType_t::RW>(store_id, (uintptr_t)addr, size,
                                       prop.alignment);
}

// Hook called for a "large" store instruction, e.g., due to a memory intrinsic.
CILKSAN_API
void __csan_large_store(csi_id_t store_id, const void *addr, size_t size,
                        store_prop_t prop) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check()) {
    DBG_TRACE(DEBUG_MEMORY, "SKIP %s wrote (%p, %ld)\n", __FUNCTION__, addr,
              size);
    return;
  }
  if (!is_execution_parallel()) {
    DBG_TRACE(DEBUG_MEMORY, "SKIP %s wrote (%p, %ld) during serial execution\n",
              __FUNCTION__, addr, size);
    return;
  }

  // Record the address of this store.
  if (__builtin_expect(!store_pc[store_id], false))
    store_pc[store_id] = CALLERPC;

  DBG_TRACE(DEBUG_MEMORY, "%s wrote (%p, %ld)\n", __FUNCTION__, addr, size);

  // Record this write.
  if (prop.is_atomic) {
    CilkSanImpl.do_atomic_write(store_id, (uintptr_t)addr, size, prop.alignment,
                                atomic_lock_id);
    return;
  }
  if (__builtin_expect(CilkSanImpl.locks_held(), false))
    CilkSanImpl.do_locked_write<MAType_t::RW>(store_id, (uintptr_t)addr, size,
                                              prop.alignment);
  else
    CilkSanImpl.do_write<MAType_t::RW>(store_id, (uintptr_t)addr, size,
                                       prop.alignment);
}

///////////////////////////////////////////////////////////////////////////
// Hooks for memory allocation

// Hook called after a stack allocation.
CILKSAN_API
void __csi_after_alloca(const csi_id_t alloca_id, const void *addr,
                        size_t size, const alloca_prop_t prop) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check())
    return;

  if (stack_low_addr > (uintptr_t)addr)
    stack_low_addr = (uintptr_t)addr;

  // Record the PC for this alloca
  if (__builtin_expect(!alloca_pc[alloca_id], false))
    alloca_pc[alloca_id] = CALLERPC;

  DBG_TRACE(DEBUG_CALLBACK, "__csi_after_alloca(%ld, %p, %ld)\n", alloca_id,
            addr, size);

  // Record the alloca and clear the allocated portion of the shadow memory.
  CilkSanImpl.record_alloc((size_t) addr, size, 2 * alloca_id);
  CilkSanImpl.clear_shadow_memory((size_t)addr, size);
  CilkSanImpl.advance_stack_frame((uintptr_t)addr);
}

// Map from malloc'd address to size of memory allocation
static AddrMap_t<size_t> malloc_sizes;

CILKSAN_API
void __csan_after_allocfn(const csi_id_t allocfn_id, const void *addr,
                          size_t size, size_t num, size_t alignment,
                          const void *oldaddr, const allocfn_prop_t prop) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check())
    return;

  DBG_TRACE(DEBUG_CALLBACK,
            "__csan_after_allocfn(%ld, %s, addr = %p, size = %ld, oldaddr = %p)\n",
            allocfn_id, __csan_get_allocfn_str(prop), addr, size, oldaddr);

  // TODO: Use alignment information
  // Record the PC for this allocation-function call
  if (__builtin_expect(!allocfn_pc[allocfn_id], false))
    allocfn_pc[allocfn_id] = CALLERPC;
  if (__builtin_expect(allocfn_prop[allocfn_id].allocfn_ty == uint8_t(-1),
                       false))
    allocfn_prop[allocfn_id] = prop;

  size_t new_size = size * num;

  // If this allocation function operated on an old address -- e.g., a realloc
  // -- then update the memory at the old address as if it was freed.
  if (oldaddr) {
    const size_t *size = malloc_sizes.get((uintptr_t)oldaddr);
    if (oldaddr != addr) {
      if (new_size > 0) {
        // Record the new allocation.
        CilkSanImpl.record_alloc((size_t)addr, new_size, 2 * allocfn_id + 1);
        CilkSanImpl.clear_shadow_memory((size_t)addr, new_size);
        malloc_sizes.insert((uintptr_t)addr, new_size);
      }

      if (malloc_sizes.contains((uintptr_t)oldaddr)) {
        if (!is_execution_parallel()) {
          CilkSanImpl.clear_alloc((size_t)oldaddr, *size);
          CilkSanImpl.clear_shadow_memory((size_t)oldaddr, *size);
        } else {
          // Take note of the freeing of the old memory.
          CilkSanImpl.record_free((uintptr_t)oldaddr, *size, allocfn_id,
                                  MAType_t::REALLOC);
        }
        malloc_sizes.remove((uintptr_t)oldaddr);
      }
    } else {
      // We're simply adjusting the allocation at the same place.
      if (malloc_sizes.contains((uintptr_t)oldaddr)) {
        size_t old_size = *size;
        if (old_size < new_size) {
          CilkSanImpl.clear_shadow_memory((size_t)addr + old_size,
                                          new_size - old_size);
        } else if (old_size > new_size) {
          if (!is_execution_parallel()) {
            CilkSanImpl.clear_alloc((size_t)oldaddr + new_size,
                                    old_size - new_size);
            CilkSanImpl.clear_shadow_memory((size_t)oldaddr + new_size,
                                            old_size - new_size);
          } else {
            // Take note of the effective free of the old space.
            CilkSanImpl.record_free((uintptr_t)oldaddr + new_size,
                                    old_size - new_size, allocfn_id,
                                    MAType_t::REALLOC);
          }
        }
        CilkSanImpl.record_alloc((size_t)addr, new_size, 2 * allocfn_id + 1);
        malloc_sizes.remove((uintptr_t)addr);
      }
      malloc_sizes.insert((uintptr_t)addr, new_size);
    }
    return;
  }

  // For many memory allocation functions, including malloc and realloc, if the
  // requested size is 0, the behavior is implementation defined.  The function
  // might return nullptr, or return a non-null pointer that won't be used to
  // access memory.  We simply don't record an allocation of zero size.
  if (0 == size)
    return;

  // Record the new allocation.
  malloc_sizes.insert((uintptr_t)addr, new_size);
  CilkSanImpl.record_alloc((size_t)addr, new_size, 2 * allocfn_id + 1);
  CilkSanImpl.clear_shadow_memory((size_t)addr, new_size);
}

CILKSAN_API void __csan_alloc_posix_memalign(const csi_id_t allocfn_id,
                                             const csi_id_t func_id,
                                             unsigned MAAP_count,
                                             const allocfn_prop_t prop,
                                             int result, void **ptr,
                                             size_t alignment, size_t size) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check())
    return;
  if (__builtin_expect(!allocfn_pc[allocfn_id], false))
    allocfn_pc[allocfn_id] = CALLERPC;
  if (__builtin_expect(allocfn_prop[allocfn_id].allocfn_ty == uint8_t(-1),
                       false))
    allocfn_prop[allocfn_id] = prop;

  if (0 == size)
    return;

  malloc_sizes.insert((uintptr_t)(*ptr), size);
  CilkSanImpl.record_alloc((size_t)(*ptr), size, 2 * allocfn_id + 1);
  CilkSanImpl.clear_shadow_memory((size_t)(*ptr), size);
}

CILKSAN_API void __csan_alloc_strdup(const csi_id_t allocfn_id,
                                     const csi_id_t func_id,
                                     unsigned MAAP_count,
                                     const allocfn_prop_t prop, char *result,
                                     const char *str) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check())
    return;
  if (__builtin_expect(!allocfn_pc[allocfn_id], false))
    allocfn_pc[allocfn_id] = CALLERPC;
  if (__builtin_expect(allocfn_prop[allocfn_id].allocfn_ty == uint8_t(-1),
                       false))
    allocfn_prop[allocfn_id] = prop;

  MAAP_t str_MAAPVal = MAAP_t::ModRef;
  if (MAAP_count > 0) {
    str_MAAPVal = MAAPs.back().second;
    MAAPs.pop();
  }

  if (nullptr == result)
    // Nothing to do if the call failed.
    return;

  size_t size = strlen(str) + 1;

  // Check the read of str
  if (is_execution_parallel() && checkMAAP(str_MAAPVal, MAAP_t::Mod)) {
    if (__builtin_expect(CilkSanImpl.locks_held(), false)) {
      CilkSanImpl.do_locked_read<MAType_t::ALLOC>(allocfn_id, (uintptr_t)str,
                                                  size, 0);
    } else {
      CilkSanImpl.do_read<MAType_t::ALLOC>(allocfn_id, (uintptr_t)str, size, 0);
    }
  }

  // Record the allocation
  malloc_sizes.insert((uintptr_t)(*result), size);
  CilkSanImpl.record_alloc((size_t)(*result), size, 2 * allocfn_id + 1);
  CilkSanImpl.clear_shadow_memory((size_t)(*result), size);

  // Check the write to result
  if (is_execution_parallel() && checkMAAP(str_MAAPVal, MAAP_t::Mod)) {
    if (__builtin_expect(CilkSanImpl.locks_held(), false)) {
      CilkSanImpl.do_locked_write<MAType_t::ALLOC>(allocfn_id,
                                                   (uintptr_t)result, size, 0);
    } else {
      CilkSanImpl.do_write<MAType_t::ALLOC>(allocfn_id, (uintptr_t)result, size,
                                            0);
    }
  }
}

CILKSAN_API void __csan_alloc_strndup(const csi_id_t allocfn_id,
                                      const csi_id_t func_id,
                                      unsigned MAAP_count,
                                      const allocfn_prop_t prop, char *result,
                                      const char *str, size_t size) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check())
    return;
  if (__builtin_expect(!allocfn_pc[allocfn_id], false))
    allocfn_pc[allocfn_id] = CALLERPC;
  if (__builtin_expect(allocfn_prop[allocfn_id].allocfn_ty == uint8_t(-1),
                       false))
    allocfn_prop[allocfn_id] = prop;

  MAAP_t str_MAAPVal = MAAP_t::ModRef;
  if (MAAP_count > 0) {
    str_MAAPVal = MAAPs.back().second;
    MAAPs.pop();
  }

  if (nullptr == result)
    // Nothing to do if the call failed.
    return;

  size_t result_size = strlen(result) + 1;

  // Check the read of str
  if (is_execution_parallel() && checkMAAP(str_MAAPVal, MAAP_t::Mod)) {
    if (__builtin_expect(CilkSanImpl.locks_held(), false)) {
      CilkSanImpl.do_locked_read<MAType_t::ALLOC>(allocfn_id, (uintptr_t)str,
                                                  result_size, 0);
    } else {
      CilkSanImpl.do_read<MAType_t::ALLOC>(allocfn_id, (uintptr_t)str,
                                           result_size, 0);
    }
  }

  // Record the allocation
  malloc_sizes.insert((uintptr_t)(*result), result_size);
  CilkSanImpl.record_alloc((size_t)(*result), result_size, 2 * allocfn_id + 1);
  CilkSanImpl.clear_shadow_memory((size_t)(*result), result_size);

  // Check the write to result
  if (is_execution_parallel() && checkMAAP(str_MAAPVal, MAAP_t::Mod)) {
    if (__builtin_expect(CilkSanImpl.locks_held(), false)) {
      CilkSanImpl.do_locked_write<MAType_t::ALLOC>(
          allocfn_id, (uintptr_t)result, result_size, 0);
    } else {
      CilkSanImpl.do_write<MAType_t::ALLOC>(allocfn_id, (uintptr_t)result,
                                            result_size, 0);
    }
  }
}

// Hook called after any free or delete.
CILKSAN_API
void __csan_after_free(const csi_id_t free_id, const void *ptr,
                       const free_prop_t prop) {
  if (!TOOL_INITIALIZED)
    return;

  if (!should_check())
    return;

  if (__builtin_expect(!free_pc[free_id], false))
    free_pc[free_id] = CALLERPC;

  const size_t *size = malloc_sizes.get((uintptr_t)ptr);
  if (malloc_sizes.contains((uintptr_t)ptr)) {
    if (!is_execution_parallel()) {
      CilkSanImpl.clear_alloc((size_t)ptr, *size);
      CilkSanImpl.clear_shadow_memory((size_t)ptr, *size);
    } else {
      // Treat a free as a write to all freed addresses.  This way the tool will
      // report a race if an operation tries to access a location that was freed
      // in parallel.
      CilkSanImpl.record_free((uintptr_t)ptr, *size, free_id, MAType_t::FREE);
    }
    malloc_sizes.remove((uintptr_t)ptr);
  }
}

// FIXME: Currently these dynamic interposers are never used, because common
// third-party libraries, such as jemalloc, do not work properly when these
// methods are dynamically interposed.  We therefore rely on Cilksan hooks to
// find allocation routines.
#if CILKSAN_DYNAMIC

static std::map<uintptr_t, size_t> pages_to_clear;

// Flag to manage initialization of memory functions.  We need this flag because
// dlsym uses some of the memory functions we are trying to interpose, which
// means that calling dlsym directly will lead to infinite recursion and a
// segfault.  Fortunately, dlsym can make do with memory-allocation functions
// returning NULL, so we return NULL when we detect this inifinite recursion.
//
// This trick seems questionable, but it also seems to be standard practice.
// It's the same trick used by memusage.c in glibc, and there's little
// documentation on better tricks.
static int mem_initialized = 0;

// Pointers to real memory functions.
typedef void*(*malloc_t)(size_t);
static malloc_t real_malloc = NULL;

typedef void*(*calloc_t)(size_t, size_t);
static calloc_t real_calloc = NULL;

typedef void*(*realloc_t)(void*, size_t);
static realloc_t real_realloc = NULL;

typedef void(*free_t)(void*);
static free_t real_free = NULL;

typedef void*(*mmap_t)(void*, size_t, int, int, int, off_t);
static mmap_t real_mmap = NULL;

#if defined(_LARGEFILE64_SOURCE)
typedef void*(*mmap64_t)(void*, size_t, int, int, int, off64_t);
static mmap64_t real_mmap64 = NULL;
#endif // defined(_LARGEFILE64_SOURCE)

typedef int(*munmap_t)(void*, size_t);
static munmap_t real_munmap = NULL;

typedef void*(*mremap_t)(void*, size_t, size_t, int, ...);
static mremap_t real_mremap = NULL;

// Helper function to get real implementations of memory functions via dlsym.
static void initialize_memory_functions() {
  disable_checking();
  mem_initialized = -1;

  real_malloc = (malloc_t)dlsym(RTLD_NEXT, "malloc");
  if (real_malloc == NULL)
    goto error_exit;

  real_calloc = (calloc_t)dlsym(RTLD_NEXT, "calloc");
  if (real_calloc == NULL)
    goto error_exit;

  real_realloc = (realloc_t)dlsym(RTLD_NEXT, "realloc");
  if (real_realloc == NULL)
    goto error_exit;

  real_free = (free_t)dlsym(RTLD_NEXT, "free");
  if (real_free == NULL)
    goto error_exit;

  real_mmap = (mmap_t)dlsym(RTLD_NEXT, "mmap");
  if (real_mmap == NULL)
    goto error_exit;

#if defined(_LARGEFILE64_SOURCE)
  real_mmap64 = (mmap64_t)dlsym(RTLD_NEXT, "mmap64");
  if (real_mmap64 == NULL)
    goto error_exit;
#endif // defined(_LARGEFILE64_SOURCE)

  real_munmap = (munmap_t)dlsym(RTLD_NEXT, "munmap");
  if (real_munmap == NULL)
    goto error_exit;

#if __linux__
  real_mremap = (mremap_t)dlsym(RTLD_NEXT, "mremap");
  if (real_mremap == NULL)
    goto error_exit;
#endif // __linux__

  mem_initialized = 1;
  enable_checking();
  return;

 error_exit:
  char *error = dlerror();
  if (error != NULL) {
    fputs(error, err_io);
    fflush(err_io);
  }
  abort();
}

// CILKSAN_API void* malloc(size_t s) {
//   // Don't try to init, since that needs malloc.
//   if (__builtin_expect(real_malloc == NULL, 0)) {
//     if (-1 == mem_initialized)
//       return NULL;
//     initialize_memory_functions();
//   }

//   disable_checking();
//   // align the allocation to simplify erasing from shadow mem
//   // uint64_t new_size = ALIGN_BY_NEXT_MAX_GRAIN_SIZE(s);
//   size_t new_size = ALIGN_FOR_MALLOC(s);
//   assert(s == new_size);
//   // call the real malloc
//   void *r = real_malloc(new_size);
//   enable_checking();

//   if (TOOL_INITIALIZED && should_check()) {
//     disable_checking();
//     malloc_sizes.insert({(uintptr_t)r, new_size});
//     // cilksan_clear_shadow_memory((size_t)r, (size_t)r+malloc_usable_size(r)-1);
//     cilksan_record_alloc((size_t)r, new_size, 0);
//     cilksan_clear_shadow_memory((size_t)r, new_size);
//     enable_checking();
//   }

//   return r;
// }

// CILKSAN_API void* calloc(size_t num, size_t s) {
//   if (__builtin_expect(real_calloc == NULL, 0)) {
//     if (-1 == mem_initialized)
//       return NULL;
//     initialize_memory_functions();
//   }

//   disable_checking();
//   void *r = real_calloc(num, s);
//   enable_checking();

//   if (TOOL_INITIALIZED && should_check()) {
//     disable_checking();
//     malloc_sizes.insert({(uintptr_t)r, s});
//     // cilksan_clear_shadow_memory((size_t)r, (size_t)r+malloc_usable_size(r)-1);
//     cilksan_record_alloc((size_t)r, num * s, 0);
//     cilksan_clear_shadow_memory((size_t)r, num * s);
//     enable_checking();
//   }

//   return r;
// }

// CILKSAN_API void free(void *ptr) {
//   if (__builtin_expect(real_free == NULL, 0)) {
//     if (-1 == mem_initialized)
//       return;
//     initialize_memory_functions();
//   }

//   disable_checking();
//   real_free(ptr);
//   enable_checking();

//   if (TOOL_INITIALIZED && should_check()) {
//     disable_checking();
//     auto iter = malloc_sizes.find((uintptr_t)ptr);
//     if (iter != malloc_sizes.end()) {
//       // cilksan_clear_shadow_memory((size_t)ptr, iter->second);

//       // Treat a free as a write to all freed addresses.  This way, the tool
//       // will report a race if an operation tries to access a location that was
//       // freed in parallel.
//       cilksan_do_write(UNKNOWN_CSI_ID, (uintptr_t)ptr, iter->second);
//       malloc_sizes.erase(iter);
//     }
//     enable_checking();
//   }
// }

// CILKSAN_API void* realloc(void *ptr, size_t s) {
//   if (__builtin_expect(real_realloc == NULL, 0)) {
//     if (-1 == mem_initialized)
//       return NULL;
//     initialize_memory_functions();
//   }

//   disable_checking();
//   void *r = real_realloc(ptr, s);
//   enable_checking();

//   if (TOOL_INITIALIZED && should_check()) {
//     disable_checking();
//     // Treat the old pointer ptr as freed and the new pointer r as freshly
//     // malloc'd.
//     auto iter = malloc_sizes.find((uintptr_t)ptr);
//     if (iter != malloc_sizes.end()) {
//       cilksan_do_write(UNKNOWN_CSI_ID, (uintptr_t)ptr, iter->second);
//       malloc_sizes.erase(iter);
//     }
//     malloc_sizes.insert({(uintptr_t)r, s});
//     // cilksan_clear_shadow_memory((size_t)r, (size_t)r+malloc_usable_size(r)-1);
//     cilksan_record_alloc((size_t)r, s, 0);
//     cilksan_clear_shadow_memory((size_t)r, s);
//     enable_checking();
//   }

//   return r;
// }

CILKSAN_API
void *mmap(void *start, size_t len, int prot, int flags, int fd, off_t offset) {
  if (__builtin_expect(real_mmap == NULL, 0)) {
    if (-1 == mem_initialized)
      return NULL;
    initialize_memory_functions();
  }

  disable_checking();
  void *r = real_mmap(start, len, prot, flags, fd, offset);
  enable_checking();

  if (TOOL_INITIALIZED && should_check()) {
    CheckingRAII nocheck;
    CilkSanImpl.record_alloc((size_t)r, len, 0);
    CilkSanImpl.clear_shadow_memory((size_t)r, len);
    pages_to_clear.insert({(uintptr_t)r, len});
    if (!(flags & MAP_ANONYMOUS))
      // This mmap is backed by a file.  Initialize the shadow memory with a
      // write to the page.
      CilkSanImpl.do_write<MAType_t::FNRW>(UNKNOWN_CSI_ID, (uintptr_t)r, len,
                                           0);
  }

  return r;
}

#if defined(_LARGEFILE64_SOURCE)
CILKSAN_API
void *mmap64(void *start, size_t len, int prot, int flags, int fd, off64_t offset) {
  if (__builtin_expect(real_mmap64 == NULL, 0)) {
    if (-1 == mem_initialized)
      return NULL;
    initialize_memory_functions();
  }

  disable_checking();
  void *r = real_mmap64(start, len, prot, flags, fd, offset);
  enable_checking();

  if (TOOL_INITIALIZED && should_check()) {
    CheckingRAII nocheck;
    CilkSanImpl.record_alloc((size_t)r, len, 0);
    CilkSanImpl.clear_shadow_memory((size_t)r, len);
    pages_to_clear.insert({(uintptr_t)r, len});
    if (!(flags & MAP_ANONYMOUS))
      // This mmap is backed by a file.  Initialize the shadow memory with a
      // write to the page.
      CilkSanImpl.do_write<MAType_t::FNRW>(UNKNOWN_CSI_ID, (uintptr_t)r, len,
                                           0);
  }

  return r;
}
#endif // defined(_LARGEFILE64_SOURCE)

CILKSAN_API
int munmap(void *start, size_t len) {
  if (__builtin_expect(real_munmap == NULL, 0)) {
    if (-1 == mem_initialized)
      return -1;
    initialize_memory_functions();
  }

  disable_checking();
  int result = real_munmap(start, len);
  enable_checking();

  if (TOOL_INITIALIZED && should_check() && (0 == result)) {
    CheckingRAII nocheck;
    auto first_page = pages_to_clear.lower_bound((uintptr_t)start);
    auto last_page = pages_to_clear.upper_bound((uintptr_t)start + len);
    for (auto curr_page = first_page; curr_page != last_page; ++curr_page) {
      // TODO: Treat munmap more like free and record a write operation on the
      // page.  Need to take care only to write pages that have content in the
      // shadow memory.  Otherwise, if the application mmap's more virtual
      // memory than physical memory, then the writes that model page unmapping
      // can blow out physical memory.
      CilkSanImpl.clear_shadow_memory((size_t)curr_page->first, curr_page->second);
      // CilkSanImpl.do_write(UNKNOWN_CSI_ID, curr_page->first, curr_page->second);
    }
    pages_to_clear.erase(first_page, last_page);
  }

  return result;
}

CILKSAN_API
void *mremap(void *start, size_t old_len, size_t len, int flags, ...) {
#if defined(MREMAP_FIXED)
  va_list ap;
  va_start (ap, flags);
  void *newaddr = (flags & MREMAP_FIXED) ? va_arg (ap, void *) : NULL;
  va_end (ap);
#endif // defined(MREMAP_FIXED)

  if (__builtin_expect(real_mremap == NULL, 0)) {
    if (-1 == mem_initialized)
      return NULL;
    initialize_memory_functions();
  }

  disable_checking();
#if defined(MREMAP_FIXED)
  void *r = real_mremap(start, old_len, len, flags, newaddr);
#else
  void *r = real_mremap(start, old_len, len, flags);
#endif // defined(MREMAP_FIXED)
  enable_checking();

  if (TOOL_INITIALIZED && should_check()) {
    CheckingRAII nocheck;
    auto iter = pages_to_clear.find((uintptr_t)start);
    if (iter != pages_to_clear.end()) {
      // TODO: Treat mremap more like free and record a write operation on the
      // page.  Need to take care only to write pages that have content in the
      // shadow memory.  Otherwise, if the application mmap's more virtual
      // memory than physical memory, then the writes that model page unmapping
      // can blow out physical memory.
      CilkSanImpl.clear_shadow_memory((size_t)iter->first, iter->second);
      // cilksan_do_write(UNKNOWN_CSI_ID, iter->first, iter->second);
      pages_to_clear.erase(iter);
    }
    // Record the new mapping.
    CilkSanImpl.record_alloc((size_t)r, len, 0);
    CilkSanImpl.clear_shadow_memory((size_t)r, len);
    pages_to_clear.insert({(uintptr_t)r, len});
  }

  return r;
}

#endif  // CILKSAN_DYNAMIC

///////////////////////////////////////////////////////////////////////////
// Function interposers for OpenCilk runtime routines

/// Wrapping for __cilkrts_internal_merge_two_rmaps Cilk runtime method for
/// performing reduce operations on reducer views.

struct cilkred_map;
struct __cilkrts_worker;

// Wrapped __cilkrts_internal_merge_two_rmaps method for dynamic
// interpositioning.
typedef cilkred_map *(*merge_two_rmaps_t)(__cilkrts_worker *, cilkred_map *,
                                          cilkred_map *);
static merge_two_rmaps_t dl___cilkrts_internal_merge_two_rmaps = NULL;

CILKSAN_API CILKSAN_WEAK cilkred_map *
__cilkrts_internal_merge_two_rmaps(__cilkrts_worker *ws, cilkred_map *left,
                                   cilkred_map *right) {
  START_DL_INTERPOSER(__cilkrts_internal_merge_two_rmaps, merge_two_rmaps_t);

  disable_checking();
  cilkred_map *res = dl___cilkrts_internal_merge_two_rmaps(ws, left, right);
  enable_checking();

  return res;
}

/// Wrapped __cilkrts_internal_merge_two_rmaps method for link-time
/// interpositioning.
CILKSAN_API cilkred_map *
__real___cilkrts_internal_merge_two_rmaps(__cilkrts_worker *ws,
                                          cilkred_map *left,
                                          cilkred_map *right) {
  return __cilkrts_internal_merge_two_rmaps(ws, left, right);
}

CILKSAN_API
cilkred_map *__wrap___cilkrts_internal_merge_two_rmaps(__cilkrts_worker *ws,
                                                       cilkred_map *left,
                                                       cilkred_map *right) {
  disable_checking();
  cilkred_map *res = __real___cilkrts_internal_merge_two_rmaps(ws, left, right);
  enable_checking();
  return res;
}

// Wrapped __cilkrts_hyper_alloc method for dynamic interpositioning.
typedef void *(*hyper_alloc_t)(void *, size_t);
static hyper_alloc_t dl___cilkrts_hyper_alloc = NULL;

CILKSAN_API CILKSAN_WEAK void*
__cilkrts_hyper_alloc(void *ignored, size_t bytes) {
  START_DL_INTERPOSER(__cilkrts_hyper_alloc, hyper_alloc_t);

  void *ptr = dl___cilkrts_hyper_alloc(ignored, bytes);
  malloc_sizes.insert((uintptr_t)ptr, bytes);
  CilkSanImpl.record_alloc((size_t)ptr, bytes, 0);
  CilkSanImpl.clear_shadow_memory((size_t)ptr, bytes);
  return ptr;
}

/// Wrapped __cilkrts_hyper_alloc method for link-time interpositioning.
CILKSAN_API void*
__real___cilkrts_hyper_alloc(void *ignored, size_t bytes) {
  return __cilkrts_hyper_alloc(ignored, bytes);
}

CILKSAN_API
void *__wrap___cilkrts_hyper_alloc(void *ignored, size_t bytes) {
  void *ptr = __real___cilkrts_hyper_alloc(ignored, bytes);
  malloc_sizes.insert((uintptr_t)ptr, bytes);
  CilkSanImpl.record_alloc((size_t)ptr, bytes, 0);
  CilkSanImpl.clear_shadow_memory((size_t)ptr, bytes);
  return ptr;
}

// Wrapped __cilkrts_hyper_dealloc method for dynamic interpositioning.
typedef void (*hyper_dealloc_t)(void *, void *);
static hyper_dealloc_t dl___cilkrts_hyper_dealloc = NULL;

CILKSAN_API CILKSAN_WEAK void
__cilkrts_hyper_dealloc(void *ignored, void *view) {
  START_DL_INTERPOSER(__cilkrts_hyper_dealloc, hyper_dealloc_t);

  const size_t *size = malloc_sizes.get((uintptr_t)view);
  if (malloc_sizes.contains((uintptr_t)view)) {
    CilkSanImpl.clear_alloc((size_t)view, *size);
    CilkSanImpl.clear_shadow_memory((size_t)view, *size);
    malloc_sizes.remove((uintptr_t)view);
  }
  dl___cilkrts_hyper_dealloc(ignored, view);
}

/// Wrapped __cilkrts_hyper_alloc method for link-time interpositioning.
CILKSAN_API void
__real___cilkrts_hyper_dealloc(void *ignored, void *view) {
  __cilkrts_hyper_dealloc(ignored, view);
}

CILKSAN_API
void __wrap___cilkrts_hyper_dealloc(void *ignored, void *view) {
  const size_t *size = malloc_sizes.get((uintptr_t)view);
  if (malloc_sizes.contains((uintptr_t)view)) {
    CilkSanImpl.clear_alloc((size_t)view, *size);
    CilkSanImpl.clear_shadow_memory((size_t)view, *size);
    malloc_sizes.remove((uintptr_t)view);
  }
  __real___cilkrts_hyper_dealloc(ignored, view);
}
