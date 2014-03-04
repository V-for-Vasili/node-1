// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "v8.h"

#include "accessors.h"
#include "api.h"
#include "bootstrapper.h"
#include "codegen.h"
#include "compilation-cache.h"
#include "conversions.h"
#include "cpu-profiler.h"
#include "debug.h"
#include "deoptimizer.h"
#include "global-handles.h"
#include "heap-profiler.h"
#include "incremental-marking.h"
#include "isolate-inl.h"
#include "mark-compact.h"
#include "natives.h"
#include "objects-visiting.h"
#include "objects-visiting-inl.h"
#include "once.h"
#include "runtime-profiler.h"
#include "scopeinfo.h"
#include "snapshot.h"
#include "store-buffer.h"
#include "utils/random-number-generator.h"
#include "utils.h"
#include "v8threads.h"
#include "vm-state-inl.h"
#if V8_TARGET_ARCH_PPC && !V8_INTERPRETED_REGEXP
#include "regexp-macro-assembler.h"
#include "ppc/regexp-macro-assembler-ppc.h"
#endif
#if V8_TARGET_ARCH_ARM && !V8_INTERPRETED_REGEXP
#include "regexp-macro-assembler.h"
#include "arm/regexp-macro-assembler-arm.h"
#endif
#if V8_TARGET_ARCH_MIPS && !V8_INTERPRETED_REGEXP
#include "regexp-macro-assembler.h"
#include "mips/regexp-macro-assembler-mips.h"
#endif
#include "full-codegen.h"

namespace v8 {
namespace internal {


Heap::Heap()
    : isolate_(NULL),
      code_range_size_(0),
// semispace_size_ should be a power of 2 and old_generation_size_ should be
// a multiple of Page::kPageSize.
      reserved_semispace_size_(8 * (kPointerSize / 4) * MB),
      max_semispace_size_(8 * (kPointerSize / 4)  * MB),
      initial_semispace_size_(Page::kPageSize),
      max_old_generation_size_(700ul * (kPointerSize / 4) * MB),
      max_executable_size_(256ul * (kPointerSize / 4) * MB),
// Variables set based on semispace_size_ and old_generation_size_ in
// ConfigureHeap (survived_since_last_expansion_, external_allocation_limit_)
// Will be 4 * reserved_semispace_size_ to ensure that young
// generation can be aligned to its size.
      maximum_committed_(0),
      old_space_growing_factor_(4),
      survived_since_last_expansion_(0),
      sweep_generation_(0),
      always_allocate_scope_depth_(0),
      linear_allocation_scope_depth_(0),
      contexts_disposed_(0),
      global_ic_age_(0),
      flush_monomorphic_ics_(false),
      scan_on_scavenge_pages_(0),
      new_space_(this),
      old_pointer_space_(NULL),
      old_data_space_(NULL),
      code_space_(NULL),
      map_space_(NULL),
      cell_space_(NULL),
      property_cell_space_(NULL),
      lo_space_(NULL),
      gc_state_(NOT_IN_GC),
      gc_post_processing_depth_(0),
      ms_count_(0),
      gc_count_(0),
      remembered_unmapped_pages_index_(0),
      unflattened_strings_length_(0),
#ifdef DEBUG
      allocation_timeout_(0),
#endif  // DEBUG
      new_space_high_promotion_mode_active_(false),
      old_generation_allocation_limit_(kMinimumOldGenerationAllocationLimit),
      size_of_old_gen_at_last_old_space_gc_(0),
      external_allocation_limit_(0),
      amount_of_external_allocated_memory_(0),
      amount_of_external_allocated_memory_at_last_global_gc_(0),
      old_gen_exhausted_(false),
      inline_allocation_disabled_(false),
      store_buffer_rebuilder_(store_buffer()),
      hidden_string_(NULL),
      gc_safe_size_of_old_object_(NULL),
      total_regexp_code_generated_(0),
      tracer_(NULL),
      young_survivors_after_last_gc_(0),
      high_survival_rate_period_length_(0),
      low_survival_rate_period_length_(0),
      survival_rate_(0),
      previous_survival_rate_trend_(Heap::STABLE),
      survival_rate_trend_(Heap::STABLE),
      max_gc_pause_(0.0),
      total_gc_time_ms_(0.0),
      max_alive_after_gc_(0),
      min_in_mutator_(kMaxInt),
      alive_after_last_gc_(0),
      last_gc_end_timestamp_(0.0),
      marking_time_(0.0),
      sweeping_time_(0.0),
      mark_compact_collector_(this),
      store_buffer_(this),
      marking_(this),
      incremental_marking_(this),
      number_idle_notifications_(0),
      last_idle_notification_gc_count_(0),
      last_idle_notification_gc_count_init_(false),
      mark_sweeps_since_idle_round_started_(0),
      gc_count_at_last_idle_gc_(0),
      scavenges_since_last_idle_round_(kIdleScavengeThreshold),
      full_codegen_bytes_generated_(0),
      crankshaft_codegen_bytes_generated_(0),
      gcs_since_last_deopt_(0),
#ifdef VERIFY_HEAP
      no_weak_object_verification_scope_depth_(0),
#endif
      allocation_sites_scratchpad_length_(0),
      promotion_queue_(this),
      configured_(false),
      external_string_table_(this),
      chunks_queued_for_free_(NULL),
      gc_callbacks_depth_(0) {
  // Allow build-time customization of the max semispace size. Building
  // V8 with snapshots and a non-default max semispace size is much
  // easier if you can define it as part of the build environment.
#if defined(V8_MAX_SEMISPACE_SIZE)
  max_semispace_size_ = reserved_semispace_size_ = V8_MAX_SEMISPACE_SIZE;
#endif

  // Ensure old_generation_size_ is a multiple of kPageSize.
  ASSERT(MB >= Page::kPageSize);

  memset(roots_, 0, sizeof(roots_[0]) * kRootListLength);
  native_contexts_list_ = NULL;
  array_buffers_list_ = Smi::FromInt(0);
  allocation_sites_list_ = Smi::FromInt(0);
  // Put a dummy entry in the remembered pages so we can find the list the
  // minidump even if there are no real unmapped pages.
  RememberUnmappedPage(NULL, false);

  ClearObjectStats(true);
}


intptr_t Heap::Capacity() {
  if (!HasBeenSetUp()) return 0;

  return new_space_.Capacity() +
      old_pointer_space_->Capacity() +
      old_data_space_->Capacity() +
      code_space_->Capacity() +
      map_space_->Capacity() +
      cell_space_->Capacity() +
      property_cell_space_->Capacity();
}


intptr_t Heap::CommittedMemory() {
  if (!HasBeenSetUp()) return 0;

  return new_space_.CommittedMemory() +
      old_pointer_space_->CommittedMemory() +
      old_data_space_->CommittedMemory() +
      code_space_->CommittedMemory() +
      map_space_->CommittedMemory() +
      cell_space_->CommittedMemory() +
      property_cell_space_->CommittedMemory() +
      lo_space_->Size();
}


size_t Heap::CommittedPhysicalMemory() {
  if (!HasBeenSetUp()) return 0;

  return new_space_.CommittedPhysicalMemory() +
      old_pointer_space_->CommittedPhysicalMemory() +
      old_data_space_->CommittedPhysicalMemory() +
      code_space_->CommittedPhysicalMemory() +
      map_space_->CommittedPhysicalMemory() +
      cell_space_->CommittedPhysicalMemory() +
      property_cell_space_->CommittedPhysicalMemory() +
      lo_space_->CommittedPhysicalMemory();
}


intptr_t Heap::CommittedMemoryExecutable() {
  if (!HasBeenSetUp()) return 0;

  return isolate()->memory_allocator()->SizeExecutable();
}


void Heap::UpdateMaximumCommitted() {
  if (!HasBeenSetUp()) return;

  intptr_t current_committed_memory = CommittedMemory();
  if (current_committed_memory > maximum_committed_) {
    maximum_committed_ = current_committed_memory;
  }
}


intptr_t Heap::Available() {
  if (!HasBeenSetUp()) return 0;

  return new_space_.Available() +
      old_pointer_space_->Available() +
      old_data_space_->Available() +
      code_space_->Available() +
      map_space_->Available() +
      cell_space_->Available() +
      property_cell_space_->Available();
}


bool Heap::HasBeenSetUp() {
  return old_pointer_space_ != NULL &&
         old_data_space_ != NULL &&
         code_space_ != NULL &&
         map_space_ != NULL &&
         cell_space_ != NULL &&
         property_cell_space_ != NULL &&
         lo_space_ != NULL;
}


int Heap::GcSafeSizeOfOldObject(HeapObject* object) {
  if (IntrusiveMarking::IsMarked(object)) {
    return IntrusiveMarking::SizeOfMarkedObject(object);
  }
  return object->SizeFromMap(object->map());
}


GarbageCollector Heap::SelectGarbageCollector(AllocationSpace space,
                                              const char** reason) {
  // Is global GC requested?
  if (space != NEW_SPACE) {
    isolate_->counters()->gc_compactor_caused_by_request()->Increment();
    *reason = "GC in old space requested";
    return MARK_COMPACTOR;
  }

  if (FLAG_gc_global || (FLAG_stress_compaction && (gc_count_ & 1) != 0)) {
    *reason = "GC in old space forced by flags";
    return MARK_COMPACTOR;
  }

  // Is enough data promoted to justify a global GC?
  if (OldGenerationAllocationLimitReached()) {
    isolate_->counters()->gc_compactor_caused_by_promoted_data()->Increment();
    *reason = "promotion limit reached";
    return MARK_COMPACTOR;
  }

  // Have allocation in OLD and LO failed?
  if (old_gen_exhausted_) {
    isolate_->counters()->
        gc_compactor_caused_by_oldspace_exhaustion()->Increment();
    *reason = "old generations exhausted";
    return MARK_COMPACTOR;
  }

  // Is there enough space left in OLD to guarantee that a scavenge can
  // succeed?
  //
  // Note that MemoryAllocator->MaxAvailable() undercounts the memory available
  // for object promotion. It counts only the bytes that the memory
  // allocator has not yet allocated from the OS and assigned to any space,
  // and does not count available bytes already in the old space or code
  // space.  Undercounting is safe---we may get an unrequested full GC when
  // a scavenge would have succeeded.
  if (isolate_->memory_allocator()->MaxAvailable() <= new_space_.Size()) {
    isolate_->counters()->
        gc_compactor_caused_by_oldspace_exhaustion()->Increment();
    *reason = "scavenge might not succeed";
    return MARK_COMPACTOR;
  }

  // Default
  *reason = NULL;
  return SCAVENGER;
}


// TODO(1238405): Combine the infrastructure for --heap-stats and
// --log-gc to avoid the complicated preprocessor and flag testing.
void Heap::ReportStatisticsBeforeGC() {
  // Heap::ReportHeapStatistics will also log NewSpace statistics when
  // compiled --log-gc is set.  The following logic is used to avoid
  // double logging.
#ifdef DEBUG
  if (FLAG_heap_stats || FLAG_log_gc) new_space_.CollectStatistics();
  if (FLAG_heap_stats) {
    ReportHeapStatistics("Before GC");
  } else if (FLAG_log_gc) {
    new_space_.ReportStatistics();
  }
  if (FLAG_heap_stats || FLAG_log_gc) new_space_.ClearHistograms();
#else
  if (FLAG_log_gc) {
    new_space_.CollectStatistics();
    new_space_.ReportStatistics();
    new_space_.ClearHistograms();
  }
#endif  // DEBUG
}


void Heap::PrintShortHeapStatistics() {
  if (!FLAG_trace_gc_verbose) return;
  PrintPID("Memory allocator,   used: %6" V8_PTR_PREFIX "d KB"
               ", available: %6" V8_PTR_PREFIX "d KB\n",
           isolate_->memory_allocator()->Size() / KB,
           isolate_->memory_allocator()->Available() / KB);
  PrintPID("New space,          used: %6" V8_PTR_PREFIX "d KB"
               ", available: %6" V8_PTR_PREFIX "d KB"
               ", committed: %6" V8_PTR_PREFIX "d KB\n",
           new_space_.Size() / KB,
           new_space_.Available() / KB,
           new_space_.CommittedMemory() / KB);
  PrintPID("Old pointers,       used: %6" V8_PTR_PREFIX "d KB"
               ", available: %6" V8_PTR_PREFIX "d KB"
               ", committed: %6" V8_PTR_PREFIX "d KB\n",
           old_pointer_space_->SizeOfObjects() / KB,
           old_pointer_space_->Available() / KB,
           old_pointer_space_->CommittedMemory() / KB);
  PrintPID("Old data space,     used: %6" V8_PTR_PREFIX "d KB"
               ", available: %6" V8_PTR_PREFIX "d KB"
               ", committed: %6" V8_PTR_PREFIX "d KB\n",
           old_data_space_->SizeOfObjects() / KB,
           old_data_space_->Available() / KB,
           old_data_space_->CommittedMemory() / KB);
  PrintPID("Code space,         used: %6" V8_PTR_PREFIX "d KB"
               ", available: %6" V8_PTR_PREFIX "d KB"
               ", committed: %6" V8_PTR_PREFIX "d KB\n",
           code_space_->SizeOfObjects() / KB,
           code_space_->Available() / KB,
           code_space_->CommittedMemory() / KB);
  PrintPID("Map space,          used: %6" V8_PTR_PREFIX "d KB"
               ", available: %6" V8_PTR_PREFIX "d KB"
               ", committed: %6" V8_PTR_PREFIX "d KB\n",
           map_space_->SizeOfObjects() / KB,
           map_space_->Available() / KB,
           map_space_->CommittedMemory() / KB);
  PrintPID("Cell space,         used: %6" V8_PTR_PREFIX "d KB"
               ", available: %6" V8_PTR_PREFIX "d KB"
               ", committed: %6" V8_PTR_PREFIX "d KB\n",
           cell_space_->SizeOfObjects() / KB,
           cell_space_->Available() / KB,
           cell_space_->CommittedMemory() / KB);
  PrintPID("PropertyCell space, used: %6" V8_PTR_PREFIX "d KB"
               ", available: %6" V8_PTR_PREFIX "d KB"
               ", committed: %6" V8_PTR_PREFIX "d KB\n",
           property_cell_space_->SizeOfObjects() / KB,
           property_cell_space_->Available() / KB,
           property_cell_space_->CommittedMemory() / KB);
  PrintPID("Large object space, used: %6" V8_PTR_PREFIX "d KB"
               ", available: %6" V8_PTR_PREFIX "d KB"
               ", committed: %6" V8_PTR_PREFIX "d KB\n",
           lo_space_->SizeOfObjects() / KB,
           lo_space_->Available() / KB,
           lo_space_->CommittedMemory() / KB);
  PrintPID("All spaces,         used: %6" V8_PTR_PREFIX "d KB"
               ", available: %6" V8_PTR_PREFIX "d KB"
               ", committed: %6" V8_PTR_PREFIX "d KB\n",
           this->SizeOfObjects() / KB,
           this->Available() / KB,
           this->CommittedMemory() / KB);
  PrintPID("External memory reported: %6" V8_PTR_PREFIX "d KB\n",
           static_cast<intptr_t>(amount_of_external_allocated_memory_ / KB));
  PrintPID("Total time spent in GC  : %.1f ms\n", total_gc_time_ms_);
}


// TODO(1238405): Combine the infrastructure for --heap-stats and
// --log-gc to avoid the complicated preprocessor and flag testing.
void Heap::ReportStatisticsAfterGC() {
  // Similar to the before GC, we use some complicated logic to ensure that
  // NewSpace statistics are logged exactly once when --log-gc is turned on.
#if defined(DEBUG)
  if (FLAG_heap_stats) {
    new_space_.CollectStatistics();
    ReportHeapStatistics("After GC");
  } else if (FLAG_log_gc) {
    new_space_.ReportStatistics();
  }
#else
  if (FLAG_log_gc) new_space_.ReportStatistics();
#endif  // DEBUG
}


void Heap::GarbageCollectionPrologue() {
  {  AllowHeapAllocation for_the_first_part_of_prologue;
    ClearJSFunctionResultCaches();
    gc_count_++;
    unflattened_strings_length_ = 0;

    if (FLAG_flush_code && FLAG_flush_code_incrementally) {
      mark_compact_collector()->EnableCodeFlushing(true);
    }

#ifdef VERIFY_HEAP
    if (FLAG_verify_heap) {
      Verify();
    }
#endif
  }

  UpdateMaximumCommitted();

#ifdef DEBUG
  ASSERT(!AllowHeapAllocation::IsAllowed() && gc_state_ == NOT_IN_GC);

  if (FLAG_gc_verbose) Print();

  ReportStatisticsBeforeGC();
#endif  // DEBUG

  store_buffer()->GCPrologue();

  if (isolate()->concurrent_osr_enabled()) {
    isolate()->optimizing_compiler_thread()->AgeBufferedOsrJobs();
  }
}


intptr_t Heap::SizeOfObjects() {
  intptr_t total = 0;
  AllSpaces spaces(this);
  for (Space* space = spaces.next(); space != NULL; space = spaces.next()) {
    total += space->SizeOfObjects();
  }
  return total;
}


void Heap::ClearAllICsByKind(Code::Kind kind) {
  HeapObjectIterator it(code_space());

  for (Object* object = it.Next(); object != NULL; object = it.Next()) {
    Code* code = Code::cast(object);
    Code::Kind current_kind = code->kind();
    if (current_kind == Code::FUNCTION ||
        current_kind == Code::OPTIMIZED_FUNCTION) {
      code->ClearInlineCaches(kind);
    }
  }
}


void Heap::RepairFreeListsAfterBoot() {
  PagedSpaces spaces(this);
  for (PagedSpace* space = spaces.next();
       space != NULL;
       space = spaces.next()) {
    space->RepairFreeListsAfterBoot();
  }
}


void Heap::ProcessPretenuringFeedback() {
  if (FLAG_allocation_site_pretenuring) {
    int tenure_decisions = 0;
    int dont_tenure_decisions = 0;
    int allocation_mementos_found = 0;
    int allocation_sites = 0;
    int active_allocation_sites = 0;

    // If the scratchpad overflowed, we have to iterate over the allocation
    // sites list.
    bool use_scratchpad =
        allocation_sites_scratchpad_length_ < kAllocationSiteScratchpadSize;

    int i = 0;
    Object* list_element = allocation_sites_list();
    bool trigger_deoptimization = false;
    while (use_scratchpad ?
              i < allocation_sites_scratchpad_length_ :
              list_element->IsAllocationSite()) {
      AllocationSite* site = use_scratchpad ?
          AllocationSite::cast(allocation_sites_scratchpad()->get(i)) :
          AllocationSite::cast(list_element);
      allocation_mementos_found += site->memento_found_count();
      if (site->memento_found_count() > 0) {
        active_allocation_sites++;
      }
      if (site->DigestPretenuringFeedback()) trigger_deoptimization = true;
      if (site->GetPretenureMode() == TENURED) {
        tenure_decisions++;
      } else {
        dont_tenure_decisions++;
      }
      allocation_sites++;
      if (use_scratchpad) {
        i++;
      } else {
        list_element = site->weak_next();
      }
    }

    if (trigger_deoptimization) {
      isolate_->stack_guard()->DeoptMarkedAllocationSites();
    }

    FlushAllocationSitesScratchpad();

    if (FLAG_trace_pretenuring_statistics &&
        (allocation_mementos_found > 0 ||
         tenure_decisions > 0 ||
         dont_tenure_decisions > 0)) {
      PrintF("GC: (mode, #visited allocation sites, #active allocation sites, "
             "#mementos, #tenure decisions, #donttenure decisions) "
             "(%s, %d, %d, %d, %d, %d)\n",
             use_scratchpad ? "use scratchpad" : "use list",
             allocation_sites,
             active_allocation_sites,
             allocation_mementos_found,
             tenure_decisions,
             dont_tenure_decisions);
    }
  }
}


void Heap::DeoptMarkedAllocationSites() {
  // TODO(hpayer): If iterating over the allocation sites list becomes a
  // performance issue, use a cache heap data structure instead (similar to the
  // allocation sites scratchpad).
  Object* list_element = allocation_sites_list();
  while (list_element->IsAllocationSite()) {
    AllocationSite* site = AllocationSite::cast(list_element);
    if (site->deopt_dependent_code()) {
      site->dependent_code()->MarkCodeForDeoptimization(
          isolate_,
          DependentCode::kAllocationSiteTenuringChangedGroup);
      site->set_deopt_dependent_code(false);
    }
    list_element = site->weak_next();
  }
  Deoptimizer::DeoptimizeMarkedCode(isolate_);
}


void Heap::GarbageCollectionEpilogue() {
  store_buffer()->GCEpilogue();

  // In release mode, we only zap the from space under heap verification.
  if (Heap::ShouldZapGarbage()) {
    ZapFromSpace();
  }

  // Process pretenuring feedback and update allocation sites.
  ProcessPretenuringFeedback();

#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) {
    Verify();
  }
#endif

  AllowHeapAllocation for_the_rest_of_the_epilogue;

#ifdef DEBUG
  if (FLAG_print_global_handles) isolate_->global_handles()->Print();
  if (FLAG_print_handles) PrintHandles();
  if (FLAG_gc_verbose) Print();
  if (FLAG_code_stats) ReportCodeStatistics("After GC");
#endif
  if (FLAG_deopt_every_n_garbage_collections > 0) {
    // TODO(jkummerow/ulan/jarin): This is not safe! We can't assume that
    // the topmost optimized frame can be deoptimized safely, because it
    // might not have a lazy bailout point right after its current PC.
    if (++gcs_since_last_deopt_ == FLAG_deopt_every_n_garbage_collections) {
      Deoptimizer::DeoptimizeAll(isolate());
      gcs_since_last_deopt_ = 0;
    }
  }

  UpdateMaximumCommitted();

  isolate_->counters()->alive_after_last_gc()->Set(
      static_cast<int>(SizeOfObjects()));

  isolate_->counters()->string_table_capacity()->Set(
      string_table()->Capacity());
  isolate_->counters()->number_of_symbols()->Set(
      string_table()->NumberOfElements());

  if (full_codegen_bytes_generated_ + crankshaft_codegen_bytes_generated_ > 0) {
    isolate_->counters()->codegen_fraction_crankshaft()->AddSample(
        static_cast<int>((crankshaft_codegen_bytes_generated_ * 100.0) /
            (crankshaft_codegen_bytes_generated_
            + full_codegen_bytes_generated_)));
  }

  if (CommittedMemory() > 0) {
    isolate_->counters()->external_fragmentation_total()->AddSample(
        static_cast<int>(100 - (SizeOfObjects() * 100.0) / CommittedMemory()));

    isolate_->counters()->heap_fraction_new_space()->
        AddSample(static_cast<int>(
            (new_space()->CommittedMemory() * 100.0) / CommittedMemory()));
    isolate_->counters()->heap_fraction_old_pointer_space()->AddSample(
        static_cast<int>(
            (old_pointer_space()->CommittedMemory() * 100.0) /
            CommittedMemory()));
    isolate_->counters()->heap_fraction_old_data_space()->AddSample(
        static_cast<int>(
            (old_data_space()->CommittedMemory() * 100.0) /
            CommittedMemory()));
    isolate_->counters()->heap_fraction_code_space()->
        AddSample(static_cast<int>(
            (code_space()->CommittedMemory() * 100.0) / CommittedMemory()));
    isolate_->counters()->heap_fraction_map_space()->AddSample(
        static_cast<int>(
            (map_space()->CommittedMemory() * 100.0) / CommittedMemory()));
    isolate_->counters()->heap_fraction_cell_space()->AddSample(
        static_cast<int>(
            (cell_space()->CommittedMemory() * 100.0) / CommittedMemory()));
    isolate_->counters()->heap_fraction_property_cell_space()->
        AddSample(static_cast<int>(
            (property_cell_space()->CommittedMemory() * 100.0) /
            CommittedMemory()));
    isolate_->counters()->heap_fraction_lo_space()->
        AddSample(static_cast<int>(
            (lo_space()->CommittedMemory() * 100.0) / CommittedMemory()));

    isolate_->counters()->heap_sample_total_committed()->AddSample(
        static_cast<int>(CommittedMemory() / KB));
    isolate_->counters()->heap_sample_total_used()->AddSample(
        static_cast<int>(SizeOfObjects() / KB));
    isolate_->counters()->heap_sample_map_space_committed()->AddSample(
        static_cast<int>(map_space()->CommittedMemory() / KB));
    isolate_->counters()->heap_sample_cell_space_committed()->AddSample(
        static_cast<int>(cell_space()->CommittedMemory() / KB));
    isolate_->counters()->
        heap_sample_property_cell_space_committed()->
            AddSample(static_cast<int>(
                property_cell_space()->CommittedMemory() / KB));
    isolate_->counters()->heap_sample_code_space_committed()->AddSample(
        static_cast<int>(code_space()->CommittedMemory() / KB));

    isolate_->counters()->heap_sample_maximum_committed()->AddSample(
        static_cast<int>(MaximumCommittedMemory() / KB));
  }

#define UPDATE_COUNTERS_FOR_SPACE(space)                                       \
  isolate_->counters()->space##_bytes_available()->Set(                        \
      static_cast<int>(space()->Available()));                                 \
  isolate_->counters()->space##_bytes_committed()->Set(                        \
      static_cast<int>(space()->CommittedMemory()));                           \
  isolate_->counters()->space##_bytes_used()->Set(                             \
      static_cast<int>(space()->SizeOfObjects()));
#define UPDATE_FRAGMENTATION_FOR_SPACE(space)                                  \
  if (space()->CommittedMemory() > 0) {                                        \
    isolate_->counters()->external_fragmentation_##space()->AddSample(         \
        static_cast<int>(100 -                                                 \
            (space()->SizeOfObjects() * 100.0) / space()->CommittedMemory())); \
  }
#define UPDATE_COUNTERS_AND_FRAGMENTATION_FOR_SPACE(space)                     \
  UPDATE_COUNTERS_FOR_SPACE(space)                                             \
  UPDATE_FRAGMENTATION_FOR_SPACE(space)

  UPDATE_COUNTERS_FOR_SPACE(new_space)
  UPDATE_COUNTERS_AND_FRAGMENTATION_FOR_SPACE(old_pointer_space)
  UPDATE_COUNTERS_AND_FRAGMENTATION_FOR_SPACE(old_data_space)
  UPDATE_COUNTERS_AND_FRAGMENTATION_FOR_SPACE(code_space)
  UPDATE_COUNTERS_AND_FRAGMENTATION_FOR_SPACE(map_space)
  UPDATE_COUNTERS_AND_FRAGMENTATION_FOR_SPACE(cell_space)
  UPDATE_COUNTERS_AND_FRAGMENTATION_FOR_SPACE(property_cell_space)
  UPDATE_COUNTERS_AND_FRAGMENTATION_FOR_SPACE(lo_space)
#undef UPDATE_COUNTERS_FOR_SPACE
#undef UPDATE_FRAGMENTATION_FOR_SPACE
#undef UPDATE_COUNTERS_AND_FRAGMENTATION_FOR_SPACE

#ifdef DEBUG
  ReportStatisticsAfterGC();
#endif  // DEBUG
  isolate_->debug()->AfterGarbageCollection();
}


void Heap::CollectAllGarbage(int flags,
                             const char* gc_reason,
                             const v8::GCCallbackFlags gc_callback_flags) {
  // Since we are ignoring the return value, the exact choice of space does
  // not matter, so long as we do not specify NEW_SPACE, which would not
  // cause a full GC.
  mark_compact_collector_.SetFlags(flags);
  CollectGarbage(OLD_POINTER_SPACE, gc_reason, gc_callback_flags);
  mark_compact_collector_.SetFlags(kNoGCFlags);
}


void Heap::CollectAllAvailableGarbage(const char* gc_reason) {
  // Since we are ignoring the return value, the exact choice of space does
  // not matter, so long as we do not specify NEW_SPACE, which would not
  // cause a full GC.
  // Major GC would invoke weak handle callbacks on weakly reachable
  // handles, but won't collect weakly reachable objects until next
  // major GC.  Therefore if we collect aggressively and weak handle callback
  // has been invoked, we rerun major GC to release objects which become
  // garbage.
  // Note: as weak callbacks can execute arbitrary code, we cannot
  // hope that eventually there will be no weak callbacks invocations.
  // Therefore stop recollecting after several attempts.
  if (isolate()->concurrent_recompilation_enabled()) {
    // The optimizing compiler may be unnecessarily holding on to memory.
    DisallowHeapAllocation no_recursive_gc;
    isolate()->optimizing_compiler_thread()->Flush();
  }
  mark_compact_collector()->SetFlags(kMakeHeapIterableMask |
                                     kReduceMemoryFootprintMask);
  isolate_->compilation_cache()->Clear();
  const int kMaxNumberOfAttempts = 7;
  const int kMinNumberOfAttempts = 2;
  for (int attempt = 0; attempt < kMaxNumberOfAttempts; attempt++) {
    if (!CollectGarbage(MARK_COMPACTOR, gc_reason, NULL) &&
        attempt + 1 >= kMinNumberOfAttempts) {
      break;
    }
  }
  mark_compact_collector()->SetFlags(kNoGCFlags);
  new_space_.Shrink();
  UncommitFromSpace();
  incremental_marking()->UncommitMarkingDeque();
}


void Heap::EnsureFillerObjectAtTop() {
  // There may be an allocation memento behind every object in new space.
  // If we evacuate a not full new space or if we are on the last page of
  // the new space, then there may be uninitialized memory behind the top
  // pointer of the new space page. We store a filler object there to
  // identify the unused space.
  Address from_top = new_space_.top();
  Address from_limit = new_space_.limit();
  if (from_top < from_limit) {
    int remaining_in_page = static_cast<int>(from_limit - from_top);
    CreateFillerObjectAt(from_top, remaining_in_page);
  }
}


