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

#include "gc/shenandoah/shenandoahHeapRegionClosures.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.hpp"
#include "gc/shenandoah/shenandoahSharedVariables.hpp"

ShenandoahSynchronizePinnedRegionStates::ShenandoahSynchronizePinnedRegionStates() :
  _lock(ShenandoahHeap::heap()->lock()) { }

void ShenandoahSynchronizePinnedRegionStates::heap_region_do(ShenandoahHeapRegion* r) {
  // Drop "pinned" state from regions that no longer have a pinned count. Put
  // regions with a pinned count into the "pinned" state.
  if (r->is_active()) {
    synchronize_pin_count(r);
  }
}

void ShenandoahSynchronizePinnedRegionStates::synchronize_pin_count(ShenandoahHeapRegion* r) {
  if (r->is_pinned()) {
    if (r->pin_count() == 0) {
      ShenandoahHeapLocker locker(_lock);
      r->make_unpinned();
    }
  } else {
    if (r->pin_count() > 0) {
      ShenandoahHeapLocker locker(_lock);
      r->make_pinned();
    }
  }
}

ShenandoahFinalMarkUpdateRegionStateClosure::ShenandoahFinalMarkUpdateRegionStateClosure(ShenandoahMarkingContext *ctx) :
        _ctx(ctx) {
  // if (UseProfileDeadPageInOld && _ctx != nullptr) {
  //   _worker_id = 0;
  //   _num_workers = ShenandoahHeap::heap()->workers()->active_workers();

  //   // bins: 2^0, ..., 2^log2i(4KB pages per region)
  //   _dead_ranges_len = log2i(ShenandoahHeapRegion::region_size_bytes() >> 12) + 1;
  //   _dead_ranges_log2_worker = NEW_C_HEAP_ARRAY(uint*, _num_workers, mtGC);

  //   for (uint i = 0; i < _num_workers; i++) {
  //     _dead_ranges_log2_worker[i] = NEW_C_HEAP_ARRAY(uint, _dead_ranges_len, mtGC);
  //     memset(_dead_ranges_log2_worker[i], 0, sizeof(uint) * _dead_ranges_len);
  //   }
  // }
}

ShenandoahFinalMarkUpdateRegionStateClosure::~ShenandoahFinalMarkUpdateRegionStateClosure() {
  // if (UseProfileDeadPageInOld && _ctx != nullptr) {
  //   dump_dead_ranges();
  //   for (uint i = 0; i < _num_workers; i++) {
  //     FREE_C_HEAP_ARRAY(uint, _dead_ranges_log2_worker[i]);
  //   }
  //   FREE_C_HEAP_ARRAY(uint*, _dead_ranges_log2_worker);
  // }
}

void ShenandoahFinalMarkUpdateRegionStateClosure::heap_region_do(ShenandoahHeapRegion* r) {
  if (r->is_active()) {
    if (_ctx != nullptr) {
      // _ctx may be null when this closure is used to sync only the pin status
      // update the watermark of old regions. For old regions we cannot reset
      // the TAMS because we rely on that to keep promoted objects alive after
      // old marking is complete.

      // All allocations past TAMS are implicitly live, adjust the region data.
      // Bitmaps/TAMS are swapped at this point, so we need to poll complete bitmap.
      HeapWord *tams = _ctx->top_at_mark_start(r);
      HeapWord *top = r->top();
      if (top > tams) {
        r->increase_live_data_alloc_words(pointer_delta(top, tams));
      }
    }

    // We are about to select the collection set, make sure it knows about
    // current pinning status. Also, this allows trashing more regions that
    // now have their pinning status dropped.
    _pins.synchronize_pin_count(r);

    // Remember limit for updating refs. It's guaranteed that we get no
    // from-space-refs written from here on.
    r->set_update_watermark_at_safepoint(r->top());

    // if (UseProfileDeadPageInOld && _ctx != nullptr && !r->is_humongous() && r->has_live()) {
    //   // Account dead ranges.
    //   account_dead_ranges(r, r->bottom(), _ctx->top_at_mark_start(r));
    // }
  } else {
    assert(!r->has_live(), "Region " SIZE_FORMAT " should have no live data", r->index());
    assert(_ctx == nullptr || _ctx->top_at_mark_start(r) == r->top(),
           "Region " SIZE_FORMAT " should have correct TAMS", r->index());
  }
}

