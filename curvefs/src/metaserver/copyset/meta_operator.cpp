/*
 *  Copyright (c) 2021 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * Date: Sat Aug  7 22:46:58 CST 2021
 * Author: wuhanqing
 */

#include "curvefs/src/metaserver/copyset/meta_operator.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "curvefs/proto/metaserver.pb.h"
#include "curvefs/src/common/rpc_stream.h"
#include "curvefs/src/metaserver/copyset/meta_operator_closure.h"
#include "curvefs/src/metaserver/copyset/raft_log_codec.h"
#include "curvefs/src/metaserver/metastore.h"
#include "curvefs/src/metaserver/streaming_utils.h"
#include "src/common/timeutility.h"

static bvar::LatencyRecorder
    g_concurrent_fast_apply_wait_latency("concurrent_fast_apply_wait");


namespace curvefs {
namespace metaserver {
namespace copyset {

using ::curve::common::TimeUtility;
using ::curvefs::common::StreamConnection;

MetaOperator::~MetaOperator() {
    if (ownRequest_ && request_) {
        delete request_;
        request_ = nullptr;
    }
}

void MetaOperator::Propose() {
    brpc::ClosureGuard doneGuard(done_);

    // check if current node is leader
    if (!IsLeaderTerm()) {
        RedirectRequest();
        return;
    }

    // check if operator can bypass propose to raft
    if (CanBypassPropose()) {
        braft::LeaderLeaseStatus lease_status;
        node_->GetLeaderLeaseStatus(&lease_status);

        // local read: read from current FSM
        if (node_->IsLeaseLeader(lease_status)) {
            FastApplyTask();
            doneGuard.release();
            return;
        }

        // illegal request, redirect
        if (node_->IsLeaseExpired(lease_status)) {
            Redirect();
            return;
        }

        // this request is NOT handled
        // lease state is LEASE_NOT_READY or LEASE_DISABLED => log read
        // reuse `ProposeTask`, propose to raft
    }

    // propose to raft
    if (ProposeTask()) {
        doneGuard.release();
    }
}

void MetaOperator::RedirectRequest() { Redirect(); }

bool MetaOperator::ProposeTask() {
    timerPropose.start();
    butil::IOBuf log;
    bool success = RaftLogCodec::Encode(GetOperatorType(), request_, &log);
    if (!success) {
        LOG(ERROR) << "meta request encode failed, type: "
                   << OperatorTypeName(GetOperatorType())
                   << ", request: " << request_->ShortDebugString();
        OnFailed(MetaStatusCode::UNKNOWN_ERROR);
        return false;
    }

    braft::Task task;
    task.data = &log;
    task.done = new MetaOperatorClosure(this);
    task.expected_term = node_->LeaderTerm();

    node_->Propose(task);

    return true;
}

void MetaOperator::FastApplyTask() {
    butil::Timer timer;
    timer.start();
    auto task =
        std::bind(&MetaOperator::OnApply, this, node_->GetAppliedIndex(),
                  new MetaOperatorClosure(this), TimeUtility::GetTimeofDayUs());
    node_->GetApplyQueue()->Push(HashCode(),
                                 GetOperatorType(), std::move(task));
    timer.stop();
    g_concurrent_fast_apply_wait_latency << timer.u_elapsed();
}

#define OPERATOR_CAN_BY_PASS_PROPOSE(TYPE)                                     \
    bool TYPE##Operator::CanBypassPropose() const {                            \
        return true;                                                           \
    }                                                                          \

// below operator are readonly, so can enable lease read
OPERATOR_CAN_BY_PASS_PROPOSE(GetDentry);
OPERATOR_CAN_BY_PASS_PROPOSE(ListDentry);
OPERATOR_CAN_BY_PASS_PROPOSE(GetInode);
OPERATOR_CAN_BY_PASS_PROPOSE(BatchGetInodeAttr);
OPERATOR_CAN_BY_PASS_PROPOSE(BatchGetXAttr);
OPERATOR_CAN_BY_PASS_PROPOSE(GetVolumeExtent);

#undef OPERATOR_CAN_BY_PASS_PROPOSE

#define OPERATOR_ON_APPLY(TYPE)                                                \
    void TYPE##Operator::OnApply(int64_t index,                                \
                                 google::protobuf::Closure *done,              \
                                 uint64_t startTimeUs) {                       \
        brpc::ClosureGuard doneGuard(done);                                    \
        uint64_t timeUs = TimeUtility::GetTimeofDayUs();                       \
        node_->GetMetric()->WaitInQueueLatency(OperatorType::TYPE,             \
                                               timeUs - startTimeUs);          \
        auto status = node_->GetMetaStore()->TYPE(                             \
            static_cast<const TYPE##Request *>(request_),                      \
            static_cast<TYPE##Response *>(response_));                         \
        uint64_t executeTime = TimeUtility::GetTimeofDayUs() - timeUs;         \
        node_->GetMetric()->ExecuteLatency(OperatorType::TYPE, executeTime);   \
        if (status == MetaStatusCode::OK) {                                    \
            node_->UpdateAppliedIndex(index);                                  \
            static_cast<TYPE##Response *>(response_)->set_appliedindex(        \
                std::max<uint64_t>(index, node_->GetAppliedIndex()));          \
            node_->GetMetric()->OnOperatorComplete(                            \
                OperatorType::TYPE,                                            \
                TimeUtility::GetTimeofDayUs() - startTimeUs, true);            \
        } else {                                                               \
            node_->GetMetric()->OnOperatorComplete(                            \
                OperatorType::TYPE,                                            \
                TimeUtility::GetTimeofDayUs() - startTimeUs, false);           \
        }                                                                      \
    }

OPERATOR_ON_APPLY(GetDentry);
OPERATOR_ON_APPLY(ListDentry);
OPERATOR_ON_APPLY(CreateDentry);
OPERATOR_ON_APPLY(DeleteDentry);
OPERATOR_ON_APPLY(GetInode);
OPERATOR_ON_APPLY(BatchGetInodeAttr);
OPERATOR_ON_APPLY(BatchGetXAttr);
OPERATOR_ON_APPLY(CreateInode);
OPERATOR_ON_APPLY(UpdateInode);
OPERATOR_ON_APPLY(DeleteInode);
OPERATOR_ON_APPLY(CreateRootInode);
OPERATOR_ON_APPLY(CreateManageInode);
OPERATOR_ON_APPLY(CreatePartition);
OPERATOR_ON_APPLY(DeletePartition);
OPERATOR_ON_APPLY(PrepareRenameTx);
OPERATOR_ON_APPLY(UpdateVolumeExtent);
OPERATOR_ON_APPLY(UpdateDeallocatableBlockGroup);

#undef OPERATOR_ON_APPLY

// NOTE: now we need struct `brpc::Controller` for sending data by stream,
// so we redefine OnApply() and OnApplyFromLog() instead of using macro.
// It may not be an elegant implementation, can you provide a better idea?
void GetOrModifyS3ChunkInfoOperator::OnApply(int64_t index,
                                             google::protobuf::Closure *done,
                                             uint64_t startTimeUs) {
    MetaStatusCode rc;
    auto request = static_cast<const GetOrModifyS3ChunkInfoRequest *>(request_);
    auto response = static_cast<GetOrModifyS3ChunkInfoResponse *>(response_);
    auto metastore = node_->GetMetaStore();
    std::shared_ptr<StreamConnection> connection;
    std::shared_ptr<Iterator> iterator;
    auto streamServer = metastore->GetStreamServer();

    {
        brpc::ClosureGuard doneGuard(done);

        rc = metastore->GetOrModifyS3ChunkInfo(request, response, &iterator);
        if (rc == MetaStatusCode::OK) {
            node_->UpdateAppliedIndex(index);
            response->set_appliedindex(
                std::max<uint64_t>(index, node_->GetAppliedIndex()));
            node_->GetMetric()->OnOperatorComplete(
                OperatorType::GetOrModifyS3ChunkInfo,
                TimeUtility::GetTimeofDayUs() - startTimeUs, true);
        } else {
            node_->GetMetric()->OnOperatorComplete(
                OperatorType::GetOrModifyS3ChunkInfo,
                TimeUtility::GetTimeofDayUs() - startTimeUs, false);
        }

        brpc::Controller *cntl = static_cast<brpc::Controller *>(cntl_);
        if (rc != MetaStatusCode::OK || !request->returns3chunkinfomap() ||
            !request->supportstreaming()) {
            return;
        }

        // rc == MetaStatusCode::OK && streaming
        connection = streamServer->Accept(cntl);
        if (nullptr == connection) {
            LOG(ERROR) << "Accept stream connection failed in server-side";
            response->set_statuscode(MetaStatusCode::RPC_STREAM_ERROR);
            return;
        }
    }

    rc = metastore->SendS3ChunkInfoByStream(connection, iterator);
    if (rc != MetaStatusCode::OK) {
        LOG(ERROR) << "Sending s3chunkinfo by stream failed";
    }
}

void GetVolumeExtentOperator::OnApply(int64_t index,
                                      google::protobuf::Closure *done,
                                      uint64_t startTimeUs) {
    brpc::ClosureGuard doneGuard(done);
    const auto *request = static_cast<const GetVolumeExtentRequest *>(request_);
    auto *response = static_cast<GetVolumeExtentResponse *>(response_);
    auto *metaStore = node_->GetMetaStore();

    auto st = metaStore->GetVolumeExtent(request, response);
    node_->GetMetric()->OnOperatorComplete(
        OperatorType::GetVolumeExtent,
        TimeUtility::GetTimeofDayUs() - startTimeUs, st == MetaStatusCode::OK);

    if (st != MetaStatusCode::OK) {
        return;
    }

    response->set_appliedindex(index);
    if (!request->streaming()) {
        return;
    }

    // in streaming mode, swap slices out and send them by streaming
    VolumeExtentSliceList extents;
    response->mutable_slices()->Swap(&extents);
    response->clear_slices();

    // accept client's streaming request
    auto *cntl = static_cast<brpc::Controller *>(cntl_);
    auto streamingServer = metaStore->GetStreamServer();
    auto connection = streamingServer->Accept(cntl);
    if (connection == nullptr) {
        LOG(ERROR) << "Accept streaming connection failed";
        response->set_statuscode(MetaStatusCode::RPC_STREAM_ERROR);
        return;
    }

    // run done
    done->Run();
    doneGuard.release();

    // send volume extent
    st = StreamingSendVolumeExtent(connection.get(), extents);
    if (st != MetaStatusCode::OK) {
        LOG(ERROR) << "Send volume extents by stream failed";
    }
}

#define OPERATOR_ON_APPLY_FROM_LOG(TYPE)                                       \
    void TYPE##Operator::OnApplyFromLog(uint64_t startTimeUs) {                \
        std::unique_ptr<TYPE##Operator> selfGuard(this);                       \
        TYPE##Response response;                                               \
        auto status = node_->GetMetaStore()->TYPE(                             \
            static_cast<const TYPE##Request *>(request_), &response);          \
        node_->GetMetric()->OnOperatorCompleteFromLog(                         \
            OperatorType::TYPE, TimeUtility::GetTimeofDayUs() - startTimeUs,   \
            status == MetaStatusCode::OK);                                     \
    }

OPERATOR_ON_APPLY_FROM_LOG(CreateDentry);
OPERATOR_ON_APPLY_FROM_LOG(DeleteDentry);
OPERATOR_ON_APPLY_FROM_LOG(CreateInode);
OPERATOR_ON_APPLY_FROM_LOG(UpdateInode);
OPERATOR_ON_APPLY_FROM_LOG(DeleteInode);
OPERATOR_ON_APPLY_FROM_LOG(CreateRootInode);
OPERATOR_ON_APPLY_FROM_LOG(CreateManageInode);
OPERATOR_ON_APPLY_FROM_LOG(CreatePartition);
OPERATOR_ON_APPLY_FROM_LOG(DeletePartition);
OPERATOR_ON_APPLY_FROM_LOG(PrepareRenameTx);
OPERATOR_ON_APPLY_FROM_LOG(UpdateVolumeExtent);
OPERATOR_ON_APPLY_FROM_LOG(UpdateDeallocatableBlockGroup);

#undef OPERATOR_ON_APPLY_FROM_LOG

void GetOrModifyS3ChunkInfoOperator::OnApplyFromLog(uint64_t startTimeUs) {
    std::unique_ptr<GetOrModifyS3ChunkInfoOperator> selfGuard(this);
    GetOrModifyS3ChunkInfoRequest request;
    GetOrModifyS3ChunkInfoResponse response;
    std::shared_ptr<Iterator> iterator;
    request = *static_cast<const GetOrModifyS3ChunkInfoRequest *>(request_);
    request.set_returns3chunkinfomap(false);
    auto status = node_->GetMetaStore()->GetOrModifyS3ChunkInfo(
        &request, &response, &iterator);
    node_->GetMetric()->OnOperatorCompleteFromLog(
        OperatorType::GetOrModifyS3ChunkInfo,
        TimeUtility::GetTimeofDayUs() - startTimeUs,
        status == MetaStatusCode::OK);
}

#define READONLY_OPERATOR_ON_APPLY_FROM_LOG(TYPE)                              \
    void TYPE##Operator::OnApplyFromLog(uint64_t startTimeUs) {                \
        (void)startTimeUs;                                                     \
        std::unique_ptr<TYPE##Operator> selfGuard(this);                       \
    }

