#include "mm_ddr5_bridge.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

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

}  // namespace

extern "C" {
void s2h_ddr5_read_data_reply(const svBitVecVal* data, unsigned char valid);
}

extern "C" void h2s_ddr5_init_bfm(const svBitVecVal* bfm_id,
                                    unsigned int data_width) {
  svScope scope = svGetScope();
  const char* scope_name = svGetNameFromScope(scope);
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
  SSLN_LOG_INFO(file_logger, "module_name={} construct end", this->name());
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
  SSLN_LOG_INFO(file_logger, "module_name={} construct end", this->name());
}

MMDDR5Bridge::~MMDDR5Bridge() {
  running_ = false;
  for (uint32_t i = 0; i < id_count; ++i) {
    ipc::buffer* data = nullptr;
    while (recv_responses[i]->try_dequeue(data)) {
      delete data;
    }
    delete recv_responses[i];
  }
  delete profiling;
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
  SSLN_LOG_INFO(file_logger, "{} register bfm_id={}, index={}", this->name(),
                bfm_id, id_count);
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

  auto* payload_ptr = new uint8_t[total_size];
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

  profiling->add_duration_time("ProcessWrite");
  delete[] payload_ptr;
  (void)mask;
}

void MMDDR5Bridge::ProcessRead(uint32_t bfm_id, uint64_t addr,
                               svBitVecVal* data) {
  AddressTranslate(addr, bfm_id, addr);
  const uint32_t index = GetPCId(bfm_id);
  const uint32_t data_bytes = BytesFromBits(bfm_data_width[index]);

  profiling->add_start_time("ProcessRead");

  const uint32_t total_size = sizeof(DDR5Payload);
  auto* payload_ptr = new uint8_t[total_size];
  auto* payload = reinterpret_cast<DDR5Payload*>(payload_ptr);
  payload->data = nullptr;
  payload->addr = addr;
  payload->command = 0;
  payload->data_size = data_bytes;
  payload->ddr5_info = bfm_id;
  payload->mask_enable = 0;

  ++bfm_read_count[index].first;
  SSLN_LOG_DEBUG(file_logger, "{} bfm_id={} read req addr={:#x} len={}",
                 this->name(), bfm_id, addr, data_bytes);

  SendMsg(payload, total_size);

  ipc::buffer* resp_data = nullptr;
  wait(resp_event_list[index]);
  if (recv_responses[index]->try_dequeue(resp_data)) {
    auto* resp_payload = reinterpret_cast<DDR5Payload*>(resp_data->data());
    std::memcpy(data, resp_payload->data, data_bytes);
    ++bfm_read_count[index].second;
    SSLN_LOG_DEBUG(file_logger, "{} bfm_id={} read resp addr={:#x} len={}",
                   this->name(), bfm_id, addr, data_bytes);
    delete resp_data;
  }

  delete[] payload_ptr;
  profiling->add_duration_time("ProcessRead");
}

void MMDDR5Bridge::ProcessReadRequest(uint32_t bfm_id, uint64_t addr) {
  AddressTranslate(addr, bfm_id, addr);
  profiling->add_start_time("ProcessReadRequest");

  const uint32_t index = GetPCId(bfm_id);
  ++bfm_read_count[index].first;

  const uint32_t data_bytes = BytesFromBits(bfm_data_width[index]);
  const uint32_t total_size = sizeof(DDR5Payload);

  auto* payload_ptr = new uint8_t[total_size];
  auto* payload = reinterpret_cast<DDR5Payload*>(payload_ptr);
  payload->data = nullptr;
  payload->addr = addr;
  payload->command = 0;
  payload->data_size = data_bytes;
  payload->ddr5_info = bfm_id;
  payload->mask_enable = 0;

  SSLN_LOG_DEBUG(file_logger, "{} bfm_id={} read req addr={:#x} len={}",
                 this->name(), bfm_id, addr, data_bytes);
  SendMsg(payload, total_size);

  profiling->add_duration_time("ProcessReadRequest");
  delete[] payload_ptr;
}

