#pragma once

#include <svdpi.h>

#if __has_include(<systemc>)
#include <systemc>
#else
namespace sc_core {
using sc_module_name = const char*;
class sc_module {
 public:
  explicit sc_module(sc_module_name name) : name_(name == nullptr ? "" : name) {}
  virtual ~sc_module() = default;
  const char* name() const { return name_; }

 private:
  const char* name_;
};
class sc_event {
 public:
  void notify() {}
};
inline void wait(const sc_event&) {}
}  // namespace sc_core
#endif

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ipc {
class buffer {
 public:
  virtual ~buffer() = default;
  virtual void* data() = 0;
};
}  // namespace ipc

namespace moodycamel {
template <typename T>
class ReaderWriterQueue {
 public:
  explicit ReaderWriterQueue(std::size_t size);
  bool enqueue(const T& item);
  bool try_dequeue(T& item);
  T peek() const;
};
}  // namespace moodycamel

namespace ssln {
namespace hybrid {

class DispatcherInterface {
 public:
  virtual ~DispatcherInterface() = default;
  virtual bool HandleData(void* data) = 0;
  virtual uint32_t GetCompID() = 0;
};

class BridgeDispatcher {
 public:
  static BridgeDispatcher& getInstance();
  void register_component(DispatcherInterface* comp);
  bool send_msg(void* msg, uint32_t msg_size);
};

class DpiDispatcher {
 public:
  static DpiDispatcher& GetInstance();
  void RegisterHdlScopeGroup(const std::string& hdl_path,
                             DispatcherInterface* comp);
};

class SslnProfiling {
 public:
  explicit SslnProfiling(const std::string& name);
  void add_start_time(const std::string& name);
  void add_duration_time(const std::string& name);
};

}  // namespace hybrid

namespace ddr5 {

enum class MsgType : uint8_t {
  HYBRID_DDR5 = 0x05,
};

#pragma pack(push, 1)
struct PayloadBase {
  uint8_t type;
  uint32_t comp_id;
  uint32_t length;
};

struct DDR5Payload : PayloadBase {
  uint8_t command;
  uint8_t mask_enable;
  uint32_t data_size;
  uint16_t ddr5_info;
  uint64_t addr;
  uint8_t* data;
};
#pragma pack(pop)

}  // namespace ddr5
}  // namespace ssln

class MMDDR5Bridge : public sc_core::sc_module,
                     public ssln::hybrid::DispatcherInterface {
 public:
  using AddrTransFunc = void (*)(uint64_t& laddr, uint32_t bfm_id,
                                 uint64_t daddr);

  MMDDR5Bridge(sc_core::sc_module_name name, std::string hdl_path,
               uint32_t comp_id, uint32_t fifo_size);
  MMDDR5Bridge(sc_core::sc_module_name name,
               ssln::hybrid::BridgeDispatcher* dispatcher,
               std::string hdl_path, uint32_t comp_id, uint32_t fifo_size);
  ~MMDDR5Bridge() override;

  bool RegisterBfm(uint32_t bfm_id, uint32_t data_width);
  void SetAddressTranslation(AddrTransFunc func);
  void AddressTranslate(uint64_t& laddr, uint32_t bfm_id, uint64_t daddr);

  void ProcessWrite(uint32_t bfm_id, uint64_t addr, const svBitVecVal* data,
                    const svBitVecVal* mask);
  void ProcessRead(uint32_t bfm_id, uint64_t addr, svBitVecVal* data);
#ifdef MM_NONBLOCKING
  void ProcessReadRequest(uint32_t bfm_id, uint64_t addr);
  void ProcessReadRequestData(uint32_t bfm_id, uint32_t left_space);
  void BlockReadData(uint32_t bfm_id);
#endif

  uint32_t GetPCId(uint32_t bfm_id);

  bool HandleData(void* data) override;
  uint32_t GetCompID() override;
  bool SendMsg(ssln::ddr5::DDR5Payload* msg, uint32_t msg_size);

  static MMDDR5Bridge* GetBridgeFromScope(svScope scope);
  void MaybeReportPerf();

 private:
  struct PerfStats {
    std::atomic<uint64_t> tx_write_req{0};
    std::atomic<uint64_t> tx_read_req_sync{0};
    std::atomic<uint64_t> tx_read_req_async{0};
    std::atomic<uint64_t> tx_read_data_req{0};
    std::atomic<uint64_t> tx_read_data_blocking{0};
    std::atomic<uint64_t> rx_read_resp{0};
    std::atomic<uint64_t> rx_nonread_resp{0};
    std::atomic<uint64_t> wait_count{0};
    std::atomic<uint64_t> wait_ns_total{0};
    std::atomic<uint64_t> wait_ns_max{0};
    std::atomic<uint64_t> queue_empty_poll{0};
    std::atomic<uint64_t> report_tick{0};
  };

  uint32_t m_comp_id;
  uint32_t queue_size;
  uint32_t id_count;
  std::atomic<bool> running_;
  PerfStats perf_;

  ssln::hybrid::BridgeDispatcher* dispatcher;
  ssln::hybrid::SslnProfiling* profiling;
  AddrTransFunc h2s_addr_translation = nullptr;

  std::mutex id_mutex;
  std::unordered_map<uint32_t, uint32_t> bfmid_index;
  std::array<std::atomic<uint32_t>, 65536> bfm_index_cache;
  std::vector<uint32_t> bfm_data_width;
  std::vector<std::pair<uint64_t, uint64_t>> bfm_read_count;
  std::vector<moodycamel::ReaderWriterQueue<ipc::buffer*>*> recv_responses;

#ifdef MM_NONBLOCKING
  std::deque<sc_core::sc_event> resp_event_list;
#else
  sc_core::sc_event resp_event;
#endif

  static std::mutex fallback_mtx_;
  static MMDDR5Bridge* fallback_instance_;
};

#ifndef UV_HYBRID_REGISTER_BFM
#define UV_HYBRID_REGISTER_BFM(scope, scope_name) ((void)(scope), (void)(scope_name))
#endif

#ifndef UV_HYBRID_GET_BRIDGE
#define UV_HYBRID_GET_BRIDGE(type, scope) type::GetBridgeFromScope(scope)
#endif
