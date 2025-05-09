/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */
#include "precompiled.hpp"

#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahMmuTracker.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"
#include "runtime/task.hpp"

class ShenandoahMmuTask : public PeriodicTask {
  ShenandoahMmuTracker* _mmu_tracker;
public:
  explicit ShenandoahMmuTask(ShenandoahMmuTracker* mmu_tracker) :
    PeriodicTask(GCPauseIntervalMillis), _mmu_tracker(mmu_tracker) {}

  void task() override {
    _mmu_tracker->report();
  }
};

class ThreadTimeAccumulator : public ThreadClosure {
 public:
  size_t total_time;
  ThreadTimeAccumulator() : total_time(0) {}
  void do_thread(Thread* thread) override {
    total_time += os::thread_cpu_time(thread);
  }
};

class ThreadUserSYSTimeAccumulator : public ThreadClosure {
 public:
  size_t total_user_time;
  size_t total_sys_time;
  ThreadUserSYSTimeAccumulator() : total_user_time(0), total_sys_time(0) {}
  void do_thread(Thread* thread) override {
    size_t user, sys;
    if (!os::slow_thread_user_sys_time(thread, &user, &sys)) {
      total_user_time += user;
      total_sys_time += sys;
    } else {
      log_info(gc)("ThreadUserSYSTimeAccumulator get thread cpu time error");
    }
  }
};

ShenandoahMmuTracker::ShenandoahMmuTracker() :
    _most_recent_timestamp(0.0),
    _most_recent_gc_time(0.0),
    _most_recent_gcu(0.0),
    _most_recent_mutator_time(0.0),
    _most_recent_mu(0.0),
    _most_recent_gc_user_time(0.0),
    _most_recent_gc_sys_time(0.0),
    _most_recent_mutator_user_time(0.0),
    _most_recent_mutator_sys_time(0.0),
    _most_recent_periodic_time_stamp(0.0),
    _most_recent_periodic_gc_time(0.0),
    _most_recent_periodic_mutator_time(0.0),
    _mmu_periodic_task(new ShenandoahMmuTask(this)),
    _young_gcs(0),
    _young_gc_user_time_seq(ShenTuneYoungInterval/*length*/, 0.5 /*decay factor*/),
    _young_gc_sys_time_seq(ShenTuneYoungInterval, 0.5),
    _young_gc_period_seq(ShenTuneYoungInterval, 0.5),
    _young_mutator_user_time_seq(ShenTuneYoungInterval, 0.5),
    _young_mutator_sys_time_seq(ShenTuneYoungInterval, 0.5) {
}

ShenandoahMmuTracker::~ShenandoahMmuTracker() {
  _mmu_periodic_task->disenroll();
  delete _mmu_periodic_task;
}

void ShenandoahMmuTracker::fetch_cpu_times(double &gc_time, double &mutator_time) {
  ThreadTimeAccumulator cl;
  // We include only the gc threads because those are the only threads
  // we are responsible for.
  ShenandoahHeap::heap()->gc_threads_do(&cl);
  double most_recent_gc_thread_time = double(cl.total_time) / NANOSECS_PER_SEC;
  gc_time = most_recent_gc_thread_time;

  double process_real_time(0.0), process_user_time(0.0), process_system_time(0.0);
  bool valid = os::getTimesSecs(&process_real_time, &process_user_time, &process_system_time);
  assert(valid, "don't know why this would not be valid");
  mutator_time =(process_user_time + process_system_time) - most_recent_gc_thread_time;
}

void ShenandoahMmuTracker::fetch_user_sys_times(double &gc_user_time, double &gc_sys_time, double &mutator_user_time, double &mutator_sys_time) {
  ThreadUserSYSTimeAccumulator cl;
  // We include only the gc threads because those are the only threads
  // we are responsible for.
  ShenandoahHeap::heap()->gc_threads_do(&cl);
  double most_recent_gc_user_time = double(cl.total_user_time) / NANOSECS_PER_SEC;
  double most_recent_gc_sys_time = double(cl.total_sys_time) / NANOSECS_PER_SEC;
  gc_user_time = most_recent_gc_user_time;
  gc_sys_time = most_recent_gc_sys_time;

  double process_real_time(0.0), process_user_time(0.0), process_system_time(0.0);
  bool valid = os::getTimesSecs(&process_real_time, &process_user_time, &process_system_time);
  assert(valid, "don't know why this would not be valid");
  mutator_sys_time = process_system_time - most_recent_gc_sys_time;
  mutator_user_time = process_user_time - most_recent_gc_user_time;
}