ShenandoahDeadRangeCounter::ShenandoahDeadRangeCounter(ShenandoahHeap* const heap, ShenandoahMarkingContext* const ctx, uint nworkers) :
  _heap(heap),
  _ctx(ctx),
  _num_workers(nworkers) {
  if (ctx != nullptr) {
    // bins: 2^0, ..., 2^log2i(4KB pages per region)
    _dead_ranges_len = log2i(ShenandoahHeapRegion::region_size_bytes() >> 12) + 1;
    _dead_ranges_log2_worker = NEW_C_HEAP_ARRAY(uint*, _num_workers, mtGC);
    _dead_pages_worker = NEW_C_HEAP_ARRAY(uint, _num_workers, mtGC);

    for (uint i = 0; i < _num_workers; i++) {
      _dead_ranges_log2_worker[i] = NEW_C_HEAP_ARRAY(uint, _dead_ranges_len, mtGC);
      memset(_dead_ranges_log2_worker[i], 0, sizeof(uint) * _dead_ranges_len);
    }
    memset(_dead_pages_worker, 0, sizeof(uint) * _num_workers);
  }
}

ShenandoahDeadRangeCounter::~ShenandoahDeadRangeCounter() {
  if (_ctx != nullptr) {
    dump_dead_ranges();
    for (uint i = 0; i < _num_workers; i++) {
      FREE_C_HEAP_ARRAY(uint, _dead_ranges_log2_worker[i]);
    }
    FREE_C_HEAP_ARRAY(uint*, _dead_ranges_log2_worker);
    FREE_C_HEAP_ARRAY(uint, _dead_pages_worker);
  }
}

inline void ShenandoahDeadRangeCounter::inc_counter(uint worker, uint dead_pages) {
  _dead_ranges_log2_worker[worker][log2i(dead_pages)] += 1;
  _dead_pages_worker[worker] += dead_pages;
}

void ShenandoahDeadRangeCounter::dump_dead_ranges() const {
  uint count = 0;
  long pte_not_p, pte_p, pte_none;

  for (uint b = 0; b < _dead_ranges_len; b++) {
    count = 0;
    for (uint w = 0; w < _num_workers; w++)
        count += _dead_ranges_log2_worker[w][b];
    if (count > 0)
      log_info(gc)("Dead Ranges bin [2^%u]: %u", b, count);
  }

  if (UseProfileTraceIncome) {
    count = 0;
    for (uint w = 0; w < _num_workers; w++)
      count += _dead_pages_worker[w];
    log_info(gc)("Dead Pages: %u", count);
  }

  if (!os::kernel_mm_rswap_page_not_present(&pte_not_p, &pte_p, &pte_none)) {
    log_info(gc)("pte_not_p %ld, pte_p %ld, pte_none %ld", pte_not_p, pte_p, pte_none);
  } else {
    log_info(gc)("pte_not_p -1, pte_p -1, pte_none -1");
  }
}

size_t ShenandoahDeadRangeCounter::account_dead_range(uint worker_id, ShenandoahHeapRegion* r, uintptr_t dead_page_start, size_t dead_pages) {
  // Account consecutive dead pages per worker.
  // log_info(gc)("worker id %d", _worker_id);
  size_t newly_dead_pages = 0;
  if (!UseProfileTraceIncome) {
    inc_counter(worker_id, dead_pages);
  }
  // Free dead range.
  if (UseFreeDeadPage) {
    // DEBUG
    // Copy::zero_to_bytes((char*)(dead_page_start << 12), dead_pages << 12);
    if (UseMadvFree) {
      if (UseProfileTraceIncome) {
        // Only for newly found consecutive dead pages
        uintptr_t i = dead_page_start;
        while (i < dead_page_start + dead_pages) {
          if (_ctx->is_marked_page((HeapWord*)(i << 12))) {
            // Found a live page at prev GC
            uintptr_t stt_i = i;
            uintptr_t j = i;
            while (j < dead_page_start + dead_pages &&
                  _ctx->is_marked_page((HeapWord*)(j << 12))) {
              j++;
            }
            uintptr_t end_i = j - 1;
            size_t sum_pages = end_i - stt_i + 1;

            newly_dead_pages += sum_pages;
            for (uintptr_t x=stt_i; x <= end_i; x++) {
              _ctx->clear_page((HeapWord*)(x << 12));
            }
            inc_counter(worker_id, sum_pages);
            os::free_page_frames(true, (char*)(stt_i << 12), sum_pages << 12);
            i = end_i + 1;
          } else {
            i++; // move to next
          }
        }
      } else {
        os::free_page_frames(true,
          (char*)(dead_page_start << 12), dead_pages << 12);
      }
    } else if (UseMadvDontneed) {
      os::free_page_frames(false,
        (char*)(dead_page_start << 12), dead_pages << 12);
    } else if (UseProfileRegionMajflt) {
      if (UseProfileTraceIncome && UseSkipswapSharedMemory) {
        // Only for newly found consecutive dead pages
        uintptr_t i = dead_page_start;
        while (i < dead_page_start + dead_pages) {
          if (_ctx->is_marked_page((HeapWord*)(i << 12))) {
            // Found a live page at prev GC
            uintptr_t stt_i = i;
            uintptr_t j = i;
            while (j < dead_page_start + dead_pages &&
                  _ctx->is_marked_page((HeapWord*)(j << 12))) {
              j++;
            }
            uintptr_t end_i = j - 1;
            size_t sum_pages = end_i - stt_i + 1;

            newly_dead_pages += sum_pages;
            for (uintptr_t x=stt_i; x <= end_i; x++) {
              _ctx->clear_page((HeapWord*)(x << 12));
              if (_heap->is_remote_page(x << 12)) {
                r->add_remote_deadpage_count(1);
              }
            }
            inc_counter(worker_id, sum_pages);
            _heap->set_free_range(stt_i << 12, sum_pages << 12);
            i = end_i + 1;
          } else {
            i++; // move to next
          }
        }
      } else {
        if(_heap->set_free_range(dead_page_start << 12, dead_pages << 12)) {
          log_info(gc)("[account_dead_range] fails adc_advise_free_range, stt: " PTR_FORMAT " end: " PTR_FORMAT, dead_page_start << 12, (dead_page_start + dead_pages) << 12);
          os::abort();
        }
      }
    }
  }

  if (!UseProfileTraceIncome) {
    return dead_pages;
  } else {
    return newly_dead_pages;
  }
}

