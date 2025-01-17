/*
 * Copyright 2024 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */
#include "cluster/data_migration_worker.h"

#include "base/vassert.h"
#include "cluster/data_migration_types.h"
#include "errc.h"
#include "logger.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "partition_leaders_table.h"
#include "rpc/connection_cache.h"
#include "ssx/future-util.h"

#include <seastar/core/future.hh>
#include <seastar/core/sleep.hh>

#include <fmt/ostream.h>

#include <optional>
#include <tuple>

namespace cluster::data_migrations {

// TODO: add configuration property
worker::worker(
  model::node_id self, partition_leaders_table& leaders, ss::abort_source& as)
  : _self(self)
  , _leaders_table(leaders)
  , _as(as)
  , _operation_timeout(5s) {}

ss::future<> worker::stop() {
    while (!_managed_ntps.empty()) {
        unmanage_ntp(_managed_ntps.end() - 1, errc::shutting_down);
    }
    if (!_gate.is_closed()) {
        co_await _gate.close();
    }
}

ss::future<errc>
worker::perform_partition_work(model::ntp&& ntp, partition_work&& work) {
    auto it = _managed_ntps.find(ntp);
    if (it == _managed_ntps.end()) {
        // not managed yet
        bool is_leader = _self == _leaders_table.get_leader(ntp);
        auto leadership_subscription
          = _leaders_table.register_leadership_change_notification(
            ntp,
            [this](
              const model::ntp& ntp, model::term_id, model::node_id leader) {
                handle_leadership_update(ntp, _self == leader);
            });
        std::tie(it, std::ignore) = _managed_ntps.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(std::move(ntp)),
          std::forward_as_tuple(
            is_leader, std::move(work), leadership_subscription));
    } else {
        // some stale work going on, kick it out and reuse its entry
        auto& ntp_state = it->second;
        ntp_state.promise->set_value(errc::invalid_data_migration_state);
        ntp_state.promise = ss::make_lw_shared<ss::promise<errc>>();
        ntp_state.is_running = false;
        ntp_state.work = std::move(work);
    }

    spawn_work_if_leader(it);
    return it->second.promise->get_future();
}

void worker::abort_partition_work(
  model::ntp&& ntp, id migration_id, state sought_state) {
    auto it = std::as_const(_managed_ntps).find(ntp);
    if (
      it != _managed_ntps.cend() && it->second.work.migration_id == migration_id
      && it->second.work.sought_state == sought_state) {
        unmanage_ntp(it, errc::invalid_data_migration_state);
    }
}

worker::ntp_state::ntp_state(
  bool is_leader,
  partition_work&& work,
  notification_id_type leadership_subscription)
  : is_leader(is_leader)
  , work(std::move(work))
  , leadership_subscription(leadership_subscription) {}

void worker::handle_operation_result(
  model::ntp ntp, id migration_id, state sought_state, errc ec) {
    auto it = _managed_ntps.find(ntp);
    if (
      it == _managed_ntps.end() || it->second.work.migration_id != migration_id
      || it->second.work.sought_state != sought_state) {
        vlog(
          dm_log.debug,
          "as part of migration {}, partition work for moving ntp {} to state "
          "{} is done with result {}, but not needed anymore",
          migration_id,
          std::move(ntp),
          sought_state,
          ec);
        return;
    }
    it->second.is_running = false;
    if (ec != errc::success && ec != errc::shutting_down) {
        // any other errors deemed retryable
        vlog(
          dm_log.info,
          "as part of migration {}, partition work for moving ntp {} to state "
          "{} returned {}, retrying",
          migration_id,
          std::move(ntp),
          sought_state,
          ec);
        spawn_work_if_leader(it);
        return;
    }
    unmanage_ntp(it, ec);
}

void worker::handle_leadership_update(const model::ntp& ntp, bool is_leader) {
    auto it = _managed_ntps.find(ntp);
    if (it == _managed_ntps.end() || it->second.is_leader == is_leader) {
        return;
    }
    it->second.is_leader = is_leader;
    if (!it->second.is_running) {
        spawn_work_if_leader(it);
    }
}

void worker::unmanage_ntp(managed_ntp_cit it, errc result) {
    _leaders_table.unregister_leadership_change_notification(
      it->second.leadership_subscription);
    it->second.promise->set_value(result);
    _managed_ntps.erase(it);
}

ss::future<errc> worker::do_work(managed_ntp_cit it) noexcept {
    const auto& ntp = it->first;
    auto sought_state = it->second.work.sought_state;
    try {
        co_return co_await std::visit(
          [this, &ntp, sought_state](auto& info) {
              return do_work(ntp, sought_state, info);
          },
          it->second.work.info);
    } catch (...) {
        vlog(
          dm_log.warn,
          "exception occured during partition work on {} towards {} state: {}",
          ntp,
          sought_state,
          std::current_exception());
        co_return errc::partition_operation_failed;
    }
}

ss::future<errc> worker::do_work(
  const model::ntp& ntp,
  state sought_state,
  const inbound_partition_work_info& pwi) {
    vassert(
      sought_state == state::prepared,
      "inbound partition work requested on {} towards {} state",
      ntp,
      sought_state);

    // todo: perform action here; remember to capture any values needed, worker
    // doesn't keep them for you across scheduling points
    std::ignore = ntp;
    std::ignore = pwi;
    return ssx::now(errc::success);
}

ss::future<errc> worker::do_work(
  const model::ntp& ntp,
  state sought_state,
  const outbound_partition_work_info& pwi) {
    switch (sought_state) {
    case state::prepared:
        // todo: perform action here; remember to capture any values needed,
        // worker doesn't keep them for you across scheduling points
        std::ignore = ntp;
        std::ignore = pwi;
        return ssx::now(errc::success);
    case state::executed:
        // todo: perform action here; remember to capture any values needed,
        // worker doesn't keep them for you across scheduling points
        std::ignore = ntp;
        std::ignore = pwi;
        return ssx::now(errc::success);
    default:
        vassert(
          false,
          "outbound partition work requested on {} towards {} state",
          ntp,
          sought_state);
    }
}

void worker::spawn_work_if_leader(managed_ntp_it it) {
    vassert(!it->second.is_running, "work already running");
    if (!it->second.is_leader) {
        return;
    }
    it->second.is_running = true;
    // this call must only tinker with `it` within the current seastar task,
    // it may be invalidated later!
    ssx::spawn_with_gate(_gate, [this, it]() {
        return do_work(it).then([ntp = it->first,
                                 migration_id = it->second.work.migration_id,
                                 sought_state = it->second.work.sought_state,
                                 this](errc ec) mutable {
            handle_operation_result(
              std::move(ntp), migration_id, sought_state, ec);
        });
    });
}

} // namespace cluster::data_migrations