// below operator are readonly, so on apply from log do nothing
READONLY_OPERATOR_ON_APPLY_FROM_LOG(GetDentry);
READONLY_OPERATOR_ON_APPLY_FROM_LOG(ListDentry);
READONLY_OPERATOR_ON_APPLY_FROM_LOG(GetInode);
READONLY_OPERATOR_ON_APPLY_FROM_LOG(BatchGetInodeAttr);
READONLY_OPERATOR_ON_APPLY_FROM_LOG(BatchGetXAttr);
READONLY_OPERATOR_ON_APPLY_FROM_LOG(GetVolumeExtent);

#undef READONLY_OPERATOR_ON_APPLY_FROM_LOG

#define OPERATOR_REDIRECT(TYPE)                                                \
    void TYPE##Operator::Redirect() {                                          \
        static_cast<TYPE##Response *>(response_)->set_statuscode(              \
            MetaStatusCode::REDIRECTED);                                       \
    }

OPERATOR_REDIRECT(GetDentry);
OPERATOR_REDIRECT(ListDentry);
OPERATOR_REDIRECT(CreateDentry);
OPERATOR_REDIRECT(DeleteDentry);
OPERATOR_REDIRECT(GetInode);
OPERATOR_REDIRECT(BatchGetInodeAttr);
OPERATOR_REDIRECT(BatchGetXAttr);
OPERATOR_REDIRECT(CreateInode);
OPERATOR_REDIRECT(UpdateInode);
OPERATOR_REDIRECT(GetOrModifyS3ChunkInfo);
OPERATOR_REDIRECT(DeleteInode);
OPERATOR_REDIRECT(CreateRootInode);
OPERATOR_REDIRECT(CreateManageInode);
OPERATOR_REDIRECT(CreatePartition);
OPERATOR_REDIRECT(DeletePartition);
OPERATOR_REDIRECT(PrepareRenameTx);
OPERATOR_REDIRECT(GetVolumeExtent);
OPERATOR_REDIRECT(UpdateVolumeExtent);
OPERATOR_REDIRECT(UpdateDeallocatableBlockGroup);