void ShenandoahDeadRangeCounter::account_dead_ranges_humongous_start(uint worker_id, ShenandoahHeapRegion* r) {
  assert(r->is_humongous_start(), "reclaim regions starting with the first one");

  oop humongous_obj = cast_to_oop(r->bottom());
  if (_ctx->is_marked(humongous_obj)) {
    return;
  }

  size_t words_size = humongous_obj->size();
  size_t required_regions = ShenandoahHeapRegion::required_regions(words_size * HeapWordSize);
  size_t index = r->index() + required_regions - 1;

  // The trailing region may not full.
  size_t remainder = words_size & ShenandoahHeapRegion::region_size_words_mask();
  size_t remainder_pages = (remainder * HeapWordSize) >> 12;

  for(uint i = 0; i < required_regions; i++) {
    // Reclaim from tail. Otherwise, assertion fails when printing region to trace log,
    // as it expects that every region belongs to a humongous region starting with a humongous start region.
    ShenandoahHeapRegion* region = _heap->get_region(index --);

    assert(region->is_humongous(), "expect correct humongous start or continuation");
    assert(!region->is_cset(), "Humongous region should not be in collection set");

    uintptr_t dead_page_start = ((uintptr_t)(region->bottom())) >> 12;
    size_t dead_pages = 0;
    size_t stt = os::rdtsc();

    // The trailing region may not full.
    if (i == required_regions && remainder_pages > 0) {
      dead_pages = remainder_pages;
    } else {
      dead_pages = region->region_size_bytes() >> 12;
    }

    // Consecutive dead pages
    dead_pages = account_dead_range(worker_id, region, dead_page_start, dead_pages);

    // update counters
    region->add_scan_deadrange_cycle(0);
    region->add_free_deadrange_cycle(os::rdtsc() - stt);
    region->add_deadrange_count(1);
    region->add_deadpage_count(dead_pages);
  }

  return ;
}