bool Heap::CollectGarbage(GarbageCollector collector,
                          const char* gc_reason,
                          const char* collector_reason,
                          const v8::GCCallbackFlags gc_callback_flags) {
  // The VM is in the GC state until exiting this function.
  VMState<GC> state(isolate_);

#ifdef DEBUG
  // Reset the allocation timeout to the GC interval, but make sure to
  // allow at least a few allocations after a collection. The reason
  // for this is that we have a lot of allocation sequences and we
  // assume that a garbage collection will allow the subsequent
  // allocation attempts to go through.
  allocation_timeout_ = Max(6, FLAG_gc_interval);
#endif

  EnsureFillerObjectAtTop();

  if (collector == SCAVENGER && !incremental_marking()->IsStopped()) {
    if (FLAG_trace_incremental_marking) {
      PrintF("[IncrementalMarking] Scavenge during marking.\n");
    }
  }

  if (collector == MARK_COMPACTOR &&
      !mark_compact_collector()->abort_incremental_marking() &&
      !incremental_marking()->IsStopped() &&
      !incremental_marking()->should_hurry() &&
      FLAG_incremental_marking_steps) {
    // Make progress in incremental marking.
    const intptr_t kStepSizeWhenDelayedByScavenge = 1 * MB *
      FullCodeGenerator::kCodeSizeMultiplier / 100;
    incremental_marking()->Step(kStepSizeWhenDelayedByScavenge,
                                IncrementalMarking::NO_GC_VIA_STACK_GUARD);
    if (!incremental_marking()->IsComplete()) {
      if (FLAG_trace_incremental_marking) {
        PrintF("[IncrementalMarking] Delaying MarkSweep.\n");
      }
      collector = SCAVENGER;
      collector_reason = "incremental marking delaying mark-sweep";
    }
  }

  bool next_gc_likely_to_collect_more = false;

  { GCTracer tracer(this, gc_reason, collector_reason);
    ASSERT(AllowHeapAllocation::IsAllowed());
    DisallowHeapAllocation no_allocation_during_gc;
    GarbageCollectionPrologue();
    // The GC count was incremented in the prologue.  Tell the tracer about
    // it.
    tracer.set_gc_count(gc_count_);

    // Tell the tracer which collector we've selected.
    tracer.set_collector(collector);

    {
      HistogramTimerScope histogram_timer_scope(
          (collector == SCAVENGER) ? isolate_->counters()->gc_scavenger()
                                   : isolate_->counters()->gc_compactor());
      next_gc_likely_to_collect_more =
          PerformGarbageCollection(collector, &tracer, gc_callback_flags);
    }

    GarbageCollectionEpilogue();
  }

  // Start incremental marking for the next cycle. The heap snapshot
  // generator needs incremental marking to stay off after it aborted.
  if (!mark_compact_collector()->abort_incremental_marking() &&
      incremental_marking()->IsStopped() &&
      incremental_marking()->WorthActivating() &&
      NextGCIsLikelyToBeFull()) {
    incremental_marking()->Start();
  }

  return next_gc_likely_to_collect_more;
}


int Heap::NotifyContextDisposed() {
  if (isolate()->concurrent_recompilation_enabled()) {
    // Flush the queued recompilation tasks.
    isolate()->optimizing_compiler_thread()->Flush();
  }
  flush_monomorphic_ics_ = true;
  AgeInlineCaches();
  return ++contexts_disposed_;
}


void Heap::MoveElements(FixedArray* array,
                        int dst_index,
                        int src_index,
                        int len) {
  if (len == 0) return;

  ASSERT(array->map() != fixed_cow_array_map());
  Object** dst_objects = array->data_start() + dst_index;
  OS::MemMove(dst_objects,
              array->data_start() + src_index,
              len * kPointerSize);
  if (!InNewSpace(array)) {
    for (int i = 0; i < len; i++) {
      // TODO(hpayer): check store buffer for entries
      if (InNewSpace(dst_objects[i])) {
        RecordWrite(array->address(), array->OffsetOfElementAt(dst_index + i));
      }
    }
  }
  incremental_marking()->RecordWrites(array);
}


#ifdef VERIFY_HEAP
// Helper class for verifying the string table.
class StringTableVerifier : public ObjectVisitor {
 public:
  void VisitPointers(Object** start, Object** end) {
    // Visit all HeapObject pointers in [start, end).
    for (Object** p = start; p < end; p++) {
      if ((*p)->IsHeapObject()) {
        // Check that the string is actually internalized.
        CHECK((*p)->IsTheHole() || (*p)->IsUndefined() ||
              (*p)->IsInternalizedString());
      }
    }
  }
};


static void VerifyStringTable(Heap* heap) {
  StringTableVerifier verifier;
  heap->string_table()->IterateElements(&verifier);
}
#endif  // VERIFY_HEAP


static bool AbortIncrementalMarkingAndCollectGarbage(
    Heap* heap,
    AllocationSpace space,
    const char* gc_reason = NULL) {
  heap->mark_compact_collector()->SetFlags(Heap::kAbortIncrementalMarkingMask);
  bool result = heap->CollectGarbage(space, gc_reason);
  heap->mark_compact_collector()->SetFlags(Heap::kNoGCFlags);
  return result;
}


void Heap::ReserveSpace(int *sizes, Address *locations_out) {
  bool gc_performed = true;
  int counter = 0;
  static const int kThreshold = 20;
  while (gc_performed && counter++ < kThreshold) {
    gc_performed = false;
    ASSERT(NEW_SPACE == FIRST_PAGED_SPACE - 1);
    for (int space = NEW_SPACE; space <= LAST_PAGED_SPACE; space++) {
      if (sizes[space] != 0) {
        AllocationResult allocation;
        if (space == NEW_SPACE) {
          allocation = new_space()->AllocateRaw(sizes[space]);
        } else {
          allocation = paged_space(space)->AllocateRaw(sizes[space]);
        }
        FreeListNode* node;
        if (!allocation.To(&node)) {
          if (space == NEW_SPACE) {
            Heap::CollectGarbage(NEW_SPACE,
                                 "failed to reserve space in the new space");
          } else {
            AbortIncrementalMarkingAndCollectGarbage(
                this,
                static_cast<AllocationSpace>(space),
                "failed to reserve space in paged space");
          }
          gc_performed = true;
          break;
        } else {
          // Mark with a free list node, in case we have a GC before
          // deserializing.
          node->set_size(this, sizes[space]);
          locations_out[space] = node->address();
        }
      }
    }
  }

  if (gc_performed) {
    // Failed to reserve the space after several attempts.
    V8::FatalProcessOutOfMemory("Heap::ReserveSpace");
  }
}


void Heap::EnsureFromSpaceIsCommitted() {
  if (new_space_.CommitFromSpaceIfNeeded()) return;

  // Committing memory to from space failed.
  // Memory is exhausted and we will die.
  V8::FatalProcessOutOfMemory("Committing semi space failed.");
}


void Heap::ClearJSFunctionResultCaches() {
  if (isolate_->bootstrapper()->IsActive()) return;

  Object* context = native_contexts_list_;
  while (!context->IsUndefined()) {
    // Get the caches for this context. GC can happen when the context
    // is not fully initialized, so the caches can be undefined.
    Object* caches_or_undefined =
        Context::cast(context)->get(Context::JSFUNCTION_RESULT_CACHES_INDEX);
    if (!caches_or_undefined->IsUndefined()) {
      FixedArray* caches = FixedArray::cast(caches_or_undefined);
      // Clear the caches:
      int length = caches->length();
      for (int i = 0; i < length; i++) {
        JSFunctionResultCache::cast(caches->get(i))->Clear();
      }
    }
    // Get the next context:
    context = Context::cast(context)->get(Context::NEXT_CONTEXT_LINK);
  }
}


void Heap::ClearNormalizedMapCaches() {
  if (isolate_->bootstrapper()->IsActive() &&
      !incremental_marking()->IsMarking()) {
    return;
  }

  Object* context = native_contexts_list_;
  while (!context->IsUndefined()) {
    // GC can happen when the context is not fully initialized,
    // so the cache can be undefined.
    Object* cache =
        Context::cast(context)->get(Context::NORMALIZED_MAP_CACHE_INDEX);
    if (!cache->IsUndefined()) {
      NormalizedMapCache::cast(cache)->Clear();
    }
    context = Context::cast(context)->get(Context::NEXT_CONTEXT_LINK);
  }
}


void Heap::UpdateSurvivalRateTrend(int start_new_space_size) {
  if (start_new_space_size == 0) return;

  double survival_rate =
      (static_cast<double>(young_survivors_after_last_gc_) * 100) /
      start_new_space_size;

  if (survival_rate > kYoungSurvivalRateHighThreshold) {
    high_survival_rate_period_length_++;
  } else {
    high_survival_rate_period_length_ = 0;
  }

  if (survival_rate < kYoungSurvivalRateLowThreshold) {
    low_survival_rate_period_length_++;
  } else {
    low_survival_rate_period_length_ = 0;
  }

  double survival_rate_diff = survival_rate_ - survival_rate;

  if (survival_rate_diff > kYoungSurvivalRateAllowedDeviation) {
    set_survival_rate_trend(DECREASING);
  } else if (survival_rate_diff < -kYoungSurvivalRateAllowedDeviation) {
    set_survival_rate_trend(INCREASING);
  } else {
    set_survival_rate_trend(STABLE);
  }

  survival_rate_ = survival_rate;
}

bool Heap::PerformGarbageCollection(
    GarbageCollector collector,
    GCTracer* tracer,
    const v8::GCCallbackFlags gc_callback_flags) {
  bool next_gc_likely_to_collect_more = false;

  if (collector != SCAVENGER) {
    PROFILE(isolate_, CodeMovingGCEvent());
  }

#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) {
    VerifyStringTable(this);
  }
#endif

  GCType gc_type =
      collector == MARK_COMPACTOR ? kGCTypeMarkSweepCompact : kGCTypeScavenge;

  { GCCallbacksScope scope(this);
    if (scope.CheckReenter()) {
      AllowHeapAllocation allow_allocation;
      GCTracer::Scope scope(tracer, GCTracer::Scope::EXTERNAL);
      VMState<EXTERNAL> state(isolate_);
      HandleScope handle_scope(isolate_);
      CallGCPrologueCallbacks(gc_type, kNoGCCallbackFlags);
    }
  }

  EnsureFromSpaceIsCommitted();

  int start_new_space_size = Heap::new_space()->SizeAsInt();

  if (IsHighSurvivalRate()) {
    // We speed up the incremental marker if it is running so that it
    // does not fall behind the rate of promotion, which would cause a
    // constantly growing old space.
    incremental_marking()->NotifyOfHighPromotionRate();
  }

  if (collector == MARK_COMPACTOR) {
    // Perform mark-sweep with optional compaction.
    MarkCompact(tracer);
    sweep_generation_++;

    UpdateSurvivalRateTrend(start_new_space_size);

    size_of_old_gen_at_last_old_space_gc_ = PromotedSpaceSizeOfObjects();

    old_generation_allocation_limit_ =
        OldGenerationAllocationLimit(size_of_old_gen_at_last_old_space_gc_);

    old_gen_exhausted_ = false;
  } else {
    tracer_ = tracer;
    Scavenge();
    tracer_ = NULL;

    UpdateSurvivalRateTrend(start_new_space_size);
  }

  if (!new_space_high_promotion_mode_active_ &&
      new_space_.Capacity() == new_space_.MaximumCapacity() &&
      IsStableOrIncreasingSurvivalTrend() &&
      IsHighSurvivalRate()) {
    // Stable high survival rates even though young generation is at
    // maximum capacity indicates that most objects will be promoted.
    // To decrease scavenger pauses and final mark-sweep pauses, we
    // have to limit maximal capacity of the young generation.
    SetNewSpaceHighPromotionModeActive(true);
    if (FLAG_trace_gc) {
      PrintPID("Limited new space size due to high promotion rate: %d MB\n",
               new_space_.InitialCapacity() / MB);
    }
    // The high promotion mode is our indicator to turn on pretenuring. We have
    // to deoptimize all optimized code in global pretenuring mode and all
    // code which should be tenured in local pretenuring mode.
    if (FLAG_pretenuring) {
      if (!FLAG_allocation_site_pretenuring) {
        isolate_->stack_guard()->FullDeopt();
      }
    }
  } else if (new_space_high_promotion_mode_active_ &&
      IsStableOrDecreasingSurvivalTrend() &&
      IsLowSurvivalRate()) {
    // Decreasing low survival rates might indicate that the above high
    // promotion mode is over and we should allow the young generation
    // to grow again.
    SetNewSpaceHighPromotionModeActive(false);
    if (FLAG_trace_gc) {
      PrintPID("Unlimited new space size due to low promotion rate: %d MB\n",
               new_space_.MaximumCapacity() / MB);
    }
    // Trigger deoptimization here to turn off global pretenuring as soon as
    // possible.
    if (FLAG_pretenuring && !FLAG_allocation_site_pretenuring) {
      isolate_->stack_guard()->FullDeopt();
    }
  }

  if (new_space_high_promotion_mode_active_ &&
      new_space_.Capacity() > new_space_.InitialCapacity()) {
    new_space_.Shrink();
  }

  isolate_->counters()->objs_since_last_young()->Set(0);

  // Callbacks that fire after this point might trigger nested GCs and
  // restart incremental marking, the assertion can't be moved down.
  ASSERT(collector == SCAVENGER || incremental_marking()->IsStopped());

  gc_post_processing_depth_++;
  { AllowHeapAllocation allow_allocation;
    GCTracer::Scope scope(tracer, GCTracer::Scope::EXTERNAL);
    next_gc_likely_to_collect_more =
        isolate_->global_handles()->PostGarbageCollectionProcessing(
            collector, tracer);
  }
  gc_post_processing_depth_--;

  isolate_->eternal_handles()->PostGarbageCollectionProcessing(this);

  // Update relocatables.
  Relocatable::PostGarbageCollectionProcessing(isolate_);

  if (collector == MARK_COMPACTOR) {
    // Register the amount of external allocated memory.
    amount_of_external_allocated_memory_at_last_global_gc_ =
        amount_of_external_allocated_memory_;
  }

  { GCCallbacksScope scope(this);
    if (scope.CheckReenter()) {
      AllowHeapAllocation allow_allocation;
      GCTracer::Scope scope(tracer, GCTracer::Scope::EXTERNAL);
      VMState<EXTERNAL> state(isolate_);
      HandleScope handle_scope(isolate_);
      CallGCEpilogueCallbacks(gc_type, gc_callback_flags);
    }
  }

#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) {
    VerifyStringTable(this);
  }
#endif

  return next_gc_likely_to_collect_more;
}


void Heap::CallGCPrologueCallbacks(GCType gc_type, GCCallbackFlags flags) {
  for (int i = 0; i < gc_prologue_callbacks_.length(); ++i) {
    if (gc_type & gc_prologue_callbacks_[i].gc_type) {
      if (!gc_prologue_callbacks_[i].pass_isolate_) {
        v8::GCPrologueCallback callback =
            reinterpret_cast<v8::GCPrologueCallback>(
                gc_prologue_callbacks_[i].callback);
        callback(gc_type, flags);
      } else {
        v8::Isolate* isolate = reinterpret_cast<v8::Isolate*>(this->isolate());
        gc_prologue_callbacks_[i].callback(isolate, gc_type, flags);
      }
    }
  }
}


void Heap::CallGCEpilogueCallbacks(GCType gc_type,
                                   GCCallbackFlags gc_callback_flags) {
  for (int i = 0; i < gc_epilogue_callbacks_.length(); ++i) {
    if (gc_type & gc_epilogue_callbacks_[i].gc_type) {
      if (!gc_epilogue_callbacks_[i].pass_isolate_) {
        v8::GCPrologueCallback callback =
            reinterpret_cast<v8::GCPrologueCallback>(
                gc_epilogue_callbacks_[i].callback);
        callback(gc_type, gc_callback_flags);
      } else {
        v8::Isolate* isolate = reinterpret_cast<v8::Isolate*>(this->isolate());
        gc_epilogue_callbacks_[i].callback(
            isolate, gc_type, gc_callback_flags);
      }
    }
  }
}


void Heap::MarkCompact(GCTracer* tracer) {
  gc_state_ = MARK_COMPACT;
  LOG(isolate_, ResourceEvent("markcompact", "begin"));

  uint64_t size_of_objects_before_gc = SizeOfObjects();

  mark_compact_collector_.Prepare(tracer);

  ms_count_++;
  tracer->set_full_gc_count(ms_count_);

  MarkCompactPrologue();

  mark_compact_collector_.CollectGarbage();

  LOG(isolate_, ResourceEvent("markcompact", "end"));

  gc_state_ = NOT_IN_GC;

  isolate_->counters()->objs_since_last_full()->Set(0);

  flush_monomorphic_ics_ = false;

  if (FLAG_allocation_site_pretenuring) {
    EvaluateOldSpaceLocalPretenuring(size_of_objects_before_gc);
  }
}


void Heap::MarkCompactPrologue() {
  // At any old GC clear the keyed lookup cache to enable collection of unused
  // maps.
  isolate_->keyed_lookup_cache()->Clear();
  isolate_->context_slot_cache()->Clear();
  isolate_->descriptor_lookup_cache()->Clear();
  RegExpResultsCache::Clear(string_split_cache());
  RegExpResultsCache::Clear(regexp_multiple_cache());

  isolate_->compilation_cache()->MarkCompactPrologue();

  CompletelyClearInstanceofCache();

  FlushNumberStringCache();
  if (FLAG_cleanup_code_caches_at_gc) {
    polymorphic_code_cache()->set_cache(undefined_value());
  }

  ClearNormalizedMapCaches();
}


// Helper class for copying HeapObjects
class ScavengeVisitor: public ObjectVisitor {
 public:
  explicit ScavengeVisitor(Heap* heap) : heap_(heap) {}

  void VisitPointer(Object** p) { ScavengePointer(p); }

  void VisitPointers(Object** start, Object** end) {
    // Copy all HeapObject pointers in [start, end)
    for (Object** p = start; p < end; p++) ScavengePointer(p);
  }

 private:
  void ScavengePointer(Object** p) {
    Object* object = *p;
    if (!heap_->InNewSpace(object)) return;
    Heap::ScavengeObject(reinterpret_cast<HeapObject**>(p),
                         reinterpret_cast<HeapObject*>(object));
  }

  Heap* heap_;
};


#ifdef VERIFY_HEAP
// Visitor class to verify pointers in code or data space do not point into
// new space.
class VerifyNonPointerSpacePointersVisitor: public ObjectVisitor {
 public:
  explicit VerifyNonPointerSpacePointersVisitor(Heap* heap) : heap_(heap) {}
  void VisitPointers(Object** start, Object**end) {
    for (Object** current = start; current < end; current++) {
      if ((*current)->IsHeapObject()) {
        CHECK(!heap_->InNewSpace(HeapObject::cast(*current)));
      }
    }
  }

 private:
  Heap* heap_;
};


static void VerifyNonPointerSpacePointers(Heap* heap) {
  // Verify that there are no pointers to new space in spaces where we
  // do not expect them.
  VerifyNonPointerSpacePointersVisitor v(heap);
  HeapObjectIterator code_it(heap->code_space());
  for (HeapObject* object = code_it.Next();
       object != NULL; object = code_it.Next())
    object->Iterate(&v);

  // The old data space was normally swept conservatively so that the iterator
  // doesn't work, so we normally skip the next bit.
  if (!heap->old_data_space()->was_swept_conservatively()) {
    HeapObjectIterator data_it(heap->old_data_space());
    for (HeapObject* object = data_it.Next();
         object != NULL; object = data_it.Next())
      object->Iterate(&v);
  }
}
#endif  // VERIFY_HEAP


void Heap::CheckNewSpaceExpansionCriteria() {
  if (new_space_.Capacity() < new_space_.MaximumCapacity() &&
      survived_since_last_expansion_ > new_space_.Capacity() &&
      !new_space_high_promotion_mode_active_) {
    // Grow the size of new space if there is room to grow, enough data
    // has survived scavenge since the last expansion and we are not in
    // high promotion mode.
    new_space_.Grow();
    survived_since_last_expansion_ = 0;
  }
}


static bool IsUnscavengedHeapObject(Heap* heap, Object** p) {
  return heap->InNewSpace(*p) &&
      !HeapObject::cast(*p)->map_word().IsForwardingAddress();
}


void Heap::ScavengeStoreBufferCallback(
    Heap* heap,
    MemoryChunk* page,
    StoreBufferEvent event) {
  heap->store_buffer_rebuilder_.Callback(page, event);
}


void StoreBufferRebuilder::Callback(MemoryChunk* page, StoreBufferEvent event) {
  if (event == kStoreBufferStartScanningPagesEvent) {
    start_of_current_page_ = NULL;
    current_page_ = NULL;
  } else if (event == kStoreBufferScanningPageEvent) {
    if (current_page_ != NULL) {
      // If this page already overflowed the store buffer during this iteration.
      if (current_page_->scan_on_scavenge()) {
        // Then we should wipe out the entries that have been added for it.
        store_buffer_->SetTop(start_of_current_page_);
      } else if (store_buffer_->Top() - start_of_current_page_ >=
                 (store_buffer_->Limit() - store_buffer_->Top()) >> 2) {
        // Did we find too many pointers in the previous page?  The heuristic is
        // that no page can take more then 1/5 the remaining slots in the store
        // buffer.
        current_page_->set_scan_on_scavenge(true);
        store_buffer_->SetTop(start_of_current_page_);
      } else {
        // In this case the page we scanned took a reasonable number of slots in
        // the store buffer.  It has now been rehabilitated and is no longer
        // marked scan_on_scavenge.
        ASSERT(!current_page_->scan_on_scavenge());
      }
    }
    start_of_current_page_ = store_buffer_->Top();
    current_page_ = page;
  } else if (event == kStoreBufferFullEvent) {
    // The current page overflowed the store buffer again.  Wipe out its entries
    // in the store buffer and mark it scan-on-scavenge again.  This may happen
    // several times while scanning.
    if (current_page_ == NULL) {
      // Store Buffer overflowed while scanning promoted objects.  These are not
      // in any particular page, though they are likely to be clustered by the
      // allocation routines.
      store_buffer_->EnsureSpace(StoreBuffer::kStoreBufferSize / 2);
    } else {
      // Store Buffer overflowed while scanning a particular old space page for
      // pointers to new space.
      ASSERT(current_page_ == page);
      ASSERT(page != NULL);
      current_page_->set_scan_on_scavenge(true);
      ASSERT(start_of_current_page_ != store_buffer_->Top());
      store_buffer_->SetTop(start_of_current_page_);
    }
  } else {
    UNREACHABLE();
  }
}


void PromotionQueue::Initialize() {
  // Assumes that a NewSpacePage exactly fits a number of promotion queue
  // entries (where each is a pair of intptr_t). This allows us to simplify
  // the test fpr when to switch pages.
  ASSERT((Page::kPageSize - MemoryChunk::kBodyOffset) % (2 * kPointerSize)
         == 0);
  limit_ = reinterpret_cast<intptr_t*>(heap_->new_space()->ToSpaceStart());
  front_ = rear_ =
      reinterpret_cast<intptr_t*>(heap_->new_space()->ToSpaceEnd());
  emergency_stack_ = NULL;
  guard_ = false;
}


void PromotionQueue::RelocateQueueHead() {
  ASSERT(emergency_stack_ == NULL);

  Page* p = Page::FromAllocationTop(reinterpret_cast<Address>(rear_));
  intptr_t* head_start = rear_;
  intptr_t* head_end =
      Min(front_, reinterpret_cast<intptr_t*>(p->area_end()));

  int entries_count =
      static_cast<int>(head_end - head_start) / kEntrySizeInWords;

  emergency_stack_ = new List<Entry>(2 * entries_count);

  while (head_start != head_end) {
    int size = static_cast<int>(*(head_start++));
    HeapObject* obj = reinterpret_cast<HeapObject*>(*(head_start++));
    emergency_stack_->Add(Entry(obj, size));
  }
  rear_ = head_end;
}


class ScavengeWeakObjectRetainer : public WeakObjectRetainer {
 public:
  explicit ScavengeWeakObjectRetainer(Heap* heap) : heap_(heap) { }

  virtual Object* RetainAs(Object* object) {
    if (!heap_->InFromSpace(object)) {
      return object;
    }

    MapWord map_word = HeapObject::cast(object)->map_word();
    if (map_word.IsForwardingAddress()) {
      return map_word.ToForwardingAddress();
    }
    return NULL;
  }

 private:
  Heap* heap_;
};


void Heap::Scavenge() {
  RelocationLock relocation_lock(this);

#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) VerifyNonPointerSpacePointers(this);
#endif

  gc_state_ = SCAVENGE;

  // Implements Cheney's copying algorithm
  LOG(isolate_, ResourceEvent("scavenge", "begin"));

  // Clear descriptor cache.
  isolate_->descriptor_lookup_cache()->Clear();

  // Used for updating survived_since_last_expansion_ at function end.
  intptr_t survived_watermark = PromotedSpaceSizeOfObjects();

  CheckNewSpaceExpansionCriteria();

  SelectScavengingVisitorsTable();

  incremental_marking()->PrepareForScavenge();

  // Flip the semispaces.  After flipping, to space is empty, from space has
  // live objects.
  new_space_.Flip();
  new_space_.ResetAllocationInfo();

  // We need to sweep newly copied objects which can be either in the
  // to space or promoted to the old generation.  For to-space
  // objects, we treat the bottom of the to space as a queue.  Newly
  // copied and unswept objects lie between a 'front' mark and the
  // allocation pointer.
  //
  // Promoted objects can go into various old-generation spaces, and
  // can be allocated internally in the spaces (from the free list).
  // We treat the top of the to space as a queue of addresses of
  // promoted objects.  The addresses of newly promoted and unswept
  // objects lie between a 'front' mark and a 'rear' mark that is
  // updated as a side effect of promoting an object.
  //
  // There is guaranteed to be enough room at the top of the to space
  // for the addresses of promoted objects: every object promoted
  // frees up its size in bytes from the top of the new space, and
  // objects are at least one pointer in size.
  Address new_space_front = new_space_.ToSpaceStart();
  promotion_queue_.Initialize();

#ifdef DEBUG
  store_buffer()->Clean();
#endif

  ScavengeVisitor scavenge_visitor(this);
  // Copy roots.
  IterateRoots(&scavenge_visitor, VISIT_ALL_IN_SCAVENGE);

  // Copy objects reachable from the old generation.
  {
    StoreBufferRebuildScope scope(this,
                                  store_buffer(),
                                  &ScavengeStoreBufferCallback);
    store_buffer()->IteratePointersToNewSpace(&ScavengeObject);
  }

  // Copy objects reachable from simple cells by scavenging cell values
  // directly.
  HeapObjectIterator cell_iterator(cell_space_);
  for (HeapObject* heap_object = cell_iterator.Next();
       heap_object != NULL;
       heap_object = cell_iterator.Next()) {
    if (heap_object->IsCell()) {
      Cell* cell = Cell::cast(heap_object);
      Address value_address = cell->ValueAddress();
      scavenge_visitor.VisitPointer(reinterpret_cast<Object**>(value_address));
    }
  }

  // Copy objects reachable from global property cells by scavenging global
  // property cell values directly.
  HeapObjectIterator js_global_property_cell_iterator(property_cell_space_);
  for (HeapObject* heap_object = js_global_property_cell_iterator.Next();
       heap_object != NULL;
       heap_object = js_global_property_cell_iterator.Next()) {
    if (heap_object->IsPropertyCell()) {
      PropertyCell* cell = PropertyCell::cast(heap_object);
      Address value_address = cell->ValueAddress();
      scavenge_visitor.VisitPointer(reinterpret_cast<Object**>(value_address));
      Address type_address = cell->TypeAddress();
      scavenge_visitor.VisitPointer(reinterpret_cast<Object**>(type_address));
    }
  }

  // Copy objects reachable from the code flushing candidates list.
  MarkCompactCollector* collector = mark_compact_collector();
  if (collector->is_code_flushing_enabled()) {
    collector->code_flusher()->IteratePointersToFromSpace(&scavenge_visitor);
  }

  // Scavenge object reachable from the native contexts list directly.
  scavenge_visitor.VisitPointer(BitCast<Object**>(&native_contexts_list_));

  new_space_front = DoScavenge(&scavenge_visitor, new_space_front);

  while (isolate()->global_handles()->IterateObjectGroups(
      &scavenge_visitor, &IsUnscavengedHeapObject)) {
    new_space_front = DoScavenge(&scavenge_visitor, new_space_front);
  }
  isolate()->global_handles()->RemoveObjectGroups();
  isolate()->global_handles()->RemoveImplicitRefGroups();

  isolate_->global_handles()->IdentifyNewSpaceWeakIndependentHandles(
      &IsUnscavengedHeapObject);
  isolate_->global_handles()->IterateNewSpaceWeakIndependentRoots(
      &scavenge_visitor);
  new_space_front = DoScavenge(&scavenge_visitor, new_space_front);

  UpdateNewSpaceReferencesInExternalStringTable(
      &UpdateNewSpaceReferenceInExternalStringTableEntry);

  promotion_queue_.Destroy();

  incremental_marking()->UpdateMarkingDequeAfterScavenge();

  ScavengeWeakObjectRetainer weak_object_retainer(this);
  ProcessWeakReferences(&weak_object_retainer);

  ASSERT(new_space_front == new_space_.top());

  // Set age mark.
  new_space_.set_age_mark(new_space_.top());

  new_space_.LowerInlineAllocationLimit(
      new_space_.inline_allocation_limit_step());

  // Update how much has survived scavenge.
  IncrementYoungSurvivorsCounter(static_cast<int>(
      (PromotedSpaceSizeOfObjects() - survived_watermark) + new_space_.Size()));

  LOG(isolate_, ResourceEvent("scavenge", "end"));

  gc_state_ = NOT_IN_GC;

  scavenges_since_last_idle_round_++;
}


