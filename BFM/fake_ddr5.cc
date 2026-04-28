#include <svdpi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * fake_ddr5.cc
 *
 * C++ DPI model for ddr5_mm_bfm.sv (pure RTL simulation path).
 *
 * Supported interface:
 *   - h2s_ddr5_init_bfm(input bit [15:0] bfm_id, input int unsigned DATA_WIDTH)
 *   - h2s_ddr5_write(...)
 *   - h2s_ddr5_read(...)
 *   - h2s_ddr5_read_request(...)
 *   - h2s_ddr5_read_data_request(...)
 *   - h2s_ddr5_read_data_blocking(...)
 */

namespace {

#if defined(MM_DDR5_SINGLE_STORAGE_NONBLOCKING)
constexpr bool kNonBlockingMode = true;
constexpr const char* kModeName = "non-blocking";
#else
constexpr bool kNonBlockingMode = false;
constexpr const char* kModeName = "blocking";
#endif

struct Ddr5InstanceState {
  unsigned int data_width = 256U;
  unsigned int data_byte_width = 32U;
  std::unordered_map<std::uint64_t, std::vector<svBitVecVal>> storage;
  std::deque<std::uint64_t> pending_reads;
};

std::mutex g_lock;
std::unordered_map<std::uint16_t, Ddr5InstanceState> g_instances;

int g_log_level = -1;
unsigned int g_log_count = 0U;
unsigned int g_log_max = 0U;
unsigned int g_log_bytes = 32U;

unsigned int ParseUnsignedEnv(const char* name, unsigned int default_v) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return default_v;
  }
  char* endptr = nullptr;
  const unsigned long v = std::strtoul(raw, &endptr, 0);
  if (endptr == raw) {
    return default_v;
  }
  return static_cast<unsigned int>(v);
}

bool ParseBoolEnv(const char* name, bool default_v) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    return default_v;
  }
  const std::string s(raw);
  return s == "1" || s == "true" || s == "TRUE" || s == "on" || s == "ON" ||
         s == "yes" || s == "YES";
}

void EnsureLogConfig() {
  if (g_log_level >= 0) {
    return;
  }
  g_log_level = static_cast<int>(ParseUnsignedEnv("DDR5_FAKE_LOG_LEVEL", 0U));
  if (g_log_level == 0 && ParseBoolEnv("DDR5_FAKE_LOG", false)) {
    g_log_level = 1;
  }
  g_log_max = ParseUnsignedEnv("DDR5_FAKE_LOG_MAX", 1000U);
  g_log_bytes = ParseUnsignedEnv("DDR5_FAKE_LOG_BYTES", 32U);
}

void LogEvent(int level, const char* fmt, ...) {
  EnsureLogConfig();
  if (g_log_level < level) {
    return;
  }
  if (g_log_max != 0U && g_log_count >= g_log_max) {
    if (g_log_count == g_log_max) {
      ++g_log_count;
      std::fprintf(stderr,
                   "[fake_ddr5.cc] log limit reached (%u), suppressing further logs\n",
                   g_log_max);
    }
    return;
  }

  ++g_log_count;
  std::fprintf(stderr, "[fake_ddr5.cc] ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
}

unsigned int WordCount(unsigned int bit_width) {
  return std::max(1U, (bit_width + 31U) / 32U);
}

std::vector<std::uint8_t> WordsToBytes(const std::vector<svBitVecVal>& words,
                                       unsigned int byte_count) {
  std::vector<std::uint8_t> out(byte_count, 0U);
  for (unsigned int i = 0; i < byte_count; ++i) {
    const unsigned int bit_index = i * 8U;
    const unsigned int word_index = bit_index / 32U;
    const unsigned int bit_offset = bit_index % 32U;
    if (word_index < words.size()) {
      out[i] = static_cast<std::uint8_t>((words[word_index] >> bit_offset) & 0xFFU);
    }
  }
  return out;
}

std::string HexPreview(const std::vector<std::uint8_t>& bytes,
                       unsigned int max_bytes) {
  if (bytes.empty()) {
    return "<empty>";
  }
  const unsigned int n = std::min<unsigned int>(max_bytes, bytes.size());
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < n; ++i) {
    if (i != 0) {
      oss << " ";
    }
    oss << std::setw(2) << static_cast<unsigned>(bytes[i]);
  }
  if (n < bytes.size()) {
    oss << " ...(" << std::dec << bytes.size() << "B)";
  }
  return oss.str();
}

std::uint32_t GetWord(const svBitVecVal* value, unsigned int index) {
  if (value == nullptr) {
    return 0U;
  }
  return static_cast<std::uint32_t>(value[index]);
}

std::uint16_t ReadU16(const svBitVecVal* value) {
  return static_cast<std::uint16_t>(GetWord(value, 0U) & 0xFFFFU);
}

std::uint64_t ReadU64(const svBitVecVal* value) {
  return static_cast<std::uint64_t>(GetWord(value, 0U)) |
         (static_cast<std::uint64_t>(GetWord(value, 1U)) << 32U);
}

