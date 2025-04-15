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
  void account_dead_ranges(HeapWord* bottom, HeapWord* limit) {
    if (((uintptr_t)limit) - ((uintptr_t)bottom) < 4096)
      return;
    assert(_worker_id < _num_workers, "Dead Range worker id overflow");
    // // DEBUG
    // log_info(gc)("begin dead ranges [" PTR_FORMAT ", " PTR_FORMAT "]", p2i(bottom), p2i(limit));

    HeapWord* start = bottom;
    HeapWord* dead_obj;
    oop obj;
    int sum_dead_pages = 0, tmp_dead_pages;

    // Scan objects
    while (start < limit) {
      obj = cast_to_oop(start);
      if (!_ctx->is_marked(obj)) { // Object is not marked
        // Dead range is [dead_obj, next live obj)
        dead_obj = start;
        start = _ctx->get_next_marked_addr(start, limit);
        tmp_dead_pages = (((uintptr_t)start) >> 12) - ((((uintptr_t)dead_obj) + 4096 -1) >> 12);
        if (tmp_dead_pages > 0) {
          assert(log2i(tmp_dead_pages) < (int)_dead_ranges_len, "dead range len %d, %d", tmp_dead_pages, _dead_ranges_len);
          _dead_ranges_log2_worker[_worker_id][log2i(tmp_dead_pages)] += 1;
          // if (dead_obj+obj->size() < start) {
          //   // DEBUG
          //   Copy::zero_to_bytes(((char*)(dead_obj+obj->size())), ((uintptr_t)start) - ((uintptr_t)(dead_obj+obj->size())));
          //   // os::free_page_frames(true,
          //   //     ((char*)bottom) + (range_start << 12), range_len << 12);
          // }
          Copy::zero_to_bytes(((char*)(dead_obj)), ((uintptr_t)start) - ((uintptr_t)(dead_obj)));
          sum_dead_pages += tmp_dead_pages;
        }
        // // DEBUG
        // log_info(gc)("dead range [" PTR_FORMAT ", " PTR_FORMAT "]", p2i(dead_obj), p2i(start));
      } else { // Object is marked
        start += obj->size();
      }
    }
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

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHHEAPREGIONCLOSURES_HPP
