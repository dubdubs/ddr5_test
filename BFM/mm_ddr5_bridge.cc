#include "mm_ddr5_bridge.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <chrono>

#ifndef QUILL_LOG_DEBUG
#define QUILL_LOG_DEBUG(logger, fmt, ...) ((void)0)
#endif
#ifndef QUILL_LOG_INFO
#define QUILL_LOG_INFO(logger, fmt, ...) ((void)0)
#endif
#ifndef QUILL_LOG_WARNING
#define QUILL_LOG_WARNING(logger, fmt, ...) ((void)0)
#endif
#ifndef QUILL_LOG_ERROR
#define QUILL_LOG_ERROR(logger, fmt, ...) ((void)0)
#endif
#ifndef QUILL_LOG_TRACE_L3
#define QUILL_LOG_TRACE_L3(logger, fmt, ...) ((void)0)
#endif

#include "sslogger.h"

using namespace ssln::ddr5;
using namespace ssln::hybrid;

static void* file_logger = nullptr;

std::mutex MMDDR5Bridge::fallback_mtx_;
MMDDR5Bridge* MMDDR5Bridge::fallback_instance_ = nullptr;

namespace {

inline uint32_t BytesFromBits(uint32_t bits) {
  return (bits + 7U) / 8U;
}

inline uint64_t ReadU64(const svBitVecVal* v) {
  return *reinterpret_cast<const uint64_t*>(v);
}

const char* kLogTag = "[MM_DDR5_BRIDGE]";
constexpr uint32_t kDataLogMaxBytes = 32;
constexpr uint32_t kInvalidIndex = 0xFFFFFFFFU;
constexpr uint64_t kPerfReportEvery = 4096;

uint8_t* AcquireTxBuffer(uint32_t size) {
  thread_local std::vector<uint8_t> buffer;
  if (buffer.size() < size) {
    buffer.resize(size);
  }
  return buffer.data();
}

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void UpdateAtomicMax(std::atomic<uint64_t>& dst, uint64_t value) {
  uint64_t cur = dst.load(std::memory_order_relaxed);
  while (cur < value &&
         !dst.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
  }
}

std::string HexPreview(const uint8_t* data, uint32_t bytes, uint32_t max_bytes) {
  if (data == nullptr || bytes == 0) {
    return "<empty>";
  }
  const uint32_t n = std::min(bytes, max_bytes);
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (uint32_t i = 0; i < n; ++i) {
    if (i != 0) {
      oss << " ";
    }
    oss << std::setw(2) << static_cast<uint32_t>(data[i]);
  }
  if (n < bytes) {
    oss << " ...(" << std::dec << bytes << "B)";
  }
  return oss.str();
}

}  // namespace

extern "C" {
void s2h_ddr5_read_data_reply(const svBitVecVal* data, unsigned char valid);
}

extern "C" void h2s_ddr5_init_bfm(const svBitVecVal* bfm_id,
                                    unsigned int data_width) {
  svScope scope = svGetScope();
  const char* scope_name = svGetNameFromScope(scope);
  SSLN_LOG_INFO(file_logger,
                "{} dpi_init scope={} bfm_id={} data_width={}",
                kLogTag,
                (scope_name == nullptr ? "<null>" : scope_name),
                *bfm_id,
                data_width);
  UV_HYBRID_REGISTER_BFM(scope, scope_name);
  UV_HYBRID_GET_BRIDGE(MMDDR5Bridge, scope)->RegisterBfm(*bfm_id, data_width);
}

#ifdef MM_NONBLOCKING
extern "C" void h2s_ddr5_write(const svBitVecVal* bfm_id,
                                const svBitVecVal* addr,
                                const svBitVecVal* data,
                                const svBitVecVal* mask) {
  UV_HYBRID_GET_BRIDGE(MMDDR5Bridge, svGetScope())
      ->ProcessWrite(*bfm_id, ReadU64(addr), data, mask);
}

extern "C" void h2s_ddr5_read(const svBitVecVal* bfm_id,
                               const svBitVecVal* addr,
                               svBitVecVal* data) {
  UV_HYBRID_GET_BRIDGE(MMDDR5Bridge, svGetScope())
      ->ProcessRead(*bfm_id, ReadU64(addr), data);
}

extern "C" void h2s_ddr5_read_request(const svBitVecVal* bfm_id,
                                       const svBitVecVal* addr) {
  UV_HYBRID_GET_BRIDGE(MMDDR5Bridge, svGetScope())
      ->ProcessReadRequest(*bfm_id, ReadU64(addr));
}

extern "C" void h2s_ddr5_read_data_request(const svBitVecVal* bfm_id,
                                            const svBitVecVal* left_space) {
  UV_HYBRID_GET_BRIDGE(MMDDR5Bridge, svGetScope())
      ->ProcessReadRequestData(*bfm_id, *left_space);
}

extern "C" void h2s_ddr5_read_data_blocking(const svBitVecVal* bfm_id) {
  UV_HYBRID_GET_BRIDGE(MMDDR5Bridge, svGetScope())->BlockReadData(*bfm_id);
}
#else
extern "C" void h2s_ddr5_write(const svBitVecVal* bfm_id,
                                const svBitVecVal* addr,
                                const svBitVecVal* data,
                                const svBitVecVal* mask) {
  UV_HYBRID_GET_BRIDGE(MMDDR5Bridge, svGetScope())
      ->ProcessWrite(*bfm_id, ReadU64(addr), data, mask);
}

extern "C" void h2s_ddr5_read(const svBitVecVal* bfm_id,
                               const svBitVecVal* addr,
                               svBitVecVal* data) {
  UV_HYBRID_GET_BRIDGE(MMDDR5Bridge, svGetScope())
      ->ProcessRead(*bfm_id, ReadU64(addr), data);
}
#endif

MMDDR5Bridge::MMDDR5Bridge(sc_core::sc_module_name name, std::string hdl_path,
                           uint32_t comp_id, uint32_t fifo_size)
    : sc_core::sc_module(name),
      m_comp_id(comp_id),
      queue_size(fifo_size),
      id_count(0),
      running_(true) {
  for (auto& item : bfm_index_cache) {
    item.store(kInvalidIndex, std::memory_order_relaxed);
  }
  SSLN_LOG_INFO(file_logger, "module_name={} construct begin, comp_id={}",
                this->name(), comp_id);
  {
    std::lock_guard<std::mutex> lock(fallback_mtx_);
    fallback_instance_ = this;
  }
  profiling = new SslnProfiling(this->name());
  dispatcher = &BridgeDispatcher::getInstance();
  dispatcher->register_component(this);
  DpiDispatcher::GetInstance().RegisterHdlScopeGroup(hdl_path, this);
  SSLN_LOG_INFO(file_logger,
                "{} module={} construct end comp_id={} queue_size={} mode={}",
                kLogTag,
                this->name(),
                m_comp_id,
                queue_size,
#ifdef MM_NONBLOCKING
                "nonblocking"
#else
                "blocking"
#endif
  );
}

MMDDR5Bridge::MMDDR5Bridge(sc_core::sc_module_name name,
                           BridgeDispatcher* in_dispatcher,
                           std::string hdl_path,
                           uint32_t comp_id,
                           uint32_t fifo_size)
    : sc_core::sc_module(name),
      m_comp_id(comp_id),
      queue_size(fifo_size),
      id_count(0),
      running_(true) {
  for (auto& item : bfm_index_cache) {
    item.store(kInvalidIndex, std::memory_order_relaxed);
  }
  SSLN_LOG_INFO(file_logger, "module_name={} construct begin, comp_id={}",
                this->name(), comp_id);
  {
    std::lock_guard<std::mutex> lock(fallback_mtx_);
    fallback_instance_ = this;
  }
  profiling = new SslnProfiling(this->name());
  dispatcher = (in_dispatcher == nullptr) ? &BridgeDispatcher::getInstance()
                                          : in_dispatcher;
  dispatcher->register_component(this);
  DpiDispatcher::GetInstance().RegisterHdlScopeGroup(hdl_path, this);
  SSLN_LOG_INFO(file_logger,
                "{} module={} construct end comp_id={} queue_size={} mode={}",
                kLogTag,
                this->name(),
                m_comp_id,
                queue_size,
#ifdef MM_NONBLOCKING
                "nonblocking"
#else
                "blocking"
#endif
  );
}

