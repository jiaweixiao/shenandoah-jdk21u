/*
 * Copyright (c) 2017, 2019, Red Hat, Inc. All rights reserved.
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

#include "gc/shared/gc_globals.hpp"
#include "gc/shared/workerPolicy.hpp"
#include "gc/shenandoah/shenandoahWorkerPolicy.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/threads.hpp"

uint ShenandoahWorkerPolicy::_prev_par_marking     = 0;
uint ShenandoahWorkerPolicy::_prev_conc_marking    = 0;
uint ShenandoahWorkerPolicy::_prev_conc_rs_scanning = 0;
uint ShenandoahWorkerPolicy::_prev_conc_evac       = 0;
uint ShenandoahWorkerPolicy::_prev_conc_root_proc  = 0;
uint ShenandoahWorkerPolicy::_prev_conc_refs_proc  = 0;
uint ShenandoahWorkerPolicy::_prev_fullgc          = 0;
uint ShenandoahWorkerPolicy::_prev_degengc         = 0;
uint ShenandoahWorkerPolicy::_prev_conc_update_ref = 0;
uint ShenandoahWorkerPolicy::_prev_par_update_ref  = 0;
uint ShenandoahWorkerPolicy::_prev_conc_cleanup    = 0;
uint ShenandoahWorkerPolicy::_prev_conc_reset      = 0;
size_t ShenandoahWorkerPolicy::_young_used         = 0;
size_t ShenandoahWorkerPolicy::_young_max          = 0;
size_t ShenandoahWorkerPolicy::_prev_young_used    = 0;
size_t ShenandoahWorkerPolicy::_prev_young_max     = 0;
uint   ShenandoahWorkerPolicy::_prev_conc_workers  = 0;

uint ShenandoahWorkerPolicy::calc_workers_for_init_marking() {
  uint active_workers = (_prev_par_marking == 0) ? ParallelGCThreads : _prev_par_marking;

  _prev_par_marking =
    WorkerPolicy::calc_active_workers(ParallelGCThreads,
                                      active_workers,
                                      Threads::number_of_non_daemon_threads());
  return _prev_par_marking;
}

uint ShenandoahWorkerPolicy::calc_workers_for_conc_marking() {
  uint active_workers = (_prev_conc_marking == 0) ?  ConcGCThreads : _prev_conc_marking;
  _prev_conc_marking =
    ShenandoahWorkerPolicy::calc_active_conc_workers(ConcGCThreads,
                                           active_workers,
                                           Threads::number_of_non_daemon_threads());
  return _prev_conc_marking;
}

uint ShenandoahWorkerPolicy::calc_workers_for_rs_scanning() {
  uint active_workers = (_prev_conc_rs_scanning == 0) ? ConcGCThreads : _prev_conc_rs_scanning;
  _prev_conc_rs_scanning =
    ShenandoahWorkerPolicy::calc_active_conc_workers(ConcGCThreads,
                                           active_workers,
                                           Threads::number_of_non_daemon_threads());
  return _prev_conc_rs_scanning;
}

// Reuse the calculation result from init marking
uint ShenandoahWorkerPolicy::calc_workers_for_final_marking() {
  return _prev_par_marking;
}

// Calculate workers for concurrent refs processing
uint ShenandoahWorkerPolicy::calc_workers_for_conc_refs_processing() {
  uint active_workers = (_prev_conc_refs_proc == 0) ? ConcGCThreads : _prev_conc_refs_proc;
  _prev_conc_refs_proc =
    ShenandoahWorkerPolicy::calc_active_conc_workers(ConcGCThreads,
                                           active_workers,
                                           Threads::number_of_non_daemon_threads());
  return _prev_conc_refs_proc;
}

// Calculate workers for concurrent root processing
uint ShenandoahWorkerPolicy::calc_workers_for_conc_root_processing() {
  uint active_workers = (_prev_conc_root_proc == 0) ? ConcGCThreads : _prev_conc_root_proc;
  _prev_conc_root_proc =
          ShenandoahWorkerPolicy::calc_active_conc_workers(ConcGCThreads,
                                                 active_workers,
                                                 Threads::number_of_non_daemon_threads());
  return _prev_conc_root_proc;
}

// Calculate workers for concurrent evacuation (concurrent GC)
uint ShenandoahWorkerPolicy::calc_workers_for_conc_evac() {
  uint active_workers = (_prev_conc_evac == 0) ? ConcGCThreads : _prev_conc_evac;
  _prev_conc_evac =
    ShenandoahWorkerPolicy::calc_active_conc_workers(ConcGCThreads,
                                           active_workers,
                                           Threads::number_of_non_daemon_threads());
  return _prev_conc_evac;
}

// Calculate workers for parallel fullgc
uint ShenandoahWorkerPolicy::calc_workers_for_fullgc() {
  uint active_workers = (_prev_fullgc == 0) ?  ParallelGCThreads : _prev_fullgc;
  _prev_fullgc =
    WorkerPolicy::calc_active_workers(ParallelGCThreads,
                                      active_workers,
                                      Threads::number_of_non_daemon_threads());
  return _prev_fullgc;
}

// Calculate workers for parallel degenerated gc
uint ShenandoahWorkerPolicy::calc_workers_for_stw_degenerated() {
  uint active_workers = (_prev_degengc == 0) ?  ParallelGCThreads : _prev_degengc;
  _prev_degengc =
    WorkerPolicy::calc_active_workers(ParallelGCThreads,
                                      active_workers,
                                      Threads::number_of_non_daemon_threads());
  return _prev_degengc;
}

// Calculate workers for concurrent reference update
uint ShenandoahWorkerPolicy::calc_workers_for_conc_update_ref() {
  uint active_workers = (_prev_conc_update_ref == 0) ? ConcGCThreads : _prev_conc_update_ref;
  _prev_conc_update_ref =
    ShenandoahWorkerPolicy::calc_active_conc_workers(ConcGCThreads,
                                           active_workers,
                                           Threads::number_of_non_daemon_threads());
  return _prev_conc_update_ref;
}

// Calculate workers for parallel reference update
uint ShenandoahWorkerPolicy::calc_workers_for_final_update_ref() {
  uint active_workers = (_prev_par_update_ref == 0) ? ParallelGCThreads : _prev_par_update_ref;
  _prev_par_update_ref =
    WorkerPolicy::calc_active_workers(ParallelGCThreads,
                                      active_workers,
                                      Threads::number_of_non_daemon_threads());
  return _prev_par_update_ref;
}

uint ShenandoahWorkerPolicy::calc_workers_for_conc_cleanup() {
  uint active_workers = (_prev_conc_cleanup == 0) ? ConcGCThreads : _prev_conc_cleanup;
  _prev_conc_cleanup =
          ShenandoahWorkerPolicy::calc_active_conc_workers(ConcGCThreads,
                                                 active_workers,
                                                 Threads::number_of_non_daemon_threads());
  return _prev_conc_cleanup;
}

uint ShenandoahWorkerPolicy::calc_workers_for_conc_reset() {
  uint active_workers = (_prev_conc_reset == 0) ? ConcGCThreads : _prev_conc_reset;
  _prev_conc_reset =
          ShenandoahWorkerPolicy::calc_active_conc_workers(ConcGCThreads,
                                                 active_workers,
                                                 Threads::number_of_non_daemon_threads());
  return _prev_conc_reset;
}

void ShenandoahWorkerPolicy::update_conc_thread_num(){
  uint new_conc_workers = _prev_conc_workers;

  if (ShenTuneConcGCThreadsYoungUtil > 0) {
    if (_young_used > _young_max * ShenTuneConcGCThreadsYoungUtil) {
      new_conc_workers *= 2;
    } else {
      new_conc_workers -= 2;
    }
  } else {
    if (_young_used > _prev_young_used){
      if(_young_used - _prev_young_used > 512 * M){
        new_conc_workers *= 2;
      } else {
        new_conc_workers += 1;
      }
    } else {
      new_conc_workers -= 2;
    }
  }

  log_info(gc, ergo)("tune conc gc threads: young util %.2f, prev workers %u, new workers %u",
          (float)_young_used / (float)_young_max, _prev_conc_workers, new_conc_workers);

  if (ShenTuneConcGCThreadsMinWorkers > 0) {
    uint minworkers = ShenTuneConcGCThreadsMinWorkers;
    _prev_conc_workers = MAX2(minworkers, MIN2(new_conc_workers, ConcGCThreads));
  } else {
    _prev_conc_workers = MAX2(5U, MIN2(new_conc_workers, ConcGCThreads));
  }
}


uint ShenandoahWorkerPolicy::calc_active_conc_workers(uintx total_workers,
                                            uintx active_workers,
                                            uintx application_workers) {
  if (!UseShenTuneConcGCThreads) {
    return WorkerPolicy::calc_active_conc_workers(total_workers,
                    active_workers, application_workers);
  } else {
    if(_prev_conc_workers == 0){
      _prev_conc_workers = ConcGCThreads;
    }
    log_info(gc)("Conc thread num: %u", _prev_conc_workers);
    return _prev_conc_workers;
  }
}