String* Heap::UpdateNewSpaceReferenceInExternalStringTableEntry(Heap* heap,
                                                                Object** p) {
  MapWord first_word = HeapObject::cast(*p)->map_word();

  if (!first_word.IsForwardingAddress()) {
    // Unreachable external string can be finalized.
    heap->FinalizeExternalString(String::cast(*p));
    return NULL;
  }

  // String is still reachable.
  return String::cast(first_word.ToForwardingAddress());
}


void Heap::UpdateNewSpaceReferencesInExternalStringTable(
    ExternalStringTableUpdaterCallback updater_func) {
#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) {
    external_string_table_.Verify();
  }
#endif

  if (external_string_table_.new_space_strings_.is_empty()) return;

  Object** start = &external_string_table_.new_space_strings_[0];
  Object** end = start + external_string_table_.new_space_strings_.length();
  Object** last = start;

  for (Object** p = start; p < end; ++p) {
    ASSERT(InFromSpace(*p));
    String* target = updater_func(this, p);

    if (target == NULL) continue;

    ASSERT(target->IsExternalString());

    if (InNewSpace(target)) {
      // String is still in new space.  Update the table entry.
      *last = target;
      ++last;
    } else {
      // String got promoted.  Move it to the old string list.
      external_string_table_.AddOldString(target);
    }
  }

  ASSERT(last <= end);
  external_string_table_.ShrinkNewStrings(static_cast<int>(last - start));
}


void Heap::UpdateReferencesInExternalStringTable(
    ExternalStringTableUpdaterCallback updater_func) {

  // Update old space string references.
  if (external_string_table_.old_space_strings_.length() > 0) {
    Object** start = &external_string_table_.old_space_strings_[0];
    Object** end = start + external_string_table_.old_space_strings_.length();
    for (Object** p = start; p < end; ++p) *p = updater_func(this, p);
  }

  UpdateNewSpaceReferencesInExternalStringTable(updater_func);
}


void Heap::ProcessWeakReferences(WeakObjectRetainer* retainer) {
  // We don't record weak slots during marking or scavenges.
  // Instead we do it once when we complete mark-compact cycle.
  // Note that write barrier has no effect if we are already in the middle of
  // compacting mark-sweep cycle and we have to record slots manually.
  bool record_slots =
      gc_state() == MARK_COMPACT &&
      mark_compact_collector()->is_compacting();
  ProcessArrayBuffers(retainer, record_slots);
  ProcessNativeContexts(retainer, record_slots);
  // TODO(mvstanton): AllocationSites only need to be processed during
  // MARK_COMPACT, as they live in old space. Verify and address.
  ProcessAllocationSites(retainer, record_slots);
}

void Heap::ProcessNativeContexts(WeakObjectRetainer* retainer,
                                 bool record_slots) {
  Object* head =
      VisitWeakList<Context>(
          this, native_contexts_list(), retainer, record_slots);
  // Update the head of the list of contexts.
  native_contexts_list_ = head;
}


void Heap::ProcessArrayBuffers(WeakObjectRetainer* retainer,
                               bool record_slots) {
  Object* array_buffer_obj =
      VisitWeakList<JSArrayBuffer>(this,
                                   array_buffers_list(),
                                   retainer, record_slots);
  set_array_buffers_list(array_buffer_obj);
}


void Heap::TearDownArrayBuffers() {
  Object* undefined = undefined_value();
  for (Object* o = array_buffers_list(); o != undefined;) {
    JSArrayBuffer* buffer = JSArrayBuffer::cast(o);
    Runtime::FreeArrayBuffer(isolate(), buffer);
    o = buffer->weak_next();
  }
  array_buffers_list_ = undefined;
}


void Heap::ProcessAllocationSites(WeakObjectRetainer* retainer,
                                  bool record_slots) {
  Object* allocation_site_obj =
      VisitWeakList<AllocationSite>(this,
                                    allocation_sites_list(),
                                    retainer, record_slots);
  set_allocation_sites_list(allocation_site_obj);
}


void Heap::ResetAllAllocationSitesDependentCode(PretenureFlag flag) {
  DisallowHeapAllocation no_allocation_scope;
  Object* cur = allocation_sites_list();
  bool marked = false;
  while (cur->IsAllocationSite()) {
    AllocationSite* casted = AllocationSite::cast(cur);
    if (casted->GetPretenureMode() == flag) {
      casted->ResetPretenureDecision();
      casted->set_deopt_dependent_code(true);
      marked = true;
    }
    cur = casted->weak_next();
  }
  if (marked) isolate_->stack_guard()->DeoptMarkedAllocationSites();
}


void Heap::EvaluateOldSpaceLocalPretenuring(
    uint64_t size_of_objects_before_gc) {
  uint64_t size_of_objects_after_gc = SizeOfObjects();
  double old_generation_survival_rate =
      (static_cast<double>(size_of_objects_after_gc) * 100) /
          static_cast<double>(size_of_objects_before_gc);

  if (old_generation_survival_rate < kOldSurvivalRateLowThreshold) {
    // Too many objects died in the old generation, pretenuring of wrong
    // allocation sites may be the cause for that. We have to deopt all
    // dependent code registered in the allocation sites to re-evaluate
    // our pretenuring decisions.
    ResetAllAllocationSitesDependentCode(TENURED);
    if (FLAG_trace_pretenuring) {
      PrintF("Deopt all allocation sites dependent code due to low survival "
             "rate in the old generation %f\n", old_generation_survival_rate);
    }
  }
}


void Heap::VisitExternalResources(v8::ExternalResourceVisitor* visitor) {
  DisallowHeapAllocation no_allocation;
  // All external strings are listed in the external string table.

  class ExternalStringTableVisitorAdapter : public ObjectVisitor {
   public:
    explicit ExternalStringTableVisitorAdapter(
        v8::ExternalResourceVisitor* visitor) : visitor_(visitor) {}
    virtual void VisitPointers(Object** start, Object** end) {
      for (Object** p = start; p < end; p++) {
        ASSERT((*p)->IsExternalString());
        visitor_->VisitExternalString(Utils::ToLocal(
            Handle<String>(String::cast(*p))));
      }
    }
   private:
    v8::ExternalResourceVisitor* visitor_;
  } external_string_table_visitor(visitor);

  external_string_table_.Iterate(&external_string_table_visitor);
}


class NewSpaceScavenger : public StaticNewSpaceVisitor<NewSpaceScavenger> {
 public:
  static inline void VisitPointer(Heap* heap, Object** p) {
    Object* object = *p;
    if (!heap->InNewSpace(object)) return;
    Heap::ScavengeObject(reinterpret_cast<HeapObject**>(p),
                         reinterpret_cast<HeapObject*>(object));
  }
};


Address Heap::DoScavenge(ObjectVisitor* scavenge_visitor,
                         Address new_space_front) {
  do {
    SemiSpace::AssertValidRange(new_space_front, new_space_.top());
    // The addresses new_space_front and new_space_.top() define a
    // queue of unprocessed copied objects.  Process them until the
    // queue is empty.
    while (new_space_front != new_space_.top()) {
      if (!NewSpacePage::IsAtEnd(new_space_front)) {
        HeapObject* object = HeapObject::FromAddress(new_space_front);
        new_space_front +=
          NewSpaceScavenger::IterateBody(object->map(), object);
      } else {
        new_space_front =
            NewSpacePage::FromLimit(new_space_front)->next_page()->area_start();
      }
    }

    // Promote and process all the to-be-promoted objects.
    {
      StoreBufferRebuildScope scope(this,
                                    store_buffer(),
                                    &ScavengeStoreBufferCallback);
      while (!promotion_queue()->is_empty()) {
        HeapObject* target;
        int size;
        promotion_queue()->remove(&target, &size);

        // Promoted object might be already partially visited
        // during old space pointer iteration. Thus we search specificly
        // for pointers to from semispace instead of looking for pointers
        // to new space.
        ASSERT(!target->IsMap());
        IterateAndMarkPointersToFromSpace(target->address(),
                                          target->address() + size,
                                          &ScavengeObject);
      }
    }

    // Take another spin if there are now unswept objects in new space
    // (there are currently no more unswept promoted objects).
  } while (new_space_front != new_space_.top());

  return new_space_front;
}


STATIC_ASSERT((FixedDoubleArray::kHeaderSize & kDoubleAlignmentMask) == 0);
STATIC_ASSERT((ConstantPoolArray::kHeaderSize & kDoubleAlignmentMask) == 0);


INLINE(static HeapObject* EnsureDoubleAligned(Heap* heap,
                                              HeapObject* object,
                                              int size));

static HeapObject* EnsureDoubleAligned(Heap* heap,
                                       HeapObject* object,
                                       int size) {
  if ((OffsetFrom(object->address()) & kDoubleAlignmentMask) != 0) {
    heap->CreateFillerObjectAt(object->address(), kPointerSize);
    return HeapObject::FromAddress(object->address() + kPointerSize);
  } else {
    heap->CreateFillerObjectAt(object->address() + size - kPointerSize,
                               kPointerSize);
    return object;
  }
}


enum LoggingAndProfiling {
  LOGGING_AND_PROFILING_ENABLED,
  LOGGING_AND_PROFILING_DISABLED
};


enum MarksHandling { TRANSFER_MARKS, IGNORE_MARKS };


template<MarksHandling marks_handling,
         LoggingAndProfiling logging_and_profiling_mode>
class ScavengingVisitor : public StaticVisitorBase {
 public:
  static void Initialize() {
    table_.Register(kVisitSeqOneByteString, &EvacuateSeqOneByteString);
    table_.Register(kVisitSeqTwoByteString, &EvacuateSeqTwoByteString);
    table_.Register(kVisitShortcutCandidate, &EvacuateShortcutCandidate);
    table_.Register(kVisitByteArray, &EvacuateByteArray);
    table_.Register(kVisitFixedArray, &EvacuateFixedArray);
    table_.Register(kVisitFixedDoubleArray, &EvacuateFixedDoubleArray);
    table_.Register(kVisitFixedTypedArray, &EvacuateFixedTypedArray);
    table_.Register(kVisitFixedFloat64Array, &EvacuateFixedFloat64Array);

    table_.Register(kVisitNativeContext,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                        template VisitSpecialized<Context::kSize>);

    table_.Register(kVisitConsString,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                        template VisitSpecialized<ConsString::kSize>);

    table_.Register(kVisitSlicedString,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                        template VisitSpecialized<SlicedString::kSize>);

    table_.Register(kVisitSymbol,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                        template VisitSpecialized<Symbol::kSize>);

    table_.Register(kVisitSharedFunctionInfo,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                        template VisitSpecialized<SharedFunctionInfo::kSize>);

    table_.Register(kVisitJSWeakMap,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                    Visit);

    table_.Register(kVisitJSWeakSet,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                    Visit);

    table_.Register(kVisitJSArrayBuffer,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                    Visit);

    table_.Register(kVisitJSTypedArray,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                    Visit);

    table_.Register(kVisitJSDataView,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                    Visit);

    table_.Register(kVisitJSRegExp,
                    &ObjectEvacuationStrategy<POINTER_OBJECT>::
                    Visit);

    if (marks_handling == IGNORE_MARKS) {
      table_.Register(kVisitJSFunction,
                      &ObjectEvacuationStrategy<POINTER_OBJECT>::
                          template VisitSpecialized<JSFunction::kSize>);
    } else {
      table_.Register(kVisitJSFunction, &EvacuateJSFunction);
    }

    table_.RegisterSpecializations<ObjectEvacuationStrategy<DATA_OBJECT>,
                                   kVisitDataObject,
                                   kVisitDataObjectGeneric>();

    table_.RegisterSpecializations<ObjectEvacuationStrategy<POINTER_OBJECT>,
                                   kVisitJSObject,
                                   kVisitJSObjectGeneric>();

    table_.RegisterSpecializations<ObjectEvacuationStrategy<POINTER_OBJECT>,
                                   kVisitStruct,
                                   kVisitStructGeneric>();
  }

  static VisitorDispatchTable<ScavengingCallback>* GetTable() {
    return &table_;
  }

 private:
  enum ObjectContents  { DATA_OBJECT, POINTER_OBJECT };

  static void RecordCopiedObject(Heap* heap, HeapObject* obj) {
    bool should_record = false;
#ifdef DEBUG
    should_record = FLAG_heap_stats;
#endif
    should_record = should_record || FLAG_log_gc;
    if (should_record) {
      if (heap->new_space()->Contains(obj)) {
        heap->new_space()->RecordAllocation(obj);
      } else {
        heap->new_space()->RecordPromotion(obj);
      }
    }
  }

  // Helper function used by CopyObject to copy a source object to an
  // allocated target object and update the forwarding pointer in the source
  // object.  Returns the target object.
  INLINE(static void MigrateObject(Heap* heap,
                                   HeapObject* source,
                                   HeapObject* target,
                                   int size)) {
    // Copy the content of source to target.
    heap->CopyBlock(target->address(), source->address(), size);

    // Set the forwarding address.
    source->set_map_word(MapWord::FromForwardingAddress(target));

    if (logging_and_profiling_mode == LOGGING_AND_PROFILING_ENABLED) {
      // Update NewSpace stats if necessary.
      RecordCopiedObject(heap, target);
      Isolate* isolate = heap->isolate();
      HeapProfiler* heap_profiler = isolate->heap_profiler();
      if (heap_profiler->is_tracking_object_moves()) {
        heap_profiler->ObjectMoveEvent(source->address(), target->address(),
                                       size);
      }
      if (isolate->logger()->is_logging_code_events() ||
          isolate->cpu_profiler()->is_profiling()) {
        if (target->IsSharedFunctionInfo()) {
          PROFILE(isolate, SharedFunctionInfoMoveEvent(
              source->address(), target->address()));
        }
      }
    }

    if (marks_handling == TRANSFER_MARKS) {
      if (Marking::TransferColor(source, target)) {
        MemoryChunk::IncrementLiveBytesFromGC(target->address(), size);
      }
    }
  }


  template<ObjectContents object_contents, int alignment>
  static inline void EvacuateObject(Map* map,
                                    HeapObject** slot,
                                    HeapObject* object,
                                    int object_size) {
    SLOW_ASSERT(object_size <= Page::kMaxRegularHeapObjectSize);
    SLOW_ASSERT(object->Size() == object_size);

    int allocation_size = object_size;
    if (alignment != kObjectAlignment) {
      ASSERT(alignment == kDoubleAlignment);
      allocation_size += kPointerSize;
    }

    Heap* heap = map->GetHeap();
    if (heap->ShouldBePromoted(object->address(), object_size)) {
      AllocationResult allocation;

      if (object_contents == DATA_OBJECT) {
        ASSERT(heap->AllowedToBeMigrated(object, OLD_DATA_SPACE));
        allocation = heap->old_data_space()->AllocateRaw(allocation_size);
      } else {
        ASSERT(heap->AllowedToBeMigrated(object, OLD_POINTER_SPACE));
        allocation = heap->old_pointer_space()->AllocateRaw(allocation_size);
      }

      HeapObject* target = NULL;  // Initialization to please compiler.
      if (allocation.To(&target)) {
        if (alignment != kObjectAlignment) {
          target = EnsureDoubleAligned(heap, target, allocation_size);
        }

        // Order is important: slot might be inside of the target if target
        // was allocated over a dead object and slot comes from the store
        // buffer.
        *slot = target;
        MigrateObject(heap, object, target, object_size);

        if (object_contents == POINTER_OBJECT) {
          if (map->instance_type() == JS_FUNCTION_TYPE) {
            heap->promotion_queue()->insert(
                target, JSFunction::kNonWeakFieldsEndOffset);
          } else {
            heap->promotion_queue()->insert(target, object_size);
          }
        }

        heap->tracer()->increment_promoted_objects_size(object_size);
        return;
      }
    }
    ASSERT(heap->AllowedToBeMigrated(object, NEW_SPACE));
    AllocationResult allocation =
        heap->new_space()->AllocateRaw(allocation_size);
    heap->promotion_queue()->SetNewLimit(heap->new_space()->top());
    HeapObject* target = HeapObject::cast(allocation.ToObjectChecked());

    if (alignment != kObjectAlignment) {
      target = EnsureDoubleAligned(heap, target, allocation_size);
    }

    // Order is important: slot might be inside of the target if target
    // was allocated over a dead object and slot comes from the store
    // buffer.
    *slot = target;
    MigrateObject(heap, object, target, object_size);
    return;
  }


  static inline void EvacuateJSFunction(Map* map,
                                        HeapObject** slot,
                                        HeapObject* object) {
    ObjectEvacuationStrategy<POINTER_OBJECT>::
        template VisitSpecialized<JSFunction::kSize>(map, slot, object);

    HeapObject* target = *slot;
    MarkBit mark_bit = Marking::MarkBitFrom(target);
    if (Marking::IsBlack(mark_bit)) {
      // This object is black and it might not be rescanned by marker.
      // We should explicitly record code entry slot for compaction because
      // promotion queue processing (IterateAndMarkPointersToFromSpace) will
      // miss it as it is not HeapObject-tagged.
      Address code_entry_slot =
          target->address() + JSFunction::kCodeEntryOffset;
      Code* code = Code::cast(Code::GetObjectFromEntryAddress(code_entry_slot));
      map->GetHeap()->mark_compact_collector()->
          RecordCodeEntrySlot(code_entry_slot, code);
    }
  }


  static inline void EvacuateFixedArray(Map* map,
                                        HeapObject** slot,
                                        HeapObject* object) {
    int object_size = FixedArray::BodyDescriptor::SizeOf(map, object);
    EvacuateObject<POINTER_OBJECT, kObjectAlignment>(
        map, slot, object, object_size);
  }


  static inline void EvacuateFixedDoubleArray(Map* map,
                                              HeapObject** slot,
                                              HeapObject* object) {
    int length = reinterpret_cast<FixedDoubleArray*>(object)->length();
    int object_size = FixedDoubleArray::SizeFor(length);
    EvacuateObject<DATA_OBJECT, kDoubleAlignment>(
        map, slot, object, object_size);
  }


  static inline void EvacuateFixedTypedArray(Map* map,
                                             HeapObject** slot,
                                             HeapObject* object) {
    int object_size = reinterpret_cast<FixedTypedArrayBase*>(object)->size();
    EvacuateObject<DATA_OBJECT, kObjectAlignment>(
        map, slot, object, object_size);
  }


  static inline void EvacuateFixedFloat64Array(Map* map,
                                               HeapObject** slot,
                                               HeapObject* object) {
    int object_size = reinterpret_cast<FixedFloat64Array*>(object)->size();
    EvacuateObject<DATA_OBJECT, kDoubleAlignment>(
        map, slot, object, object_size);
  }


  static inline void EvacuateByteArray(Map* map,
                                       HeapObject** slot,
                                       HeapObject* object) {
    int object_size = reinterpret_cast<ByteArray*>(object)->ByteArraySize();
    EvacuateObject<DATA_OBJECT, kObjectAlignment>(
        map, slot, object, object_size);
  }


  static inline void EvacuateSeqOneByteString(Map* map,
                                            HeapObject** slot,
                                            HeapObject* object) {
    int object_size = SeqOneByteString::cast(object)->
        SeqOneByteStringSize(map->instance_type());
    EvacuateObject<DATA_OBJECT, kObjectAlignment>(
        map, slot, object, object_size);
  }


  static inline void EvacuateSeqTwoByteString(Map* map,
                                              HeapObject** slot,
                                              HeapObject* object) {
    int object_size = SeqTwoByteString::cast(object)->
        SeqTwoByteStringSize(map->instance_type());
    EvacuateObject<DATA_OBJECT, kObjectAlignment>(
        map, slot, object, object_size);
  }


  static inline bool IsShortcutCandidate(int type) {
    return ((type & kShortcutTypeMask) == kShortcutTypeTag);
  }

  static inline void EvacuateShortcutCandidate(Map* map,
                                               HeapObject** slot,
                                               HeapObject* object) {
    ASSERT(IsShortcutCandidate(map->instance_type()));

    Heap* heap = map->GetHeap();

    if (marks_handling == IGNORE_MARKS &&
        ConsString::cast(object)->unchecked_second() ==
        heap->empty_string()) {
      HeapObject* first =
          HeapObject::cast(ConsString::cast(object)->unchecked_first());

      *slot = first;

      if (!heap->InNewSpace(first)) {
        object->set_map_word(MapWord::FromForwardingAddress(first));
        return;
      }

      MapWord first_word = first->map_word();
      if (first_word.IsForwardingAddress()) {
        HeapObject* target = first_word.ToForwardingAddress();

        *slot = target;
        object->set_map_word(MapWord::FromForwardingAddress(target));
        return;
      }

      heap->DoScavengeObject(first->map(), slot, first);
      object->set_map_word(MapWord::FromForwardingAddress(*slot));
      return;
    }

    int object_size = ConsString::kSize;
    EvacuateObject<POINTER_OBJECT, kObjectAlignment>(
        map, slot, object, object_size);
  }

  template<ObjectContents object_contents>
  class ObjectEvacuationStrategy {
   public:
    template<int object_size>
    static inline void VisitSpecialized(Map* map,
                                        HeapObject** slot,
                                        HeapObject* object) {
      EvacuateObject<object_contents, kObjectAlignment>(
          map, slot, object, object_size);
    }

    static inline void Visit(Map* map,
                             HeapObject** slot,
                             HeapObject* object) {
      int object_size = map->instance_size();
      EvacuateObject<object_contents, kObjectAlignment>(
          map, slot, object, object_size);
    }
  };

  static VisitorDispatchTable<ScavengingCallback> table_;
};


template<MarksHandling marks_handling,
         LoggingAndProfiling logging_and_profiling_mode>
VisitorDispatchTable<ScavengingCallback>
    ScavengingVisitor<marks_handling, logging_and_profiling_mode>::table_;


static void InitializeScavengingVisitorsTables() {
  ScavengingVisitor<TRANSFER_MARKS,
                    LOGGING_AND_PROFILING_DISABLED>::Initialize();
  ScavengingVisitor<IGNORE_MARKS, LOGGING_AND_PROFILING_DISABLED>::Initialize();
  ScavengingVisitor<TRANSFER_MARKS,
                    LOGGING_AND_PROFILING_ENABLED>::Initialize();
  ScavengingVisitor<IGNORE_MARKS, LOGGING_AND_PROFILING_ENABLED>::Initialize();
}


void Heap::SelectScavengingVisitorsTable() {
  bool logging_and_profiling =
      isolate()->logger()->is_logging() ||
      isolate()->cpu_profiler()->is_profiling() ||
      (isolate()->heap_profiler() != NULL &&
       isolate()->heap_profiler()->is_tracking_object_moves());

  if (!incremental_marking()->IsMarking()) {
    if (!logging_and_profiling) {
      scavenging_visitors_table_.CopyFrom(
          ScavengingVisitor<IGNORE_MARKS,
                            LOGGING_AND_PROFILING_DISABLED>::GetTable());
    } else {
      scavenging_visitors_table_.CopyFrom(
          ScavengingVisitor<IGNORE_MARKS,
                            LOGGING_AND_PROFILING_ENABLED>::GetTable());
    }
  } else {
    if (!logging_and_profiling) {
      scavenging_visitors_table_.CopyFrom(
          ScavengingVisitor<TRANSFER_MARKS,
                            LOGGING_AND_PROFILING_DISABLED>::GetTable());
    } else {
      scavenging_visitors_table_.CopyFrom(
          ScavengingVisitor<TRANSFER_MARKS,
                            LOGGING_AND_PROFILING_ENABLED>::GetTable());
    }

    if (incremental_marking()->IsCompacting()) {
      // When compacting forbid short-circuiting of cons-strings.
      // Scavenging code relies on the fact that new space object
      // can't be evacuated into evacuation candidate but
      // short-circuiting violates this assumption.
      scavenging_visitors_table_.Register(
          StaticVisitorBase::kVisitShortcutCandidate,
          scavenging_visitors_table_.GetVisitorById(
              StaticVisitorBase::kVisitConsString));
    }
  }
}


void Heap::ScavengeObjectSlow(HeapObject** p, HeapObject* object) {
  SLOW_ASSERT(object->GetIsolate()->heap()->InFromSpace(object));
  MapWord first_word = object->map_word();
  SLOW_ASSERT(!first_word.IsForwardingAddress());
  Map* map = first_word.ToMap();
  map->GetHeap()->DoScavengeObject(map, p, object);
}


AllocationResult Heap::AllocatePartialMap(InstanceType instance_type,
                                          int instance_size) {
  Object* result;
  AllocationResult allocation = AllocateRaw(Map::kSize, MAP_SPACE, MAP_SPACE);
  if (!allocation.To(&result)) return allocation;

  // Map::cast cannot be used due to uninitialized map field.
  reinterpret_cast<Map*>(result)->set_map(raw_unchecked_meta_map());
  reinterpret_cast<Map*>(result)->set_instance_type(instance_type);
  reinterpret_cast<Map*>(result)->set_instance_size(instance_size);
  reinterpret_cast<Map*>(result)->set_visitor_id(
        StaticVisitorBase::GetVisitorId(instance_type, instance_size));
  reinterpret_cast<Map*>(result)->set_inobject_properties(0);
  reinterpret_cast<Map*>(result)->set_pre_allocated_property_fields(0);
  reinterpret_cast<Map*>(result)->set_unused_property_fields(0);
  reinterpret_cast<Map*>(result)->set_bit_field(0);
  reinterpret_cast<Map*>(result)->set_bit_field2(0);
  int bit_field3 = Map::EnumLengthBits::encode(kInvalidEnumCacheSentinel) |
                   Map::OwnsDescriptors::encode(true);
  reinterpret_cast<Map*>(result)->set_bit_field3(bit_field3);
  return result;
}


AllocationResult Heap::AllocateMap(InstanceType instance_type,
                                   int instance_size,
                                   ElementsKind elements_kind) {
  HeapObject* result;
  AllocationResult allocation = AllocateRaw(Map::kSize, MAP_SPACE, MAP_SPACE);
  if (!allocation.To(&result)) return allocation;

  result->set_map_no_write_barrier(meta_map());
  Map* map = Map::cast(result);
  map->set_instance_type(instance_type);
  map->set_visitor_id(
      StaticVisitorBase::GetVisitorId(instance_type, instance_size));
  map->set_prototype(null_value(), SKIP_WRITE_BARRIER);
  map->set_constructor(null_value(), SKIP_WRITE_BARRIER);
  map->set_instance_size(instance_size);
  map->set_inobject_properties(0);
  map->set_pre_allocated_property_fields(0);
  map->set_code_cache(empty_fixed_array(), SKIP_WRITE_BARRIER);
  map->set_dependent_code(DependentCode::cast(empty_fixed_array()),
                          SKIP_WRITE_BARRIER);
  map->init_back_pointer(undefined_value());
  map->set_unused_property_fields(0);
  map->set_instance_descriptors(empty_descriptor_array());
  map->set_bit_field(0);
  map->set_bit_field2(1 << Map::kIsExtensible);
  int bit_field3 = Map::EnumLengthBits::encode(kInvalidEnumCacheSentinel) |
                   Map::OwnsDescriptors::encode(true);
  map->set_bit_field3(bit_field3);
  map->set_elements_kind(elements_kind);

  return map;
}


AllocationResult Heap::AllocateFillerObject(int size,
                                            bool double_align,
                                            AllocationSpace space) {
  HeapObject* obj;
  { AllocationResult allocation = AllocateRaw(size, space, space);
    if (!allocation.To(&obj)) return allocation;
  }
#ifdef DEBUG
  MemoryChunk* chunk = MemoryChunk::FromAddress(obj->address());
  ASSERT(chunk->owner()->identity() == space);
#endif
  CreateFillerObjectAt(obj->address(), size);
  return obj;
}