bool MaskBitSet(const svBitVecVal* mask, unsigned int bit_index) {
  const unsigned int word_index = bit_index / 32U;
  const unsigned int bit_offset = bit_index % 32U;
  return ((GetWord(mask, word_index) >> bit_offset) & 1U) != 0U;
}

std::uint8_t GetByte(const std::vector<svBitVecVal>& words, unsigned int byte_index) {
  const unsigned int bit_index = byte_index * 8U;
  const unsigned int word_index = bit_index / 32U;
  const unsigned int bit_offset = bit_index % 32U;
  if (word_index >= words.size()) {
    return 0U;
  }
  return static_cast<std::uint8_t>((words[word_index] >> bit_offset) & 0xFFU);
}

void SetByte(std::vector<svBitVecVal>* words, unsigned int byte_index,
             std::uint8_t value) {
  const unsigned int bit_index = byte_index * 8U;
  const unsigned int word_index = bit_index / 32U;
  const unsigned int bit_offset = bit_index % 32U;
  if (words == nullptr || word_index >= words->size()) {
    return;
  }
  const svBitVecVal clear_mask =
      ~(static_cast<svBitVecVal>(0xFFU) << bit_offset);
  (*words)[word_index] = ((*words)[word_index] & clear_mask) |
                         (static_cast<svBitVecVal>(value) << bit_offset);
}

Ddr5InstanceState& Instance(std::uint16_t id) { return g_instances[id]; }

std::vector<svBitVecVal> ZeroData(unsigned int data_width) {
  return std::vector<svBitVecVal>(WordCount(data_width), 0U);
}

std::vector<svBitVecVal> ReadDataLocked(std::uint16_t id, std::uint64_t addr) {
  Ddr5InstanceState& st = Instance(id);
  const auto it = st.storage.find(addr);
  if (it == st.storage.end()) {
    return ZeroData(st.data_width);
  }
  return it->second;
}

bool PopPendingRead(std::uint16_t id, std::uint64_t* addr);
bool HasPendingRead(std::uint16_t id);

}  // namespace

#ifdef MM_DDR5_SINGLE_STORAGE_NONBLOCKING
extern "C" void s2h_ddr5_read_data_reply(const svBitVecVal* data, unsigned char valid);
#endif

namespace {

void SendReadDataReply(const std::vector<svBitVecVal>& data, unsigned char valid) {
#ifdef MM_DDR5_SINGLE_STORAGE_NONBLOCKING
  s2h_ddr5_read_data_reply(data.data(), valid);
#else
  (void)data;
  (void)valid;
#endif
}

bool PopPendingRead(std::uint16_t id, std::uint64_t* addr) {
  if (addr == nullptr) {
    return false;
  }
  bool has_req = false;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    Ddr5InstanceState& st = Instance(id);
    if (!st.pending_reads.empty()) {
      *addr = st.pending_reads.front();
      st.pending_reads.pop_front();
      has_req = true;
    }
  }
  return has_req;
}

bool HasPendingRead(std::uint16_t id) {
  std::lock_guard<std::mutex> lock(g_lock);
  const Ddr5InstanceState& st = Instance(id);
  return !st.pending_reads.empty();
}

}  // namespace

extern "C" void h2s_ddr5_init_bfm(const svBitVecVal* bfm_id,
                                   unsigned int data_width) {
  const std::uint16_t id = ReadU16(bfm_id);
  const unsigned int dw = (data_width == 0U) ? 256U : data_width;
  svScope scope = svGetScope();
  const char* hdl_path =
      (scope != nullptr) ? svGetNameFromScope(scope) : "<null_scope>";
  std::lock_guard<std::mutex> lock(g_lock);
  Ddr5InstanceState& st = Instance(id);
  st.data_width = dw;
  st.data_byte_width = (dw + 7U) / 8U;
  st.storage.clear();
  st.pending_reads.clear();
  LogEvent(1, "init mode=%s path=%s bfm_id=0x%04x data_width=%u data_byte_width=%u",
           kModeName,
           hdl_path, static_cast<unsigned>(id), st.data_width,
           st.data_byte_width);
}

extern "C" void h2s_ddr5_write(const svBitVecVal* bfm_id,
                                const svBitVecVal* addr,
                                const svBitVecVal* data,
                                const svBitVecVal* mask) {
  const std::uint16_t id = ReadU16(bfm_id);
  const std::uint64_t address = ReadU64(addr);
  std::lock_guard<std::mutex> lock(g_lock);
  Ddr5InstanceState& st = Instance(id);
  std::vector<svBitVecVal>& cur = st.storage[address];
  if (cur.empty()) {
    cur = ZeroData(st.data_width);
  }
  std::vector<svBitVecVal> in = ZeroData(st.data_width);
  if (data != nullptr) {
    std::memcpy(in.data(), data, in.size() * sizeof(svBitVecVal));
  }
  for (unsigned int i = 0; i < st.data_byte_width; ++i) {
    if (MaskBitSet(mask, i)) {
      SetByte(&cur, i, GetByte(in, i));
    }
  }
  LogEvent(2, "write bfm_id=0x%04x addr=0x%016llx data=%s",
           static_cast<unsigned>(id), static_cast<unsigned long long>(address),
           HexPreview(WordsToBytes(in, st.data_byte_width), g_log_bytes).c_str());
}