MMDDR5Bridge::~MMDDR5Bridge() {
  running_ = false;
  SSLN_LOG_INFO(file_logger,
                "{} module={} destruct begin id_count={}",
                kLogTag,
                this->name(),
                id_count);
  for (uint32_t i = 0; i < id_count; ++i) {
    ipc::buffer* data = nullptr;
    while (recv_responses[i]->try_dequeue(data)) {
      delete data;
    }
    delete recv_responses[i];
  }
  delete profiling;
  SSLN_LOG_INFO(file_logger, "{} module={} destruct end", kLogTag, this->name());
  {
    std::lock_guard<std::mutex> lock(fallback_mtx_);
    if (fallback_instance_ == this) {
      fallback_instance_ = nullptr;
    }
  }
}

void MMDDR5Bridge::SetAddressTranslation(AddrTransFunc func) {
  h2s_addr_translation = func;
}

void MMDDR5Bridge::AddressTranslate(uint64_t& laddr, uint32_t bfm_id,
                                    uint64_t daddr) {
  if (h2s_addr_translation != nullptr) {
    h2s_addr_translation(laddr, bfm_id, daddr);
    return;
  }
  laddr = daddr;
}

bool MMDDR5Bridge::RegisterBfm(uint32_t bfm_id, uint32_t data_width) {
  std::unique_lock<std::mutex> lock(id_mutex);
  recv_responses.push_back(
      new moodycamel::ReaderWriterQueue<ipc::buffer*>(queue_size));
  bfm_data_width.push_back(data_width);
  bfm_read_count.push_back(std::make_pair(0, 0));
  bfmid_index[bfm_id] = id_count;
  if (bfm_id < bfm_index_cache.size()) {
    bfm_index_cache[bfm_id].store(id_count, std::memory_order_release);
  }
  SSLN_LOG_INFO(file_logger,
                "{} register module={} bfm_id={} index={} data_width={} queue_size={}",
                kLogTag,
                this->name(),
                bfm_id,
                id_count,
                data_width,
                queue_size);
#ifdef MM_NONBLOCKING
  resp_event_list.emplace_back();
#endif
  ++id_count;
  return true;
}

#ifdef MM_NONBLOCKING
void MMDDR5Bridge::ProcessWrite(uint32_t bfm_id, uint64_t addr,
                                const svBitVecVal* data,
                                const svBitVecVal* mask) {
  AddressTranslate(addr, bfm_id, addr);
  profiling->add_start_time("ProcessWrite");

  const uint32_t index = GetPCId(bfm_id);
  const uint32_t data_bytes = BytesFromBits(bfm_data_width[index]);
  const uint32_t total_size = sizeof(DDR5Payload) + data_bytes;

  auto* payload_ptr = AcquireTxBuffer(total_size);
  auto* payload = reinterpret_cast<DDR5Payload*>(payload_ptr);
  payload->data = reinterpret_cast<uint8_t*>(payload) + sizeof(DDR5Payload);
  payload->addr = addr;
  payload->command = 1;
  payload->data_size = data_bytes;
  payload->ddr5_info = bfm_id;
  payload->mask_enable = 0;
  if (data != nullptr) {
    std::memcpy(payload->data, data, data_bytes);
  }

  SSLN_LOG_DEBUG(file_logger, "{} bfm_id={} write addr={:#x} len={}",
                 this->name(), bfm_id, addr, data_bytes);
  SSLN_LOG_DEBUG(file_logger,
                 "{} tx_write_data index={} bfm_id={} addr={:#x} bytes={} data=[{}]",
                 kLogTag,
                 index,
                 bfm_id,
                 addr,
                 data_bytes,
                 HexPreview(payload->data, data_bytes, kDataLogMaxBytes));
  SendMsg(payload, total_size);
  perf_.tx_write_req.fetch_add(1, std::memory_order_relaxed);
  MaybeReportPerf();

  profiling->add_duration_time("ProcessWrite");
  (void)mask;
}