const Heap::StringTypeTable Heap::string_type_table[] = {
#define STRING_TYPE_ELEMENT(type, size, name, camel_name)                      \
  {type, size, k##camel_name##MapRootIndex},
  STRING_TYPE_LIST(STRING_TYPE_ELEMENT)
#undef STRING_TYPE_ELEMENT
};


const Heap::ConstantStringTable Heap::constant_string_table[] = {
#define CONSTANT_STRING_ELEMENT(name, contents)                                \
  {contents, k##name##RootIndex},
  INTERNALIZED_STRING_LIST(CONSTANT_STRING_ELEMENT)
#undef CONSTANT_STRING_ELEMENT
};


const Heap::StructTable Heap::struct_table[] = {
#define STRUCT_TABLE_ELEMENT(NAME, Name, name)                                 \
  { NAME##_TYPE, Name::kSize, k##Name##MapRootIndex },
  STRUCT_LIST(STRUCT_TABLE_ELEMENT)
#undef STRUCT_TABLE_ELEMENT
};


bool Heap::CreateInitialMaps() {
  HeapObject* obj;
  { AllocationResult allocation = AllocatePartialMap(MAP_TYPE, Map::kSize);
    if (!allocation.To(&obj)) return false;
  }
  // Map::cast cannot be used due to uninitialized map field.
  Map* new_meta_map = reinterpret_cast<Map*>(obj);
  set_meta_map(new_meta_map);
  new_meta_map->set_map(new_meta_map);

  { // Partial map allocation
#define ALLOCATE_PARTIAL_MAP(instance_type, size, field_name)                  \
    { Map* map;                                                                \
      if (!AllocatePartialMap((instance_type), (size)).To(&map)) return false; \
      set_##field_name##_map(map);                                             \
    }

    ALLOCATE_PARTIAL_MAP(FIXED_ARRAY_TYPE, kVariableSizeSentinel, fixed_array);
    ALLOCATE_PARTIAL_MAP(ODDBALL_TYPE, Oddball::kSize, undefined);
    ALLOCATE_PARTIAL_MAP(ODDBALL_TYPE, Oddball::kSize, null);
    ALLOCATE_PARTIAL_MAP(CONSTANT_POOL_ARRAY_TYPE, kVariableSizeSentinel,
                         constant_pool_array);

#undef ALLOCATE_PARTIAL_MAP
  }

  // Allocate the empty array.
  { AllocationResult allocation = AllocateEmptyFixedArray();
    if (!allocation.To(&obj)) return false;
  }
  set_empty_fixed_array(FixedArray::cast(obj));

  { AllocationResult allocation = Allocate(null_map(), OLD_POINTER_SPACE);
    if (!allocation.To(&obj)) return false;
  }
  set_null_value(Oddball::cast(obj));
  Oddball::cast(obj)->set_kind(Oddball::kNull);

  { AllocationResult allocation = Allocate(undefined_map(), OLD_POINTER_SPACE);
    if (!allocation.To(&obj)) return false;
  }
  set_undefined_value(Oddball::cast(obj));
  Oddball::cast(obj)->set_kind(Oddball::kUndefined);
  ASSERT(!InNewSpace(undefined_value()));

  // Set preliminary exception sentinel value before actually initializing it.
  set_exception(null_value());

  // Allocate the empty descriptor array.
  { AllocationResult allocation = AllocateEmptyFixedArray();
    if (!allocation.To(&obj)) return false;
  }
  set_empty_descriptor_array(DescriptorArray::cast(obj));

  // Allocate the constant pool array.
  { AllocationResult allocation = AllocateEmptyConstantPoolArray();
    if (!allocation.To(&obj)) return false;
  }
  set_empty_constant_pool_array(ConstantPoolArray::cast(obj));

  // Fix the instance_descriptors for the existing maps.
  meta_map()->set_code_cache(empty_fixed_array());
  meta_map()->set_dependent_code(DependentCode::cast(empty_fixed_array()));
  meta_map()->init_back_pointer(undefined_value());
  meta_map()->set_instance_descriptors(empty_descriptor_array());

  fixed_array_map()->set_code_cache(empty_fixed_array());
  fixed_array_map()->set_dependent_code(
      DependentCode::cast(empty_fixed_array()));
  fixed_array_map()->init_back_pointer(undefined_value());
  fixed_array_map()->set_instance_descriptors(empty_descriptor_array());

  undefined_map()->set_code_cache(empty_fixed_array());
  undefined_map()->set_dependent_code(DependentCode::cast(empty_fixed_array()));
  undefined_map()->init_back_pointer(undefined_value());
  undefined_map()->set_instance_descriptors(empty_descriptor_array());

  null_map()->set_code_cache(empty_fixed_array());
  null_map()->set_dependent_code(DependentCode::cast(empty_fixed_array()));
  null_map()->init_back_pointer(undefined_value());
  null_map()->set_instance_descriptors(empty_descriptor_array());

  constant_pool_array_map()->set_code_cache(empty_fixed_array());
  constant_pool_array_map()->set_dependent_code(
      DependentCode::cast(empty_fixed_array()));
  constant_pool_array_map()->init_back_pointer(undefined_value());
  constant_pool_array_map()->set_instance_descriptors(empty_descriptor_array());

  // Fix prototype object for existing maps.
  meta_map()->set_prototype(null_value());
  meta_map()->set_constructor(null_value());

  fixed_array_map()->set_prototype(null_value());
  fixed_array_map()->set_constructor(null_value());

  undefined_map()->set_prototype(null_value());
  undefined_map()->set_constructor(null_value());

  null_map()->set_prototype(null_value());
  null_map()->set_constructor(null_value());

  constant_pool_array_map()->set_prototype(null_value());
  constant_pool_array_map()->set_constructor(null_value());

  { // Map allocation
#define ALLOCATE_MAP(instance_type, size, field_name)                          \
    { Map* map;                                                                \
      if (!AllocateMap((instance_type), size).To(&map)) return false;          \
      set_##field_name##_map(map);                                             \
    }

#define ALLOCATE_VARSIZE_MAP(instance_type, field_name)                        \
    ALLOCATE_MAP(instance_type, kVariableSizeSentinel, field_name)

    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, fixed_cow_array)
    ASSERT(fixed_array_map() != fixed_cow_array_map());

    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, scope_info)
    ALLOCATE_MAP(HEAP_NUMBER_TYPE, HeapNumber::kSize, heap_number)
    ALLOCATE_MAP(SYMBOL_TYPE, Symbol::kSize, symbol)
    ALLOCATE_MAP(FOREIGN_TYPE, Foreign::kSize, foreign)

    ALLOCATE_MAP(ODDBALL_TYPE, Oddball::kSize, the_hole);
    ALLOCATE_MAP(ODDBALL_TYPE, Oddball::kSize, boolean);
    ALLOCATE_MAP(ODDBALL_TYPE, Oddball::kSize, uninitialized);
    ALLOCATE_MAP(ODDBALL_TYPE, Oddball::kSize, arguments_marker);
    ALLOCATE_MAP(ODDBALL_TYPE, Oddball::kSize, no_interceptor_result_sentinel);
    ALLOCATE_MAP(ODDBALL_TYPE, Oddball::kSize, exception);
    ALLOCATE_MAP(ODDBALL_TYPE, Oddball::kSize, termination_exception);

    for (unsigned i = 0; i < ARRAY_SIZE(string_type_table); i++) {
      const StringTypeTable& entry = string_type_table[i];
      { AllocationResult allocation = AllocateMap(entry.type, entry.size);
        if (!allocation.To(&obj)) return false;
      }
      // Mark cons string maps as unstable, because their objects can change
      // maps during GC.
      Map* map = Map::cast(obj);
      if (StringShape(entry.type).IsCons()) map->mark_unstable();
      roots_[entry.index] = map;
    }

    ALLOCATE_VARSIZE_MAP(STRING_TYPE, undetectable_string)
    undetectable_string_map()->set_is_undetectable();

    ALLOCATE_VARSIZE_MAP(ASCII_STRING_TYPE, undetectable_ascii_string);
    undetectable_ascii_string_map()->set_is_undetectable();

    ALLOCATE_VARSIZE_MAP(FIXED_DOUBLE_ARRAY_TYPE, fixed_double_array)
    ALLOCATE_VARSIZE_MAP(BYTE_ARRAY_TYPE, byte_array)
    ALLOCATE_VARSIZE_MAP(FREE_SPACE_TYPE, free_space)

#define ALLOCATE_EXTERNAL_ARRAY_MAP(Type, type, TYPE, ctype, size)            \
    ALLOCATE_MAP(EXTERNAL_##TYPE##_ARRAY_TYPE, ExternalArray::kAlignedSize,   \
        external_##type##_array)

     TYPED_ARRAYS(ALLOCATE_EXTERNAL_ARRAY_MAP)
#undef ALLOCATE_EXTERNAL_ARRAY_MAP

#define ALLOCATE_FIXED_TYPED_ARRAY_MAP(Type, type, TYPE, ctype, size)         \
    ALLOCATE_VARSIZE_MAP(FIXED_##TYPE##_ARRAY_TYPE,                           \
        fixed_##type##_array)

     TYPED_ARRAYS(ALLOCATE_FIXED_TYPED_ARRAY_MAP)
#undef ALLOCATE_FIXED_TYPED_ARRAY_MAP

    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, sloppy_arguments_elements)

    ALLOCATE_VARSIZE_MAP(CODE_TYPE, code)

    ALLOCATE_MAP(CELL_TYPE, Cell::kSize, cell)
    ALLOCATE_MAP(PROPERTY_CELL_TYPE, PropertyCell::kSize, global_property_cell)
    ALLOCATE_MAP(FILLER_TYPE, kPointerSize, one_pointer_filler)
    ALLOCATE_MAP(FILLER_TYPE, 2 * kPointerSize, two_pointer_filler)


    for (unsigned i = 0; i < ARRAY_SIZE(struct_table); i++) {
      const StructTable& entry = struct_table[i];
      Map* map;
      if (!AllocateMap(entry.type, entry.size).To(&map))
        return false;
      roots_[entry.index] = map;
    }

    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, hash_table)
    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, ordered_hash_table)

    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, function_context)
    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, catch_context)
    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, with_context)
    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, block_context)
    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, module_context)
    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, global_context)

    ALLOCATE_VARSIZE_MAP(FIXED_ARRAY_TYPE, native_context)
    native_context_map()->set_dictionary_map(true);
    native_context_map()->set_visitor_id(
        StaticVisitorBase::kVisitNativeContext);

    ALLOCATE_MAP(SHARED_FUNCTION_INFO_TYPE, SharedFunctionInfo::kAlignedSize,
        shared_function_info)

    ALLOCATE_MAP(JS_MESSAGE_OBJECT_TYPE, JSMessageObject::kSize,
        message_object)
    ALLOCATE_MAP(JS_OBJECT_TYPE, JSObject::kHeaderSize + kPointerSize,
        external)
    external_map()->set_is_extensible(false);