void ShenandoahDeadRangeCounter::account_dead_ranges_regular(uint worker_id, ShenandoahHeapRegion* r, HeapWord* bottom, HeapWord* limit) {
  // Scan region to find the consecutive dead pages.
  if (((uintptr_t)limit) - ((uintptr_t)bottom) < 4096)
    return;
  assert(worker_id < _num_workers, "Dead Range worker id overflow");
  // // DEBUG
  // log_info(gc)("begin dead ranges [" PTR_FORMAT ", " PTR_FORMAT "]", p2i(bottom), p2i(limit));

  HeapWord* start = bottom;
  HeapWord* dead_obj;
  uintptr_t dead_page_start, live_page_start;
  oop obj;
  size_t tmp_dead_pages = 0;
  size_t sum_dead_pages = 0;
  size_t tmp_stt = 0;
  size_t tmp_free_cycle = 0;
  size_t stt_cycle = os::rdtsc();

  // Scan objects
  while (start < limit) {
    obj = cast_to_oop(start);
    if (!_ctx->is_marked(obj)) { // Object is not marked
      // Dead range is [dead_obj, next live obj)
      dead_obj = start;
      start = _ctx->get_next_marked_addr(start, limit);
      dead_page_start = (((uintptr_t)dead_obj) + 4096 -1) >> 12;
      live_page_start = ((uintptr_t)start) >> 12;
      tmp_dead_pages = live_page_start - dead_page_start;
      if (live_page_start > dead_page_start) {
        assert(log2i(tmp_dead_pages) < _dead_ranges_len, "dead range len %d, %d", tmp_dead_pages, _dead_ranges_len);

        tmp_stt = os::rdtsc();
        // Consecutive dead pages
        tmp_dead_pages = account_dead_range(worker_id, r, dead_page_start, tmp_dead_pages);
        tmp_free_cycle += os::rdtsc() - tmp_stt;
        sum_dead_pages += tmp_dead_pages;
      }
      // // DEBUG
      // log_info(gc)("dead range [" PTR_FORMAT ", " PTR_FORMAT "]", p2i(dead_obj), p2i(start));
    } else { // Object is marked
      HeapWord *start_orig = start;
      // Scan end bitmap
      HeapWord *obj_end = _ctx->get_next_marked_end_addr(start_orig, limit);
      start = obj_end + 1;
      assert((start_orig + obj->size() >= limit) || (start_orig + obj->size() < limit && start_orig + obj->size() == start), "fail to scan end bitmap start_orig+objsize %p, start %p, limit %p, objsize %lu", (void*)(start_orig+obj->size()), (void*)start, (void*)limit, obj->size());

      if (UseProfileTraceIncome) {
        _ctx->mark_page(obj);
      }
    }
  }
  r->add_scan_deadrange_cycle(os::rdtsc() - stt_cycle - tmp_free_cycle);
  r->add_free_deadrange_cycle(tmp_free_cycle);
  r->add_deadrange_count(1);
  r->add_deadpage_count(sum_dead_pages);
}

void ShenandoahPostMarkFreeDeadRangeClosure::heap_region_do(ShenandoahHeapRegion* r) {
  if (r->is_active() && _ctx != nullptr) {
    // Account dead ranges.
    if (!r->is_humongous()) {
      // Regular region
      _res->account_dead_ranges_regular(_worker_id, r, r->bottom(), _ctx->top_at_mark_start(r));
    } else if (r->is_humongous_start()) {
      // Humongous start region
      _res->account_dead_ranges_humongous_start(_worker_id, r);
    }
  }
}

void ShenandoahTrashCSetFreeDeadRangeClosure::heap_region_do(ShenandoahHeapRegion* r) {
  if (_ctx != nullptr) {
    // Collection set only has regular regions
    _res->account_dead_ranges_regular(_worker_id, r, r->bottom(), r->end());
  }
}

void ShenandoahPostCompactFreeDeadRangeClosure::heap_region_do(ShenandoahHeapRegion* r) {
  if (_ctx != nullptr) {
    if (r->is_regular() && r->used() == 0) {
      // Reclaim empty regular regions
      _res->account_dead_ranges_regular(_worker_id, r, r->bottom(), r->end());
    } else if (r->used() > 0 && r->free() >= 4096) {
      // Collection set only has regular regions
      // Free [top, end)
      _res->account_dead_ranges_regular(_worker_id, r, r->top(), r->end());
    }
  }
}

void ShenandoahPostMarkFreeDeadRangeTask::do_work(uint worker_id) {
  ShenandoahHeapRegion *r;
  ShenandoahGeneration *active_gen = _heap->active_generation();
  ShenandoahPostMarkFreeDeadRangeClosure cl(_heap->marking_context(), _res);
  ShenandoahAffiliation active_type = FREE;
  bool is_global = false;

  if (active_gen->is_young())
    active_type = YOUNG_GENERATION;
  else if (active_gen->is_old())
    active_type = OLD_GENERATION;
  else
    is_global = true;
  // log_info(gc)("worker id %u", worker_id);
  cl.set_worker(worker_id);
  while ((r = _regions->next()) != nullptr) {
    // [madv free] [profile marking income]
    // How about FREE affiliation? We skip it since the a YOUNG_GENERATION region
    // has no live after Young marking is still YOUNG_GENERATION.
    if ((!is_global && r->affiliation() == active_type) ||
        (is_global && r->affiliation() != FREE))
      cl.heap_region_do(r);
    if (_heap->check_cancelled_gc_and_yield(_concurrent)) {
      return;
    }
  }
}
