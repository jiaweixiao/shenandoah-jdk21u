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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHHEAPREGIONCLOSURES_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHHEAPREGIONCLOSURES_HPP


#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"

// Applies the given closure to all regions with the given affiliation
template<ShenandoahAffiliation AFFILIATION>
class ShenandoahIncludeRegionClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahHeapRegionClosure* _closure;

public:
  explicit ShenandoahIncludeRegionClosure(ShenandoahHeapRegionClosure* closure): _closure(closure) {}

  void heap_region_do(ShenandoahHeapRegion* r) override {
    if (r->affiliation() == AFFILIATION) {
      _closure->heap_region_do(r);
    }
  }

  bool is_thread_safe() override {
    return _closure->is_thread_safe();
  }
};

// Applies the given closure to all regions without the given affiliation
template<ShenandoahAffiliation AFFILIATION>
class ShenandoahExcludeRegionClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahHeapRegionClosure* _closure;

public:
  explicit ShenandoahExcludeRegionClosure(ShenandoahHeapRegionClosure* closure): _closure(closure) {}

  void heap_region_do(ShenandoahHeapRegion* r) override {
    if (r->affiliation() != AFFILIATION) {
      _closure->heap_region_do(r);
    }
  }

  bool is_thread_safe() override {
    return _closure->is_thread_safe();
  }
};

// Makes regions pinned or unpinned according to the region's pin count
class ShenandoahSynchronizePinnedRegionStates : public ShenandoahHeapRegionClosure {
private:
  ShenandoahHeapLock* const _lock;

public:
  ShenandoahSynchronizePinnedRegionStates();

  void heap_region_do(ShenandoahHeapRegion* r) override;
  bool is_thread_safe() override { return true; }

  void synchronize_pin_count(ShenandoahHeapRegion* r);
};

class ShenandoahMarkingContext;

// Synchronizes region pinned status, sets update watermark and adjust live data tally for regions
class ShenandoahFinalMarkUpdateRegionStateClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahMarkingContext* const _ctx;
  ShenandoahSynchronizePinnedRegionStates _pins;

  // Per worker bins: 2^0, ..., 2^log2i(4KB pages per region)
  uint** _dead_ranges_log2_worker;
  uint _dead_ranges_len;
  uint _num_workers;

  // [madv free]
  // Find dead page in region by scan marked objects.
  // Here each worker claims one of the old generation regions.
  void account_dead_ranges(ShenandoahHeapRegion* r, HeapWord* bottom, HeapWord* limit) {
    // uint worker_id = r->worker_id();
    // uint worker_id = _worker_id;
    // if (((uintptr_t)limit) - ((uintptr_t)bottom) < 4096)
    //   return;
    // assert(worker_id < _num_workers, "Dead Range worker id overflow");
    // // // DEBUG
    // // log_info(gc)("begin dead ranges [" PTR_FORMAT ", " PTR_FORMAT "]", p2i(bottom), p2i(limit));

    // HeapWord* start = bottom;
    // HeapWord* dead_obj;
    // uintptr_t dead_page_start, live_page_start;
    // oop obj;
    // int tmp_dead_pages;
    // size_t tmp_stt, tmp_free_cycle = 0;
    // size_t stt_cycle = os::rdtsc();

    // // Scan objects
    // while (start < limit) {
    //   obj = cast_to_oop(start);
    //   if (!_ctx->is_marked(obj)) { // Object is not marked
    //     // Dead range is [dead_obj, next live obj)
    //     dead_obj = start;
    //     start = _ctx->get_next_marked_addr(start, limit);
    //     dead_page_start = (((uintptr_t)dead_obj) + 4096 -1) >> 12;
    //     live_page_start = ((uintptr_t)start) >> 12;
    //     tmp_dead_pages = live_page_start - dead_page_start;
    //     if (tmp_dead_pages > 0) {
    //       assert(log2i(tmp_dead_pages) < (int)_dead_ranges_len, "dead range len %d, %d", tmp_dead_pages, _dead_ranges_len);
    //       // Account consecutive dead pages per worker.
    //       // log_info(gc)("worker id %d", worker_id);
    //       _dead_ranges_log2_worker[worker_id][log2i(tmp_dead_pages)] += 1;
    //       // Free dead range.
    //       if (UseFreeDeadPage) {
    //         tmp_stt = os::rdtsc();
    //         // DEBUG
    //         // Copy::zero_to_bytes((char*)dead_obj, (uintptr_t)start - (uintptr_t)dead_obj);
    //         // Copy::zero_to_bytes((char*)(dead_page_start << 12), tmp_dead_pages << 12);
    //         if (UseProfileRegionMajflt) {
    //           if(os::adc_advise_free_range(dead_page_start << 12, live_page_start << 12)) {
    //             log_info(gc)("[account_dead_ranges] fails adc_advise_free_range, stt: " PTR_FORMAT " end: " PTR_FORMAT, dead_page_start << 12, live_page_start << 12);
    //             os::abort();
    //           }
    //         } else if (UseMadvFree) {
    //           os::free_page_frames(true,
    //             (char*)(dead_page_start << 12), tmp_dead_pages << 12);
    //         } else if (UseMadvDontneed) {
    //           os::free_page_frames(false,
    //             (char*)(dead_page_start << 12), tmp_dead_pages << 12);
    //         }
    //         tmp_free_cycle += os::rdtsc() - tmp_stt;
    //       }
    //     }
    //     // // DEBUG
    //     // log_info(gc)("dead range [" PTR_FORMAT ", " PTR_FORMAT "]", p2i(dead_obj), p2i(start));
    //   } else { // Object is marked
    //     start += obj->size();
    //   }
    // }
    // r->add_scan_deadrange_cycle(os::rdtsc() - stt_cycle - tmp_free_cycle);
    // r->add_free_deadrange_cycle(tmp_free_cycle);
    // r->add_deadrange_count(1);
  }

  void dump_dead_ranges() {
    uint count = 0;

    for (uint b = 0; b < _dead_ranges_len; b++) {
      count = 0;
      for (uint w = 0; w < _num_workers; w++)
          count += _dead_ranges_log2_worker[w][b];
      if (count > 0)
        log_info(gc)("Dead Ranges bin [2^%u]: %u", b, count);
    }
  }