void ShenandoahMmuTracker::update_utilization(size_t gcid, const char* msg, bool is_young) {
  if (UseShenTuneYoungSize || UseShenFixYoungSize) {
    update_utilization_farmem(gcid, msg, is_young);
    return;
  }

  double current = os::elapsedTime();
  _most_recent_gcid = gcid;
  _most_recent_is_full = false;

  if (gcid == 0) {
    fetch_cpu_times(_most_recent_gc_time, _most_recent_mutator_time);

    _most_recent_timestamp = current;
  } else {
    double gc_cycle_period = current - _most_recent_timestamp;
    _most_recent_timestamp = current;

    double gc_thread_time, mutator_thread_time;
    fetch_cpu_times(gc_thread_time, mutator_thread_time);
    double gc_time = gc_thread_time - _most_recent_gc_time;
    _most_recent_gc_time = gc_thread_time;
    _most_recent_gcu = gc_time / (_active_processors * gc_cycle_period);
    double mutator_time = mutator_thread_time - _most_recent_mutator_time;
    _most_recent_mutator_time = mutator_thread_time;
    _most_recent_mu = mutator_time / (_active_processors * gc_cycle_period);
    log_info(gc, ergo)("At end of %s: GCU: %.1f%%, MU: %.1f%% during period of %.3fs",
                       msg, _most_recent_gcu * 100, _most_recent_mu * 100, gc_cycle_period);
  }
}

void ShenandoahMmuTracker::update_utilization_farmem(size_t gcid, const char* msg, bool is_young) {
  double current = os::elapsedTime();
  _most_recent_gcid = gcid;
  _most_recent_is_full = false;

  if (gcid == 0) {
    fetch_user_sys_times(_most_recent_gc_user_time, _most_recent_gc_sys_time, _most_recent_mutator_user_time, _most_recent_mutator_sys_time);

    _most_recent_timestamp = current;
  } else {
    double gc_cycle_period = current - _most_recent_timestamp;
    _most_recent_timestamp = current;

    double gc_thread_user_time, gc_thread_sys_time, mutator_thread_user_time, mutator_thread_sys_time;
    fetch_user_sys_times(gc_thread_user_time, gc_thread_sys_time, mutator_thread_user_time, mutator_thread_sys_time);

    double gc_user_time = gc_thread_user_time - _most_recent_gc_user_time;
    double gc_sys_time = gc_thread_sys_time - _most_recent_gc_sys_time;
    gc_user_time = gc_user_time >= 0 ? gc_user_time : 0;
    gc_sys_time = gc_sys_time >= 0 ? gc_sys_time : 0;
    _most_recent_gc_user_time = gc_thread_user_time;
    _most_recent_gc_sys_time = gc_thread_sys_time;
    _most_recent_gc_time = gc_thread_user_time + gc_thread_sys_time;
    _most_recent_gcu = (gc_user_time + gc_sys_time) / (_active_processors * gc_cycle_period);

    double mutator_user_time = mutator_thread_user_time - _most_recent_mutator_user_time;
    double mutator_sys_time = mutator_thread_sys_time - _most_recent_mutator_sys_time;
    mutator_user_time = mutator_user_time >= 0 ? mutator_user_time : 0;
    mutator_sys_time = mutator_sys_time >= 0 ? mutator_sys_time : 0;
    _most_recent_mutator_user_time = mutator_thread_user_time;
    _most_recent_mutator_sys_time = mutator_thread_sys_time;
    _most_recent_mutator_time = mutator_thread_user_time + mutator_thread_sys_time;
    _most_recent_mu = (mutator_user_time + mutator_sys_time) / (_active_processors * gc_cycle_period);

    if (is_young) {
      _young_gcs += 1;
      _young_gc_user_time_seq.add(gc_user_time);
      _young_gc_sys_time_seq.add(gc_sys_time);
      _young_gc_period_seq.add(gc_cycle_period);
      _young_mutator_user_time_seq.add(mutator_user_time);
      _young_mutator_sys_time_seq.add(mutator_sys_time);
    }

    log_info(gc, ergo)("At end of %s: GCU: %.1f%%, MU: %.1f%% during period of %.3fs",
                       msg, _most_recent_gcu * 100, _most_recent_mu * 100, gc_cycle_period);
    log_info(gc, ergo)("GCK2U: %.1f%%, MK2U: %.1f%%, K2U: %.1f%%",
                       gc_sys_time / gc_user_time * 100, mutator_sys_time / mutator_user_time * 100, (gc_sys_time + mutator_sys_time) / (gc_user_time + mutator_user_time) * 100);
    log_info(gc, ergo)("gc_utime: %.1fms, gc_stime: %.1fms, mut_utime: %.1fms, mut_stime: %.1fms, period: %.3fs",
                       gc_user_time*1000, gc_sys_time*1000, mutator_user_time*1000, mutator_sys_time*1000, gc_cycle_period);
    log_info(gc, ergo)("most recent mut user: %.1fs, sys: %.1fs",
                       _most_recent_mutator_user_time, _most_recent_mutator_sys_time);
  }
}

void ShenandoahMmuTracker::record_young(size_t gcid) {
  update_utilization(gcid, "Concurrent Young GC", true /*is_young*/);
}

void ShenandoahMmuTracker::record_global(size_t gcid) {
  update_utilization(gcid, "Concurrent Global GC");
}

void ShenandoahMmuTracker::record_bootstrap(size_t gcid) {
  // Not likely that this will represent an "ideal" GCU, but doesn't hurt to try
  update_utilization(gcid, "Concurrent Bootstrap GC");
}

void ShenandoahMmuTracker::record_old_marking_increment(bool old_marking_done) {
  // No special processing for old marking
  double now = os::elapsedTime();
  double duration = now - _most_recent_timestamp;

  double gc_time, mutator_time;
  fetch_cpu_times(gc_time, mutator_time);
  double gcu = (gc_time - _most_recent_gc_time) / duration;
  double mu = (mutator_time - _most_recent_mutator_time) / duration;
  log_info(gc, ergo)("At end of %s: GCU: %.1f%%, MU: %.1f%% for duration %.3fs (totals to be subsumed in next gc report)",
                     old_marking_done? "last OLD marking increment": "OLD marking increment",
                     gcu * 100, mu * 100, duration);
}

void ShenandoahMmuTracker::record_mixed(size_t gcid) {
  update_utilization(gcid, "Mixed Concurrent GC");
}

void ShenandoahMmuTracker::record_degenerated(size_t gcid, bool is_old_bootstrap) {
  if ((gcid == _most_recent_gcid) && _most_recent_is_full) {
    // Do nothing.  This is a redundant recording for the full gc that just completed.
    // TODO: avoid making the call to record_degenerated() in the case that this degenerated upgraded to full gc.
  } else if (is_old_bootstrap) {
    update_utilization(gcid, "Degenerated Bootstrap Old GC");
  } else {
    update_utilization(gcid, "Degenerated Young GC", true /*is_young*/);
  }
}

void ShenandoahMmuTracker::record_full(size_t gcid) {
  update_utilization(gcid, "Full GC");
  _most_recent_is_full = true;
}

void ShenandoahMmuTracker::report() {
  // This is only called by the periodic thread.
  double current = os::elapsedTime();
  double time_delta = current - _most_recent_periodic_time_stamp;
  _most_recent_periodic_time_stamp = current;

  double gc_time, mutator_time;
  fetch_cpu_times(gc_time, mutator_time);

  double gc_delta = gc_time - _most_recent_periodic_gc_time;
  _most_recent_periodic_gc_time = gc_time;

  double mutator_delta = mutator_time - _most_recent_periodic_mutator_time;
  _most_recent_periodic_mutator_time = mutator_time;

  double mu = mutator_delta / (_active_processors * time_delta);
  double gcu = gc_delta / (_active_processors * time_delta);
  log_info(gc)("Periodic Sample: GCU = %.3f%%, MU = %.3f%% during most recent %.1fs", gcu * 100, mu * 100, time_delta);
}

// void ShenandoahMmuTracker::report_farmem() {
//   // This is only called by the periodic thread.
//   double current = os::elapsedTime();
//   double time_delta = current - _most_recent_periodic_time_stamp;
//   _most_recent_periodic_time_stamp = current;

