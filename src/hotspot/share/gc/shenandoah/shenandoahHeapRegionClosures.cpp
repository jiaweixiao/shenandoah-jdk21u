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

ShenandoahDeadRangeCounter::ShenandoahDeadRangeCounter(ShenandoahMarkingContext *ctx, uint nworkers) : 
  _ctx(ctx),
  _num_workers(nworkers) {
  if (ctx != nullptr) {
    // bins: 2^0, ..., 2^log2i(4KB pages per region)
    _dead_ranges_len = log2i(ShenandoahHeapRegion::region_size_bytes() >> 12) + 1;
    _dead_ranges_log2_worker = NEW_C_HEAP_ARRAY(uint*, _num_workers, mtGC);

    for (uint i = 0; i < _num_workers; i++) {
      _dead_ranges_log2_worker[i] = NEW_C_HEAP_ARRAY(uint, _dead_ranges_len, mtGC);
      memset(_dead_ranges_log2_worker[i], 0, sizeof(uint) * _dead_ranges_len);
    }
  }
}

ShenandoahDeadRangeCounter::~ShenandoahDeadRangeCounter() {
  if (_ctx != nullptr) {
    dump_dead_ranges();
    for (uint i = 0; i < _num_workers; i++) {
      FREE_C_HEAP_ARRAY(uint, _dead_ranges_log2_worker[i]);
    }
    FREE_C_HEAP_ARRAY(uint*, _dead_ranges_log2_worker);
  }
}

inline void ShenandoahDeadRangeCounter::add_counter(uint worker, uint index, uint i) {
  _dead_ranges_log2_worker[worker][index] += i;
}

void ShenandoahDeadRangeCounter::dump_dead_ranges() const {
  uint count = 0;

  for (uint b = 0; b < _dead_ranges_len; b++) {
    count = 0;
    for (uint w = 0; w < _num_workers; w++)
        count += _dead_ranges_log2_worker[w][b];
    if (count > 0)
      log_info(gc)("Dead Ranges bin [2^%u]: %u", b, count);
  }
}

ShenandoahFreeDeadRangeClosure::ShenandoahFreeDeadRangeClosure(ShenandoahMarkingContext *ctx, ShenandoahDeadRangeCounter *res) :
  _ctx(ctx),
  _res(res) {}

void ShenandoahFreeDeadRangeClosure::account_dead_ranges(ShenandoahHeapRegion* r, HeapWord* bottom, HeapWord* limit) {
  if (((uintptr_t)limit) - ((uintptr_t)bottom) < 4096)
    return;
  assert(_worker_id < _res->nworkers(), "Dead Range worker id overflow");
  // // DEBUG
  // log_info(gc)("begin dead ranges [" PTR_FORMAT ", " PTR_FORMAT "]", p2i(bottom), p2i(limit));

  HeapWord* start = bottom;
  HeapWord* dead_obj;
  uintptr_t dead_page_start, live_page_start;
  oop obj;
  int tmp_dead_pages;
  int dead_ranges_len = (int)_res->dead_ranges_len();
  size_t tmp_stt, tmp_free_cycle = 0;
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
      if (tmp_dead_pages > 0) {
        assert(log2i(tmp_dead_pages) < dead_ranges_len, "dead range len %d, %d", tmp_dead_pages, dead_ranges_len);
        // Account consecutive dead pages per worker.
        // log_info(gc)("worker id %d", _worker_id);
        _res->add_counter(_worker_id, log2i(tmp_dead_pages), 1);
        // Free dead range.
        if (UseFreeDeadPage) {
          tmp_stt = os::rdtsc();
          // DEBUG
          // Copy::zero_to_bytes((char*)dead_obj, (uintptr_t)start - (uintptr_t)dead_obj);
          // Copy::zero_to_bytes((char*)(dead_page_start << 12), tmp_dead_pages << 12);
          if (UseProfileRegionMajflt) {
            if(os::adc_advise_free_range(dead_page_start << 12, live_page_start << 12)) {
              log_info(gc)("[account_dead_ranges] fails adc_advise_free_range, stt: " PTR_FORMAT " end: " PTR_FORMAT, dead_page_start << 12, live_page_start << 12);
              os::abort();
            }
          } else if (UseMadvFree) {
            os::free_page_frames(true,
              (char*)(dead_page_start << 12), tmp_dead_pages << 12);
          } else if (UseMadvDontneed) {
            os::free_page_frames(false,
              (char*)(dead_page_start << 12), tmp_dead_pages << 12);
          }
          tmp_free_cycle += os::rdtsc() - tmp_stt;
        }
      }
      // // DEBUG
      // log_info(gc)("dead range [" PTR_FORMAT ", " PTR_FORMAT "]", p2i(dead_obj), p2i(start));
    } else { // Object is marked
      start += obj->size();
    }
  }
  r->add_scan_deadrange_cycle(os::rdtsc() - stt_cycle - tmp_free_cycle);
  r->add_free_deadrange_cycle(tmp_free_cycle);
  r->add_deadrange_count(1);
}

void ShenandoahFreeDeadRangeClosure::heap_region_do(ShenandoahHeapRegion* r) {
  if (r->is_active() && _ctx != nullptr && !r->is_humongous() && r->has_live()) {
      // Account dead ranges.
      account_dead_ranges(r, r->bottom(), _ctx->top_at_mark_start(r));
  }
}

void ShenandoahFreeDeadRangeTask::do_work(uint worker_id) {
  ShenandoahHeapRegion *r;
  ShenandoahGeneration *active_gen = _sh->active_generation();
  ShenandoahFreeDeadRangeClosure cl(ShenandoahHeap::heap()->marking_context(), _res);
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
    if (r->affiliation() == active_type || is_global)
      cl.heap_region_do(r);
    if (_sh->check_cancelled_gc_and_yield(_concurrent)) {
      return;
    }
  }
}