#undef OPERATOR_REDIRECT

#define OPERATOR_ON_FAILED(TYPE)                                               \
    void TYPE##Operator::OnFailed(MetaStatusCode code) {                       \
        static_cast<TYPE##Response *>(response_)->set_statuscode(code);        \
    }

OPERATOR_ON_FAILED(GetDentry);
OPERATOR_ON_FAILED(ListDentry);
OPERATOR_ON_FAILED(CreateDentry);
OPERATOR_ON_FAILED(DeleteDentry);
OPERATOR_ON_FAILED(GetInode);
OPERATOR_ON_FAILED(BatchGetInodeAttr);
OPERATOR_ON_FAILED(BatchGetXAttr);
OPERATOR_ON_FAILED(CreateInode);
OPERATOR_ON_FAILED(UpdateInode);
OPERATOR_ON_FAILED(GetOrModifyS3ChunkInfo);
OPERATOR_ON_FAILED(DeleteInode);
OPERATOR_ON_FAILED(CreateRootInode);
OPERATOR_ON_FAILED(CreateManageInode);
OPERATOR_ON_FAILED(CreatePartition);
OPERATOR_ON_FAILED(DeletePartition);
OPERATOR_ON_FAILED(PrepareRenameTx);
OPERATOR_ON_FAILED(GetVolumeExtent);
OPERATOR_ON_FAILED(UpdateVolumeExtent);
OPERATOR_ON_FAILED(UpdateDeallocatableBlockGroup);

#undef OPERATOR_ON_FAILED

#define OPERATOR_HASH_CODE(TYPE)                                               \
    uint64_t TYPE##Operator::HashCode() const {                                \
        return static_cast<const TYPE##Request *>(request_)->partitionid();    \
    }

OPERATOR_HASH_CODE(GetDentry);
OPERATOR_HASH_CODE(ListDentry);
OPERATOR_HASH_CODE(CreateDentry);
OPERATOR_HASH_CODE(DeleteDentry);
OPERATOR_HASH_CODE(GetInode);
OPERATOR_HASH_CODE(BatchGetInodeAttr);
OPERATOR_HASH_CODE(BatchGetXAttr);
OPERATOR_HASH_CODE(CreateInode);
OPERATOR_HASH_CODE(UpdateInode);
OPERATOR_HASH_CODE(GetOrModifyS3ChunkInfo);
OPERATOR_HASH_CODE(DeleteInode);
OPERATOR_HASH_CODE(CreateRootInode);
OPERATOR_HASH_CODE(CreateManageInode);
OPERATOR_HASH_CODE(PrepareRenameTx);
OPERATOR_HASH_CODE(DeletePartition);
OPERATOR_HASH_CODE(GetVolumeExtent);
OPERATOR_HASH_CODE(UpdateVolumeExtent);
OPERATOR_HASH_CODE(UpdateDeallocatableBlockGroup);


#undef OPERATOR_HASH_CODE

#define PARTITION_OPERATOR_HASH_CODE(TYPE)                                     \
    uint64_t TYPE##Operator::HashCode() const {                                \
        return static_cast<const TYPE##Request *>(request_)                    \
            ->partition()                                                      \
            .partitionid();                                                    \
    }

PARTITION_OPERATOR_HASH_CODE(CreatePartition);

#undef PARTITION_OPERATOR_HASH_CODE

#define OPERATOR_TYPE(TYPE)                                                    \
    OperatorType TYPE##Operator::GetOperatorType() const {                     \
        return OperatorType::TYPE;                                             \
    }

OPERATOR_TYPE(GetDentry);
OPERATOR_TYPE(ListDentry);
OPERATOR_TYPE(CreateDentry);
OPERATOR_TYPE(DeleteDentry);
OPERATOR_TYPE(GetInode);
OPERATOR_TYPE(BatchGetInodeAttr);
OPERATOR_TYPE(BatchGetXAttr);
OPERATOR_TYPE(CreateInode);
OPERATOR_TYPE(UpdateInode);
OPERATOR_TYPE(GetOrModifyS3ChunkInfo);
OPERATOR_TYPE(DeleteInode);
OPERATOR_TYPE(CreateRootInode);
OPERATOR_TYPE(CreateManageInode);
OPERATOR_TYPE(PrepareRenameTx);
OPERATOR_TYPE(CreatePartition);
OPERATOR_TYPE(DeletePartition);
OPERATOR_TYPE(GetVolumeExtent);
OPERATOR_TYPE(UpdateVolumeExtent);
OPERATOR_TYPE(UpdateDeallocatableBlockGroup);

#undef OPERATOR_TYPE

}  // namespace copyset
}  // namespace metaserver
}  // namespace curvefs
