/**
 * Copyright 2020 Huawei Technologies Co., Ltd

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 * http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/
#include <random>
#include "minddata/dataset/engine/cache/cache_service.h"
#include "minddata/dataset/engine/cache/cache_server.h"
#include "minddata/dataset/engine/cache/cache_numa.h"
#include "minddata/dataset/util/random.h"
#include "minddata/dataset/util/slice.h"

namespace mindspore {
namespace dataset {
CacheService::CacheService(uint64_t mem_sz, const std::string &root, bool generate_id)
    : root_(root),
      cache_mem_sz_(mem_sz * 1048576L),  // mem_sz is in MB unit
      cp_(nullptr),
      next_id_(0),
      generate_id_(generate_id),
      num_clients_(0),
      st_(generate_id ? CacheServiceState::kBuildPhase : CacheServiceState::kNone) {}

CacheService::~CacheService() { (void)ServiceStop(); }

Status CacheService::DoServiceStart() {
  CacheServer &cs = CacheServer::GetInstance();
  float memory_cap_ratio = cs.GetMemoryCapRatio();
  if (cache_mem_sz_ > 0) {
    auto avail_mem = CacheServerHW::GetTotalSystemMemory();
    if (cache_mem_sz_ > avail_mem) {
      // Output a warning that we use more than recommended. If we fail to allocate, we will fail anyway.
      MS_LOG(WARNING) << "Requesting cache size " << cache_mem_sz_ << " while available system memory " << avail_mem;
    }
    memory_cap_ratio = static_cast<float>(cache_mem_sz_) / avail_mem;
  }
  numa_pool_ = std::make_shared<NumaMemoryPool>(cs.GetHWControl(), memory_cap_ratio);
  // It is possible we aren't able to allocate the pool for many reasons.
  std::vector<numa_id_t> avail_nodes = numa_pool_->GetAvailableNodes();
  if (avail_nodes.empty()) {
    RETURN_STATUS_UNEXPECTED("Unable to bring up numa memory pool");
  }
  // Put together a CachePool for backing up the Tensor.
  cp_ = std::make_shared<CachePool>(numa_pool_, root_);
  RETURN_IF_NOT_OK(cp_->ServiceStart());
  // Assign a name to this cache. Used for exclusive connection. But we can just use CachePool's name.
  cookie_ = cp_->MyName();
  return Status::OK();
}

Status CacheService::DoServiceStop() {
  if (cp_ != nullptr) {
    RETURN_IF_NOT_OK(cp_->ServiceStop());
  }
  return Status::OK();
}

Status CacheService::CacheRow(const std::vector<const void *> &buf, row_id_type *row_id_generated) {
  SharedLock rw(&rw_lock_);
  RETURN_UNEXPECTED_IF_NULL(row_id_generated);
  if (st_ == CacheServiceState::kFetchPhase) {
    // For this kind of cache service, once we are done with the build phase into fetch phase, we can't
    // allow other to cache more rows.
    RETURN_STATUS_UNEXPECTED("Can't accept cache request in fetch phase");
  }
  if (st_ == CacheServiceState::kNoLocking) {
    // We ignore write this request once we turn off locking on the B+ tree. So we will just
    // return out of memory from now on.
    return Status(StatusCode::kOutOfMemory);
  }
  try {
    // The first buffer is a flatbuffer which describes the rest of the buffers follow
    auto fb = buf.front();
    RETURN_UNEXPECTED_IF_NULL(fb);
    auto msg = GetTensorRowHeaderMsg(fb);
    // If the server side is designed to ignore incoming row id, we generate row id.
    if (generate_id_) {
      *row_id_generated = GetNextRowId();
      // Some debug information on how many rows we have generated so far.
      if ((*row_id_generated) % 1000 == 0) {
        MS_LOG(DEBUG) << "Number of rows cached: " << (*row_id_generated) + 1;
      }
    } else {
      if (msg->row_id() < 0) {
        std::string errMsg = "Expect positive row id: " + std::to_string(msg->row_id());
        RETURN_STATUS_UNEXPECTED(errMsg);
      }
      *row_id_generated = msg->row_id();
    }
    auto size_of_this = msg->size_of_this();
    size_t total_sz = size_of_this;
    auto column_hdr = msg->column();
    // Number of tensor buffer should match the number of columns plus one.
    if (buf.size() != column_hdr->size() + 1) {
      std::string errMsg = "Column count does not match. Expect " + std::to_string(column_hdr->size() + 1) +
                           " but get " + std::to_string(buf.size());
      RETURN_STATUS_UNEXPECTED(errMsg);
    }
    // Next we store in either memory or on disk. Low level code will consolidate everything in one piece.
    std::vector<ReadableSlice> all_data;
    all_data.reserve(column_hdr->size() + 1);
    all_data.emplace_back(fb, size_of_this);
    for (auto i = 0; i < column_hdr->size(); ++i) {
      all_data.emplace_back(buf.at(i + 1), msg->data_sz()->Get(i));
      total_sz += msg->data_sz()->Get(i);
    }
    // Now we cache the buffer.
    Status rc = cp_->Insert(*row_id_generated, all_data);
    if (rc == Status(StatusCode::kDuplicateKey)) {
      MS_LOG(DEBUG) << "Ignoring duplicate key.";
    } else {
      RETURN_IF_NOT_OK(rc);
    }
    return Status::OK();
  } catch (const std::exception &e) {
    RETURN_STATUS_UNEXPECTED(e.what());
  }
}

Status CacheService::FastCacheRow(const ReadableSlice &src, row_id_type *row_id_generated) {
  SharedLock rw(&rw_lock_);
  RETURN_UNEXPECTED_IF_NULL(row_id_generated);
  if (st_ == CacheServiceState::kFetchPhase) {
    // For this kind of cache service, once we are done with the build phase into fetch phase, we can't
    // allow other to cache more rows.
    RETURN_STATUS_UNEXPECTED("Can't accept cache request in fetch phase");
  }
  if (st_ == CacheServiceState::kNoLocking) {
    // We ignore write this request once we turn off locking on the B+ tree. So we will just
    // return out of memory from now on.
    return Status(StatusCode::kOutOfMemory);
  }
  try {
    // If we don't need to generate id, we need to find it from the buffer.
    if (generate_id_) {
      *row_id_generated = GetNextRowId();
      // Some debug information on how many rows we have generated so far.
      if ((*row_id_generated) % 1000 == 0) {
        MS_LOG(DEBUG) << "Number of rows cached: " << (*row_id_generated) + 1;
      }
    } else {
      auto msg = GetTensorRowHeaderMsg(src.GetPointer());
      if (msg->row_id() < 0) {
        std::string errMsg = "Expect positive row id: " + std::to_string(msg->row_id());
        RETURN_STATUS_UNEXPECTED(errMsg);
      }
      *row_id_generated = msg->row_id();
    }
    // Now we cache the buffer.
    Status rc = cp_->Insert(*row_id_generated, {src});
    if (rc == Status(StatusCode::kDuplicateKey)) {
      MS_LOG(DEBUG) << "Ignoring duplicate key.";
    } else {
      RETURN_IF_NOT_OK(rc);
    }
    return Status::OK();
  } catch (const std::exception &e) {
    RETURN_STATUS_UNEXPECTED(e.what());
  }
}

std::ostream &operator<<(std::ostream &out, const CacheService &cs) {
  // Then show any custom derived-internal stuff
  out << "\nCache memory size: " << cs.cache_mem_sz_;
  out << "\nSpill path: ";
  if (cs.root_.empty()) {
    out << "None";
  } else {
    out << cs.GetSpillPath();
  }
  return out;
}

Path CacheService::GetSpillPath() const { return cp_->GetSpillPath(); }

Status CacheService::FindKeysMiss(std::vector<row_id_type> *out) {
  RETURN_UNEXPECTED_IF_NULL(out);
  std::unique_lock<std::mutex> lock(get_key_miss_mux_);
  if (key_miss_results_ == nullptr) {
    // Just do it once.
    key_miss_results_ = std::make_shared<std::vector<row_id_type>>();
    auto stat = cp_->GetStat(true);
    key_miss_results_->push_back(stat.min_key);
    key_miss_results_->push_back(stat.max_key);
    key_miss_results_->insert(key_miss_results_->end(), stat.gap.begin(), stat.gap.end());
  }
  out->insert(out->end(), key_miss_results_->begin(), key_miss_results_->end());
  return Status::OK();
}

Status CacheService::GetStat(CacheService::ServiceStat *out) {
  SharedLock rw(&rw_lock_);
  RETURN_UNEXPECTED_IF_NULL(out);
  out->stat_ = cp_->GetStat();
  out->state_ = static_cast<ServiceStat::state_type>(st_);
  return Status::OK();
}

Status CacheService::PreBatchFetch(connection_id_type connection_id, const std::vector<row_id_type> &v,
                                   const std::shared_ptr<flatbuffers::FlatBufferBuilder> &fbb) {
  SharedLock rw(&rw_lock_);
  std::vector<flatbuffers::Offset<DataLocatorMsg>> datalocator_v;
  datalocator_v.reserve(v.size());
  for (auto row_id : v) {
    flatbuffers::Offset<DataLocatorMsg> offset;
    RETURN_IF_NOT_OK(cp_->GetDataLocator(row_id, fbb, &offset));
    datalocator_v.push_back(offset);
  }
  auto offset_v = fbb->CreateVector(datalocator_v);
  BatchDataLocatorMsgBuilder bld(*fbb);
  bld.add_connection_id(connection_id);
  bld.add_rows(offset_v);
  auto offset_final = bld.Finish();
  fbb->Finish(offset_final);
  return Status::OK();
}

Status CacheService::BatchFetch(const std::shared_ptr<flatbuffers::FlatBufferBuilder> &fbb, WritableSlice *out) const {
  RETURN_UNEXPECTED_IF_NULL(out);
  SharedLock rw(&rw_lock_);
  if (st_ == CacheServiceState::kBuildPhase) {
    // For this kind of cache service, we can't fetch yet until we are done with caching all the rows.
    RETURN_STATUS_UNEXPECTED("Can't accept cache request in fetch phase");
  }
  CacheServer &cs = CacheServer::GetInstance();
  int32_t numQ = cs.GetNumGrpcWorkers();
  auto rng = GetRandomDevice();
  std::uniform_int_distribution<session_id_type> distribution(0, numQ - 1);
  int32_t qID = distribution(rng);
  std::vector<CacheServerRequest *> cache_rq_list;
  auto p = flatbuffers::GetRoot<BatchDataLocatorMsg>(fbb->GetBufferPointer());
  const auto num_elements = p->rows()->size();
  auto connection_id = p->connection_id();
  cache_rq_list.reserve(num_elements);
  int64_t data_offset = (num_elements + 1) * sizeof(int64_t);
  auto *offset_array = reinterpret_cast<int64_t *>(out->GetMutablePointer());
  offset_array[0] = data_offset;
  for (auto i = 0; i < num_elements; ++i) {
    auto data_locator = p->rows()->Get(i);
    auto node_id = data_locator->node_id();
    size_t sz = data_locator->size();
    void *source_addr = reinterpret_cast<void *>(data_locator->addr());
    auto key = data_locator->key();
    // Please read the comment in CacheServer::BatchFetchRows where we allocate
    // the buffer big enough so each thread (which we are going to dispatch) will
    // not run into false sharing problem. We are going to round up sz to 4k.
    auto sz_4k = round_up_4K(sz);
    offset_array[i + 1] = offset_array[i] + sz_4k;
    if (sz > 0) {
      WritableSlice row_data(*out, offset_array[i], sz);
      // Get a request and send to the proper worker (at some numa node) to do the fetch.
      worker_id_t worker_id = cs.IsNumaAffinityOn() ? cs.GetWorkerByNumaId(node_id) : cs.GetRandomWorker();
      CacheServerRequest *cache_rq;
      RETURN_IF_NOT_OK(cs.GetFreeRequestTag(qID++ % numQ, &cache_rq));
      cache_rq_list.push_back(cache_rq);
      // Set up all the necessarily field.
      cache_rq->type_ = BaseRequest::RequestType::kInternalFetchRow;
      cache_rq->st_ = CacheServerRequest::STATE::PROCESS;
      cache_rq->rq_.set_connection_id(connection_id);
      cache_rq->rq_.set_type(static_cast<int16_t>(cache_rq->type_));
      auto dest_addr = row_data.GetMutablePointer();
      flatbuffers::FlatBufferBuilder fb2;
      FetchRowMsgBuilder bld(fb2);
      bld.add_key(key);
      bld.add_size(sz);
      bld.add_source_addr(reinterpret_cast<int64_t>(source_addr));
      bld.add_dest_addr(reinterpret_cast<int64_t>(dest_addr));
      auto offset = bld.Finish();
      fb2.Finish(offset);
      cache_rq->rq_.add_buf_data(fb2.GetBufferPointer(), fb2.GetSize());
      RETURN_IF_NOT_OK(cs.PushRequest(worker_id, cache_rq));
    }
  }
  // Now wait for all of them to come back. Let go of the shared lock. We shouldn't be holding
  // any lock while we can wait for a long time.
  rw.Unlock();
  Status rc;
  for (CacheServerRequest *rq : cache_rq_list) {
    RETURN_IF_NOT_OK(rq->Wait());
    if (rq->rc_.IsError() && !rq->rc_.IsInterrupted() && rc.IsOk()) {
      rc = rq->rc_;
    }
    RETURN_IF_NOT_OK(cs.ReturnRequestTag(rq));
  }
  return rc;
}

Status CacheService::InternalFetchRow(const FetchRowMsg *p) {
  RETURN_UNEXPECTED_IF_NULL(p);
  SharedLock rw(&rw_lock_);
  size_t bytesRead = 0;
  int64_t key = p->key();
  size_t sz = p->size();
  void *source_addr = reinterpret_cast<void *>(p->source_addr());
  void *dest_addr = reinterpret_cast<void *>(p->dest_addr());
  WritableSlice dest(dest_addr, sz);
  if (source_addr != nullptr) {
    // We are not checking if the row is still present but simply use the information passed in.
    // This saves another tree lookup and is faster.
    ReadableSlice src(source_addr, sz);
    RETURN_IF_NOT_OK(WritableSlice::Copy(&dest, src));
  } else {
    RETURN_IF_NOT_OK(cp_->Read(key, &dest, &bytesRead));
    if (bytesRead != sz) {
      std::string errMsg = "Unexpected length. Read " + std::to_string(bytesRead) + ". Expected " + std::to_string(sz) +
                           "." + " Internal key: " + std::to_string(key);
      MS_LOG(ERROR) << errMsg;
      RETURN_STATUS_UNEXPECTED(errMsg);
    }
  }
  return Status::OK();
}

Status CacheService::CacheSchema(const void *buf, int64_t len) {
  UniqueLock rw(&rw_lock_);
  // In case we are calling the same function from multiple threads, only
  // the first one is considered. Rest is ignored.
  if (schema_.empty()) {
    schema_.assign(static_cast<const char *>(buf), len);
  } else {
    MS_LOG(DEBUG) << "Caching Schema already done";
  }
  return Status::OK();
}

Status CacheService::FetchSchema(std::string *out) const {
  SharedLock rw(&rw_lock_);
  if (st_ == CacheServiceState::kBuildPhase) {
    // For this kind of cache service, we can't fetch yet until we are done with caching all the rows.
    RETURN_STATUS_UNEXPECTED("Can't accept cache request in fetch phase");
  }
  RETURN_UNEXPECTED_IF_NULL(out);
  // We are going to use std::string to allocate and hold the result which will be eventually
  // 'moved' to the protobuf message (which underneath is also a std::string) for the purpose
  // to minimize memory copy.
  std::string mem(schema_);
  if (!mem.empty()) {
    *out = std::move(mem);
  } else {
    return Status(StatusCode::kFileNotExist, __LINE__, __FILE__, "No schema has been cached");
  }
  return Status::OK();
}

Status CacheService::BuildPhaseDone() {
  if (HasBuildPhase()) {
    // Exclusive lock to switch phase
    UniqueLock rw(&rw_lock_);
    st_ = CacheServiceState::kFetchPhase;
    cp_->SetLocking(false);
    return Status::OK();
  } else {
    RETURN_STATUS_UNEXPECTED("Not a cache that has a build phase");
  }
}

Status CacheService::ToggleWriteMode(bool on_off) {
  UniqueLock rw(&rw_lock_);
  if (HasBuildPhase()) {
    RETURN_STATUS_UNEXPECTED("Not applicable to non-mappable dataset");
  } else {
    // If we stop accepting write request, we turn off locking for the
    // underlying B+ tree. All future write request we will return kOutOfMemory.
    if (st_ == CacheServiceState::kNone && !on_off) {
      st_ = CacheServiceState::kNoLocking;
      cp_->SetLocking(on_off);
      MS_LOG(WARNING) << "Locking mode is switched off.";
    } else if (st_ == CacheServiceState::kNoLocking && on_off) {
      st_ = CacheServiceState::kNone;
      cp_->SetLocking(on_off);
    }
  }
  return Status::OK();
}
}  // namespace dataset
}  // namespace mindspore
