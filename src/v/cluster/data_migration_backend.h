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
#pragma once
#include "cluster/data_migration_table.h"
#include "cluster/shard_table.h"
#include "container/chunked_hash_map.h"
#include "data_migration_types.h"
#include "fwd.h"
#include "ssx/semaphore.h"
#include "utils/mutex.h"

#include <seastar/core/abort_source.hh>
#include <seastar/core/lowres_clock.hh>

namespace cluster::data_migrations {

/*
 * Cluster-wide coordinator for migrations,
 * as well as node coordinator for local partition-specific actions
 */
class backend {
public:
    backend(
      migrations_table& table,
      frontend& frontend,
      ss::sharded<worker>& worker,
      partition_leaders_table& leaders_table,
      topic_table& topic_table,
      shard_table& shard_table,
      ss::abort_source& as);

    void start();
    ss::future<> stop();

private:
    struct topic_reconciliation_state {
        size_t idx_in_migration;
        chunked_hash_map<model::partition_id, std::vector<model::node_id>>
          outstanding_partitions;
    };
    struct migration_reconciliation_state {
        explicit migration_reconciliation_state(state sought_state)
          : sought_state(sought_state){};
        state sought_state;
        chunked_hash_map<model::topic_namespace, topic_reconciliation_state>
          outstanding_topics;
    };
    using migration_reconciliation_states_t
      = absl::flat_hash_map<id, migration_reconciliation_state>;

    struct replica_work_state {
        id migration_id;
        state sought_state;
        // shard may only be assigned if replica_status is can_run
        std::optional<seastar::shard_id> shard;
        migrated_replica_status status
          = migrated_replica_status::waiting_for_rpc;
        replica_work_state(id migration_id, state sought_state)
          : migration_id(migration_id)
          , sought_state(sought_state) {}
    };

private:
    /* loop management */
    ss::future<> loop_once();
    ss::future<> work_once();
    void wakeup();

    /* event handlers outside main loop */
    ss::future<> handle_raft0_leadership_update();
    ss::future<> handle_migration_update(id id);
    void handle_shard_update(
      const model::ntp& ntp, raft::group_id, std::optional<ss::shard_id> shard);

    /* RPC and raft0 actions */
    ss::future<> send_rpc(model::node_id node_id);
    ss::future<check_ntp_states_reply>
    check_ntp_states_locally(check_ntp_states_request&& req);
    void to_advance(id migration_id, state sought_state);
    void spawn_advances();

    /* communication with workers */
    void start_partition_work(
      const model::ntp& ntp, const replica_work_state& rwstate);
    void stop_partition_work(
      const model::ntp& ntp, const replica_work_state& rwstate);
    void
    on_partition_work_completed(model::ntp&& ntp, id migration, state state);

    /* deferred event handlers */
    ss::future<> process_delta(cluster::topic_table_delta&& delta);

    /* helpers */
    void update_partition_shard(
      const model::ntp& ntp,
      replica_work_state& rwstate,
      std::optional<ss::shard_id> new_shard);
    void mark_migration_step_done_for_ntp(
      migration_reconciliation_state& rs, const model::ntp& ntp);
    void drop_migration_reconciliation_rstate(
      migration_reconciliation_states_t::const_iterator rs_it);
    void clear_tstate_belongings(
      const model::topic_namespace& nt,
      const topic_reconciliation_state& tstate);

    ss::future<> reconcile_migration(
      migration_reconciliation_state& mrstate,
      const migration_metadata& metadata);

    ss::future<> reconcile_topic(
      const model::topic_namespace& nt,
      topic_reconciliation_state& tstate,
      id migration,
      state sought_state,
      bool schedule_local_work);

    std::optional<std::reference_wrapper<replica_work_state>>
    get_replica_work_state(const model::ntp& ntp);
    bool has_local_replica(const model::ntp& ntp);

    inbound_partition_work_info get_partition_work_info(
      const model::ntp& ntp, const inbound_migration& im, id migration_id);
    outbound_partition_work_info get_partition_work_info(
      const model::ntp& ntp, const outbound_migration& om, id migration_id);
    partition_work_info get_partition_work_info(
      const model::ntp& ntp, const migration_metadata& metadata);
    /*
     * Reconciliation-related data.
     *
     * When we are not the coordinator, _mrstates stores sought states and
     * topics only, but no partititons, _nstates and _nodes_to_retry are
     * empty
     *
     * The following invariants can only be violated between tasks by a fiber
     * that has the lock.
     *
     * When we are the coordinator:
     * - _mrstates and _nstates store the same set of migration-ntp
     * combinations.
     * - For each node there is no more than one RPC in flight at a time.
     * - Nodes in _nstates = nodes in _nodes_to_retry ⊔ nodes of in-flight
     * RPCs.
     *
     * - _advance_requests is only modified by the work cycle
     * - _mrstates, _nstates and _nodes_to_retry are only modified under lock
     *
     * - _work_states only contains topics present in _mrstates
     */
    migration_reconciliation_states_t _migration_states;
    // reverse map for topics in mrstates
    using topic_migration_map_t = chunked_hash_map<model::topic_namespace, id>;
    topic_migration_map_t _topic_migration_map;
    using node_state = chunked_hash_map<model::ntp, id>;
    chunked_hash_map<model::node_id, node_state> _node_states;
    using deadline_t = model::timeout_clock::time_point;
    chunked_hash_map<model::node_id, deadline_t> _nodes_to_retry;
    struct advance_info {
        state sought_state;
        bool sent = false;
        explicit advance_info(state sought_state)
          : sought_state(sought_state) {}
    };
    absl::flat_hash_map<id, advance_info> _advance_requests;
    chunked_vector<topic_table_delta> _unprocessed_deltas;

    /* Node-local data */
    using topic_work_state_t
      = chunked_hash_map<model::partition_id, replica_work_state>;
    chunked_hash_map<model::topic_namespace, topic_work_state_t> _work_states;

    chunked_hash_map<model::node_id, check_ntp_states_reply> _rpc_responses;

    model::node_id _self;
    migrations_table& _table;
    frontend& _frontend;
    ss::sharded<worker>& _worker;
    partition_leaders_table& _leaders_table;
    topic_table& _topic_table;
    shard_table& _shard_table;
    ss::abort_source& _as;

    ss::gate _gate;
    ssx::semaphore _sem{0, "c/data-migration-be"};
    mutex _mutex{"c/data-migration-be::lock"};
    ss::timer<ss::lowres_clock> _timer{[this]() { wakeup(); }};

    bool _is_raft0_leader;
    bool _is_coordinator;
    migrations_table::notification_id _table_notification_id;
    cluster::notification_id_type _plt_raft0_leadership_notification_id;
    cluster::notification_id_type _topic_table_notification_id;
    cluster::notification_id_type _shard_notification_id;

    friend irpc_frontend;
};
} // namespace cluster::data_migrations