#undef ALLOCATE_VARSIZE_MAP
#undef ALLOCATE_MAP
  }

  { // Empty arrays
    { ByteArray* byte_array;
      if (!AllocateByteArray(0, TENURED).To(&byte_array)) return false;
      set_empty_byte_array(byte_array);
    }

#define ALLOCATE_EMPTY_EXTERNAL_ARRAY(Type, type, TYPE, ctype, size)           \
    { ExternalArray* obj;                                                      \
      if (!AllocateEmptyExternalArray(kExternal##Type##Array).To(&obj))        \
          return false;                                                        \
      set_empty_external_##type##_array(obj);                                  \
    }

    TYPED_ARRAYS(ALLOCATE_EMPTY_EXTERNAL_ARRAY)
#undef ALLOCATE_EMPTY_EXTERNAL_ARRAY

#define ALLOCATE_EMPTY_FIXED_TYPED_ARRAY(Type, type, TYPE, ctype, size)        \
    { FixedTypedArrayBase* obj;                                                \
      if (!AllocateEmptyFixedTypedArray(kExternal##Type##Array).To(&obj))      \
          return false;                                                        \
      set_empty_fixed_##type##_array(obj);                                     \
    }

    TYPED_ARRAYS(ALLOCATE_EMPTY_FIXED_TYPED_ARRAY)
#undef ALLOCATE_EMPTY_FIXED_TYPED_ARRAY
  }
  ASSERT(!InNewSpace(empty_fixed_array()));
  return true;
}


AllocationResult Heap::AllocateHeapNumber(double value,
                                          PretenureFlag pretenure) {
  // Statically ensure that it is safe to allocate heap numbers in paged
  // spaces.
  int size = HeapNumber::kSize;
  STATIC_ASSERT(HeapNumber::kSize <= Page::kMaxRegularHeapObjectSize);

  AllocationSpace space = SelectSpace(size, OLD_DATA_SPACE, pretenure);

  HeapObject* result;
  { AllocationResult allocation = AllocateRaw(size, space, OLD_DATA_SPACE);
    if (!allocation.To(&result)) return allocation;
  }

  result->set_map_no_write_barrier(heap_number_map());
  HeapNumber::cast(result)->set_value(value);
  return result;
}


AllocationResult Heap::AllocateCell(Object* value) {
  int size = Cell::kSize;
  STATIC_ASSERT(Cell::kSize <= Page::kMaxRegularHeapObjectSize);

  HeapObject* result;
  { AllocationResult allocation = AllocateRaw(size, CELL_SPACE, CELL_SPACE);
    if (!allocation.To(&result)) return allocation;
  }
  result->set_map_no_write_barrier(cell_map());
  Cell::cast(result)->set_value(value);
  return result;
}


AllocationResult Heap::AllocatePropertyCell() {
  int size = PropertyCell::kSize;
  STATIC_ASSERT(PropertyCell::kSize <= Page::kMaxRegularHeapObjectSize);

  HeapObject* result;
  AllocationResult allocation =
      AllocateRaw(size, PROPERTY_CELL_SPACE, PROPERTY_CELL_SPACE);
  if (!allocation.To(&result)) return allocation;

  result->set_map_no_write_barrier(global_property_cell_map());
  PropertyCell* cell = PropertyCell::cast(result);
  cell->set_dependent_code(DependentCode::cast(empty_fixed_array()),
                           SKIP_WRITE_BARRIER);
  cell->set_value(the_hole_value());
  cell->set_type(HeapType::None());
  return result;
}


void Heap::CreateApiObjects() {
  HandleScope scope(isolate());
  Factory* factory = isolate()->factory();
  Handle<Map> new_neander_map =
      factory->NewMap(JS_OBJECT_TYPE, JSObject::kHeaderSize);

  // Don't use Smi-only elements optimizations for objects with the neander
  // map. There are too many cases where element values are set directly with a
  // bottleneck to trap the Smi-only -> fast elements transition, and there
  // appears to be no benefit for optimize this case.
  new_neander_map->set_elements_kind(TERMINAL_FAST_ELEMENTS_KIND);
  set_neander_map(*new_neander_map);

  Handle<JSObject> listeners = factory->NewNeanderObject();
  Handle<FixedArray> elements = factory->NewFixedArray(2);
  elements->set(0, Smi::FromInt(0));
  listeners->set_elements(*elements);
  set_message_listeners(*listeners);
}


void Heap::CreateJSEntryStub() {
  JSEntryStub stub(isolate());
  set_js_entry_code(*stub.GetCode());
}


void Heap::CreateJSConstructEntryStub() {
  JSConstructEntryStub stub(isolate());
  set_js_construct_entry_code(*stub.GetCode());
}


void Heap::CreateFixedStubs() {
  // Here we create roots for fixed stubs. They are needed at GC
  // for cooking and uncooking (check out frames.cc).
  // The eliminates the need for doing dictionary lookup in the
  // stub cache for these stubs.
  HandleScope scope(isolate());

  // Create stubs that should be there, so we don't unexpectedly have to
  // create them if we need them during the creation of another stub.
  // Stub creation mixes raw pointers and handles in an unsafe manner so
  // we cannot create stubs while we are creating stubs.
  CodeStub::GenerateStubsAheadOfTime(isolate());

  // MacroAssembler::Abort calls (usually enabled with --debug-code) depend on
  // CEntryStub, so we need to call GenerateStubsAheadOfTime before JSEntryStub
  // is created.

  // gcc-4.4 has problem generating correct code of following snippet:
  // {  JSEntryStub stub;
  //    js_entry_code_ = *stub.GetCode();
  // }
  // {  JSConstructEntryStub stub;
  //    js_construct_entry_code_ = *stub.GetCode();
  // }
  // To workaround the problem, make separate functions without inlining.
  Heap::CreateJSEntryStub();
  Heap::CreateJSConstructEntryStub();
}


void Heap::CreateInitialObjects() {
  HandleScope scope(isolate());
  Factory* factory = isolate()->factory();

  // The -0 value must be set before NumberFromDouble works.
  set_minus_zero_value(*factory->NewHeapNumber(-0.0, TENURED));
  ASSERT(std::signbit(minus_zero_value()->Number()) != 0);

  set_nan_value(*factory->NewHeapNumber(OS::nan_value(), TENURED));
  set_infinity_value(*factory->NewHeapNumber(V8_INFINITY, TENURED));

  // The hole has not been created yet, but we want to put something
  // predictable in the gaps in the string table, so lets make that Smi zero.
  set_the_hole_value(reinterpret_cast<Oddball*>(Smi::FromInt(0)));

  // Allocate initial string table.
  set_string_table(*StringTable::New(isolate(), kInitialStringTableSize));

  // Finish initializing oddballs after creating the string table.
  Oddball::Initialize(isolate(),
                      factory->undefined_value(),
                      "undefined",
                      factory->nan_value(),
                      Oddball::kUndefined);

  // Initialize the null_value.
  Oddball::Initialize(isolate(),
                      factory->null_value(),
                      "null",
                      handle(Smi::FromInt(0), isolate()),
                      Oddball::kNull);

  set_true_value(*factory->NewOddball(factory->boolean_map(),
                                      "true",
                                      handle(Smi::FromInt(1), isolate()),
                                      Oddball::kTrue));

  set_false_value(*factory->NewOddball(factory->boolean_map(),
                                       "false",
                                       handle(Smi::FromInt(0), isolate()),
                                       Oddball::kFalse));

  set_the_hole_value(*factory->NewOddball(factory->the_hole_map(),
                                          "hole",
                                          handle(Smi::FromInt(-1), isolate()),
                                          Oddball::kTheHole));

  set_uninitialized_value(
      *factory->NewOddball(factory->uninitialized_map(),
                           "uninitialized",
                           handle(Smi::FromInt(-1), isolate()),
                           Oddball::kUninitialized));

  set_arguments_marker(*factory->NewOddball(factory->arguments_marker_map(),
                                            "arguments_marker",
                                            handle(Smi::FromInt(-4), isolate()),
                                            Oddball::kArgumentMarker));

  set_no_interceptor_result_sentinel(
      *factory->NewOddball(factory->no_interceptor_result_sentinel_map(),
                           "no_interceptor_result_sentinel",
                           handle(Smi::FromInt(-2), isolate()),
                           Oddball::kOther));

  set_termination_exception(
      *factory->NewOddball(factory->termination_exception_map(),
                           "termination_exception",
                           handle(Smi::FromInt(-3), isolate()),
                           Oddball::kOther));

  set_exception(
      *factory->NewOddball(factory->exception_map(),
                           "exception",
                           handle(Smi::FromInt(-5), isolate()),
                           Oddball::kException));

  for (unsigned i = 0; i < ARRAY_SIZE(constant_string_table); i++) {
    Handle<String> str =
        factory->InternalizeUtf8String(constant_string_table[i].contents);
    roots_[constant_string_table[i].index] = *str;
  }

  // Allocate the hidden string which is used to identify the hidden properties
  // in JSObjects. The hash code has a special value so that it will not match
  // the empty string when searching for the property. It cannot be part of the
  // loop above because it needs to be allocated manually with the special
  // hash code in place. The hash code for the hidden_string is zero to ensure
  // that it will always be at the first entry in property descriptors.
  hidden_string_ = *factory->NewOneByteInternalizedString(
      OneByteVector("", 0), String::kEmptyStringHash);

  // Create the code_stubs dictionary. The initial size is set to avoid
  // expanding the dictionary during bootstrapping.
  set_code_stubs(*UnseededNumberDictionary::New(isolate(), 128));

  // Create the non_monomorphic_cache used in stub-cache.cc. The initial size
  // is set to avoid expanding the dictionary during bootstrapping.
  set_non_monomorphic_cache(*UnseededNumberDictionary::New(isolate(), 64));

  set_polymorphic_code_cache(PolymorphicCodeCache::cast(
      *factory->NewStruct(POLYMORPHIC_CODE_CACHE_TYPE)));

  set_instanceof_cache_function(Smi::FromInt(0));
  set_instanceof_cache_map(Smi::FromInt(0));
  set_instanceof_cache_answer(Smi::FromInt(0));

  CreateFixedStubs();

  // Allocate the dictionary of intrinsic function names.
  Handle<NameDictionary> intrinsic_names =
      NameDictionary::New(isolate(), Runtime::kNumFunctions);
  Runtime::InitializeIntrinsicFunctionNames(isolate(), intrinsic_names);
  set_intrinsic_function_names(*intrinsic_names);

  set_number_string_cache(*factory->NewFixedArray(
      kInitialNumberStringCacheSize * 2, TENURED));

  // Allocate cache for single character one byte strings.
  set_single_character_string_cache(*factory->NewFixedArray(
      String::kMaxOneByteCharCode + 1, TENURED));

  // Allocate cache for string split and regexp-multiple.
  set_string_split_cache(*factory->NewFixedArray(
      RegExpResultsCache::kRegExpResultsCacheSize, TENURED));
  set_regexp_multiple_cache(*factory->NewFixedArray(
      RegExpResultsCache::kRegExpResultsCacheSize, TENURED));

  // Allocate cache for external strings pointing to native source code.
  set_natives_source_cache(*factory->NewFixedArray(
      Natives::GetBuiltinsCount()));

  set_undefined_cell(*factory->NewCell(factory->undefined_value()));

  // The symbol registry is initialized lazily.
  set_symbol_registry(undefined_value());

  // Allocate object to hold object observation state.
  set_observation_state(*factory->NewJSObjectFromMap(
      factory->NewMap(JS_OBJECT_TYPE, JSObject::kHeaderSize)));

  // Allocate object to hold object microtask state.
  set_microtask_state(*factory->NewJSObjectFromMap(
      factory->NewMap(JS_OBJECT_TYPE, JSObject::kHeaderSize)));

  set_frozen_symbol(*factory->NewPrivateSymbol());
  set_nonexistent_symbol(*factory->NewPrivateSymbol());
  set_elements_transition_symbol(*factory->NewPrivateSymbol());
  set_uninitialized_symbol(*factory->NewPrivateSymbol());
  set_megamorphic_symbol(*factory->NewPrivateSymbol());
  set_observed_symbol(*factory->NewPrivateSymbol());

  Handle<SeededNumberDictionary> slow_element_dictionary =
      SeededNumberDictionary::New(isolate(), 0, TENURED);
  slow_element_dictionary->set_requires_slow_elements();
  set_empty_slow_element_dictionary(*slow_element_dictionary);

  set_materialized_objects(*factory->NewFixedArray(0, TENURED));

  // Handling of script id generation is in Factory::NewScript.
  set_last_script_id(Smi::FromInt(v8::UnboundScript::kNoScriptId));

  set_allocation_sites_scratchpad(*factory->NewFixedArray(
      kAllocationSiteScratchpadSize, TENURED));
  InitializeAllocationSitesScratchpad();

  // Initialize keyed lookup cache.
  isolate_->keyed_lookup_cache()->Clear();

  // Initialize context slot cache.
  isolate_->context_slot_cache()->Clear();

  // Initialize descriptor cache.
  isolate_->descriptor_lookup_cache()->Clear();

  // Initialize compilation cache.
  isolate_->compilation_cache()->Clear();
}


bool Heap::RootCanBeWrittenAfterInitialization(Heap::RootListIndex root_index) {
  RootListIndex writable_roots[] = {
    kStoreBufferTopRootIndex,
    kStackLimitRootIndex,
    kNumberStringCacheRootIndex,
    kInstanceofCacheFunctionRootIndex,
    kInstanceofCacheMapRootIndex,
    kInstanceofCacheAnswerRootIndex,
    kCodeStubsRootIndex,
    kNonMonomorphicCacheRootIndex,
    kPolymorphicCodeCacheRootIndex,
    kLastScriptIdRootIndex,
    kEmptyScriptRootIndex,
    kRealStackLimitRootIndex,
    kArgumentsAdaptorDeoptPCOffsetRootIndex,
    kConstructStubDeoptPCOffsetRootIndex,
    kGetterStubDeoptPCOffsetRootIndex,
    kSetterStubDeoptPCOffsetRootIndex,
    kStringTableRootIndex,
  };

  for (unsigned int i = 0; i < ARRAY_SIZE(writable_roots); i++) {
    if (root_index == writable_roots[i])
      return true;
  }
  return false;
}


bool Heap::RootCanBeTreatedAsConstant(RootListIndex root_index) {
  return !RootCanBeWrittenAfterInitialization(root_index) &&
      !InNewSpace(roots_array_start()[root_index]);
}


Object* RegExpResultsCache::Lookup(Heap* heap,
                                   String* key_string,
                                   Object* key_pattern,
                                   ResultsCacheType type) {
  FixedArray* cache;
  if (!key_string->IsInternalizedString()) return Smi::FromInt(0);
  if (type == STRING_SPLIT_SUBSTRINGS) {
    ASSERT(key_pattern->IsString());
    if (!key_pattern->IsInternalizedString()) return Smi::FromInt(0);
    cache = heap->string_split_cache();
  } else {
    ASSERT(type == REGEXP_MULTIPLE_INDICES);
    ASSERT(key_pattern->IsFixedArray());
    cache = heap->regexp_multiple_cache();
  }

  uint32_t hash = key_string->Hash();
  uint32_t index = ((hash & (kRegExpResultsCacheSize - 1)) &
      ~(kArrayEntriesPerCacheEntry - 1));
  if (cache->get(index + kStringOffset) == key_string &&
      cache->get(index + kPatternOffset) == key_pattern) {
    return cache->get(index + kArrayOffset);
  }
  index =
      ((index + kArrayEntriesPerCacheEntry) & (kRegExpResultsCacheSize - 1));
  if (cache->get(index + kStringOffset) == key_string &&
      cache->get(index + kPatternOffset) == key_pattern) {
    return cache->get(index + kArrayOffset);
  }
  return Smi::FromInt(0);
}


void RegExpResultsCache::Enter(Isolate* isolate,
                               Handle<String> key_string,
                               Handle<Object> key_pattern,
                               Handle<FixedArray> value_array,
                               ResultsCacheType type) {
  Factory* factory = isolate->factory();
  Handle<FixedArray> cache;
  if (!key_string->IsInternalizedString()) return;
  if (type == STRING_SPLIT_SUBSTRINGS) {
    ASSERT(key_pattern->IsString());
    if (!key_pattern->IsInternalizedString()) return;
    cache = factory->string_split_cache();
  } else {
    ASSERT(type == REGEXP_MULTIPLE_INDICES);
    ASSERT(key_pattern->IsFixedArray());
    cache = factory->regexp_multiple_cache();
  }

  uint32_t hash = key_string->Hash();
  uint32_t index = ((hash & (kRegExpResultsCacheSize - 1)) &
      ~(kArrayEntriesPerCacheEntry - 1));
  if (cache->get(index + kStringOffset) == Smi::FromInt(0)) {
    cache->set(index + kStringOffset, *key_string);
    cache->set(index + kPatternOffset, *key_pattern);
    cache->set(index + kArrayOffset, *value_array);
  } else {
    uint32_t index2 =
        ((index + kArrayEntriesPerCacheEntry) & (kRegExpResultsCacheSize - 1));
    if (cache->get(index2 + kStringOffset) == Smi::FromInt(0)) {
      cache->set(index2 + kStringOffset, *key_string);
      cache->set(index2 + kPatternOffset, *key_pattern);
      cache->set(index2 + kArrayOffset, *value_array);
    } else {
      cache->set(index2 + kStringOffset, Smi::FromInt(0));
      cache->set(index2 + kPatternOffset, Smi::FromInt(0));
      cache->set(index2 + kArrayOffset, Smi::FromInt(0));
      cache->set(index + kStringOffset, *key_string);
      cache->set(index + kPatternOffset, *key_pattern);
      cache->set(index + kArrayOffset, *value_array);
    }
  }
  // If the array is a reasonably short list of substrings, convert it into a
  // list of internalized strings.
  if (type == STRING_SPLIT_SUBSTRINGS && value_array->length() < 100) {
    for (int i = 0; i < value_array->length(); i++) {
      Handle<String> str(String::cast(value_array->get(i)), isolate);
      Handle<String> internalized_str = factory->InternalizeString(str);
      value_array->set(i, *internalized_str);
    }
  }
  // Convert backing store to a copy-on-write array.
  value_array->set_map_no_write_barrier(*factory->fixed_cow_array_map());
}


void RegExpResultsCache::Clear(FixedArray* cache) {
  for (int i = 0; i < kRegExpResultsCacheSize; i++) {
    cache->set(i, Smi::FromInt(0));
  }
}


int Heap::FullSizeNumberStringCacheLength() {
  // Compute the size of the number string cache based on the max newspace size.
  // The number string cache has a minimum size based on twice the initial cache
  // size to ensure that it is bigger after being made 'full size'.
  int number_string_cache_size = max_semispace_size_ / 512;
  number_string_cache_size = Max(kInitialNumberStringCacheSize * 2,
                                 Min(0x4000, number_string_cache_size));
  // There is a string and a number per entry so the length is twice the number
  // of entries.
  return number_string_cache_size * 2;
}


void Heap::FlushNumberStringCache() {
  // Flush the number to string cache.
  int len = number_string_cache()->length();
  for (int i = 0; i < len; i++) {
    number_string_cache()->set_undefined(i);
  }
}


void Heap::FlushAllocationSitesScratchpad() {
  for (int i = 0; i < allocation_sites_scratchpad_length_; i++) {
    allocation_sites_scratchpad()->set_undefined(i);
  }
  allocation_sites_scratchpad_length_ = 0;
}


void Heap::InitializeAllocationSitesScratchpad() {
  ASSERT(allocation_sites_scratchpad()->length() ==
         kAllocationSiteScratchpadSize);
  for (int i = 0; i < kAllocationSiteScratchpadSize; i++) {
    allocation_sites_scratchpad()->set_undefined(i);
  }
}


void Heap::AddAllocationSiteToScratchpad(AllocationSite* site,
                                         ScratchpadSlotMode mode) {
  if (allocation_sites_scratchpad_length_ < kAllocationSiteScratchpadSize) {
    // We cannot use the normal write-barrier because slots need to be
    // recorded with non-incremental marking as well. We have to explicitly
    // record the slot to take evacuation candidates into account.
    allocation_sites_scratchpad()->set(
        allocation_sites_scratchpad_length_, site, SKIP_WRITE_BARRIER);
    Object** slot = allocation_sites_scratchpad()->RawFieldOfElementAt(
        allocation_sites_scratchpad_length_);

    if (mode == RECORD_SCRATCHPAD_SLOT) {
      // We need to allow slots buffer overflow here since the evacuation
      // candidates are not part of the global list of old space pages and
      // releasing an evacuation candidate due to a slots buffer overflow
      // results in lost pages.
      mark_compact_collector()->RecordSlot(
          slot, slot, *slot, SlotsBuffer::IGNORE_OVERFLOW);
    }
    allocation_sites_scratchpad_length_++;
  }
}


Map* Heap::MapForExternalArrayType(ExternalArrayType array_type) {
  return Map::cast(roots_[RootIndexForExternalArrayType(array_type)]);
}


Heap::RootListIndex Heap::RootIndexForExternalArrayType(
    ExternalArrayType array_type) {
  switch (array_type) {
#define ARRAY_TYPE_TO_ROOT_INDEX(Type, type, TYPE, ctype, size)               \
    case kExternal##Type##Array:                                              \
      return kExternal##Type##ArrayMapRootIndex;

    TYPED_ARRAYS(ARRAY_TYPE_TO_ROOT_INDEX)
#undef ARRAY_TYPE_TO_ROOT_INDEX

    default:
      UNREACHABLE();
      return kUndefinedValueRootIndex;
  }
}


Map* Heap::MapForFixedTypedArray(ExternalArrayType array_type) {
  return Map::cast(roots_[RootIndexForFixedTypedArray(array_type)]);
}


Heap::RootListIndex Heap::RootIndexForFixedTypedArray(
    ExternalArrayType array_type) {
  switch (array_type) {
#define ARRAY_TYPE_TO_ROOT_INDEX(Type, type, TYPE, ctype, size)               \
    case kExternal##Type##Array:                                              \
      return kFixed##Type##ArrayMapRootIndex;

    TYPED_ARRAYS(ARRAY_TYPE_TO_ROOT_INDEX)
#undef ARRAY_TYPE_TO_ROOT_INDEX

    default:
      UNREACHABLE();
      return kUndefinedValueRootIndex;
  }
}


Heap::RootListIndex Heap::RootIndexForEmptyExternalArray(
    ElementsKind elementsKind) {
  switch (elementsKind) {
#define ELEMENT_KIND_TO_ROOT_INDEX(Type, type, TYPE, ctype, size)             \
    case EXTERNAL_##TYPE##_ELEMENTS:                                          \
      return kEmptyExternal##Type##ArrayRootIndex;

    TYPED_ARRAYS(ELEMENT_KIND_TO_ROOT_INDEX)
#undef ELEMENT_KIND_TO_ROOT_INDEX

    default:
      UNREACHABLE();
      return kUndefinedValueRootIndex;
  }
}


Heap::RootListIndex Heap::RootIndexForEmptyFixedTypedArray(
    ElementsKind elementsKind) {
  switch (elementsKind) {
#define ELEMENT_KIND_TO_ROOT_INDEX(Type, type, TYPE, ctype, size)             \
    case TYPE##_ELEMENTS:                                                     \
      return kEmptyFixed##Type##ArrayRootIndex;

    TYPED_ARRAYS(ELEMENT_KIND_TO_ROOT_INDEX)
#undef ELEMENT_KIND_TO_ROOT_INDEX
    default:
      UNREACHABLE();
      return kUndefinedValueRootIndex;
  }
}


ExternalArray* Heap::EmptyExternalArrayForMap(Map* map) {
  return ExternalArray::cast(
      roots_[RootIndexForEmptyExternalArray(map->elements_kind())]);
}


FixedTypedArrayBase* Heap::EmptyFixedTypedArrayForMap(Map* map) {
  return FixedTypedArrayBase::cast(
      roots_[RootIndexForEmptyFixedTypedArray(map->elements_kind())]);
}


AllocationResult Heap::AllocateForeign(Address address,
                                       PretenureFlag pretenure) {
  // Statically ensure that it is safe to allocate foreigns in paged spaces.
  STATIC_ASSERT(Foreign::kSize <= Page::kMaxRegularHeapObjectSize);
  AllocationSpace space = (pretenure == TENURED) ? OLD_DATA_SPACE : NEW_SPACE;
  Foreign* result;
  AllocationResult allocation = Allocate(foreign_map(), space);
  if (!allocation.To(&result)) return allocation;
  result->set_foreign_address(address);
  return result;
}


AllocationResult Heap::AllocateByteArray(int length, PretenureFlag pretenure) {
  if (length < 0 || length > ByteArray::kMaxLength) {
    v8::internal::Heap::FatalProcessOutOfMemory("invalid array length", true);
  }
  int size = ByteArray::SizeFor(length);
  AllocationSpace space = SelectSpace(size, OLD_DATA_SPACE, pretenure);
  HeapObject* result;
  { AllocationResult allocation = AllocateRaw(size, space, OLD_DATA_SPACE);
    if (!allocation.To(&result)) return allocation;
  }

  result->set_map_no_write_barrier(byte_array_map());
  ByteArray::cast(result)->set_length(length);
  return result;
}


void Heap::CreateFillerObjectAt(Address addr, int size) {
  if (size == 0) return;
  HeapObject* filler = HeapObject::FromAddress(addr);
  if (size == kPointerSize) {
    filler->set_map_no_write_barrier(one_pointer_filler_map());
  } else if (size == 2 * kPointerSize) {
    filler->set_map_no_write_barrier(two_pointer_filler_map());
  } else {
    filler->set_map_no_write_barrier(free_space_map());
    FreeSpace::cast(filler)->set_size(size);
  }
}


bool Heap::CanMoveObjectStart(HeapObject* object) {
  Address address = object->address();
  bool is_in_old_pointer_space = InOldPointerSpace(address);
  bool is_in_old_data_space = InOldDataSpace(address);

  if (lo_space()->Contains(object)) return false;

  Page* page = Page::FromAddress(address);
  // We can move the object start if:
  // (1) the object is not in old pointer or old data space,
  // (2) the page of the object was already swept,
  // (3) the page was already concurrently swept. This case is an optimization
  // for concurrent sweeping. The WasSwept predicate for concurrently swept
  // pages is set after sweeping all pages.
  return (!is_in_old_pointer_space && !is_in_old_data_space) ||
         page->WasSwept() ||
         (mark_compact_collector()->AreSweeperThreadsActivated() &&
              page->parallel_sweeping() <=
                  MemoryChunk::PARALLEL_SWEEPING_FINALIZE);
}


void Heap::AdjustLiveBytes(Address address, int by, InvocationMode mode) {
  if (incremental_marking()->IsMarking() &&
      Marking::IsBlack(Marking::MarkBitFrom(address))) {
    if (mode == FROM_GC) {
      MemoryChunk::IncrementLiveBytesFromGC(address, by);
    } else {
      MemoryChunk::IncrementLiveBytesFromMutator(address, by);
    }
  }
}


AllocationResult Heap::AllocateExternalArray(int length,
                                         ExternalArrayType array_type,
                                         void* external_pointer,
                                         PretenureFlag pretenure) {
  int size = ExternalArray::kAlignedSize;
  AllocationSpace space = SelectSpace(size, OLD_DATA_SPACE, pretenure);
  HeapObject* result;
  { AllocationResult allocation = AllocateRaw(size, space, OLD_DATA_SPACE);
    if (!allocation.To(&result)) return allocation;
  }

  result->set_map_no_write_barrier(
      MapForExternalArrayType(array_type));
  ExternalArray::cast(result)->set_length(length);
  ExternalArray::cast(result)->set_external_pointer(external_pointer);
  return result;
}

static void ForFixedTypedArray(ExternalArrayType array_type,
                               int* element_size,
                               ElementsKind* element_kind) {
  switch (array_type) {
#define TYPED_ARRAY_CASE(Type, type, TYPE, ctype, size)                       \
    case kExternal##Type##Array:                                              \
      *element_size = size;                                                   \
      *element_kind = TYPE##_ELEMENTS;                                        \
      return;

    TYPED_ARRAYS(TYPED_ARRAY_CASE)
#undef TYPED_ARRAY_CASE

    default:
      *element_size = 0;  // Bogus
      *element_kind = UINT8_ELEMENTS;  // Bogus
      UNREACHABLE();
  }
}


AllocationResult Heap::AllocateFixedTypedArray(int length,
                                               ExternalArrayType array_type,
                                               PretenureFlag pretenure) {
  int element_size;
  ElementsKind elements_kind;
  ForFixedTypedArray(array_type, &element_size, &elements_kind);
  int size = OBJECT_POINTER_ALIGN(
      length * element_size + FixedTypedArrayBase::kDataOffset);
#ifndef V8_HOST_ARCH_64_BIT
  if (array_type == kExternalFloat64Array) {
    size += kPointerSize;
  }
#endif
  AllocationSpace space = SelectSpace(size, OLD_DATA_SPACE, pretenure);

  HeapObject* object;
  AllocationResult allocation = AllocateRaw(size, space, OLD_DATA_SPACE);
  if (!allocation.To(&object)) return allocation;

  if (array_type == kExternalFloat64Array) {
    object = EnsureDoubleAligned(this, object, size);
  }

  object->set_map(MapForFixedTypedArray(array_type));
  FixedTypedArrayBase* elements = FixedTypedArrayBase::cast(object);
  elements->set_length(length);
  memset(elements->DataPtr(), 0, elements->DataSize());
  return elements;
}


AllocationResult Heap::AllocateCode(int object_size,
                                bool immovable) {
  ASSERT(IsAligned(static_cast<intptr_t>(object_size), kCodeAlignment));
  AllocationResult allocation;
  // Large code objects and code objects which should stay at a fixed address
  // are allocated in large object space.
  HeapObject* result;
  bool force_lo_space = object_size > code_space()->AreaSize();
  if (force_lo_space) {
    allocation = lo_space_->AllocateRaw(object_size, EXECUTABLE);
  } else {
    allocation = AllocateRaw(object_size, CODE_SPACE, CODE_SPACE);
  }
  if (!allocation.To(&result)) return allocation;

  if (immovable && !force_lo_space &&
     // Objects on the first page of each space are never moved.
     !code_space_->FirstPage()->Contains(result->address())) {
    // Discard the first code allocation, which was on a page where it could be
    // moved.
    CreateFillerObjectAt(result->address(), object_size);
    allocation = lo_space_->AllocateRaw(object_size, EXECUTABLE);
    if (!allocation.To(&result)) return allocation;
  }

  result->set_map_no_write_barrier(code_map());
  Code* code = Code::cast(result);
  ASSERT(!isolate_->code_range()->exists() ||
      isolate_->code_range()->contains(code->address()));
  code->set_gc_metadata(Smi::FromInt(0));
  code->set_ic_age(global_ic_age_);
  return code;
}


AllocationResult Heap::CopyCode(Code* code) {
  AllocationResult allocation;
  HeapObject* new_constant_pool;
  if (FLAG_enable_ool_constant_pool &&
      code->constant_pool() != empty_constant_pool_array()) {
    // Copy the constant pool, since edits to the copied code may modify
    // the constant pool.
    allocation = CopyConstantPoolArray(code->constant_pool());
    if (!allocation.To(&new_constant_pool)) return allocation;
  } else {
    new_constant_pool = empty_constant_pool_array();
  }

  // Allocate an object the same size as the code object.
  int obj_size = code->Size();
  if (obj_size > code_space()->AreaSize()) {
    allocation = lo_space_->AllocateRaw(obj_size, EXECUTABLE);
  } else {
    allocation = AllocateRaw(obj_size, CODE_SPACE, CODE_SPACE);
  }

  HeapObject* result;
  if (!allocation.To(&result)) return allocation;

  // Copy code object.
  Address old_addr = code->address();
  Address new_addr = result->address();
  CopyBlock(new_addr, old_addr, obj_size);
  Code* new_code = Code::cast(result);

  // Update the constant pool.
  new_code->set_constant_pool(new_constant_pool);

  // Relocate the copy.
  ASSERT(!isolate_->code_range()->exists() ||
      isolate_->code_range()->contains(code->address()));
  new_code->Relocate(new_addr - old_addr);
  return new_code;
}


AllocationResult Heap::CopyCode(Code* code, Vector<byte> reloc_info) {
  // Allocate ByteArray and ConstantPoolArray before the Code object, so that we
  // do not risk leaving uninitialized Code object (and breaking the heap).
  ByteArray* reloc_info_array;
  { AllocationResult allocation =
        AllocateByteArray(reloc_info.length(), TENURED);
    if (!allocation.To(&reloc_info_array)) return allocation;
  }
  HeapObject* new_constant_pool;
  if (FLAG_enable_ool_constant_pool &&
      code->constant_pool() != empty_constant_pool_array()) {
    // Copy the constant pool, since edits to the copied code may modify
    // the constant pool.
    AllocationResult allocation =
        CopyConstantPoolArray(code->constant_pool());
    if (!allocation.To(&new_constant_pool)) return allocation;
  } else {
    new_constant_pool = empty_constant_pool_array();
  }

  int new_body_size = RoundUp(code->instruction_size(), kObjectAlignment);

  int new_obj_size = Code::SizeFor(new_body_size);

  Address old_addr = code->address();

  size_t relocation_offset =
      static_cast<size_t>(code->instruction_end() - old_addr);

  AllocationResult allocation;
  if (new_obj_size > code_space()->AreaSize()) {
    allocation = lo_space_->AllocateRaw(new_obj_size, EXECUTABLE);
  } else {
    allocation = AllocateRaw(new_obj_size, CODE_SPACE, CODE_SPACE);
  }

  HeapObject* result;
  if (!allocation.To(&result)) return allocation;

  // Copy code object.
  Address new_addr = result->address();

  // Copy header and instructions.
  CopyBytes(new_addr, old_addr, relocation_offset);

  Code* new_code = Code::cast(result);
  new_code->set_relocation_info(reloc_info_array);

  // Update constant pool.
  new_code->set_constant_pool(new_constant_pool);

  // Copy patched rinfo.
  CopyBytes(new_code->relocation_start(),
            reloc_info.start(),
            static_cast<size_t>(reloc_info.length()));

  // Relocate the copy.
  ASSERT(!isolate_->code_range()->exists() ||
      isolate_->code_range()->contains(code->address()));
  new_code->Relocate(new_addr - old_addr);

#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) code->ObjectVerify();
#endif
  return new_code;
}


void Heap::InitializeAllocationMemento(AllocationMemento* memento,
                                       AllocationSite* allocation_site) {
  memento->set_map_no_write_barrier(allocation_memento_map());
  ASSERT(allocation_site->map() == allocation_site_map());
  memento->set_allocation_site(allocation_site, SKIP_WRITE_BARRIER);
  if (FLAG_allocation_site_pretenuring) {
    allocation_site->IncrementMementoCreateCount();
  }
}


AllocationResult Heap::Allocate(Map* map, AllocationSpace space,
                            AllocationSite* allocation_site) {
  ASSERT(gc_state_ == NOT_IN_GC);
  ASSERT(map->instance_type() != MAP_TYPE);
  // If allocation failures are disallowed, we may allocate in a different
  // space when new space is full and the object is not a large object.
  AllocationSpace retry_space =
      (space != NEW_SPACE) ? space : TargetSpaceId(map->instance_type());
  int size = map->instance_size();
  if (allocation_site != NULL) {
    size += AllocationMemento::kSize;
  }
  HeapObject* result;
  AllocationResult allocation = AllocateRaw(size, space, retry_space);
  if (!allocation.To(&result)) return allocation;
  // No need for write barrier since object is white and map is in old space.
  result->set_map_no_write_barrier(map);
  if (allocation_site != NULL) {
    AllocationMemento* alloc_memento = reinterpret_cast<AllocationMemento*>(
        reinterpret_cast<Address>(result) + map->instance_size());
    InitializeAllocationMemento(alloc_memento, allocation_site);
  }
  return result;
}


#if defined(__GNUC__) && (__GNUC__ == 4) && (__GNUC_MINOR__ <= 4)
// Work around bad optimization by GCC 4.4.6 on PPC Linux
#pragma GCC optimize "O0"
#endif
AllocationResult Heap::AllocateArgumentsObject(Object* callee, int length) {
  // To get fast allocation and map sharing for arguments objects we
  // allocate them based on an arguments boilerplate.

  JSObject* boilerplate;
  int arguments_object_size;
  bool strict_mode_callee = callee->IsJSFunction() &&
      JSFunction::cast(callee)->shared()->strict_mode() == STRICT;
  if (strict_mode_callee) {
    boilerplate =
        isolate()->context()->native_context()->strict_arguments_boilerplate();
    arguments_object_size = kStrictArgumentsObjectSize;
  } else {
    boilerplate =
        isolate()->context()->native_context()->sloppy_arguments_boilerplate();
    arguments_object_size = kSloppyArgumentsObjectSize;
  }

  // Check that the size of the boilerplate matches our
  // expectations. The ArgumentsAccessStub::GenerateNewObject relies
  // on the size being a known constant.
  ASSERT(arguments_object_size == boilerplate->map()->instance_size());

  // Do the allocation.
  HeapObject* result;
  { AllocationResult allocation =
        AllocateRaw(arguments_object_size, NEW_SPACE, OLD_POINTER_SPACE);
    if (!allocation.To(&result)) return allocation;
  }

  // Copy the content. The arguments boilerplate doesn't have any
  // fields that point to new space so it's safe to skip the write
  // barrier here.
  CopyBlock(result->address(), boilerplate->address(), JSObject::kHeaderSize);

  // Set the length property.
  JSObject* js_obj = JSObject::cast(result);
  js_obj->InObjectPropertyAtPut(
      kArgumentsLengthIndex, Smi::FromInt(length), SKIP_WRITE_BARRIER);
  // Set the callee property for sloppy mode arguments object only.
  if (!strict_mode_callee) {
    js_obj->InObjectPropertyAtPut(kArgumentsCalleeIndex, callee);
  }

  // Check the state of the object
  ASSERT(js_obj->HasFastProperties());
  ASSERT(js_obj->HasFastObjectElements());

  return js_obj;
}
#if defined(__GNUC__) && (__GNUC__ == 4) && (__GNUC_MINOR__ <= 4)
#pragma GCC reset_options
#endif


void Heap::InitializeJSObjectFromMap(JSObject* obj,
                                     FixedArray* properties,
                                     Map* map) {
  obj->set_properties(properties);
  obj->initialize_elements();
  // TODO(1240798): Initialize the object's body using valid initial values
  // according to the object's initial map.  For example, if the map's
  // instance type is JS_ARRAY_TYPE, the length field should be initialized
  // to a number (e.g. Smi::FromInt(0)) and the elements initialized to a
  // fixed array (e.g. Heap::empty_fixed_array()).  Currently, the object
  // verification code has to cope with (temporarily) invalid objects.  See
  // for example, JSArray::JSArrayVerify).
  Object* filler;
  // We cannot always fill with one_pointer_filler_map because objects
  // created from API functions expect their internal fields to be initialized
  // with undefined_value.
  // Pre-allocated fields need to be initialized with undefined_value as well
  // so that object accesses before the constructor completes (e.g. in the
  // debugger) will not cause a crash.
  if (map->constructor()->IsJSFunction() &&
      JSFunction::cast(map->constructor())->shared()->
          IsInobjectSlackTrackingInProgress()) {
    // We might want to shrink the object later.
    ASSERT(obj->GetInternalFieldCount() == 0);
    filler = Heap::one_pointer_filler_map();
  } else {
    filler = Heap::undefined_value();
  }
  obj->InitializeBody(map, Heap::undefined_value(), filler);
}


AllocationResult Heap::AllocateJSObjectFromMap(
    Map* map,
    PretenureFlag pretenure,
    bool allocate_properties,
    AllocationSite* allocation_site) {
  // JSFunctions should be allocated using AllocateFunction to be
  // properly initialized.
  ASSERT(map->instance_type() != JS_FUNCTION_TYPE);

  // Both types of global objects should be allocated using
  // AllocateGlobalObject to be properly initialized.
  ASSERT(map->instance_type() != JS_GLOBAL_OBJECT_TYPE);
  ASSERT(map->instance_type() != JS_BUILTINS_OBJECT_TYPE);

  // Allocate the backing storage for the properties.
  FixedArray* properties;
  if (allocate_properties) {
    int prop_size = map->InitialPropertiesLength();
    ASSERT(prop_size >= 0);
    { AllocationResult allocation = AllocateFixedArray(prop_size, pretenure);
      if (!allocation.To(&properties)) return allocation;
    }
  } else {
    properties = empty_fixed_array();
  }

  // Allocate the JSObject.
  int size = map->instance_size();
  AllocationSpace space = SelectSpace(size, OLD_POINTER_SPACE, pretenure);
  JSObject* js_obj;
  AllocationResult allocation = Allocate(map, space, allocation_site);
  if (!allocation.To(&js_obj)) return allocation;

  // Initialize the JSObject.
  InitializeJSObjectFromMap(js_obj, properties, map);
  ASSERT(js_obj->HasFastElements() ||
         js_obj->HasExternalArrayElements() ||
         js_obj->HasFixedTypedArrayElements());
  return js_obj;
}


AllocationResult Heap::AllocateJSObject(JSFunction* constructor,
                                        PretenureFlag pretenure,
                                        AllocationSite* allocation_site) {
  ASSERT(constructor->has_initial_map());

  // Allocate the object based on the constructors initial map.
  AllocationResult allocation = AllocateJSObjectFromMap(
      constructor->initial_map(), pretenure, true, allocation_site);
#ifdef DEBUG
  // Make sure result is NOT a global object if valid.
  HeapObject* obj;
  ASSERT(!allocation.To(&obj) || !obj->IsGlobalObject());
#endif
  return allocation;
}


AllocationResult Heap::CopyJSObject(JSObject* source, AllocationSite* site) {
  // Never used to copy functions.  If functions need to be copied we
  // have to be careful to clear the literals array.
  SLOW_ASSERT(!source->IsJSFunction());

  // Make the clone.
  Map* map = source->map();
  int object_size = map->instance_size();
  HeapObject* clone;

  ASSERT(site == NULL || AllocationSite::CanTrack(map->instance_type()));

  WriteBarrierMode wb_mode = UPDATE_WRITE_BARRIER;

  // If we're forced to always allocate, we use the general allocation
  // functions which may leave us with an object in old space.
  if (always_allocate()) {
    { AllocationResult allocation =
          AllocateRaw(object_size, NEW_SPACE, OLD_POINTER_SPACE);
      if (!allocation.To(&clone)) return allocation;
    }
    Address clone_address = clone->address();
    CopyBlock(clone_address,
              source->address(),
              object_size);
    // Update write barrier for all fields that lie beyond the header.
    RecordWrites(clone_address,
                 JSObject::kHeaderSize,
                 (object_size - JSObject::kHeaderSize) / kPointerSize);
  } else {
    wb_mode = SKIP_WRITE_BARRIER;

    { int adjusted_object_size = site != NULL
          ? object_size + AllocationMemento::kSize
          : object_size;
    AllocationResult allocation =
          AllocateRaw(adjusted_object_size, NEW_SPACE, NEW_SPACE);
      if (!allocation.To(&clone)) return allocation;
    }
    SLOW_ASSERT(InNewSpace(clone));
    // Since we know the clone is allocated in new space, we can copy
    // the contents without worrying about updating the write barrier.
    CopyBlock(clone->address(),
              source->address(),
              object_size);

    if (site != NULL) {
      AllocationMemento* alloc_memento = reinterpret_cast<AllocationMemento*>(
          reinterpret_cast<Address>(clone) + object_size);
      InitializeAllocationMemento(alloc_memento, site);
    }
  }

  SLOW_ASSERT(
      JSObject::cast(clone)->GetElementsKind() == source->GetElementsKind());
  FixedArrayBase* elements = FixedArrayBase::cast(source->elements());
  FixedArray* properties = FixedArray::cast(source->properties());
  // Update elements if necessary.
  if (elements->length() > 0) {
    FixedArrayBase* elem;
    { AllocationResult allocation;
      if (elements->map() == fixed_cow_array_map()) {
        allocation = FixedArray::cast(elements);
      } else if (source->HasFastDoubleElements()) {
        allocation = CopyFixedDoubleArray(FixedDoubleArray::cast(elements));
      } else {
        allocation = CopyFixedArray(FixedArray::cast(elements));
      }
      if (!allocation.To(&elem)) return allocation;
    }
    JSObject::cast(clone)->set_elements(elem, wb_mode);
  }
  // Update properties if necessary.
  if (properties->length() > 0) {
    FixedArray* prop;
    { AllocationResult allocation = CopyFixedArray(properties);
      if (!allocation.To(&prop)) return allocation;
    }
    JSObject::cast(clone)->set_properties(prop, wb_mode);
  }
  // Return the new clone.
  return clone;
}


AllocationResult Heap::AllocateStringFromUtf8Slow(Vector<const char> string,
                                                  int non_ascii_start,
                                                  PretenureFlag pretenure) {
  // Continue counting the number of characters in the UTF-8 string, starting
  // from the first non-ascii character or word.
  Access<UnicodeCache::Utf8Decoder>
      decoder(isolate_->unicode_cache()->utf8_decoder());
  decoder->Reset(string.start() + non_ascii_start,
                 string.length() - non_ascii_start);
  int utf16_length = decoder->Utf16Length();
  ASSERT(utf16_length > 0);
  // Allocate string.
  HeapObject* result;
  {
    int chars = non_ascii_start + utf16_length;
    AllocationResult allocation = AllocateRawTwoByteString(chars, pretenure);
    if (!allocation.To(&result) || result->IsException()) {
      return allocation;
    }
  }
  // Copy ascii portion.
  uint16_t* data = SeqTwoByteString::cast(result)->GetChars();
  if (non_ascii_start != 0) {
    const char* ascii_data = string.start();
    for (int i = 0; i < non_ascii_start; i++) {
      *data++ = *ascii_data++;
    }
  }
  // Now write the remainder.
  decoder->WriteUtf16(data, utf16_length);
  return result;
}


AllocationResult Heap::AllocateStringFromTwoByte(Vector<const uc16> string,
                                                 PretenureFlag pretenure) {
  // Check if the string is an ASCII string.
  HeapObject* result;
  int length = string.length();
  const uc16* start = string.start();

  if (String::IsOneByte(start, length)) {
    AllocationResult allocation = AllocateRawOneByteString(length, pretenure);
    if (!allocation.To(&result) || result->IsException()) {
      return allocation;
    }
    CopyChars(SeqOneByteString::cast(result)->GetChars(), start, length);
  } else {  // It's not a one byte string.
    AllocationResult allocation = AllocateRawTwoByteString(length, pretenure);
    if (!allocation.To(&result) || result->IsException()) {
      return allocation;
    }
    CopyChars(SeqTwoByteString::cast(result)->GetChars(), start, length);
  }
  return result;
}


static inline void WriteOneByteData(Vector<const char> vector,
                                    uint8_t* chars,
                                    int len) {
  // Only works for ascii.
  ASSERT(vector.length() == len);
  OS::MemCopy(chars, vector.start(), len);
}

static inline void WriteTwoByteData(Vector<const char> vector,
                                    uint16_t* chars,
                                    int len) {
  const uint8_t* stream = reinterpret_cast<const uint8_t*>(vector.start());
  unsigned stream_length = vector.length();
  while (stream_length != 0) {
    unsigned consumed = 0;
    uint32_t c = unibrow::Utf8::ValueOf(stream, stream_length, &consumed);
    ASSERT(c != unibrow::Utf8::kBadChar);
    ASSERT(consumed <= stream_length);
    stream_length -= consumed;
    stream += consumed;
    if (c > unibrow::Utf16::kMaxNonSurrogateCharCode) {
      len -= 2;
      if (len < 0) break;
      *chars++ = unibrow::Utf16::LeadSurrogate(c);
      *chars++ = unibrow::Utf16::TrailSurrogate(c);
    } else {
      len -= 1;
      if (len < 0) break;
      *chars++ = c;
    }
  }
  ASSERT(stream_length == 0);
  ASSERT(len == 0);
}


static inline void WriteOneByteData(String* s, uint8_t* chars, int len) {
  ASSERT(s->length() == len);
  String::WriteToFlat(s, chars, 0, len);
}


static inline void WriteTwoByteData(String* s, uint16_t* chars, int len) {
  ASSERT(s->length() == len);
  String::WriteToFlat(s, chars, 0, len);
}


template<bool is_one_byte, typename T>
AllocationResult Heap::AllocateInternalizedStringImpl(
    T t, int chars, uint32_t hash_field) {
  ASSERT(chars >= 0);
  // Compute map and object size.
  int size;
  Map* map;

  if (chars < 0 || chars > String::kMaxLength) {
    return isolate()->ThrowInvalidStringLength();
  }
  if (is_one_byte) {
    map = ascii_internalized_string_map();
    size = SeqOneByteString::SizeFor(chars);
  } else {
    map = internalized_string_map();
    size = SeqTwoByteString::SizeFor(chars);
  }
  AllocationSpace space = SelectSpace(size, OLD_DATA_SPACE, TENURED);

  // Allocate string.
  HeapObject* result;
  { AllocationResult allocation = AllocateRaw(size, space, OLD_DATA_SPACE);
    if (!allocation.To(&result)) return allocation;
  }

  result->set_map_no_write_barrier(map);
  // Set length and hash fields of the allocated string.
  String* answer = String::cast(result);
  answer->set_length(chars);
  answer->set_hash_field(hash_field);

  ASSERT_EQ(size, answer->Size());

  if (is_one_byte) {
    WriteOneByteData(t, SeqOneByteString::cast(answer)->GetChars(), chars);
  } else {
    WriteTwoByteData(t, SeqTwoByteString::cast(answer)->GetChars(), chars);
  }
  return answer;
}


// Need explicit instantiations.
template
AllocationResult Heap::AllocateInternalizedStringImpl<true>(
    String*, int, uint32_t);
template
AllocationResult Heap::AllocateInternalizedStringImpl<false>(
    String*, int, uint32_t);
template
AllocationResult Heap::AllocateInternalizedStringImpl<false>(
    Vector<const char>, int, uint32_t);


AllocationResult Heap::AllocateRawOneByteString(int length,
                                                PretenureFlag pretenure) {
  if (length < 0 || length > String::kMaxLength) {
    return isolate()->ThrowInvalidStringLength();
  }
  int size = SeqOneByteString::SizeFor(length);
  ASSERT(size <= SeqOneByteString::kMaxSize);
  AllocationSpace space = SelectSpace(size, OLD_DATA_SPACE, pretenure);

  HeapObject* result;
  { AllocationResult allocation = AllocateRaw(size, space, OLD_DATA_SPACE);
    if (!allocation.To(&result)) return allocation;
  }

  // Partially initialize the object.
  result->set_map_no_write_barrier(ascii_string_map());
  String::cast(result)->set_length(length);
  String::cast(result)->set_hash_field(String::kEmptyHashField);
  ASSERT_EQ(size, HeapObject::cast(result)->Size());

  return result;
}


AllocationResult Heap::AllocateRawTwoByteString(int length,
                                                PretenureFlag pretenure) {
  if (length < 0 || length > String::kMaxLength) {
    return isolate()->ThrowInvalidStringLength();
  }
  int size = SeqTwoByteString::SizeFor(length);
  ASSERT(size <= SeqTwoByteString::kMaxSize);
  AllocationSpace space = SelectSpace(size, OLD_DATA_SPACE, pretenure);

  HeapObject* result;
  { AllocationResult allocation = AllocateRaw(size, space, OLD_DATA_SPACE);
    if (!allocation.To(&result)) return allocation;
  }

  // Partially initialize the object.
  result->set_map_no_write_barrier(string_map());
  String::cast(result)->set_length(length);
  String::cast(result)->set_hash_field(String::kEmptyHashField);
  ASSERT_EQ(size, HeapObject::cast(result)->Size());
  return result;
}


AllocationResult Heap::AllocateEmptyFixedArray() {
  int size = FixedArray::SizeFor(0);
  HeapObject* result;
  { AllocationResult allocation =
        AllocateRaw(size, OLD_DATA_SPACE, OLD_DATA_SPACE);
    if (!allocation.To(&result)) return allocation;
  }
  // Initialize the object.
  result->set_map_no_write_barrier(fixed_array_map());
  FixedArray::cast(result)->set_length(0);
  return result;
}


AllocationResult Heap::AllocateEmptyExternalArray(
    ExternalArrayType array_type) {
  return AllocateExternalArray(0, array_type, NULL, TENURED);
}


AllocationResult Heap::CopyAndTenureFixedCOWArray(FixedArray* src) {
  if (!InNewSpace(src)) {
    return src;
  }

  int len = src->length();
  HeapObject* obj;
  { AllocationResult allocation = AllocateRawFixedArray(len, TENURED);
    if (!allocation.To(&obj)) return allocation;
  }
  obj->set_map_no_write_barrier(fixed_array_map());
  FixedArray* result = FixedArray::cast(obj);
  result->set_length(len);

  // Copy the content
  DisallowHeapAllocation no_gc;
  WriteBarrierMode mode = result->GetWriteBarrierMode(no_gc);
  for (int i = 0; i < len; i++) result->set(i, src->get(i), mode);

  // TODO(mvstanton): The map is set twice because of protection against calling
  // set() on a COW FixedArray. Issue v8:3221 created to track this, and
  // we might then be able to remove this whole method.
  HeapObject::cast(obj)->set_map_no_write_barrier(fixed_cow_array_map());
  return result;
}


AllocationResult Heap::AllocateEmptyFixedTypedArray(
    ExternalArrayType array_type) {
  return AllocateFixedTypedArray(0, array_type, TENURED);
}


AllocationResult Heap::CopyFixedArrayWithMap(FixedArray* src, Map* map) {
  int len = src->length();
  HeapObject* obj;
  { AllocationResult allocation = AllocateRawFixedArray(len, NOT_TENURED);
    if (!allocation.To(&obj)) return allocation;
  }
  if (InNewSpace(obj)) {
    obj->set_map_no_write_barrier(map);
    CopyBlock(obj->address() + kPointerSize,
              src->address() + kPointerSize,
              FixedArray::SizeFor(len) - kPointerSize);
    return obj;
  }
  obj->set_map_no_write_barrier(map);
  FixedArray* result = FixedArray::cast(obj);
  result->set_length(len);

  // Copy the content
  DisallowHeapAllocation no_gc;
  WriteBarrierMode mode = result->GetWriteBarrierMode(no_gc);
  for (int i = 0; i < len; i++) result->set(i, src->get(i), mode);
  return result;
}


AllocationResult Heap::CopyFixedDoubleArrayWithMap(FixedDoubleArray* src,
                                                   Map* map) {
  int len = src->length();
  HeapObject* obj;
  { AllocationResult allocation = AllocateRawFixedDoubleArray(len, NOT_TENURED);
    if (!allocation.To(&obj)) return allocation;
  }
  obj->set_map_no_write_barrier(map);
  CopyBlock(
      obj->address() + FixedDoubleArray::kLengthOffset,
      src->address() + FixedDoubleArray::kLengthOffset,
      FixedDoubleArray::SizeFor(len) - FixedDoubleArray::kLengthOffset);
  return obj;
}


AllocationResult Heap::CopyConstantPoolArrayWithMap(ConstantPoolArray* src,
                                                    Map* map) {
  int int64_entries = src->count_of_int64_entries();
  int code_ptr_entries = src->count_of_code_ptr_entries();
  int heap_ptr_entries = src->count_of_heap_ptr_entries();
  int int32_entries = src->count_of_int32_entries();
  HeapObject* obj;
  { AllocationResult allocation =
        AllocateConstantPoolArray(int64_entries, code_ptr_entries,
                                  heap_ptr_entries, int32_entries);
    if (!allocation.To(&obj)) return allocation;
  }
  obj->set_map_no_write_barrier(map);
  int size = ConstantPoolArray::SizeFor(
        int64_entries, code_ptr_entries, heap_ptr_entries, int32_entries);
  CopyBlock(
      obj->address() + ConstantPoolArray::kLengthOffset,
      src->address() + ConstantPoolArray::kLengthOffset,
      size - ConstantPoolArray::kLengthOffset);
  return obj;
}


AllocationResult Heap::AllocateRawFixedArray(int length,
                                             PretenureFlag pretenure) {
  if (length < 0 || length > FixedArray::kMaxLength) {
    v8::internal::Heap::FatalProcessOutOfMemory("invalid array length", true);
  }
  int size = FixedArray::SizeFor(length);
  AllocationSpace space = SelectSpace(size, OLD_POINTER_SPACE, pretenure);

  return AllocateRaw(size, space, OLD_POINTER_SPACE);
}


AllocationResult Heap::AllocateFixedArrayWithFiller(int length,
                                                    PretenureFlag pretenure,
                                                    Object* filler) {
  ASSERT(length >= 0);
  ASSERT(empty_fixed_array()->IsFixedArray());
  if (length == 0) return empty_fixed_array();

  ASSERT(!InNewSpace(filler));
  HeapObject* result;
  { AllocationResult allocation = AllocateRawFixedArray(length, pretenure);
    if (!allocation.To(&result)) return allocation;
  }

  result->set_map_no_write_barrier(fixed_array_map());
  FixedArray* array = FixedArray::cast(result);
  array->set_length(length);
  MemsetPointer(array->data_start(), filler, length);
  return array;
}


AllocationResult Heap::AllocateFixedArray(int length, PretenureFlag pretenure) {
  return AllocateFixedArrayWithFiller(length, pretenure, undefined_value());
}


AllocationResult Heap::AllocateUninitializedFixedArray(int length) {
  if (length == 0) return empty_fixed_array();

  HeapObject* obj;
  { AllocationResult allocation = AllocateRawFixedArray(length, NOT_TENURED);
    if (!allocation.To(&obj)) return allocation;
  }

  obj->set_map_no_write_barrier(fixed_array_map());
  FixedArray::cast(obj)->set_length(length);
  return obj;
}


AllocationResult Heap::AllocateUninitializedFixedDoubleArray(
    int length,
    PretenureFlag pretenure) {
  if (length == 0) return empty_fixed_array();

  HeapObject* elements;
  AllocationResult allocation = AllocateRawFixedDoubleArray(length, pretenure);
  if (!allocation.To(&elements)) return allocation;

  elements->set_map_no_write_barrier(fixed_double_array_map());
  FixedDoubleArray::cast(elements)->set_length(length);
  return elements;
}


AllocationResult Heap::AllocateRawFixedDoubleArray(int length,
                                                   PretenureFlag pretenure) {
  if (length < 0 || length > FixedDoubleArray::kMaxLength) {
    v8::internal::Heap::FatalProcessOutOfMemory("invalid array length", true);
  }
  int size = FixedDoubleArray::SizeFor(length);
#ifndef V8_HOST_ARCH_64_BIT
  size += kPointerSize;
#endif
  AllocationSpace space = SelectSpace(size, OLD_DATA_SPACE, pretenure);

  HeapObject* object;
  { AllocationResult allocation = AllocateRaw(size, space, OLD_DATA_SPACE);
    if (!allocation.To(&object)) return allocation;
  }

  return EnsureDoubleAligned(this, object, size);
}


AllocationResult Heap::AllocateConstantPoolArray(int number_of_int64_entries,
                                                 int number_of_code_ptr_entries,
                                                 int number_of_heap_ptr_entries,
                                                 int number_of_int32_entries) {
  CHECK(number_of_int64_entries >= 0 &&
        number_of_int64_entries <= ConstantPoolArray::kMaxEntriesPerType &&
        number_of_code_ptr_entries >= 0 &&
        number_of_code_ptr_entries <= ConstantPoolArray::kMaxEntriesPerType &&
        number_of_heap_ptr_entries >= 0 &&
        number_of_heap_ptr_entries <= ConstantPoolArray::kMaxEntriesPerType &&
        number_of_int32_entries >= 0 &&
        number_of_int32_entries <= ConstantPoolArray::kMaxEntriesPerType);
  int size = ConstantPoolArray::SizeFor(number_of_int64_entries,
                                        number_of_code_ptr_entries,
                                        number_of_heap_ptr_entries,
                                        number_of_int32_entries);
#ifndef V8_HOST_ARCH_64_BIT
  size += kPointerSize;
#endif
  AllocationSpace space = SelectSpace(size, OLD_POINTER_SPACE, TENURED);

  HeapObject* object;
  { AllocationResult allocation = AllocateRaw(size, space, OLD_POINTER_SPACE);
    if (!allocation.To(&object)) return allocation;
  }
  object = EnsureDoubleAligned(this, object, size);
  object->set_map_no_write_barrier(constant_pool_array_map());

  ConstantPoolArray* constant_pool = ConstantPoolArray::cast(object);
  constant_pool->Init(number_of_int64_entries,
                      number_of_code_ptr_entries,
                      number_of_heap_ptr_entries,
                      number_of_int32_entries);
  if (number_of_code_ptr_entries > 0) {
    int offset =
        constant_pool->OffsetOfElementAt(constant_pool->first_code_ptr_index());
    MemsetPointer(
        reinterpret_cast<Address*>(HeapObject::RawField(constant_pool, offset)),
        isolate()->builtins()->builtin(Builtins::kIllegal)->entry(),
        number_of_code_ptr_entries);
  }
  if (number_of_heap_ptr_entries > 0) {
    int offset =
        constant_pool->OffsetOfElementAt(constant_pool->first_heap_ptr_index());
    MemsetPointer(
        HeapObject::RawField(constant_pool, offset),
        undefined_value(),
        number_of_heap_ptr_entries);
  }
  return constant_pool;
}


AllocationResult Heap::AllocateEmptyConstantPoolArray() {
  int size = ConstantPoolArray::SizeFor(0, 0, 0, 0);
  HeapObject* result;
  { AllocationResult allocation =
        AllocateRaw(size, OLD_DATA_SPACE, OLD_DATA_SPACE);
    if (!allocation.To(&result)) return allocation;
  }
  result->set_map_no_write_barrier(constant_pool_array_map());
  ConstantPoolArray::cast(result)->Init(0, 0, 0, 0);
  return result;
}


AllocationResult Heap::AllocateSymbol() {
  // Statically ensure that it is safe to allocate symbols in paged spaces.
  STATIC_ASSERT(Symbol::kSize <= Page::kMaxRegularHeapObjectSize);

  HeapObject* result;
  AllocationResult allocation =
      AllocateRaw(Symbol::kSize, OLD_POINTER_SPACE, OLD_POINTER_SPACE);
  if (!allocation.To(&result)) return allocation;

  result->set_map_no_write_barrier(symbol_map());

  // Generate a random hash value.
  int hash;
  int attempts = 0;
  do {
    hash = isolate()->random_number_generator()->NextInt() & Name::kHashBitMask;
    attempts++;
  } while (hash == 0 && attempts < 30);
  if (hash == 0) hash = 1;  // never return 0

  Symbol::cast(result)->set_hash_field(
      Name::kIsNotArrayIndexMask | (hash << Name::kHashShift));
  Symbol::cast(result)->set_name(undefined_value());
  Symbol::cast(result)->set_flags(Smi::FromInt(0));

  ASSERT(!Symbol::cast(result)->is_private());
  return result;
}


AllocationResult Heap::AllocateStruct(InstanceType type) {
  Map* map;
  switch (type) {
#define MAKE_CASE(NAME, Name, name) \
    case NAME##_TYPE: map = name##_map(); break;
STRUCT_LIST(MAKE_CASE)
#undef MAKE_CASE
    default:
      UNREACHABLE();
      return exception();
  }
  int size = map->instance_size();
  AllocationSpace space = SelectSpace(size, OLD_POINTER_SPACE, TENURED);
  Struct* result;
  { AllocationResult allocation = Allocate(map, space);
    if (!allocation.To(&result)) return allocation;
  }
  result->InitializeBody(size);
  return result;
}


bool Heap::IsHeapIterable() {
  return (!old_pointer_space()->was_swept_conservatively() &&
          !old_data_space()->was_swept_conservatively());
}


void Heap::EnsureHeapIsIterable() {
  ASSERT(AllowHeapAllocation::IsAllowed());
  if (!IsHeapIterable()) {
    CollectAllGarbage(kMakeHeapIterableMask, "Heap::EnsureHeapIsIterable");
  }
  ASSERT(IsHeapIterable());
}


void Heap::AdvanceIdleIncrementalMarking(intptr_t step_size) {
  incremental_marking()->Step(step_size,
                              IncrementalMarking::NO_GC_VIA_STACK_GUARD);

  if (incremental_marking()->IsComplete()) {
    bool uncommit = false;
    if (gc_count_at_last_idle_gc_ == gc_count_) {
      // No GC since the last full GC, the mutator is probably not active.
      isolate_->compilation_cache()->Clear();
      uncommit = true;
    }
    CollectAllGarbage(kNoGCFlags, "idle notification: finalize incremental");
    mark_sweeps_since_idle_round_started_++;
    gc_count_at_last_idle_gc_ = gc_count_;
    if (uncommit) {
      new_space_.Shrink();
      UncommitFromSpace();
    }
  }
}


bool Heap::IdleNotification(int hint) {
  // Hints greater than this value indicate that
  // the embedder is requesting a lot of GC work.
  const int kMaxHint = 1000;
  const int kMinHintForIncrementalMarking = 10;
  // Minimal hint that allows to do full GC.
  const int kMinHintForFullGC = 100;
  intptr_t size_factor = Min(Max(hint, 20), kMaxHint) / 4;
  // The size factor is in range [5..250]. The numbers here are chosen from
  // experiments. If you changes them, make sure to test with
  // chrome/performance_ui_tests --gtest_filter="GeneralMixMemoryTest.*
  intptr_t step_size =
      size_factor * IncrementalMarking::kAllocatedThreshold;

  if (contexts_disposed_ > 0) {
    contexts_disposed_ = 0;
    int mark_sweep_time = Min(TimeMarkSweepWouldTakeInMs(), 1000);
    if (hint >= mark_sweep_time && !FLAG_expose_gc &&
        incremental_marking()->IsStopped()) {
      HistogramTimerScope scope(isolate_->counters()->gc_context());
      CollectAllGarbage(kReduceMemoryFootprintMask,
                        "idle notification: contexts disposed");
    } else {
      AdvanceIdleIncrementalMarking(step_size);
    }

    // After context disposal there is likely a lot of garbage remaining, reset
    // the idle notification counters in order to trigger more incremental GCs
    // on subsequent idle notifications.
    StartIdleRound();
    return false;
  }

  if (!FLAG_incremental_marking || Serializer::enabled(isolate_)) {
    return IdleGlobalGC();
  }

  // By doing small chunks of GC work in each IdleNotification,
  // perform a round of incremental GCs and after that wait until
  // the mutator creates enough garbage to justify a new round.
  // An incremental GC progresses as follows:
  // 1. many incremental marking steps,
  // 2. one old space mark-sweep-compact,
  // Use mark-sweep-compact events to count incremental GCs in a round.

  if (mark_sweeps_since_idle_round_started_ >= kMaxMarkSweepsInIdleRound) {
    if (EnoughGarbageSinceLastIdleRound()) {
      StartIdleRound();
    } else {
      return true;
    }
  }

  int remaining_mark_sweeps = kMaxMarkSweepsInIdleRound -
                              mark_sweeps_since_idle_round_started_;

  if (incremental_marking()->IsStopped()) {
    // If there are no more than two GCs left in this idle round and we are
    // allowed to do a full GC, then make those GCs full in order to compact
    // the code space.
    // TODO(ulan): Once we enable code compaction for incremental marking,
    // we can get rid of this special case and always start incremental marking.
    if (remaining_mark_sweeps <= 2 && hint >= kMinHintForFullGC) {
      CollectAllGarbage(kReduceMemoryFootprintMask,
                        "idle notification: finalize idle round");
      mark_sweeps_since_idle_round_started_++;
    } else if (hint > kMinHintForIncrementalMarking) {
      incremental_marking()->Start();
    }
  }
  if (!incremental_marking()->IsStopped() &&
      hint > kMinHintForIncrementalMarking) {
    AdvanceIdleIncrementalMarking(step_size);
  }

  if (mark_sweeps_since_idle_round_started_ >= kMaxMarkSweepsInIdleRound) {
    FinishIdleRound();
    return true;
  }

  // If the IdleNotifcation is called with a large hint we will wait for
  // the sweepter threads here.
  if (hint >= kMinHintForFullGC &&
      mark_compact_collector()->IsConcurrentSweepingInProgress()) {
    mark_compact_collector()->WaitUntilSweepingCompleted();
  }

  return false;
}


bool Heap::IdleGlobalGC() {
  static const int kIdlesBeforeScavenge = 4;
  static const int kIdlesBeforeMarkSweep = 7;
  static const int kIdlesBeforeMarkCompact = 8;
  static const int kMaxIdleCount = kIdlesBeforeMarkCompact + 1;
  static const unsigned int kGCsBetweenCleanup = 4;

  if (!last_idle_notification_gc_count_init_) {
    last_idle_notification_gc_count_ = gc_count_;
    last_idle_notification_gc_count_init_ = true;
  }

  bool uncommit = true;
  bool finished = false;

  // Reset the number of idle notifications received when a number of
  // GCs have taken place. This allows another round of cleanup based
  // on idle notifications if enough work has been carried out to
  // provoke a number of garbage collections.
  if (gc_count_ - last_idle_notification_gc_count_ < kGCsBetweenCleanup) {
    number_idle_notifications_ =
        Min(number_idle_notifications_ + 1, kMaxIdleCount);
  } else {
    number_idle_notifications_ = 0;
    last_idle_notification_gc_count_ = gc_count_;
  }

  if (number_idle_notifications_ == kIdlesBeforeScavenge) {
    CollectGarbage(NEW_SPACE, "idle notification");
    new_space_.Shrink();
    last_idle_notification_gc_count_ = gc_count_;
  } else if (number_idle_notifications_ == kIdlesBeforeMarkSweep) {
    // Before doing the mark-sweep collections we clear the
    // compilation cache to avoid hanging on to source code and
    // generated code for cached functions.
    isolate_->compilation_cache()->Clear();

    CollectAllGarbage(kReduceMemoryFootprintMask, "idle notification");
    new_space_.Shrink();
    last_idle_notification_gc_count_ = gc_count_;

  } else if (number_idle_notifications_ == kIdlesBeforeMarkCompact) {
    CollectAllGarbage(kReduceMemoryFootprintMask, "idle notification");
    new_space_.Shrink();
    last_idle_notification_gc_count_ = gc_count_;
    number_idle_notifications_ = 0;
    finished = true;
  } else if (number_idle_notifications_ > kIdlesBeforeMarkCompact) {
    // If we have received more than kIdlesBeforeMarkCompact idle
    // notifications we do not perform any cleanup because we don't
    // expect to gain much by doing so.
    finished = true;
  }

  if (uncommit) UncommitFromSpace();

  return finished;
}


#ifdef DEBUG

void Heap::Print() {
  if (!HasBeenSetUp()) return;
  isolate()->PrintStack(stdout);
  AllSpaces spaces(this);
  for (Space* space = spaces.next(); space != NULL; space = spaces.next()) {
    space->Print();
  }
}


void Heap::ReportCodeStatistics(const char* title) {
  PrintF(">>>>>> Code Stats (%s) >>>>>>\n", title);
  PagedSpace::ResetCodeStatistics(isolate());
  // We do not look for code in new space, map space, or old space.  If code
  // somehow ends up in those spaces, we would miss it here.
  code_space_->CollectCodeStatistics();
  lo_space_->CollectCodeStatistics();
  PagedSpace::ReportCodeStatistics(isolate());
}


// This function expects that NewSpace's allocated objects histogram is
// populated (via a call to CollectStatistics or else as a side effect of a
// just-completed scavenge collection).
void Heap::ReportHeapStatistics(const char* title) {
  USE(title);
  PrintF(">>>>>> =============== %s (%d) =============== >>>>>>\n",
         title, gc_count_);
  PrintF("old_generation_allocation_limit_ %" V8_PTR_PREFIX "d\n",
         old_generation_allocation_limit_);

  PrintF("\n");
  PrintF("Number of handles : %d\n", HandleScope::NumberOfHandles(isolate_));
  isolate_->global_handles()->PrintStats();
  PrintF("\n");

  PrintF("Heap statistics : ");
  isolate_->memory_allocator()->ReportStatistics();
  PrintF("To space : ");
  new_space_.ReportStatistics();
  PrintF("Old pointer space : ");
  old_pointer_space_->ReportStatistics();
  PrintF("Old data space : ");
  old_data_space_->ReportStatistics();
  PrintF("Code space : ");
  code_space_->ReportStatistics();
  PrintF("Map space : ");
  map_space_->ReportStatistics();
  PrintF("Cell space : ");
  cell_space_->ReportStatistics();
  PrintF("PropertyCell space : ");
  property_cell_space_->ReportStatistics();
  PrintF("Large object space : ");
  lo_space_->ReportStatistics();
  PrintF(">>>>>> ========================================= >>>>>>\n");
}

#endif  // DEBUG

bool Heap::Contains(HeapObject* value) {
  return Contains(value->address());
}


bool Heap::Contains(Address addr) {
  if (isolate_->memory_allocator()->IsOutsideAllocatedSpace(addr)) return false;
  return HasBeenSetUp() &&
    (new_space_.ToSpaceContains(addr) ||
     old_pointer_space_->Contains(addr) ||
     old_data_space_->Contains(addr) ||
     code_space_->Contains(addr) ||
     map_space_->Contains(addr) ||
     cell_space_->Contains(addr) ||
     property_cell_space_->Contains(addr) ||
     lo_space_->SlowContains(addr));
}


bool Heap::InSpace(HeapObject* value, AllocationSpace space) {
  return InSpace(value->address(), space);
}


bool Heap::InSpace(Address addr, AllocationSpace space) {
  if (isolate_->memory_allocator()->IsOutsideAllocatedSpace(addr)) return false;
  if (!HasBeenSetUp()) return false;

  switch (space) {
    case NEW_SPACE:
      return new_space_.ToSpaceContains(addr);
    case OLD_POINTER_SPACE:
      return old_pointer_space_->Contains(addr);
    case OLD_DATA_SPACE:
      return old_data_space_->Contains(addr);
    case CODE_SPACE:
      return code_space_->Contains(addr);
    case MAP_SPACE:
      return map_space_->Contains(addr);
    case CELL_SPACE:
      return cell_space_->Contains(addr);
    case PROPERTY_CELL_SPACE:
      return property_cell_space_->Contains(addr);
    case LO_SPACE:
      return lo_space_->SlowContains(addr);
    case INVALID_SPACE:
      break;
  }
  UNREACHABLE();
  return false;
}


#ifdef VERIFY_HEAP
void Heap::Verify() {
  CHECK(HasBeenSetUp());
  HandleScope scope(isolate());

  store_buffer()->Verify();

  VerifyPointersVisitor visitor;
  IterateRoots(&visitor, VISIT_ONLY_STRONG);

  VerifySmisVisitor smis_visitor;
  IterateSmiRoots(&smis_visitor);

  new_space_.Verify();

  old_pointer_space_->Verify(&visitor);
  map_space_->Verify(&visitor);

  VerifyPointersVisitor no_dirty_regions_visitor;
  old_data_space_->Verify(&no_dirty_regions_visitor);
  code_space_->Verify(&no_dirty_regions_visitor);
  cell_space_->Verify(&no_dirty_regions_visitor);
  property_cell_space_->Verify(&no_dirty_regions_visitor);

  lo_space_->Verify();
}
#endif


void Heap::ZapFromSpace() {
  NewSpacePageIterator it(new_space_.FromSpaceStart(),
                          new_space_.FromSpaceEnd());
  while (it.has_next()) {
    NewSpacePage* page = it.next();
    for (Address cursor = page->area_start(), limit = page->area_end();
         cursor < limit;
         cursor += kPointerSize) {
      Memory::Address_at(cursor) = kFromSpaceZapValue;
    }
  }
}


void Heap::IterateAndMarkPointersToFromSpace(Address start,
                                             Address end,
                                             ObjectSlotCallback callback) {
  Address slot_address = start;

  // We are not collecting slots on new space objects during mutation
  // thus we have to scan for pointers to evacuation candidates when we
  // promote objects. But we should not record any slots in non-black
  // objects. Grey object's slots would be rescanned.
  // White object might not survive until the end of collection
  // it would be a violation of the invariant to record it's slots.
  bool record_slots = false;
  if (incremental_marking()->IsCompacting()) {
    MarkBit mark_bit = Marking::MarkBitFrom(HeapObject::FromAddress(start));
    record_slots = Marking::IsBlack(mark_bit);
  }

  while (slot_address < end) {
    Object** slot = reinterpret_cast<Object**>(slot_address);
    Object* object = *slot;
    // If the store buffer becomes overfull we mark pages as being exempt from
    // the store buffer.  These pages are scanned to find pointers that point
    // to the new space.  In that case we may hit newly promoted objects and
    // fix the pointers before the promotion queue gets to them.  Thus the 'if'.
    if (object->IsHeapObject()) {
      if (Heap::InFromSpace(object)) {
        callback(reinterpret_cast<HeapObject**>(slot),
                 HeapObject::cast(object));
        Object* new_object = *slot;
        if (InNewSpace(new_object)) {
          SLOW_ASSERT(Heap::InToSpace(new_object));
          SLOW_ASSERT(new_object->IsHeapObject());
          store_buffer_.EnterDirectlyIntoStoreBuffer(
              reinterpret_cast<Address>(slot));
        }
        SLOW_ASSERT(!MarkCompactCollector::IsOnEvacuationCandidate(new_object));
      } else if (record_slots &&
                 MarkCompactCollector::IsOnEvacuationCandidate(object)) {
        mark_compact_collector()->RecordSlot(slot, slot, object);
      }
    }
    slot_address += kPointerSize;
  }
}


#ifdef DEBUG
typedef bool (*CheckStoreBufferFilter)(Object** addr);


bool IsAMapPointerAddress(Object** addr) {
  uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  int mod = a % Map::kSize;
  return mod >= Map::kPointerFieldsBeginOffset &&
         mod < Map::kPointerFieldsEndOffset;
}


bool EverythingsAPointer(Object** addr) {
  return true;
}


static void CheckStoreBuffer(Heap* heap,
                             Object** current,
                             Object** limit,
                             Object**** store_buffer_position,
                             Object*** store_buffer_top,
                             CheckStoreBufferFilter filter,
                             Address special_garbage_start,
                             Address special_garbage_end) {
  Map* free_space_map = heap->free_space_map();
  for ( ; current < limit; current++) {
    Object* o = *current;
    Address current_address = reinterpret_cast<Address>(current);
    // Skip free space.
    if (o == free_space_map) {
      Address current_address = reinterpret_cast<Address>(current);
      FreeSpace* free_space =
          FreeSpace::cast(HeapObject::FromAddress(current_address));
      int skip = free_space->Size();
      ASSERT(current_address + skip <= reinterpret_cast<Address>(limit));
      ASSERT(skip > 0);
      current_address += skip - kPointerSize;
      current = reinterpret_cast<Object**>(current_address);
      continue;
    }
    // Skip the current linear allocation space between top and limit which is
    // unmarked with the free space map, but can contain junk.
    if (current_address == special_garbage_start &&
        special_garbage_end != special_garbage_start) {
      current_address = special_garbage_end - kPointerSize;
      current = reinterpret_cast<Object**>(current_address);
      continue;
    }
    if (!(*filter)(current)) continue;
    ASSERT(current_address < special_garbage_start ||
           current_address >= special_garbage_end);
    ASSERT(reinterpret_cast<uintptr_t>(o) != kFreeListZapValue);
    // We have to check that the pointer does not point into new space
    // without trying to cast it to a heap object since the hash field of
    // a string can contain values like 1 and 3 which are tagged null
    // pointers.
    if (!heap->InNewSpace(o)) continue;
    while (**store_buffer_position < current &&
           *store_buffer_position < store_buffer_top) {
      (*store_buffer_position)++;
    }
    if (**store_buffer_position != current ||
        *store_buffer_position == store_buffer_top) {
      Object** obj_start = current;
      while (!(*obj_start)->IsMap()) obj_start--;
      UNREACHABLE();
    }
  }
}


// Check that the store buffer contains all intergenerational pointers by
// scanning a page and ensuring that all pointers to young space are in the
// store buffer.
void Heap::OldPointerSpaceCheckStoreBuffer() {
  OldSpace* space = old_pointer_space();
  PageIterator pages(space);

  store_buffer()->SortUniq();

  while (pages.has_next()) {
    Page* page = pages.next();
    Object** current = reinterpret_cast<Object**>(page->area_start());

    Address end = page->area_end();

    Object*** store_buffer_position = store_buffer()->Start();
    Object*** store_buffer_top = store_buffer()->Top();

    Object** limit = reinterpret_cast<Object**>(end);
    CheckStoreBuffer(this,
                     current,
                     limit,
                     &store_buffer_position,
                     store_buffer_top,
                     &EverythingsAPointer,
                     space->top(),
                     space->limit());
  }
}


void Heap::MapSpaceCheckStoreBuffer() {
  MapSpace* space = map_space();
  PageIterator pages(space);

  store_buffer()->SortUniq();

  while (pages.has_next()) {
    Page* page = pages.next();
    Object** current = reinterpret_cast<Object**>(page->area_start());

    Address end = page->area_end();

    Object*** store_buffer_position = store_buffer()->Start();
    Object*** store_buffer_top = store_buffer()->Top();

    Object** limit = reinterpret_cast<Object**>(end);
    CheckStoreBuffer(this,
                     current,
                     limit,
                     &store_buffer_position,
                     store_buffer_top,
                     &IsAMapPointerAddress,
                     space->top(),
                     space->limit());
  }
}


void Heap::LargeObjectSpaceCheckStoreBuffer() {
  LargeObjectIterator it(lo_space());
  for (HeapObject* object = it.Next(); object != NULL; object = it.Next()) {
    // We only have code, sequential strings, or fixed arrays in large
    // object space, and only fixed arrays can possibly contain pointers to
    // the young generation.
    if (object->IsFixedArray()) {
      Object*** store_buffer_position = store_buffer()->Start();
      Object*** store_buffer_top = store_buffer()->Top();
      Object** current = reinterpret_cast<Object**>(object->address());
      Object** limit =
          reinterpret_cast<Object**>(object->address() + object->Size());
      CheckStoreBuffer(this,
                       current,
                       limit,
                       &store_buffer_position,
                       store_buffer_top,
                       &EverythingsAPointer,
                       NULL,
                       NULL);
    }
  }
}
#endif


void Heap::IterateRoots(ObjectVisitor* v, VisitMode mode) {
  IterateStrongRoots(v, mode);
  IterateWeakRoots(v, mode);
}


void Heap::IterateWeakRoots(ObjectVisitor* v, VisitMode mode) {
  v->VisitPointer(reinterpret_cast<Object**>(&roots_[kStringTableRootIndex]));
  v->Synchronize(VisitorSynchronization::kStringTable);
  if (mode != VISIT_ALL_IN_SCAVENGE &&
      mode != VISIT_ALL_IN_SWEEP_NEWSPACE) {
    // Scavenge collections have special processing for this.
    external_string_table_.Iterate(v);
  }
  v->Synchronize(VisitorSynchronization::kExternalStringsTable);
}


void Heap::IterateSmiRoots(ObjectVisitor* v) {
  // Acquire execution access since we are going to read stack limit values.
  ExecutionAccess access(isolate());
  v->VisitPointers(&roots_[kSmiRootsStart], &roots_[kRootListLength]);
  v->Synchronize(VisitorSynchronization::kSmiRootList);
}


void Heap::IterateStrongRoots(ObjectVisitor* v, VisitMode mode) {
  v->VisitPointers(&roots_[0], &roots_[kStrongRootListLength]);
  v->Synchronize(VisitorSynchronization::kStrongRootList);

  v->VisitPointer(BitCast<Object**>(&hidden_string_));
  v->Synchronize(VisitorSynchronization::kInternalizedString);

  isolate_->bootstrapper()->Iterate(v);
  v->Synchronize(VisitorSynchronization::kBootstrapper);
  isolate_->Iterate(v);
  v->Synchronize(VisitorSynchronization::kTop);
  Relocatable::Iterate(isolate_, v);
  v->Synchronize(VisitorSynchronization::kRelocatable);

  if (isolate_->deoptimizer_data() != NULL) {
    isolate_->deoptimizer_data()->Iterate(v);
  }
  v->Synchronize(VisitorSynchronization::kDebug);
  isolate_->compilation_cache()->Iterate(v);
  v->Synchronize(VisitorSynchronization::kCompilationCache);

  // Iterate over local handles in handle scopes.
  isolate_->handle_scope_implementer()->Iterate(v);
  isolate_->IterateDeferredHandles(v);
  v->Synchronize(VisitorSynchronization::kHandleScope);

  // Iterate over the builtin code objects and code stubs in the
  // heap. Note that it is not necessary to iterate over code objects
  // on scavenge collections.
  if (mode != VISIT_ALL_IN_SCAVENGE) {
    isolate_->builtins()->IterateBuiltins(v);
  }
  v->Synchronize(VisitorSynchronization::kBuiltins);

  // Iterate over global handles.
  switch (mode) {
    case VISIT_ONLY_STRONG:
      isolate_->global_handles()->IterateStrongRoots(v);
      break;
    case VISIT_ALL_IN_SCAVENGE:
      isolate_->global_handles()->IterateNewSpaceStrongAndDependentRoots(v);
      break;
    case VISIT_ALL_IN_SWEEP_NEWSPACE:
    case VISIT_ALL:
      isolate_->global_handles()->IterateAllRoots(v);
      break;
  }
  v->Synchronize(VisitorSynchronization::kGlobalHandles);

  // Iterate over eternal handles.
  if (mode == VISIT_ALL_IN_SCAVENGE) {
    isolate_->eternal_handles()->IterateNewSpaceRoots(v);
  } else {
    isolate_->eternal_handles()->IterateAllRoots(v);
  }
  v->Synchronize(VisitorSynchronization::kEternalHandles);

  // Iterate over pointers being held by inactive threads.
  isolate_->thread_manager()->Iterate(v);
  v->Synchronize(VisitorSynchronization::kThreadManager);

  // Iterate over the pointers the Serialization/Deserialization code is
  // holding.
  // During garbage collection this keeps the partial snapshot cache alive.
  // During deserialization of the startup snapshot this creates the partial
  // snapshot cache and deserializes the objects it refers to.  During
  // serialization this does nothing, since the partial snapshot cache is
  // empty.  However the next thing we do is create the partial snapshot,
  // filling up the partial snapshot cache with objects it needs as we go.
  SerializerDeserializer::Iterate(isolate_, v);
  // We don't do a v->Synchronize call here, because in debug mode that will
  // output a flag to the snapshot.  However at this point the serializer and
  // deserializer are deliberately a little unsynchronized (see above) so the
  // checking of the sync flag in the snapshot would fail.
}


// TODO(1236194): Since the heap size is configurable on the command line
// and through the API, we should gracefully handle the case that the heap
// size is not big enough to fit all the initial objects.
bool Heap::ConfigureHeap(int max_semispace_size,
                         intptr_t max_old_space_size,
                         intptr_t max_executable_size,
                         intptr_t code_range_size) {
  if (HasBeenSetUp()) return false;

  // If max space size flags are specified overwrite the configuration.
  if (FLAG_max_new_space_size > 0) {
    max_semispace_size = (FLAG_max_new_space_size / 2) * kLumpOfMemory;
  }
  if (FLAG_max_old_space_size > 0) {
    max_old_space_size = FLAG_max_old_space_size * kLumpOfMemory;
  }
  if (FLAG_max_executable_size > 0) {
    max_executable_size = FLAG_max_executable_size * kLumpOfMemory;
  }

  if (FLAG_stress_compaction) {
    // This will cause more frequent GCs when stressing.
    max_semispace_size_ = Page::kPageSize;
  }

  if (max_semispace_size > 0) {
    if (max_semispace_size < Page::kPageSize) {
      max_semispace_size = Page::kPageSize;
      if (FLAG_trace_gc) {
        PrintPID("Max semispace size cannot be less than %dkbytes\n",
                 Page::kPageSize >> 10);
      }
    }
    max_semispace_size_ = max_semispace_size;
  }

  if (Snapshot::IsEnabled()) {
    // If we are using a snapshot we always reserve the default amount
    // of memory for each semispace because code in the snapshot has
    // write-barrier code that relies on the size and alignment of new
    // space.  We therefore cannot use a larger max semispace size
    // than the default reserved semispace size.
    if (max_semispace_size_ > reserved_semispace_size_) {
      max_semispace_size_ = reserved_semispace_size_;
      if (FLAG_trace_gc) {
        PrintPID("Max semispace size cannot be more than %dkbytes\n",
                 reserved_semispace_size_ >> 10);
      }
    }
  } else {
    // If we are not using snapshots we reserve space for the actual
    // max semispace size.
    reserved_semispace_size_ = max_semispace_size_;
  }

  if (max_old_space_size > 0) max_old_generation_size_ = max_old_space_size;
  if (max_executable_size > 0) {
    max_executable_size_ = RoundUp(max_executable_size, Page::kPageSize);
  }

  // The max executable size must be less than or equal to the max old
  // generation size.
  if (max_executable_size_ > max_old_generation_size_) {
    max_executable_size_ = max_old_generation_size_;
  }

  // The new space size must be a power of two to support single-bit testing
  // for containment.
  max_semispace_size_ = RoundUpToPowerOf2(max_semispace_size_);
  reserved_semispace_size_ = RoundUpToPowerOf2(reserved_semispace_size_);
  initial_semispace_size_ = Min(initial_semispace_size_, max_semispace_size_);

  // The external allocation limit should be below 256 MB on all architectures
  // to avoid unnecessary low memory notifications, as that is the threshold
  // for some embedders.
  external_allocation_limit_ = 12 * max_semispace_size_;
  ASSERT(external_allocation_limit_ <= 256 * MB);

  // The old generation is paged and needs at least one page for each space.
  int paged_space_count = LAST_PAGED_SPACE - FIRST_PAGED_SPACE + 1;
  max_old_generation_size_ = Max(static_cast<intptr_t>(paged_space_count *
                                                       Page::kPageSize),
                                 RoundUp(max_old_generation_size_,
                                         Page::kPageSize));

  // We rely on being able to allocate new arrays in paged spaces.
  ASSERT(Page::kMaxRegularHeapObjectSize >=
         (JSArray::kSize +
          FixedArray::SizeFor(JSObject::kInitialMaxFastElementArray) +
          AllocationMemento::kSize));

  code_range_size_ = code_range_size;

  // We set the old generation growing factor to 2 to grow the heap slower on
  // memory-constrained devices.
  if (max_old_generation_size_ <= kMaxOldSpaceSizeMediumMemoryDevice) {
    old_space_growing_factor_ = 2;
  }

  configured_ = true;
  return true;
}


bool Heap::ConfigureHeapDefault() {
  return ConfigureHeap(static_cast<intptr_t>(FLAG_max_new_space_size / 2) * KB,
                       static_cast<intptr_t>(FLAG_max_old_space_size) * MB,
                       static_cast<intptr_t>(FLAG_max_executable_size) * MB,
                       static_cast<intptr_t>(0));
}


void Heap::RecordStats(HeapStats* stats, bool take_snapshot) {
  *stats->start_marker = HeapStats::kStartMarker;
  *stats->end_marker = HeapStats::kEndMarker;
  *stats->new_space_size = new_space_.SizeAsInt();
  *stats->new_space_capacity = static_cast<int>(new_space_.Capacity());
  *stats->old_pointer_space_size = old_pointer_space_->SizeOfObjects();
  *stats->old_pointer_space_capacity = old_pointer_space_->Capacity();
  *stats->old_data_space_size = old_data_space_->SizeOfObjects();
  *stats->old_data_space_capacity = old_data_space_->Capacity();
  *stats->code_space_size = code_space_->SizeOfObjects();
  *stats->code_space_capacity = code_space_->Capacity();
  *stats->map_space_size = map_space_->SizeOfObjects();
  *stats->map_space_capacity = map_space_->Capacity();
  *stats->cell_space_size = cell_space_->SizeOfObjects();
  *stats->cell_space_capacity = cell_space_->Capacity();
  *stats->property_cell_space_size = property_cell_space_->SizeOfObjects();
  *stats->property_cell_space_capacity = property_cell_space_->Capacity();
  *stats->lo_space_size = lo_space_->Size();
  isolate_->global_handles()->RecordStats(stats);
  *stats->memory_allocator_size = isolate()->memory_allocator()->Size();
  *stats->memory_allocator_capacity =
      isolate()->memory_allocator()->Size() +
      isolate()->memory_allocator()->Available();
  *stats->os_error = OS::GetLastError();
      isolate()->memory_allocator()->Available();
  if (take_snapshot) {
    HeapIterator iterator(this);
    for (HeapObject* obj = iterator.next();
         obj != NULL;
         obj = iterator.next()) {
      InstanceType type = obj->map()->instance_type();
      ASSERT(0 <= type && type <= LAST_TYPE);
      stats->objects_per_type[type]++;
      stats->size_per_type[type] += obj->Size();
    }
  }
}


intptr_t Heap::PromotedSpaceSizeOfObjects() {
  return old_pointer_space_->SizeOfObjects()
      + old_data_space_->SizeOfObjects()
      + code_space_->SizeOfObjects()
      + map_space_->SizeOfObjects()
      + cell_space_->SizeOfObjects()
      + property_cell_space_->SizeOfObjects()
      + lo_space_->SizeOfObjects();
}


int64_t Heap::PromotedExternalMemorySize() {
  if (amount_of_external_allocated_memory_
      <= amount_of_external_allocated_memory_at_last_global_gc_) return 0;
  return amount_of_external_allocated_memory_
      - amount_of_external_allocated_memory_at_last_global_gc_;
}


void Heap::EnableInlineAllocation() {
  if (!inline_allocation_disabled_) return;
  inline_allocation_disabled_ = false;

  // Update inline allocation limit for new space.
  new_space()->UpdateInlineAllocationLimit(0);
}


void Heap::DisableInlineAllocation() {
  if (inline_allocation_disabled_) return;
  inline_allocation_disabled_ = true;

  // Update inline allocation limit for new space.
  new_space()->UpdateInlineAllocationLimit(0);

  // Update inline allocation limit for old spaces.
  PagedSpaces spaces(this);
  for (PagedSpace* space = spaces.next();
       space != NULL;
       space = spaces.next()) {
    space->EmptyAllocationInfo();
  }
}


V8_DECLARE_ONCE(initialize_gc_once);

static void InitializeGCOnce() {
  InitializeScavengingVisitorsTables();
  NewSpaceScavenger::Initialize();
  MarkCompactCollector::Initialize();
}


bool Heap::SetUp() {
#ifdef DEBUG
  allocation_timeout_ = FLAG_gc_interval;
#endif

  // Initialize heap spaces and initial maps and objects. Whenever something
  // goes wrong, just return false. The caller should check the results and
  // call Heap::TearDown() to release allocated memory.
  //
  // If the heap is not yet configured (e.g. through the API), configure it.
  // Configuration is based on the flags new-space-size (really the semispace
  // size) and old-space-size if set or the initial values of semispace_size_
  // and old_generation_size_ otherwise.
  if (!configured_) {
    if (!ConfigureHeapDefault()) return false;
  }

  CallOnce(&initialize_gc_once, &InitializeGCOnce);

  MarkMapPointersAsEncoded(false);

  // Set up memory allocator.
  if (!isolate_->memory_allocator()->SetUp(MaxReserved(), MaxExecutableSize()))
      return false;

  // Set up new space.
  if (!new_space_.SetUp(reserved_semispace_size_, max_semispace_size_)) {
    return false;
  }

  // Initialize old pointer space.
  old_pointer_space_ =
      new OldSpace(this,
                   max_old_generation_size_,
                   OLD_POINTER_SPACE,
                   NOT_EXECUTABLE);
  if (old_pointer_space_ == NULL) return false;
  if (!old_pointer_space_->SetUp()) return false;

  // Initialize old data space.
  old_data_space_ =
      new OldSpace(this,
                   max_old_generation_size_,
                   OLD_DATA_SPACE,
                   NOT_EXECUTABLE);
  if (old_data_space_ == NULL) return false;
  if (!old_data_space_->SetUp()) return false;

  if (!isolate_->code_range()->SetUp(code_range_size_)) return false;

  // Initialize the code space, set its maximum capacity to the old
  // generation size. It needs executable memory.
  code_space_ =
      new OldSpace(this, max_old_generation_size_, CODE_SPACE, EXECUTABLE);
  if (code_space_ == NULL) return false;
  if (!code_space_->SetUp()) return false;

  // Initialize map space.
  map_space_ = new MapSpace(this, max_old_generation_size_, MAP_SPACE);
  if (map_space_ == NULL) return false;
  if (!map_space_->SetUp()) return false;

  // Initialize simple cell space.
  cell_space_ = new CellSpace(this, max_old_generation_size_, CELL_SPACE);
  if (cell_space_ == NULL) return false;
  if (!cell_space_->SetUp()) return false;

  // Initialize global property cell space.
  property_cell_space_ = new PropertyCellSpace(this, max_old_generation_size_,
                                               PROPERTY_CELL_SPACE);
  if (property_cell_space_ == NULL) return false;
  if (!property_cell_space_->SetUp()) return false;

  // The large object code space may contain code or data.  We set the memory
  // to be non-executable here for safety, but this means we need to enable it
  // explicitly when allocating large code objects.
  lo_space_ = new LargeObjectSpace(this, max_old_generation_size_, LO_SPACE);
  if (lo_space_ == NULL) return false;
  if (!lo_space_->SetUp()) return false;

  // Set up the seed that is used to randomize the string hash function.
  ASSERT(hash_seed() == 0);
  if (FLAG_randomize_hashes) {
    if (FLAG_hash_seed == 0) {
      int rnd = isolate()->random_number_generator()->NextInt();
      set_hash_seed(Smi::FromInt(rnd & Name::kHashBitMask));
    } else {
      set_hash_seed(Smi::FromInt(FLAG_hash_seed));
    }
  }

  LOG(isolate_, IntPtrTEvent("heap-capacity", Capacity()));
  LOG(isolate_, IntPtrTEvent("heap-available", Available()));

  store_buffer()->SetUp();

  mark_compact_collector()->SetUp();

  return true;
}


bool Heap::CreateHeapObjects() {
  // Create initial maps.
  if (!CreateInitialMaps()) return false;
  CreateApiObjects();

  // Create initial objects
  CreateInitialObjects();
  CHECK_EQ(0, gc_count_);

  native_contexts_list_ = undefined_value();
  array_buffers_list_ = undefined_value();
  allocation_sites_list_ = undefined_value();
  weak_object_to_code_table_ = undefined_value();
  return true;
}


void Heap::SetStackLimits() {
  ASSERT(isolate_ != NULL);
  ASSERT(isolate_ == isolate());
  // On 64 bit machines, pointers are generally out of range of Smis.  We write
  // something that looks like an out of range Smi to the GC.

  // Set up the special root array entries containing the stack limits.
  // These are actually addresses, but the tag makes the GC ignore it.
  roots_[kStackLimitRootIndex] =
      reinterpret_cast<Object*>(
          (isolate_->stack_guard()->jslimit() & ~kSmiTagMask) | kSmiTag);
  roots_[kRealStackLimitRootIndex] =
      reinterpret_cast<Object*>(
          (isolate_->stack_guard()->real_jslimit() & ~kSmiTagMask) | kSmiTag);
}


void Heap::TearDown() {
#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) {
    Verify();
  }
#endif

  UpdateMaximumCommitted();

  if (FLAG_print_cumulative_gc_stat) {
    PrintF("\n");
    PrintF("gc_count=%d ", gc_count_);
    PrintF("mark_sweep_count=%d ", ms_count_);
    PrintF("max_gc_pause=%.1f ", get_max_gc_pause());
    PrintF("total_gc_time=%.1f ", total_gc_time_ms_);
    PrintF("min_in_mutator=%.1f ", get_min_in_mutator());
    PrintF("max_alive_after_gc=%" V8_PTR_PREFIX "d ",
           get_max_alive_after_gc());
    PrintF("total_marking_time=%.1f ", marking_time());
    PrintF("total_sweeping_time=%.1f ", sweeping_time());
    PrintF("\n\n");
  }

  if (FLAG_print_max_heap_committed) {
    PrintF("\n");
    PrintF("maximum_committed_by_heap=%" V8_PTR_PREFIX "d ",
      MaximumCommittedMemory());
    PrintF("maximum_committed_by_new_space=%" V8_PTR_PREFIX "d ",
      new_space_.MaximumCommittedMemory());
    PrintF("maximum_committed_by_old_pointer_space=%" V8_PTR_PREFIX "d ",
      old_data_space_->MaximumCommittedMemory());
    PrintF("maximum_committed_by_old_data_space=%" V8_PTR_PREFIX "d ",
      old_pointer_space_->MaximumCommittedMemory());
    PrintF("maximum_committed_by_old_data_space=%" V8_PTR_PREFIX "d ",
      old_pointer_space_->MaximumCommittedMemory());
    PrintF("maximum_committed_by_code_space=%" V8_PTR_PREFIX "d ",
      code_space_->MaximumCommittedMemory());
    PrintF("maximum_committed_by_map_space=%" V8_PTR_PREFIX "d ",
      map_space_->MaximumCommittedMemory());
    PrintF("maximum_committed_by_cell_space=%" V8_PTR_PREFIX "d ",
      cell_space_->MaximumCommittedMemory());
    PrintF("maximum_committed_by_property_space=%" V8_PTR_PREFIX "d ",
      property_cell_space_->MaximumCommittedMemory());
    PrintF("maximum_committed_by_lo_space=%" V8_PTR_PREFIX "d ",
      lo_space_->MaximumCommittedMemory());
    PrintF("\n\n");
  }

  TearDownArrayBuffers();

  isolate_->global_handles()->TearDown();

  external_string_table_.TearDown();

  mark_compact_collector()->TearDown();

  new_space_.TearDown();

  if (old_pointer_space_ != NULL) {
    old_pointer_space_->TearDown();
    delete old_pointer_space_;
    old_pointer_space_ = NULL;
  }

  if (old_data_space_ != NULL) {
    old_data_space_->TearDown();
    delete old_data_space_;
    old_data_space_ = NULL;
  }

  if (code_space_ != NULL) {
    code_space_->TearDown();
    delete code_space_;
    code_space_ = NULL;
  }

  if (map_space_ != NULL) {
    map_space_->TearDown();
    delete map_space_;
    map_space_ = NULL;
  }

  if (cell_space_ != NULL) {
    cell_space_->TearDown();
    delete cell_space_;
    cell_space_ = NULL;
  }

  if (property_cell_space_ != NULL) {
    property_cell_space_->TearDown();
    delete property_cell_space_;
    property_cell_space_ = NULL;
  }

  if (lo_space_ != NULL) {
    lo_space_->TearDown();
    delete lo_space_;
    lo_space_ = NULL;
  }

  store_buffer()->TearDown();
  incremental_marking()->TearDown();

  isolate_->memory_allocator()->TearDown();
}


void Heap::AddGCPrologueCallback(v8::Isolate::GCPrologueCallback callback,
                                 GCType gc_type,
                                 bool pass_isolate) {
  ASSERT(callback != NULL);
  GCPrologueCallbackPair pair(callback, gc_type, pass_isolate);
  ASSERT(!gc_prologue_callbacks_.Contains(pair));
  return gc_prologue_callbacks_.Add(pair);
}


void Heap::RemoveGCPrologueCallback(v8::Isolate::GCPrologueCallback callback) {
  ASSERT(callback != NULL);
  for (int i = 0; i < gc_prologue_callbacks_.length(); ++i) {
    if (gc_prologue_callbacks_[i].callback == callback) {
      gc_prologue_callbacks_.Remove(i);
      return;
    }
  }
  UNREACHABLE();
}


void Heap::AddGCEpilogueCallback(v8::Isolate::GCEpilogueCallback callback,
                                 GCType gc_type,
                                 bool pass_isolate) {
  ASSERT(callback != NULL);
  GCEpilogueCallbackPair pair(callback, gc_type, pass_isolate);
  ASSERT(!gc_epilogue_callbacks_.Contains(pair));
  return gc_epilogue_callbacks_.Add(pair);
}


void Heap::RemoveGCEpilogueCallback(v8::Isolate::GCEpilogueCallback callback) {
  ASSERT(callback != NULL);
  for (int i = 0; i < gc_epilogue_callbacks_.length(); ++i) {
    if (gc_epilogue_callbacks_[i].callback == callback) {
      gc_epilogue_callbacks_.Remove(i);
      return;
    }
  }
  UNREACHABLE();
}


// TODO(ishell): Find a better place for this.
void Heap::AddWeakObjectToCodeDependency(Handle<Object> obj,
                                         Handle<DependentCode> dep) {
  ASSERT(!InNewSpace(*obj));
  ASSERT(!InNewSpace(*dep));
  HandleScope scope(isolate());
  Handle<WeakHashTable> table(WeakHashTable::cast(weak_object_to_code_table_),
                              isolate());
  table = WeakHashTable::Put(table, obj, dep);

  if (ShouldZapGarbage() && weak_object_to_code_table_ != *table) {
    WeakHashTable::cast(weak_object_to_code_table_)->Zap(the_hole_value());
  }
  set_weak_object_to_code_table(*table);
  ASSERT_EQ(*dep, table->Lookup(obj));
}


DependentCode* Heap::LookupWeakObjectToCodeDependency(Handle<Object> obj) {
  Object* dep = WeakHashTable::cast(weak_object_to_code_table_)->Lookup(obj);
  if (dep->IsDependentCode()) return DependentCode::cast(dep);
  return DependentCode::cast(empty_fixed_array());
}


void Heap::EnsureWeakObjectToCodeTable() {
  if (!weak_object_to_code_table()->IsHashTable()) {
    set_weak_object_to_code_table(*WeakHashTable::New(
        isolate(), 16, USE_DEFAULT_MINIMUM_CAPACITY, TENURED));
  }
}


void Heap::FatalProcessOutOfMemory(const char* location, bool take_snapshot) {
  v8::internal::V8::FatalProcessOutOfMemory(location, take_snapshot);
}

#ifdef DEBUG

class PrintHandleVisitor: public ObjectVisitor {
 public:
  void VisitPointers(Object** start, Object** end) {
    for (Object** p = start; p < end; p++)
      PrintF("  handle %p to %p\n",
             reinterpret_cast<void*>(p),
             reinterpret_cast<void*>(*p));
  }
};


void Heap::PrintHandles() {
  PrintF("Handles:\n");
  PrintHandleVisitor v;
  isolate_->handle_scope_implementer()->Iterate(&v);
}

#endif


Space* AllSpaces::next() {
  switch (counter_++) {
    case NEW_SPACE:
      return heap_->new_space();
    case OLD_POINTER_SPACE:
      return heap_->old_pointer_space();
    case OLD_DATA_SPACE:
      return heap_->old_data_space();
    case CODE_SPACE:
      return heap_->code_space();
    case MAP_SPACE:
      return heap_->map_space();
    case CELL_SPACE:
      return heap_->cell_space();
    case PROPERTY_CELL_SPACE:
      return heap_->property_cell_space();
    case LO_SPACE:
      return heap_->lo_space();
    default:
      return NULL;
  }
}


PagedSpace* PagedSpaces::next() {
  switch (counter_++) {
    case OLD_POINTER_SPACE:
      return heap_->old_pointer_space();
    case OLD_DATA_SPACE:
      return heap_->old_data_space();
    case CODE_SPACE:
      return heap_->code_space();
    case MAP_SPACE:
      return heap_->map_space();
    case CELL_SPACE:
      return heap_->cell_space();
    case PROPERTY_CELL_SPACE:
      return heap_->property_cell_space();
    default:
      return NULL;
  }
}



OldSpace* OldSpaces::next() {
  switch (counter_++) {
    case OLD_POINTER_SPACE:
      return heap_->old_pointer_space();
    case OLD_DATA_SPACE:
      return heap_->old_data_space();
    case CODE_SPACE:
      return heap_->code_space();
    default:
      return NULL;
  }
}


SpaceIterator::SpaceIterator(Heap* heap)
    : heap_(heap),
      current_space_(FIRST_SPACE),
      iterator_(NULL),
      size_func_(NULL) {
}


SpaceIterator::SpaceIterator(Heap* heap, HeapObjectCallback size_func)
    : heap_(heap),
      current_space_(FIRST_SPACE),
      iterator_(NULL),
      size_func_(size_func) {
}


SpaceIterator::~SpaceIterator() {
  // Delete active iterator if any.
  delete iterator_;
}


bool SpaceIterator::has_next() {
  // Iterate until no more spaces.
  return current_space_ != LAST_SPACE;
}


ObjectIterator* SpaceIterator::next() {
  if (iterator_ != NULL) {
    delete iterator_;
    iterator_ = NULL;
    // Move to the next space
    current_space_++;
    if (current_space_ > LAST_SPACE) {
      return NULL;
    }
  }

  // Return iterator for the new current space.
  return CreateIterator();
}


// Create an iterator for the space to iterate.
ObjectIterator* SpaceIterator::CreateIterator() {
  ASSERT(iterator_ == NULL);

  switch (current_space_) {
    case NEW_SPACE:
      iterator_ = new SemiSpaceIterator(heap_->new_space(), size_func_);
      break;
    case OLD_POINTER_SPACE:
      iterator_ =
          new HeapObjectIterator(heap_->old_pointer_space(), size_func_);
      break;
    case OLD_DATA_SPACE:
      iterator_ = new HeapObjectIterator(heap_->old_data_space(), size_func_);
      break;
    case CODE_SPACE:
      iterator_ = new HeapObjectIterator(heap_->code_space(), size_func_);
      break;
    case MAP_SPACE:
      iterator_ = new HeapObjectIterator(heap_->map_space(), size_func_);
      break;
    case CELL_SPACE:
      iterator_ = new HeapObjectIterator(heap_->cell_space(), size_func_);
      break;
    case PROPERTY_CELL_SPACE:
      iterator_ = new HeapObjectIterator(heap_->property_cell_space(),
                                         size_func_);
      break;
    case LO_SPACE:
      iterator_ = new LargeObjectIterator(heap_->lo_space(), size_func_);
      break;
  }

  // Return the newly allocated iterator;
  ASSERT(iterator_ != NULL);
  return iterator_;
}


class HeapObjectsFilter {
 public:
  virtual ~HeapObjectsFilter() {}
  virtual bool SkipObject(HeapObject* object) = 0;
};


class UnreachableObjectsFilter : public HeapObjectsFilter {
 public:
  explicit UnreachableObjectsFilter(Heap* heap) : heap_(heap) {
    MarkReachableObjects();
  }

  ~UnreachableObjectsFilter() {
    heap_->mark_compact_collector()->ClearMarkbits();
  }

  bool SkipObject(HeapObject* object) {
    MarkBit mark_bit = Marking::MarkBitFrom(object);
    return !mark_bit.Get();
  }

 private:
  class MarkingVisitor : public ObjectVisitor {
   public:
    MarkingVisitor() : marking_stack_(10) {}

    void VisitPointers(Object** start, Object** end) {
      for (Object** p = start; p < end; p++) {
        if (!(*p)->IsHeapObject()) continue;
        HeapObject* obj = HeapObject::cast(*p);
        MarkBit mark_bit = Marking::MarkBitFrom(obj);
        if (!mark_bit.Get()) {
          mark_bit.Set();
          marking_stack_.Add(obj);
        }
      }
    }

    void TransitiveClosure() {
      while (!marking_stack_.is_empty()) {
        HeapObject* obj = marking_stack_.RemoveLast();
        obj->Iterate(this);
      }
    }

   private:
    List<HeapObject*> marking_stack_;
  };

  void MarkReachableObjects() {
    MarkingVisitor visitor;
    heap_->IterateRoots(&visitor, VISIT_ALL);
    visitor.TransitiveClosure();
  }

  Heap* heap_;
  DisallowHeapAllocation no_allocation_;
};


HeapIterator::HeapIterator(Heap* heap)
    : heap_(heap),
      filtering_(HeapIterator::kNoFiltering),
      filter_(NULL) {
  Init();
}


HeapIterator::HeapIterator(Heap* heap,
                           HeapIterator::HeapObjectsFiltering filtering)
    : heap_(heap),
      filtering_(filtering),
      filter_(NULL) {
  Init();
}


HeapIterator::~HeapIterator() {
  Shutdown();
}


void HeapIterator::Init() {
  // Start the iteration.
  space_iterator_ = new SpaceIterator(heap_);
  switch (filtering_) {
    case kFilterUnreachable:
      filter_ = new UnreachableObjectsFilter(heap_);
      break;
    default:
      break;
  }
  object_iterator_ = space_iterator_->next();
}


void HeapIterator::Shutdown() {
#ifdef DEBUG
  // Assert that in filtering mode we have iterated through all
  // objects. Otherwise, heap will be left in an inconsistent state.
  if (filtering_ != kNoFiltering) {
    ASSERT(object_iterator_ == NULL);
  }
#endif
  // Make sure the last iterator is deallocated.
  delete space_iterator_;
  space_iterator_ = NULL;
  object_iterator_ = NULL;
  delete filter_;
  filter_ = NULL;
}


HeapObject* HeapIterator::next() {
  if (filter_ == NULL) return NextObject();

  HeapObject* obj = NextObject();
  while (obj != NULL && filter_->SkipObject(obj)) obj = NextObject();
  return obj;
}


HeapObject* HeapIterator::NextObject() {
  // No iterator means we are done.
  if (object_iterator_ == NULL) return NULL;

  if (HeapObject* obj = object_iterator_->next_object()) {
    // If the current iterator has more objects we are fine.
    return obj;
  } else {
    // Go though the spaces looking for one that has objects.
    while (space_iterator_->has_next()) {
      object_iterator_ = space_iterator_->next();
      if (HeapObject* obj = object_iterator_->next_object()) {
        return obj;
      }
    }
  }
  // Done with the last space.
  object_iterator_ = NULL;
  return NULL;
}


void HeapIterator::reset() {
  // Restart the iterator.
  Shutdown();
  Init();
}


#ifdef DEBUG

Object* const PathTracer::kAnyGlobalObject = NULL;

class PathTracer::MarkVisitor: public ObjectVisitor {
 public:
  explicit MarkVisitor(PathTracer* tracer) : tracer_(tracer) {}
  void VisitPointers(Object** start, Object** end) {
    // Scan all HeapObject pointers in [start, end)
    for (Object** p = start; !tracer_->found() && (p < end); p++) {
      if ((*p)->IsHeapObject())
        tracer_->MarkRecursively(p, this);
    }
  }

 private:
  PathTracer* tracer_;
};


class PathTracer::UnmarkVisitor: public ObjectVisitor {
 public:
  explicit UnmarkVisitor(PathTracer* tracer) : tracer_(tracer) {}
  void VisitPointers(Object** start, Object** end) {
    // Scan all HeapObject pointers in [start, end)
    for (Object** p = start; p < end; p++) {
      if ((*p)->IsHeapObject())
        tracer_->UnmarkRecursively(p, this);
    }
  }

 private:
  PathTracer* tracer_;
};


void PathTracer::VisitPointers(Object** start, Object** end) {
  bool done = ((what_to_find_ == FIND_FIRST) && found_target_);
  // Visit all HeapObject pointers in [start, end)
  for (Object** p = start; !done && (p < end); p++) {
    if ((*p)->IsHeapObject()) {
      TracePathFrom(p);
      done = ((what_to_find_ == FIND_FIRST) && found_target_);
    }
  }
}


void PathTracer::Reset() {
  found_target_ = false;
  object_stack_.Clear();
}


void PathTracer::TracePathFrom(Object** root) {
  ASSERT((search_target_ == kAnyGlobalObject) ||
         search_target_->IsHeapObject());
  found_target_in_trace_ = false;
  Reset();

  MarkVisitor mark_visitor(this);
  MarkRecursively(root, &mark_visitor);

  UnmarkVisitor unmark_visitor(this);
  UnmarkRecursively(root, &unmark_visitor);

  ProcessResults();
}


static bool SafeIsNativeContext(HeapObject* obj) {
  return obj->map() == obj->GetHeap()->raw_unchecked_native_context_map();
}


void PathTracer::MarkRecursively(Object** p, MarkVisitor* mark_visitor) {
  if (!(*p)->IsHeapObject()) return;

  HeapObject* obj = HeapObject::cast(*p);

  Object* map = obj->map();

  if (!map->IsHeapObject()) return;  // visited before

  if (found_target_in_trace_) return;  // stop if target found
  object_stack_.Add(obj);
  if (((search_target_ == kAnyGlobalObject) && obj->IsJSGlobalObject()) ||
      (obj == search_target_)) {
    found_target_in_trace_ = true;
    found_target_ = true;
    return;
  }

  bool is_native_context = SafeIsNativeContext(obj);

  // not visited yet
  Map* map_p = reinterpret_cast<Map*>(HeapObject::cast(map));

  Address map_addr = map_p->address();

  obj->set_map_no_write_barrier(reinterpret_cast<Map*>(map_addr + kMarkTag));

  // Scan the object body.
  if (is_native_context && (visit_mode_ == VISIT_ONLY_STRONG)) {
    // This is specialized to scan Context's properly.
    Object** start = reinterpret_cast<Object**>(obj->address() +
                                                Context::kHeaderSize);
    Object** end = reinterpret_cast<Object**>(obj->address() +
        Context::kHeaderSize + Context::FIRST_WEAK_SLOT * kPointerSize);
    mark_visitor->VisitPointers(start, end);
  } else {
    obj->IterateBody(map_p->instance_type(),
                     obj->SizeFromMap(map_p),
                     mark_visitor);
  }

  // Scan the map after the body because the body is a lot more interesting
  // when doing leak detection.
  MarkRecursively(&map, mark_visitor);

  if (!found_target_in_trace_)  // don't pop if found the target
    object_stack_.RemoveLast();
}


void PathTracer::UnmarkRecursively(Object** p, UnmarkVisitor* unmark_visitor) {
  if (!(*p)->IsHeapObject()) return;

  HeapObject* obj = HeapObject::cast(*p);

  Object* map = obj->map();

  if (map->IsHeapObject()) return;  // unmarked already

  Address map_addr = reinterpret_cast<Address>(map);

  map_addr -= kMarkTag;

  ASSERT_TAG_ALIGNED(map_addr);

  HeapObject* map_p = HeapObject::FromAddress(map_addr);

  obj->set_map_no_write_barrier(reinterpret_cast<Map*>(map_p));

  UnmarkRecursively(reinterpret_cast<Object**>(&map_p), unmark_visitor);

  obj->IterateBody(Map::cast(map_p)->instance_type(),
                   obj->SizeFromMap(Map::cast(map_p)),
                   unmark_visitor);
}


void PathTracer::ProcessResults() {
  if (found_target_) {
    PrintF("=====================================\n");
    PrintF("====        Path to object       ====\n");
    PrintF("=====================================\n\n");

    ASSERT(!object_stack_.is_empty());
    for (int i = 0; i < object_stack_.length(); i++) {
      if (i > 0) PrintF("\n     |\n     |\n     V\n\n");
      Object* obj = object_stack_[i];
      obj->Print();
    }
    PrintF("=====================================\n");
  }
}


// Triggers a depth-first traversal of reachable objects from one
// given root object and finds a path to a specific heap object and
// prints it.
void Heap::TracePathToObjectFrom(Object* target, Object* root) {
  PathTracer tracer(target, PathTracer::FIND_ALL, VISIT_ALL);
  tracer.VisitPointer(&root);
}


// Triggers a depth-first traversal of reachable objects from roots
// and finds a path to a specific heap object and prints it.
void Heap::TracePathToObject(Object* target) {
  PathTracer tracer(target, PathTracer::FIND_ALL, VISIT_ALL);
  IterateRoots(&tracer, VISIT_ONLY_STRONG);
}


// Triggers a depth-first traversal of reachable objects from roots
// and finds a path to any global object and prints it. Useful for
// determining the source for leaks of global objects.
void Heap::TracePathToGlobal() {
  PathTracer tracer(PathTracer::kAnyGlobalObject,
                    PathTracer::FIND_ALL,
                    VISIT_ALL);
  IterateRoots(&tracer, VISIT_ONLY_STRONG);
}
#endif


static intptr_t CountTotalHolesSize(Heap* heap) {
  intptr_t holes_size = 0;
  OldSpaces spaces(heap);
  for (OldSpace* space = spaces.next();
       space != NULL;
       space = spaces.next()) {
    holes_size += space->Waste() + space->Available();
  }
  return holes_size;
}


GCTracer::GCTracer(Heap* heap,
                   const char* gc_reason,
                   const char* collector_reason)
    : start_time_(0.0),
      start_object_size_(0),
      start_memory_size_(0),
      gc_count_(0),
      full_gc_count_(0),
      allocated_since_last_gc_(0),
      spent_in_mutator_(0),
      promoted_objects_size_(0),
      nodes_died_in_new_space_(0),
      nodes_copied_in_new_space_(0),
      nodes_promoted_(0),
      heap_(heap),
      gc_reason_(gc_reason),
      collector_reason_(collector_reason) {
  if (!FLAG_trace_gc && !FLAG_print_cumulative_gc_stat) return;
  start_time_ = OS::TimeCurrentMillis();
  start_object_size_ = heap_->SizeOfObjects();
  start_memory_size_ = heap_->isolate()->memory_allocator()->Size();

  for (int i = 0; i < Scope::kNumberOfScopes; i++) {
    scopes_[i] = 0;
  }

  in_free_list_or_wasted_before_gc_ = CountTotalHolesSize(heap);

  allocated_since_last_gc_ =
      heap_->SizeOfObjects() - heap_->alive_after_last_gc_;

  if (heap_->last_gc_end_timestamp_ > 0) {
    spent_in_mutator_ = Max(start_time_ - heap_->last_gc_end_timestamp_, 0.0);
  }

  steps_count_ = heap_->incremental_marking()->steps_count();
  steps_took_ = heap_->incremental_marking()->steps_took();
  longest_step_ = heap_->incremental_marking()->longest_step();
  steps_count_since_last_gc_ =
      heap_->incremental_marking()->steps_count_since_last_gc();
  steps_took_since_last_gc_ =
      heap_->incremental_marking()->steps_took_since_last_gc();
}


GCTracer::~GCTracer() {
  // Printf ONE line iff flag is set.
  if (!FLAG_trace_gc && !FLAG_print_cumulative_gc_stat) return;

  bool first_gc = (heap_->last_gc_end_timestamp_ == 0);

  heap_->alive_after_last_gc_ = heap_->SizeOfObjects();
  heap_->last_gc_end_timestamp_ = OS::TimeCurrentMillis();

  double time = heap_->last_gc_end_timestamp_ - start_time_;

  // Update cumulative GC statistics if required.
  if (FLAG_print_cumulative_gc_stat) {
    heap_->total_gc_time_ms_ += time;
    heap_->max_gc_pause_ = Max(heap_->max_gc_pause_, time);
    heap_->max_alive_after_gc_ = Max(heap_->max_alive_after_gc_,
                                     heap_->alive_after_last_gc_);
    if (!first_gc) {
      heap_->min_in_mutator_ = Min(heap_->min_in_mutator_,
                                   spent_in_mutator_);
    }
  } else if (FLAG_trace_gc_verbose) {
    heap_->total_gc_time_ms_ += time;
  }

  if (collector_ == SCAVENGER && FLAG_trace_gc_ignore_scavenger) return;

  heap_->AddMarkingTime(scopes_[Scope::MC_MARK]);

  if (FLAG_print_cumulative_gc_stat && !FLAG_trace_gc) return;
  PrintPID("%8.0f ms: ", heap_->isolate()->time_millis_since_init());

  if (!FLAG_trace_gc_nvp) {
    int external_time = static_cast<int>(scopes_[Scope::EXTERNAL]);

    double end_memory_size_mb =
        static_cast<double>(heap_->isolate()->memory_allocator()->Size()) / MB;

    PrintF("%s %.1f (%.1f) -> %.1f (%.1f) MB, ",
           CollectorString(),
           static_cast<double>(start_object_size_) / MB,
           static_cast<double>(start_memory_size_) / MB,
           SizeOfHeapObjects(),
           end_memory_size_mb);

    if (external_time > 0) PrintF("%d / ", external_time);
    PrintF("%.1f ms", time);
    if (steps_count_ > 0) {
      if (collector_ == SCAVENGER) {
        PrintF(" (+ %.1f ms in %d steps since last GC)",
               steps_took_since_last_gc_,
               steps_count_since_last_gc_);
      } else {
        PrintF(" (+ %.1f ms in %d steps since start of marking, "
                   "biggest step %.1f ms)",
               steps_took_,
               steps_count_,
               longest_step_);
      }
    }

    if (gc_reason_ != NULL) {
      PrintF(" [%s]", gc_reason_);
    }

    if (collector_reason_ != NULL) {
      PrintF(" [%s]", collector_reason_);
    }

    PrintF(".\n");
  } else {
    PrintF("pause=%.1f ", time);
    PrintF("mutator=%.1f ", spent_in_mutator_);
    PrintF("gc=");
    switch (collector_) {
      case SCAVENGER:
        PrintF("s");
        break;
      case MARK_COMPACTOR:
        PrintF("ms");
        break;
      default:
        UNREACHABLE();
    }
    PrintF(" ");

    PrintF("external=%.1f ", scopes_[Scope::EXTERNAL]);
    PrintF("mark=%.1f ", scopes_[Scope::MC_MARK]);
    PrintF("sweep=%.2f ", scopes_[Scope::MC_SWEEP]);
    PrintF("sweepns=%.2f ", scopes_[Scope::MC_SWEEP_NEWSPACE]);
    PrintF("sweepos=%.2f ", scopes_[Scope::MC_SWEEP_OLDSPACE]);
    PrintF("evacuate=%.1f ", scopes_[Scope::MC_EVACUATE_PAGES]);
    PrintF("new_new=%.1f ", scopes_[Scope::MC_UPDATE_NEW_TO_NEW_POINTERS]);
    PrintF("root_new=%.1f ", scopes_[Scope::MC_UPDATE_ROOT_TO_NEW_POINTERS]);
    PrintF("old_new=%.1f ", scopes_[Scope::MC_UPDATE_OLD_TO_NEW_POINTERS]);
    PrintF("compaction_ptrs=%.1f ",
        scopes_[Scope::MC_UPDATE_POINTERS_TO_EVACUATED]);
    PrintF("intracompaction_ptrs=%.1f ",
        scopes_[Scope::MC_UPDATE_POINTERS_BETWEEN_EVACUATED]);
    PrintF("misc_compaction=%.1f ", scopes_[Scope::MC_UPDATE_MISC_POINTERS]);
    PrintF("weakcollection_process=%.1f ",
        scopes_[Scope::MC_WEAKCOLLECTION_PROCESS]);
    PrintF("weakcollection_clear=%.1f ",
        scopes_[Scope::MC_WEAKCOLLECTION_CLEAR]);

    PrintF("total_size_before=%" V8_PTR_PREFIX "d ", start_object_size_);
    PrintF("total_size_after=%" V8_PTR_PREFIX "d ", heap_->SizeOfObjects());
    PrintF("holes_size_before=%" V8_PTR_PREFIX "d ",
           in_free_list_or_wasted_before_gc_);
    PrintF("holes_size_after=%" V8_PTR_PREFIX "d ", CountTotalHolesSize(heap_));

    PrintF("allocated=%" V8_PTR_PREFIX "d ", allocated_since_last_gc_);
    PrintF("promoted=%" V8_PTR_PREFIX "d ", promoted_objects_size_);
    PrintF("nodes_died_in_new=%d ", nodes_died_in_new_space_);
    PrintF("nodes_copied_in_new=%d ", nodes_copied_in_new_space_);
    PrintF("nodes_promoted=%d ", nodes_promoted_);

    if (collector_ == SCAVENGER) {
      PrintF("stepscount=%d ", steps_count_since_last_gc_);
      PrintF("stepstook=%.1f ", steps_took_since_last_gc_);
    } else {
      PrintF("stepscount=%d ", steps_count_);
      PrintF("stepstook=%.1f ", steps_took_);
      PrintF("longeststep=%.1f ", longest_step_);
    }

    PrintF("\n");
  }

  heap_->PrintShortHeapStatistics();
}


const char* GCTracer::CollectorString() {
  switch (collector_) {
    case SCAVENGER:
      return "Scavenge";
    case MARK_COMPACTOR:
      return "Mark-sweep";
  }
  return "Unknown GC";
}


int KeyedLookupCache::Hash(Handle<Map> map, Handle<Name> name) {
  DisallowHeapAllocation no_gc;
  // Uses only lower 32 bits if pointers are larger.
  uintptr_t addr_hash =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(*map)) >> kMapHashShift;
  return static_cast<uint32_t>((addr_hash ^ name->Hash()) & kCapacityMask);
}


int KeyedLookupCache::Lookup(Handle<Map> map, Handle<Name> name) {
  DisallowHeapAllocation no_gc;
  int index = (Hash(map, name) & kHashMask);
  for (int i = 0; i < kEntriesPerBucket; i++) {
    Key& key = keys_[index + i];
    if ((key.map == *map) && key.name->Equals(*name)) {
      return field_offsets_[index + i];
    }
  }
  return kNotFound;
}


void KeyedLookupCache::Update(Handle<Map> map,
                              Handle<Name> name,
                              int field_offset) {
  DisallowHeapAllocation no_gc;
  if (!name->IsUniqueName()) {
    if (!StringTable::InternalizeStringIfExists(name->GetIsolate(),
                                                Handle<String>::cast(name)).
        ToHandle(&name)) {
      return;
    }
  }
  // This cache is cleared only between mark compact passes, so we expect the
  // cache to only contain old space names.
  ASSERT(!map->GetIsolate()->heap()->InNewSpace(*name));

  int index = (Hash(map, name) & kHashMask);
  // After a GC there will be free slots, so we use them in order (this may
  // help to get the most frequently used one in position 0).
  for (int i = 0; i< kEntriesPerBucket; i++) {
    Key& key = keys_[index];
    Object* free_entry_indicator = NULL;
    if (key.map == free_entry_indicator) {
      key.map = *map;
      key.name = *name;
      field_offsets_[index + i] = field_offset;
      return;
    }
  }
  // No free entry found in this bucket, so we move them all down one and
  // put the new entry at position zero.
  for (int i = kEntriesPerBucket - 1; i > 0; i--) {
    Key& key = keys_[index + i];
    Key& key2 = keys_[index + i - 1];
    key = key2;
    field_offsets_[index + i] = field_offsets_[index + i - 1];
  }

  // Write the new first entry.
  Key& key = keys_[index];
  key.map = *map;
  key.name = *name;
  field_offsets_[index] = field_offset;
}


void KeyedLookupCache::Clear() {
  for (int index = 0; index < kLength; index++) keys_[index].map = NULL;
}


void DescriptorLookupCache::Clear() {
  for (int index = 0; index < kLength; index++) keys_[index].source = NULL;
}


void ExternalStringTable::CleanUp() {
  int last = 0;
  for (int i = 0; i < new_space_strings_.length(); ++i) {
    if (new_space_strings_[i] == heap_->the_hole_value()) {
      continue;
    }
    ASSERT(new_space_strings_[i]->IsExternalString());
    if (heap_->InNewSpace(new_space_strings_[i])) {
      new_space_strings_[last++] = new_space_strings_[i];
    } else {
      old_space_strings_.Add(new_space_strings_[i]);
    }
  }
  new_space_strings_.Rewind(last);
  new_space_strings_.Trim();

  last = 0;
  for (int i = 0; i < old_space_strings_.length(); ++i) {
    if (old_space_strings_[i] == heap_->the_hole_value()) {
      continue;
    }
    ASSERT(old_space_strings_[i]->IsExternalString());
    ASSERT(!heap_->InNewSpace(old_space_strings_[i]));
    old_space_strings_[last++] = old_space_strings_[i];
  }
  old_space_strings_.Rewind(last);
  old_space_strings_.Trim();
#ifdef VERIFY_HEAP
  if (FLAG_verify_heap) {
    Verify();
  }
#endif
}


void ExternalStringTable::TearDown() {
  for (int i = 0; i < new_space_strings_.length(); ++i) {
    heap_->FinalizeExternalString(ExternalString::cast(new_space_strings_[i]));
  }
  new_space_strings_.Free();
  for (int i = 0; i < old_space_strings_.length(); ++i) {
    heap_->FinalizeExternalString(ExternalString::cast(old_space_strings_[i]));
  }
  old_space_strings_.Free();
}


void Heap::QueueMemoryChunkForFree(MemoryChunk* chunk) {
  chunk->set_next_chunk(chunks_queued_for_free_);
  chunks_queued_for_free_ = chunk;
}


void Heap::FreeQueuedChunks() {
  if (chunks_queued_for_free_ == NULL) return;
  MemoryChunk* next;
  MemoryChunk* chunk;
  for (chunk = chunks_queued_for_free_; chunk != NULL; chunk = next) {
    next = chunk->next_chunk();
    chunk->SetFlag(MemoryChunk::ABOUT_TO_BE_FREED);

    if (chunk->owner()->identity() == LO_SPACE) {
      // StoreBuffer::Filter relies on MemoryChunk::FromAnyPointerAddress.
      // If FromAnyPointerAddress encounters a slot that belongs to a large
      // chunk queued for deletion it will fail to find the chunk because
      // it try to perform a search in the list of pages owned by of the large
      // object space and queued chunks were detached from that list.
      // To work around this we split large chunk into normal kPageSize aligned
      // pieces and initialize size, owner and flags field of every piece.
      // If FromAnyPointerAddress encounters a slot that belongs to one of
      // these smaller pieces it will treat it as a slot on a normal Page.
      Address chunk_end = chunk->address() + chunk->size();
      MemoryChunk* inner = MemoryChunk::FromAddress(
          chunk->address() + Page::kPageSize);
      MemoryChunk* inner_last = MemoryChunk::FromAddress(chunk_end - 1);
      while (inner <= inner_last) {
        // Size of a large chunk is always a multiple of
        // OS::AllocateAlignment() so there is always
        // enough space for a fake MemoryChunk header.
        Address area_end = Min(inner->address() + Page::kPageSize, chunk_end);
        // Guard against overflow.
        if (area_end < inner->address()) area_end = chunk_end;
        inner->SetArea(inner->address(), area_end);
        inner->set_size(Page::kPageSize);
        inner->set_owner(lo_space());
        inner->SetFlag(MemoryChunk::ABOUT_TO_BE_FREED);
        inner = MemoryChunk::FromAddress(
            inner->address() + Page::kPageSize);
      }
    }
  }
  isolate_->heap()->store_buffer()->Compact();
  isolate_->heap()->store_buffer()->Filter(MemoryChunk::ABOUT_TO_BE_FREED);
  for (chunk = chunks_queued_for_free_; chunk != NULL; chunk = next) {
    next = chunk->next_chunk();
    isolate_->memory_allocator()->Free(chunk);
  }
  chunks_queued_for_free_ = NULL;
}


void Heap::RememberUnmappedPage(Address page, bool compacted) {
  uintptr_t p = reinterpret_cast<uintptr_t>(page);
  // Tag the page pointer to make it findable in the dump file.
  if (compacted) {
    p ^= 0xc1ead & (Page::kPageSize - 1);  // Cleared.
  } else {
    p ^= 0x1d1ed & (Page::kPageSize - 1);  // I died.
  }
  remembered_unmapped_pages_[remembered_unmapped_pages_index_] =
      reinterpret_cast<Address>(p);
  remembered_unmapped_pages_index_++;
  remembered_unmapped_pages_index_ %= kRememberedUnmappedPages;
}


void Heap::ClearObjectStats(bool clear_last_time_stats) {
  memset(object_counts_, 0, sizeof(object_counts_));
  memset(object_sizes_, 0, sizeof(object_sizes_));
  if (clear_last_time_stats) {
    memset(object_counts_last_time_, 0, sizeof(object_counts_last_time_));
    memset(object_sizes_last_time_, 0, sizeof(object_sizes_last_time_));
  }
}


static LazyMutex checkpoint_object_stats_mutex = LAZY_MUTEX_INITIALIZER;


void Heap::CheckpointObjectStats() {
  LockGuard<Mutex> lock_guard(checkpoint_object_stats_mutex.Pointer());
  Counters* counters = isolate()->counters();
#define ADJUST_LAST_TIME_OBJECT_COUNT(name)                                    \
  counters->count_of_##name()->Increment(                                      \
      static_cast<int>(object_counts_[name]));                                 \
  counters->count_of_##name()->Decrement(                                      \
      static_cast<int>(object_counts_last_time_[name]));                       \
  counters->size_of_##name()->Increment(                                       \
      static_cast<int>(object_sizes_[name]));                                  \
  counters->size_of_##name()->Decrement(                                       \
      static_cast<int>(object_sizes_last_time_[name]));
  INSTANCE_TYPE_LIST(ADJUST_LAST_TIME_OBJECT_COUNT)