void MMDDR5Bridge::ProcessRead(uint32_t bfm_id, uint64_t addr,
                               svBitVecVal* data) {
  AddressTranslate(addr, bfm_id, addr);
  const uint32_t index = GetPCId(bfm_id);
  const uint32_t data_bytes = BytesFromBits(bfm_data_width[index]);

  profiling->add_start_time("ProcessRead");

  const uint32_t total_size = sizeof(DDR5Payload);
  auto* payload_ptr = AcquireTxBuffer(total_size);
  auto* payload = reinterpret_cast<DDR5Payload*>(payload_ptr);
  payload->data = nullptr;
  payload->addr = addr;
  payload->command = 0;
  payload->data_size = data_bytes;
  payload->ddr5_info = bfm_id;
  payload->mask_enable = 0;

  ++bfm_read_count[index].first;
  SSLN_LOG_DEBUG(file_logger,
                 "{} read_req_sync index={} bfm_id={} addr={:#x} len={} pending(req={},resp={})",
                 kLogTag,
                 index,
                 bfm_id,
                 addr,
                 data_bytes,
                 bfm_read_count[index].first,
                 bfm_read_count[index].second);

  SendMsg(payload, total_size);
  perf_.tx_read_req_sync.fetch_add(1, std::memory_order_relaxed);

  ipc::buffer* resp_data = nullptr;
  const uint64_t wait_start_ns = NowNs();
  SSLN_LOG_TRACE_L3(file_logger, "{} read_req_sync_wait index={} bfm_id={}",
                    kLogTag, index, bfm_id);
  wait(resp_event_list[index]);
  const uint64_t wait_cost_ns = NowNs() - wait_start_ns;
  perf_.wait_count.fetch_add(1, std::memory_order_relaxed);
  perf_.wait_ns_total.fetch_add(wait_cost_ns, std::memory_order_relaxed);
  UpdateAtomicMax(perf_.wait_ns_max, wait_cost_ns);
  if (recv_responses[index]->try_dequeue(resp_data)) {
    auto* resp_payload = reinterpret_cast<DDR5Payload*>(resp_data->data());
    std::memcpy(data, resp_payload->data, data_bytes);
    ++bfm_read_count[index].second;
    SSLN_LOG_DEBUG(file_logger,
                   "{} read_resp_sync_data index={} bfm_id={} addr={:#x} len={} data=[{}] pending(req={},resp={})",
                   kLogTag,
                   index,
                   bfm_id,
                   addr,
                   data_bytes,
                   HexPreview(reinterpret_cast<const uint8_t*>(data), data_bytes, kDataLogMaxBytes),
                   bfm_read_count[index].first,
                   bfm_read_count[index].second);
    delete resp_data;
    perf_.rx_read_resp.fetch_add(1, std::memory_order_relaxed);
  }

  profiling->add_duration_time("ProcessRead");
  MaybeReportPerf();
}

void MMDDR5Bridge::ProcessReadRequest(uint32_t bfm_id, uint64_t addr) {
  AddressTranslate(addr, bfm_id, addr);
  profiling->add_start_time("ProcessReadRequest");

  const uint32_t index = GetPCId(bfm_id);
  ++bfm_read_count[index].first;

  const uint32_t data_bytes = BytesFromBits(bfm_data_width[index]);
  const uint32_t total_size = sizeof(DDR5Payload);

  auto* payload_ptr = AcquireTxBuffer(total_size);
  auto* payload = reinterpret_cast<DDR5Payload*>(payload_ptr);
  payload->data = nullptr;
  payload->addr = addr;
  payload->command = 0;
  payload->data_size = data_bytes;
  payload->ddr5_info = bfm_id;
  payload->mask_enable = 0;

  SSLN_LOG_DEBUG(file_logger,
                 "{} read_req_async index={} bfm_id={} addr={:#x} len={} pending(req={},resp={})",
                 kLogTag,
                 index,
                 bfm_id,
                 addr,
                 data_bytes,
                 bfm_read_count[index].first,
                 bfm_read_count[index].second);
  SendMsg(payload, total_size);
  perf_.tx_read_req_async.fetch_add(1, std::memory_order_relaxed);

  profiling->add_duration_time("ProcessReadRequest");
  MaybeReportPerf();
}

void MMDDR5Bridge::ProcessReadRequestData(uint32_t bfm_id,
                                          uint32_t left_space) {
  profiling->add_start_time("ProcessReadRequestData");
  const uint32_t index = GetPCId(bfm_id);

  if (left_space == 0U) {
    SSLN_LOG_TRACE_L3(file_logger,
                      "{} read_data_req_skip index={} bfm_id={} left_space=0",
                      kLogTag,
                      index,
                      bfm_id);
    s2h_ddr5_read_data_reply(reinterpret_cast<svBitVecVal*>(&bfm_id), 0);
    profiling->add_duration_time("ProcessReadRequestData");
    perf_.tx_read_data_req.fetch_add(1, std::memory_order_relaxed);
    MaybeReportPerf();
    return;
  }

  ipc::buffer* data = nullptr;
  perf_.tx_read_data_req.fetch_add(1, std::memory_order_relaxed);
  if (recv_responses[index]->try_dequeue(data)) {
    auto* resp_payload = reinterpret_cast<DDR5Payload*>(data->data());
    ++bfm_read_count[index].second;
    SSLN_LOG_DEBUG(file_logger,
                   "{} read_data_reply_data index={} bfm_id={} addr={:#x} valid=1 bytes={} data=[{}] pending(req={},resp={})",
                   kLogTag,
                   index,
                   bfm_id,
                   resp_payload->addr,
                   resp_payload->data_size,
                   HexPreview(resp_payload->data, resp_payload->data_size, kDataLogMaxBytes),
                   bfm_read_count[index].first,
                   bfm_read_count[index].second);
    s2h_ddr5_read_data_reply(reinterpret_cast<svBitVecVal*>(resp_payload->data),
                             1);
    delete data;
    perf_.rx_read_resp.fetch_add(1, std::memory_order_relaxed);
  } else {
    SSLN_LOG_TRACE_L3(file_logger,
                      "{} read_data_reply index={} bfm_id={} valid=0 queue_empty pending(req={},resp={})",
                      kLogTag,
                      index,
                      bfm_id,
                      bfm_read_count[index].first,
                      bfm_read_count[index].second);
    s2h_ddr5_read_data_reply(reinterpret_cast<svBitVecVal*>(&bfm_id), 0);
    perf_.queue_empty_poll.fetch_add(1, std::memory_order_relaxed);
  }
  profiling->add_duration_time("ProcessReadRequestData");
  MaybeReportPerf();
}

void MMDDR5Bridge::BlockReadData(uint32_t bfm_id) {
  profiling->add_start_time("BlockReadData");
  const uint32_t index = GetPCId(bfm_id);

  const bool no_wait_data =
      (bfm_read_count[index].first == bfm_read_count[index].second);
  perf_.tx_read_data_blocking.fetch_add(1, std::memory_order_relaxed);
  if (no_wait_data) {
    SSLN_LOG_DEBUG(file_logger,
                   "{} read_blocking_invalid index={} bfm_id={} req={} resp={}",
                   kLogTag,
                   index,
                   bfm_id,
                   bfm_read_count[index].first,
                   bfm_read_count[index].second);
    s2h_ddr5_read_data_reply(reinterpret_cast<svBitVecVal*>(&bfm_id), 0);
    profiling->add_duration_time("BlockReadData");
    perf_.queue_empty_poll.fetch_add(1, std::memory_order_relaxed);
    MaybeReportPerf();
    return;
  }

  ipc::buffer* data = nullptr;
  while (true) {
    if (recv_responses[index]->peek() == nullptr) {
      SSLN_LOG_TRACE_L3(file_logger,
                        "{} read_blocking_wait index={} bfm_id={}",
                        kLogTag,
                        index,
                        bfm_id);
      const uint64_t wait_start_ns = NowNs();
      wait(resp_event_list[index]);
      const uint64_t wait_cost_ns = NowNs() - wait_start_ns;
      perf_.wait_count.fetch_add(1, std::memory_order_relaxed);
      perf_.wait_ns_total.fetch_add(wait_cost_ns, std::memory_order_relaxed);
      UpdateAtomicMax(perf_.wait_ns_max, wait_cost_ns);
    }
    if (!recv_responses[index]->try_dequeue(data)) {
      continue;
    }

    auto* resp_payload = reinterpret_cast<DDR5Payload*>(data->data());
    ++bfm_read_count[index].second;
    SSLN_LOG_DEBUG(file_logger,
                   "{} read_blocking_reply_data index={} bfm_id={} addr={:#x} bytes={} data=[{}] pending(req={},resp={})",
                   kLogTag,
                   index,
                   bfm_id,
                   resp_payload->addr,
                   resp_payload->data_size,
                   HexPreview(resp_payload->data, resp_payload->data_size, kDataLogMaxBytes),
                   bfm_read_count[index].first,
                   bfm_read_count[index].second);
    s2h_ddr5_read_data_reply(reinterpret_cast<svBitVecVal*>(resp_payload->data),
                             1);
    delete data;
    perf_.rx_read_resp.fetch_add(1, std::memory_order_relaxed);
    break;
  }

  profiling->add_duration_time("BlockReadData");
  MaybeReportPerf();
}

#else
void MMDDR5Bridge::ProcessWrite(uint32_t bfm_id, uint64_t addr,
                                const svBitVecVal* data,
                                const svBitVecVal* mask) {
  AddressTranslate(addr, bfm_id, addr);
  const uint32_t index = GetPCId(bfm_id);
  const uint32_t data_bytes = BytesFromBits(bfm_data_width[index]);

  profiling->add_start_time("ProcessWrite");

  const uint32_t total_size = sizeof(DDR5Payload) + data_bytes;
  auto* payload_ptr = AcquireTxBuffer(total_size);
  auto* payload = reinterpret_cast<DDR5Payload*>(payload_ptr);
  payload->data = reinterpret_cast<uint8_t*>(payload) + sizeof(DDR5Payload);
  payload->addr = addr;
  payload->command = 1;
  payload->data_size = data_bytes;
  payload->ddr5_info = bfm_id;
  payload->mask_enable = 0;
  if (data != nullptr) {
    std::memcpy(payload->data, data, data_bytes);
  }

  SSLN_LOG_DEBUG(file_logger, "{} bfm_id={} write addr={:#x} len={}",
                 this->name(), bfm_id, addr, data_bytes);
  SendMsg(payload, total_size);

  ipc::buffer* resp_data = nullptr;
  wait(resp_event);
  if (recv_responses[index]->try_dequeue(resp_data)) {
    delete resp_data;
  }

  profiling->add_duration_time("ProcessWrite");
  (void)mask;
}

void MMDDR5Bridge::ProcessRead(uint32_t bfm_id, uint64_t addr,
                               svBitVecVal* data) {
  AddressTranslate(addr, bfm_id, addr);
  const uint32_t index = GetPCId(bfm_id);
  const uint32_t data_bytes = BytesFromBits(bfm_data_width[index]);

  profiling->add_start_time("ProcessRead");

  const uint32_t total_size = sizeof(DDR5Payload);
  auto* payload_ptr = AcquireTxBuffer(total_size);
  auto* payload = reinterpret_cast<DDR5Payload*>(payload_ptr);
  payload->data = nullptr;
  payload->addr = addr;
  payload->command = 0;
  payload->data_size = data_bytes;
  payload->ddr5_info = bfm_id;
  payload->mask_enable = 0;

  SendMsg(payload, total_size);

  ipc::buffer* resp_data = nullptr;
  wait(resp_event);
  if (recv_responses[index]->try_dequeue(resp_data)) {
    auto* resp_payload = reinterpret_cast<DDR5Payload*>(resp_data->data());
    std::memcpy(data, resp_payload->data, data_bytes);
    SSLN_LOG_DEBUG(file_logger,
                   "{} read_resp_blocking_data index={} bfm_id={} addr={:#x} len={} data=[{}]",
                   kLogTag,
                   index,
                   bfm_id,
                   addr,
                   data_bytes,
                   HexPreview(reinterpret_cast<const uint8_t*>(data), data_bytes, kDataLogMaxBytes));
    delete resp_data;
  }

  profiling->add_duration_time("ProcessRead");
}
#endif

uint32_t MMDDR5Bridge::GetPCId(uint32_t bfm_id) {
  if (bfm_id < bfm_index_cache.size()) {
    const uint32_t cached =
        bfm_index_cache[bfm_id].load(std::memory_order_acquire);
    if (cached != kInvalidIndex) {
      return cached;
    }
  }

  std::unique_lock<std::mutex> lock(id_mutex);
  auto it = bfmid_index.find(bfm_id);
  if (it == bfmid_index.end()) {
    const uint32_t new_index = id_count++;
    bfmid_index[bfm_id] = new_index;
    if (bfm_id < bfm_index_cache.size()) {
      bfm_index_cache[bfm_id].store(new_index, std::memory_order_release);
    }
    return new_index;
  }
  if (bfm_id < bfm_index_cache.size()) {
    bfm_index_cache[bfm_id].store(it->second, std::memory_order_release);
  }
  return it->second;
}