//   double gc_time, mutator_time;
//   fetch_cpu_times(gc_time, mutator_time);

//   double gc_delta = gc_time - _most_recent_periodic_gc_time;
//   _most_recent_periodic_gc_time = gc_time;

//   double mutator_delta = mutator_time - _most_recent_periodic_mutator_time;
//   _most_recent_periodic_mutator_time = mutator_time;

//   double mu = mutator_delta / (_active_processors * time_delta);
//   double gcu = gc_delta / (_active_processors * time_delta);
//   log_info(gc)("Periodic Sample: GCU = %.3f%%, MU = %.3f%% during most recent %.1fs", gcu * 100, mu * 100, time_delta);
// }

void ShenandoahMmuTracker::initialize() {
  // initialize static data
  _active_processors = os::initial_active_processor_count();

  _most_recent_periodic_time_stamp = os::elapsedTime();
  fetch_cpu_times(_most_recent_periodic_gc_time, _most_recent_periodic_mutator_time);
  _mmu_periodic_task->enroll();
}

ShenandoahGenerationSizer::ShenandoahGenerationSizer()
  : _sizer_kind(SizerDefaults),
    _min_desired_young_regions(0),
    _max_desired_young_regions(0),
    _recent_tune_young_gcs(0) {
  log_info(gc)("Sizer enter");

  if (FLAG_IS_CMDLINE(NewRatio)) {
    if (FLAG_IS_CMDLINE(NewSize) || FLAG_IS_CMDLINE(MaxNewSize)) {
      log_warning(gc, ergo)("-XX:NewSize and -XX:MaxNewSize override -XX:NewRatio");
    } else {
      _sizer_kind = SizerNewRatio;
      log_info(gc)("Sizer ratio");
      return;
    }
  }

  if (NewSize > MaxNewSize) {
    if (FLAG_IS_CMDLINE(MaxNewSize)) {
      log_warning(gc, ergo)("NewSize (" SIZE_FORMAT "k) is greater than the MaxNewSize (" SIZE_FORMAT "k). "
                            "A new max generation size of " SIZE_FORMAT "k will be used.",
                            NewSize/K, MaxNewSize/K, NewSize/K);
    }
    log_info(gc)("Sizer set ergo");

    FLAG_SET_ERGO(MaxNewSize, NewSize);
  }

  if (FLAG_IS_CMDLINE(NewSize)) {
    _min_desired_young_regions = MAX2(uint(NewSize / ShenandoahHeapRegion::region_size_bytes()), 1U);
    if (FLAG_IS_CMDLINE(MaxNewSize)) {
      _max_desired_young_regions = MAX2(uint(MaxNewSize / ShenandoahHeapRegion::region_size_bytes()), 1U);
      _sizer_kind = SizerMaxAndNewSize;
      log_info(gc)("SizerMaxAndNewSize, min_new %lu, max_new %lu", _min_desired_young_regions, _max_desired_young_regions);
    } else {
      _sizer_kind = SizerNewSizeOnly;
      log_info(gc)("SizerNewSizeOnly");
    }
  } else if (FLAG_IS_CMDLINE(MaxNewSize)) {
    _max_desired_young_regions = MAX2(uint(MaxNewSize / ShenandoahHeapRegion::region_size_bytes()), 1U);
    _sizer_kind = SizerMaxNewSizeOnly;
    log_info(gc)("SizerMaxNewSizeOnly");
  }
}

size_t ShenandoahGenerationSizer::calculate_min_young_regions(size_t heap_region_count) {
  size_t min_young_regions = (heap_region_count * ShenandoahMinYoungPercentage) / 100;
  return MAX2(min_young_regions, (size_t) 1U);
}

size_t ShenandoahGenerationSizer::calculate_max_young_regions(size_t heap_region_count) {
  size_t max_young_regions = (heap_region_count * ShenandoahMaxYoungPercentage) / 100;
  return MAX2(max_young_regions, (size_t) 1U);
}

void ShenandoahGenerationSizer::recalculate_min_max_young_length(size_t heap_region_count) {
  assert(heap_region_count > 0, "Heap must be initialized");

  switch (_sizer_kind) {
    case SizerDefaults:
      _min_desired_young_regions = calculate_min_young_regions(heap_region_count);
      _max_desired_young_regions = calculate_max_young_regions(heap_region_count);
      break;
    case SizerNewSizeOnly:
      _max_desired_young_regions = calculate_max_young_regions(heap_region_count);
      _max_desired_young_regions = MAX2(_min_desired_young_regions, _max_desired_young_regions);
      break;
    case SizerMaxNewSizeOnly:
      _min_desired_young_regions = calculate_min_young_regions(heap_region_count);
      _min_desired_young_regions = MIN2(_min_desired_young_regions, _max_desired_young_regions);
      break;
    case SizerMaxAndNewSize:
      // Do nothing. Values set on the command line, don't update them at runtime.
      break;
    case SizerNewRatio:
      _min_desired_young_regions = MAX2(uint(heap_region_count / (NewRatio + 1)), 1U);
      _max_desired_young_regions = _min_desired_young_regions;
      break;
    default:
      ShouldNotReachHere();
  }

  assert(_min_desired_young_regions <= _max_desired_young_regions, "Invalid min/max young gen size values");
}

void ShenandoahGenerationSizer::heap_size_changed(size_t heap_size) {
  recalculate_min_max_young_length(heap_size / ShenandoahHeapRegion::region_size_bytes());
}

// Returns true iff transfer is successful
bool ShenandoahGenerationSizer::transfer_to_old(size_t regions) const {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahGeneration* old_gen = heap->old_generation();
  ShenandoahGeneration* young_gen = heap->young_generation();
  size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();
  size_t bytes_to_transfer = regions * region_size_bytes;

  if (young_gen->free_unaffiliated_regions() < regions) {
    return false;
  } else if (old_gen->max_capacity() + bytes_to_transfer > heap->max_size_for(old_gen)) {
    return false;
  } else if (young_gen->max_capacity() - bytes_to_transfer < heap->min_size_for(young_gen)) {
    return false;
  } else {
    young_gen->decrease_capacity(bytes_to_transfer);
    old_gen->increase_capacity(bytes_to_transfer);
    size_t new_size = old_gen->max_capacity();
    log_info(gc)("Transfer " SIZE_FORMAT " region(s) from %s to %s, yielding increased size: " SIZE_FORMAT "%s",
                 regions, young_gen->name(), old_gen->name(),
                 byte_size_in_proper_unit(new_size), proper_unit_for_byte_size(new_size));
    return true;
  }
}

// This is used when promoting humongous or highly utilized regular regions in place.  It is not required in this situation
// that the transferred regions be unaffiliated.
void ShenandoahGenerationSizer::force_transfer_to_old(size_t regions) const {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahGeneration* old_gen = heap->old_generation();
  ShenandoahGeneration* young_gen = heap->young_generation();
  size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();
  size_t bytes_to_transfer = regions * region_size_bytes;

  young_gen->decrease_capacity(bytes_to_transfer);
  old_gen->increase_capacity(bytes_to_transfer);
  size_t new_size = old_gen->max_capacity();
  log_info(gc)("Forcing transfer of " SIZE_FORMAT " region(s) from %s to %s, yielding increased size: " SIZE_FORMAT "%s",
               regions, young_gen->name(), old_gen->name(),
               byte_size_in_proper_unit(new_size), proper_unit_for_byte_size(new_size));
}


bool ShenandoahGenerationSizer::transfer_to_young(size_t regions) const {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahGeneration* old_gen = heap->old_generation();
  ShenandoahGeneration* young_gen = heap->young_generation();
  size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();
  size_t bytes_to_transfer = regions * region_size_bytes;

  if (old_gen->free_unaffiliated_regions() < regions) {
    return false;
  } else if (young_gen->max_capacity() + bytes_to_transfer > heap->max_size_for(young_gen)) {
    return false;
  } else if (old_gen->max_capacity() - bytes_to_transfer < heap->min_size_for(old_gen)) {
    return false;
  } else {
    old_gen->decrease_capacity(bytes_to_transfer);
    young_gen->increase_capacity(bytes_to_transfer);
    size_t new_size = young_gen->max_capacity();
    log_info(gc)("Transfer " SIZE_FORMAT " region(s) from %s to %s, yielding increased size: " SIZE_FORMAT "%s",
                 regions, old_gen->name(), young_gen->name(),
                 byte_size_in_proper_unit(new_size), proper_unit_for_byte_size(new_size));
    return true;
  }
}

size_t ShenandoahGenerationSizer::min_young_size() const {
  return min_young_regions() * ShenandoahHeapRegion::region_size_bytes();
}

size_t ShenandoahGenerationSizer::max_young_size() const {
  return max_young_regions() * ShenandoahHeapRegion::region_size_bytes();
}

double young_decre_factor(double user, double sys){
  return 0.3 - user / (5 * sys);
}

void ShenandoahGenerationSizer::adaptive_recalculate_min_max_young_length(ShenandoahMmuTracker* mmu_tracker) {
  size_t young_gcs = mmu_tracker->young_gcs();
  // Tune young size once every ShenTuneYoungInterval young gcs.
  if (young_gcs % ShenTuneYoungInterval != 0 || young_gcs == _recent_tune_young_gcs)
    return;
  _recent_tune_young_gcs = young_gcs;

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahGeneration* young_gen = heap->young_generation();
  ShenandoahGeneration* old_gen = heap->old_generation();
  size_t region_size_bytes = ShenandoahHeapRegion::region_size_bytes();
  size_t heap_size_bytes = heap->max_capacity();
  size_t young_size_bytes_orig = young_gen->soft_max_capacity();
  size_t young_size_bytes_new = 0;
  size_t increase_bytes = 0, decrease_bytes = 0;

  double gc_user_time = mmu_tracker->young_gc_user_time_davg();
  double gc_sys_time = mmu_tracker->young_gc_sys_time_davg();
  double gc_period_time = mmu_tracker->young_gc_period_davg();
  double mut_user_time = mmu_tracker->young_mutator_user_time_davg();
  double mut_sys_time = mmu_tracker->young_mutator_sys_time_davg();

  if (gc_user_time > mmu_tracker->active_processors() * gc_period_time * ShenTuneYoungMMU) {
    // Another option 1: keep young_avail / old_avail < 0.05
    //         option 2: increase 1/10 of available in old gen
    // Too many user time on gc, increase young by step
    increase_bytes = ShenTuneYoungIncreStepRegions * region_size_bytes;
    young_size_bytes_new = young_size_bytes_orig + increase_bytes;
    if (young_size_bytes_new >= heap_size_bytes || young_size_bytes_new <= 0)
      increase_bytes = 0;
  } else if (gc_sys_time > gc_user_time * ShenTuneYoungGCK2U &&
             mut_sys_time > mut_user_time * ShenTuneYoungMUTK2U) {
    // The WSS of young gc is large, decrease young by ratio
    double mut_decre = 2*mut_sys_time > mut_user_time ? young_decre_factor(mut_user_time, mut_sys_time) : 0;
    double gc_decre = 2*gc_sys_time > gc_user_time ? young_decre_factor(gc_user_time, gc_sys_time) : 0;
    double decre = (mut_decre - gc_decre) / 4 + gc_decre;
    decrease_bytes = (size_t)((double)young_size_bytes_orig * decre);
    // decrease_bytes = (size_t)((double)young_size_bytes_orig * gc_decre);
    decrease_bytes = align_up(decrease_bytes, region_size_bytes);
    young_size_bytes_new = young_size_bytes_orig - decrease_bytes;
    if (young_size_bytes_new < region_size_bytes || young_size_bytes_new <= 0)
      decrease_bytes = 0;
  }

  if (increase_bytes > 0 || decrease_bytes > 0) {
    ShenandoahHeapLocker locker(heap->lock());
    _max_desired_young_regions = (size_t)(young_size_bytes_new / region_size_bytes);
    young_gen->set_max_capacity(young_size_bytes_new);
    young_gen->set_soft_max_capacity(young_size_bytes_new);
    old_gen->set_max_capacity(heap_size_bytes - young_size_bytes_new);
    old_gen->set_soft_max_capacity(heap_size_bytes - young_size_bytes_new);
    log_info(gc, ergo)("[adaptive young] %s young for %ld bytes, new young %ld bytes",
            increase_bytes > 0 ? "incre" : "decre",
            increase_bytes > 0 ? increase_bytes : decrease_bytes,
            young_size_bytes_new);
  } else {
    log_info(gc, ergo)("[adaptive young] skip");
  }
}