#undef ADJUST_LAST_TIME_OBJECT_COUNT
  int index;
#define ADJUST_LAST_TIME_OBJECT_COUNT(name)               \
  index = FIRST_CODE_KIND_SUB_TYPE + Code::name;          \
  counters->count_of_CODE_TYPE_##name()->Increment(       \
      static_cast<int>(object_counts_[index]));           \
  counters->count_of_CODE_TYPE_##name()->Decrement(       \
      static_cast<int>(object_counts_last_time_[index])); \
  counters->size_of_CODE_TYPE_##name()->Increment(        \
      static_cast<int>(object_sizes_[index]));            \
  counters->size_of_CODE_TYPE_##name()->Decrement(        \
      static_cast<int>(object_sizes_last_time_[index]));
  CODE_KIND_LIST(ADJUST_LAST_TIME_OBJECT_COUNT)
#undef ADJUST_LAST_TIME_OBJECT_COUNT
#define ADJUST_LAST_TIME_OBJECT_COUNT(name)               \
  index = FIRST_FIXED_ARRAY_SUB_TYPE + name;              \
  counters->count_of_FIXED_ARRAY_##name()->Increment(     \
      static_cast<int>(object_counts_[index]));           \
  counters->count_of_FIXED_ARRAY_##name()->Decrement(     \
      static_cast<int>(object_counts_last_time_[index])); \
  counters->size_of_FIXED_ARRAY_##name()->Increment(      \
      static_cast<int>(object_sizes_[index]));            \
  counters->size_of_FIXED_ARRAY_##name()->Decrement(      \
      static_cast<int>(object_sizes_last_time_[index]));
  FIXED_ARRAY_SUB_INSTANCE_TYPE_LIST(ADJUST_LAST_TIME_OBJECT_COUNT)
#undef ADJUST_LAST_TIME_OBJECT_COUNT
#define ADJUST_LAST_TIME_OBJECT_COUNT(name)                                   \
  index =                                                                     \
      FIRST_CODE_AGE_SUB_TYPE + Code::k##name##CodeAge - Code::kFirstCodeAge; \
  counters->count_of_CODE_AGE_##name()->Increment(                            \
      static_cast<int>(object_counts_[index]));                               \
  counters->count_of_CODE_AGE_##name()->Decrement(                            \
      static_cast<int>(object_counts_last_time_[index]));                     \
  counters->size_of_CODE_AGE_##name()->Increment(                             \
      static_cast<int>(object_sizes_[index]));                                \
  counters->size_of_CODE_AGE_##name()->Decrement(                             \
      static_cast<int>(object_sizes_last_time_[index]));
  CODE_AGE_LIST_COMPLETE(ADJUST_LAST_TIME_OBJECT_COUNT)
#undef ADJUST_LAST_TIME_OBJECT_COUNT

  OS::MemCopy(object_counts_last_time_, object_counts_, sizeof(object_counts_));
  OS::MemCopy(object_sizes_last_time_, object_sizes_, sizeof(object_sizes_));
  ClearObjectStats();
}

} }  // namespace v8::internal
