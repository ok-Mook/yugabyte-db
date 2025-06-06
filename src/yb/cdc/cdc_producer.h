// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

#pragma once

#include <memory>

#include <boost/functional/hash.hpp>
#include <boost/unordered_map.hpp>

#include "yb/cdc/cdc_service.pb.h"
#include "yb/cdc/cdc_types.h"
#include "yb/client/client_fwd.h"
#include "yb/common/common_fwd.h"
#include "yb/common/opid.h"
#include "yb/common/transaction.h"
#include "yb/consensus/consensus_fwd.h"
#include "yb/dockv/dockv_fwd.h"
#include "yb/tablet/tablet_fwd.h"
#include "yb/util/monotime.h"
#include "yb/master/master_replication.pb.h"
#include "yb/gutil/thread_annotations.h"

namespace yb {

class MemTracker;

namespace cdc {

struct SchemaDetails {
  SchemaVersion schema_version;
  std::shared_ptr<Schema> schema;
};
// We will maintain a map for each stream, tablet pait. The schema details will correspond to the
// the current 'running' schema.
using SchemaDetailsMap = std::map<TableId, SchemaDetails>;
using TableSchemaPackingStorage = std::unordered_map<TableId, dockv::SchemaPackingStorage>;
using consensus::HaveMoreMessages;

struct CDCThroughputMetrics {
  uint64_t records_sent = 0;
  uint64_t bytes_sent = 0;
};

using UpdateOnSplitOpFunc = std::function<Status(const consensus::ReplicateMsg&)>;

class StreamMetadata;

struct XClusterGetChangesContext {
  const xrepl::StreamId& stream_id;
  const TabletId&  tablet_id;
  const OpId& from_op_id;
  const std::shared_ptr<tablet::TabletPeer>& tablet_peer;
  UpdateOnSplitOpFunc update_on_split_op_func;
  const std::shared_ptr<MemTracker>& mem_tracker;
  const CoarseTimePoint& deadline;
  StreamMetadata* stream_metadata;
  consensus::ReplicateMsgsHolder* msgs_holder;
  GetChangesResponsePB* resp;
  HaveMoreMessages* have_more_messages;
  int64_t* last_readable_opid_index;
};

Status GetChangesForCDCSDK(
    const xrepl::StreamId& stream_id,
    const TabletId& tablet_id,
    const CDCSDKCheckpointPB& from_op_id,
    const StreamMetadata& stream_metadata,
    const tablet::TabletPeerPtr& tablet_peer,
    const std::shared_ptr<MemTracker>& mem_tracker,
    const EnumOidLabelMap& enum_oid_label_map,
    const CompositeAttsMap& composite_atts_map,
    CDCSDKRequestSource request_source,
    client::YBClient* client,
    consensus::ReplicateMsgsHolder* msgs_holder,
    GetChangesResponsePB* resp,
    HybridTime* commit_timestamp,
    SchemaDetailsMap* cached_schema_details,
    TableSchemaPackingStorage* schema_packing_storages,
    OpId* last_streamed_op_id,
    int64_t safe_hybrid_time_req,
    std::optional<uint64_t> consistent_snapshot_time,
    int wal_segment_index_req,
    int64_t* last_readable_opid_index = nullptr,
    const TableId& colocated_table_id = "",
    CoarseTimePoint deadline = CoarseTimePoint::max(),
    std::optional<uint64> getchanges_resp_max_size_bytes = std::nullopt,
    CDCThroughputMetrics* throughput_metrics = nullptr);

bool IsReplicationSlotStream(const StreamMetadata& stream_metadata);

Status GetChangesForXCluster(const XClusterGetChangesContext& context);
}  // namespace cdc
}  // namespace yb