bool MMDDR5Bridge::HandleData(void* data) {
  auto* resp_data = static_cast<ipc::buffer*>(data);
  auto* resp_payload = reinterpret_cast<DDR5Payload*>(resp_data->data());

  if (resp_payload->command == 0) {
    resp_payload->data =
        reinterpret_cast<uint8_t*>(resp_payload) + sizeof(DDR5Payload);
  }

  const uint32_t index = GetPCId(resp_payload->ddr5_info);


#ifdef MM_NONBLOCKING
  if (resp_payload->command == 0) {
    if (!recv_responses[index]->enqueue(resp_data)) {
      SSLN_LOG_ERROR(file_logger,
                     "{} rx_enqueue_fail index={} bfm_id={} command={}",
                     kLogTag,
                     index,
                     resp_payload->ddr5_info,
                     resp_payload->command);
      throw std::runtime_error(std::string(this->name()) +
                               " FIFO overflow detected! Queue is full.");
    }
    SSLN_LOG_TRACE_L3(file_logger,
                      "{} rx_enqueue_ok index={} bfm_id={} command={} addr={:#x}",
                      kLogTag,
                      index,
                      resp_payload->ddr5_info,
                      resp_payload->command,
                      resp_payload->addr);
    resp_event_list[index].notify();
    perf_.rx_read_resp.fetch_add(1, std::memory_order_relaxed);
  } else {
    SSLN_LOG_TRACE_L3(file_logger,
                      "{} rx_ignore_nonread index={} bfm_id={} command={}",
                      kLogTag,
                      index,
                      resp_payload->ddr5_info,
                      resp_payload->command);
    delete resp_data;
    perf_.rx_nonread_resp.fetch_add(1, std::memory_order_relaxed);
  }
#else
  if (!recv_responses[index]->enqueue(resp_data)) {
    SSLN_LOG_ERROR(file_logger,
                   "{} rx_enqueue_fail index={} bfm_id={} command={}",
                   kLogTag,
                   index,
                   resp_payload->ddr5_info,
                   resp_payload->command);
    throw std::runtime_error(std::string(this->name()) +
                             " FIFO overflow detected! Queue is full.");
  }
  SSLN_LOG_TRACE_L3(file_logger,
                    "{} rx_enqueue_ok index={} bfm_id={} command={} addr={:#x}",
                    kLogTag,
                    index,
                    resp_payload->ddr5_info,
                    resp_payload->command,
                    resp_payload->addr);
  resp_event.notify();
#endif
  MaybeReportPerf();
  return true;
}

uint32_t MMDDR5Bridge::GetCompID() {
  return m_comp_id;
}



bool MMDDR5Bridge::SendMsg(DDR5Payload* msg, uint32_t msg_size) {
  msg->comp_id = m_comp_id;
  msg->type = static_cast<uint8_t>(MsgType::HYBRID_DDR5);
  msg->length = msg_size - sizeof(PayloadBase);
  SSLN_LOG_TRACE_L3(file_logger,
                    "{} tx_send comp_id={} command={} bfm_id={} addr={:#x} data_size={} msg_size={}",
                    kLogTag,
                    msg->comp_id,
                    msg->command,
                    msg->ddr5_info,
                    msg->addr,
                    msg->data_size,
                    msg_size);
  return dispatcher->send_msg(msg, msg_size);
}

void MMDDR5Bridge::MaybeReportPerf() {
  const uint64_t tick = perf_.report_tick.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((tick % kPerfReportEvery) != 0) {
    return;
  }

  const uint64_t wait_count = perf_.wait_count.load(std::memory_order_relaxed);
  const uint64_t wait_ns_total = perf_.wait_ns_total.load(std::memory_order_relaxed);
  const uint64_t wait_ns_avg = (wait_count == 0) ? 0 : (wait_ns_total / wait_count);
  SSLN_LOG_INFO(
      file_logger,
      "{} perf tick={} tx(wr={},rd_sync={},rd_async={},rd_data_req={},rd_data_blk={}) rx(read={},nonread={}) wait(cnt={},avg_ns={},max_ns={}) queue_empty={}",
      kLogTag,
      tick,
      perf_.tx_write_req.load(std::memory_order_relaxed),
      perf_.tx_read_req_sync.load(std::memory_order_relaxed),
      perf_.tx_read_req_async.load(std::memory_order_relaxed),
      perf_.tx_read_data_req.load(std::memory_order_relaxed),
      perf_.tx_read_data_blocking.load(std::memory_order_relaxed),
      perf_.rx_read_resp.load(std::memory_order_relaxed),
      perf_.rx_nonread_resp.load(std::memory_order_relaxed),
      wait_count,
      wait_ns_avg,
      perf_.wait_ns_max.load(std::memory_order_relaxed),
      perf_.queue_empty_poll.load(std::memory_order_relaxed));
}