void MMDDR5Bridge::ProcessReadRequestData(uint32_t bfm_id,
                                          uint32_t left_space) {
  profiling->add_start_time("ProcessReadRequestData");
  const uint32_t index = GetPCId(bfm_id);

  if (left_space == 0U) {
    s2h_ddr5_read_data_reply(reinterpret_cast<svBitVecVal*>(&bfm_id), 0);
    profiling->add_duration_time("ProcessReadRequestData");
    return;
  }

  ipc::buffer* data = nullptr;
  if (recv_responses[index]->try_dequeue(data)) {
    auto* resp_payload = reinterpret_cast<DDR5Payload*>(data->data());
    ++bfm_read_count[index].second;
    SSLN_LOG_DEBUG(file_logger,
                   "{} bfm_id={} ReadRequestData reply data addr={:#x}",
                   this->name(), bfm_id, resp_payload->addr);
    s2h_ddr5_read_data_reply(reinterpret_cast<svBitVecVal*>(resp_payload->data),
                             1);
    delete data;
  } else {
    SSLN_LOG_TRACE_L3(file_logger,
                      "{} bfm_id={} ReadRequestData reply no data",
                      this->name(), bfm_id);
    s2h_ddr5_read_data_reply(reinterpret_cast<svBitVecVal*>(&bfm_id), 0);
  }
  profiling->add_duration_time("ProcessReadRequestData");
}

void MMDDR5Bridge::BlockReadData(uint32_t bfm_id) {
  profiling->add_start_time("BlockReadData");
  const uint32_t index = GetPCId(bfm_id);

  const bool no_wait_data =
      (bfm_read_count[index].first == bfm_read_count[index].second);
  if (no_wait_data) {
    SSLN_LOG_DEBUG(
        file_logger,
        "{} bfm_id={} blocking read invalid: req={}, resp={}",
        this->name(), bfm_id, bfm_read_count[index].first,
        bfm_read_count[index].second);
    s2h_ddr5_read_data_reply(reinterpret_cast<svBitVecVal*>(&bfm_id), 0);
    profiling->add_duration_time("BlockReadData");
    return;
  }

  ipc::buffer* data = nullptr;
  while (true) {
    if (recv_responses[index]->peek() == nullptr) {
      wait(resp_event_list[index]);
    }
    if (!recv_responses[index]->try_dequeue(data)) {
      continue;
    }

    auto* resp_payload = reinterpret_cast<DDR5Payload*>(data->data());
    ++bfm_read_count[index].second;
    s2h_ddr5_read_data_reply(reinterpret_cast<svBitVecVal*>(resp_payload->data),
                             1);
    delete data;
    break;
  }

  profiling->add_duration_time("BlockReadData");
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
  auto* payload_ptr = new uint8_t[total_size];
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

  delete[] payload_ptr;
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
  auto* payload_ptr = new uint8_t[total_size];
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
    delete resp_data;
  }

  delete[] payload_ptr;
  profiling->add_duration_time("ProcessRead");
}
#endif

uint32_t MMDDR5Bridge::GetPCId(uint32_t bfm_id) {
  std::unique_lock<std::mutex> lock(id_mutex);
  auto it = bfmid_index.find(bfm_id);
  if (it == bfmid_index.end()) {
    bfmid_index[bfm_id] = id_count++;
    return bfmid_index[bfm_id];
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
  if (!recv_responses[index]->enqueue(resp_data)) {
    throw std::runtime_error(std::string(this->name()) +
                             " FIFO overflow detected! Queue is full.");
  }

#ifdef MM_NONBLOCKING
  if (resp_payload->command == 0) {
    resp_event_list[index].notify();
  }
#else
  resp_event.notify();
#endif
  return true;
}

uint32_t MMDDR5Bridge::GetCompID() {
  return m_comp_id;
}

MMDDR5Bridge* MMDDR5Bridge::GetBridgeFromScope(svScope scope) {
  (void)scope;
  std::lock_guard<std::mutex> lock(fallback_mtx_);
  return fallback_instance_;
}

bool MMDDR5Bridge::SendMsg(DDR5Payload* msg, uint32_t msg_size) {
  msg->comp_id = m_comp_id;
  msg->type = static_cast<uint8_t>(MsgType::HYBRID_DDR5);
  msg->length = msg_size - sizeof(PayloadBase);
  return dispatcher->send_msg(msg, msg_size);
}