extern "C" void h2s_ddr5_read(const svBitVecVal* bfm_id,
                               const svBitVecVal* addr,
                               svBitVecVal* data) {
  const std::uint16_t id = ReadU16(bfm_id);
  const std::uint64_t address = ReadU64(addr);
  std::vector<svBitVecVal> value;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    value = ReadDataLocked(id, address);
  }
  if (data != nullptr) {
    std::memcpy(data, value.data(), value.size() * sizeof(svBitVecVal));
  }
  unsigned int byte_count = 0U;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    byte_count = Instance(id).data_byte_width;
  }
  LogEvent(2, "read bfm_id=0x%04x addr=0x%016llx data=%s",
           static_cast<unsigned>(id), static_cast<unsigned long long>(address),
           HexPreview(WordsToBytes(value, byte_count), g_log_bytes).c_str());
}

extern "C" void h2s_ddr5_read_request(const svBitVecVal* bfm_id,
                                       const svBitVecVal* addr) {
  const std::uint16_t id = ReadU16(bfm_id);
  const std::uint64_t address = ReadU64(addr);
  if (!kNonBlockingMode) {
    LogEvent(1,
             "read_request ignored in blocking mode bfm_id=0x%04x addr=0x%016llx",
             static_cast<unsigned>(id), static_cast<unsigned long long>(address));
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_lock);
    Instance(id).pending_reads.push_back(address);
  }
  LogEvent(2, "read_request bfm_id=0x%04x addr=0x%016llx", static_cast<unsigned>(id),
           static_cast<unsigned long long>(address));
}

extern "C" void h2s_ddr5_read_data_request(const svBitVecVal* bfm_id,
                                            const svBitVecVal* left_space) {
  const std::uint16_t id = ReadU16(bfm_id);
  if (!kNonBlockingMode) {
    LogEvent(1,
             "read_data_request ignored in blocking mode bfm_id=0x%04x",
             static_cast<unsigned>(id));
    return;
  }

  std::vector<svBitVecVal> out;
  unsigned int byte_count = 0U;
  std::uint64_t addr = 0U;
  bool has_req = false;

  if (ReadU16(left_space) == 0U) {
    {
      std::lock_guard<std::mutex> lock(g_lock);
      const Ddr5InstanceState& st = Instance(id);
      out = ZeroData(st.data_width);
      byte_count = st.data_byte_width;
    }
    SendReadDataReply(out, 0U);
    LogEvent(2, "read_data_request bfm_id=0x%04x left_space=0 -> invalid",
             static_cast<unsigned>(id));
    return;
  }

  has_req = PopPendingRead(id, &addr);
  {
    std::lock_guard<std::mutex> lock(g_lock);
    const Ddr5InstanceState& st = Instance(id);
    out = has_req ? ReadDataLocked(id, addr) : ZeroData(st.data_width);
    byte_count = st.data_byte_width;
  }
  SendReadDataReply(out, has_req ? 1U : 0U);
  LogEvent(2, "read_data_request bfm_id=0x%04x addr=0x%016llx valid=%u",
           static_cast<unsigned>(id), static_cast<unsigned long long>(addr),
           has_req ? 1U : 0U);
  if (has_req) {
    LogEvent(3, "reply_data bfm_id=0x%04x addr=0x%016llx data=%s",
             static_cast<unsigned>(id), static_cast<unsigned long long>(addr),
             HexPreview(WordsToBytes(out, byte_count), g_log_bytes).c_str());
  }
}

extern "C" void h2s_ddr5_read_data_blocking(const svBitVecVal* bfm_id) {
  if (!kNonBlockingMode) {
    const std::uint16_t id = ReadU16(bfm_id);
    LogEvent(1,
             "read_data_blocking ignored in blocking mode bfm_id=0x%04x",
             static_cast<unsigned>(id));
    return;
  }

  // Keep a single reply call site: blocking path still funnels through
  // h2s_ddr5_read_data_request(), but with explicit blocking semantics.
  const std::uint16_t id = ReadU16(bfm_id);
  if (!HasPendingRead(id)) {
    const svBitVecVal left_space_zero[1] = {0U};
    h2s_ddr5_read_data_request(bfm_id, left_space_zero);
    LogEvent(2, "read_data_blocking bfm_id=0x%04x no_pending_request -> invalid",
             static_cast<unsigned>(id));
    return;
  }

  const svBitVecVal left_space_nonzero[1] = {0xFFFFU};
  h2s_ddr5_read_data_request(bfm_id, left_space_nonzero);
  LogEvent(2, "read_data_blocking bfm_id=0x%04x pending_request -> request_data",
           static_cast<unsigned>(id));
}
