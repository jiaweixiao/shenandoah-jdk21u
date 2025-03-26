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

  // [madv free]
  // Find dead page in region.
  // First merge consecutive pages and record their length.
  // Each element is the number of range with given length.

  // Per worker bins: 2^0, ..., 2^log2i(4KB pages per region)
  uint** _dead_ranges_log2_worker;
  uint _dead_ranges_len;
  uint _num_workers;
  uint* _dead_pages_worker;

  void account_dead_ranges(ShenandoahHeapRegion* r, HeapWord* start, HeapWord* limit) {
    assert(_worker_id < _num_workers, "Dead Range worker id overflow");
    // [madv free]
    // Find dead page in region.
    // Here each worker claims one of the old generation regions.

    // Ceiling of number of pages.
    uint num_pages = (((uintptr_t)limit) - ((uintptr_t)start) + 4096 - 1) >> 12;
    if (num_pages == 0)
      return;

    // Ceiling of length of bitmap array.
    uint bitmap_len = (num_pages + BitsPerWord - 1) >> LogBitsPerWord;
    // log_info(gc)("Dead num_pages %u bitmap_len %u", num_pages, bitmap_len);
    BitMap::bm_word_t live_page_bits[bitmap_len] = {0};
    BitMapView bm(live_page_bits, num_pages);

    size_t obj_size, live_bytes = 0;
    uint start_id, end_id;
    oop obj; 

    while (start < limit) {
      obj = cast_to_oop(start);

      if (_ctx->is_marked(obj)) { // Object is marked.
        assert(obj->klass() != nullptr, "klass should not be nullptr");
        // Obj size in heap word.
        obj_size = obj->size();
        start_id = (((uintptr_t)start) - ((uintptr_t)r->bottom())) >> 12;
        // Ceiling of id
        end_id = (((uintptr_t)start) - ((uintptr_t)r->bottom()) + (obj_size << LogHeapWordSize) + 4096 - 1) >> 12;
        assert(start_id != end_id, "Dead Pages of Region 1");
        for (; start_id < end_id; start_id++) {
          bm.set_bit(start_id);
        }
        live_bytes += obj_size << LogHeapWordSize;
        start += obj_size;
      } else { // Object is not marked.
        start = _ctx->get_next_marked_addr(start, limit);
      }
    }
    // If the last obj is not aligned to page,
    // set it marked since it has objs above tams.
    if (((uintptr_t)limit) % 4096)
      bm.set_bit(num_pages - 1);

    // First merge consecutive pages and record their length.
    // Each element is the number of range with given length.
    uint range_start = 0, range_len = 0;
    bool is_prev_dead = false;
    for (uint i = 0; i < num_pages; i++) {
      if (bm.at(i) == false) { // Dead Page
        if (is_prev_dead) // Continue dead range
          range_len += 1;
        else { // Start dead range
          range_start = i;
          range_len = 1;
        }
        is_prev_dead = true;
      } else { // Live Page
        if (is_prev_dead) { // Finish dead range
          assert(log2i(range_len) < (int)_dead_ranges_len, "dead range len");
          // log_info(gc)("Dead Range worker id %u", _worker_id);
          _dead_ranges_log2_worker[_worker_id][log2i(range_len)] += 1;
        }
        is_prev_dead = false;
      }
    }
    if (is_prev_dead) { // Last page is dead
        assert(log2i(range_len) < (int)_dead_ranges_len, "dead range len");
      _dead_ranges_log2_worker[_worker_id][log2i(range_len)] += 1;
    }

    uint count = num_pages - bm.count_one_bits();
    if (count > 0) {
      _dead_pages_worker[_worker_id] += count;
      // log_info(gc)("Dead Pages of Region %lu: %u, live bytes %lu",
      //         r->index(), count, live_bytes);
    }
  }

  void dump_dead_ranges() {
    uint count = 0;
    for (uint i = 0; i < _num_workers; i++)
      count += _dead_pages_worker[i];
    log_info(gc)("Dead Pages: %u", count);

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