public:
  explicit ShenandoahFinalMarkUpdateRegionStateClosure(ShenandoahMarkingContext* ctx);
  ~ShenandoahFinalMarkUpdateRegionStateClosure();

  void heap_region_do(ShenandoahHeapRegion* r) override;
  bool is_thread_safe() override { return true; }
};

class ShenandoahDeadRangeCounter : public StackObj {
private:
  ShenandoahMarkingContext *_ctx;

  // Per worker bins: 2^0, ..., 2^log2i(4KB pages per region)
  uint** _dead_ranges_log2_worker;
  uint _dead_ranges_len;
  uint _num_workers;

  // No implicit copying: iterators should be passed by reference to capture the state
  NONCOPYABLE(ShenandoahDeadRangeCounter);

public:
  ShenandoahDeadRangeCounter(ShenandoahMarkingContext *ctx, uint nworkers);
  ~ShenandoahDeadRangeCounter();

  void add_counter(uint worker, uint index, uint i);
  uint nworkers() { return _num_workers; };
  uint dead_ranges_len() { return _dead_ranges_len; };
  // Reset iterator to default state
  void dump_dead_ranges() const;
};

// Synchronizes region pinned status, sets update watermark and adjust live data tally for regions
class ShenandoahFreeDeadRangeClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahMarkingContext* const _ctx;
  ShenandoahDeadRangeCounter* const _res;

  // [madv free]
  // Find dead page in region by scan marked objects.
  // Here each worker claims one of the old generation regions.
  void account_dead_ranges(ShenandoahHeapRegion* r, HeapWord* bottom, HeapWord* limit);

public:
  explicit ShenandoahFreeDeadRangeClosure(ShenandoahMarkingContext *ctx, ShenandoahDeadRangeCounter *res);

  void heap_region_do(ShenandoahHeapRegion* r) override;
  bool is_thread_safe() override { return true; }
};

class ShenandoahFreeDeadRangeTask : public WorkerTask {
private:
  ShenandoahHeap* const _sh;
  ShenandoahRegionIterator* const _regions;
  ShenandoahDeadRangeCounter* const _res;
  bool _concurrent;
public:
  ShenandoahFreeDeadRangeTask(ShenandoahHeap* sh,
                           ShenandoahRegionIterator* iterator,
                           ShenandoahDeadRangeCounter* res,
                           bool concurrent) :
    WorkerTask("Shenandoah Free Dead Range"),
    _sh(sh),
    _regions(iterator),
    _res(res),
    _concurrent(concurrent)
  {}

  void work(uint worker_id) {
    if (_concurrent) {
      ShenandoahConcurrentWorkerSession worker_session(worker_id);
      ShenandoahSuspendibleThreadSetJoiner stsj;
      do_work(worker_id);
    } else {
      ShenandoahParallelWorkerSession worker_session(worker_id);
      do_work(worker_id);
    }
  }

private:
  void do_work(uint worker_id);
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHHEAPREGIONCLOSURES_HPP
