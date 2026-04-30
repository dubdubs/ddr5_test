#include <svdpi.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct Ddr5InstanceConfig {
  unsigned int addr_width = 64;
  unsigned int data_width = 256;
  unsigned int data_byte_width = 32;
};

struct ReadRequest {
  uint16_t bfm_id = 0;
  uint64_t addr = 0;
};

struct Ddr5InstanceState {
  Ddr5InstanceConfig config;
  std::unordered_map<uint64_t, std::vector<svBitVecVal>> storage;
  std::deque<ReadRequest> pending_reads;
};

std::mutex g_mutex;
std::unordered_map<uint16_t, Ddr5InstanceState> g_instances;

extern "C" void s2h_ddr5_read_data_reply(const svBitVecVal* data, unsigned char valid);

int WordCount(unsigned int bit_width) {
  return std::max(1U, (bit_width + 31U) / 32U);
}

uint32_t GetWord(const svBitVecVal* value, int index) {
  if (value == nullptr) {
    return 0;
  }
  return static_cast<uint32_t>(value[index]);
}

uint16_t ReadU16(const svBitVecVal* value) {
  return static_cast<uint16_t>(GetWord(value, 0) & 0xffffU);
}

uint64_t ReadU64(const svBitVecVal* value) {
  return static_cast<uint64_t>(GetWord(value, 0)) |
         (static_cast<uint64_t>(GetWord(value, 1)) << 32);
}

bool MaskBitSet(const svBitVecVal* mask, unsigned int bit_index) {
  const unsigned int word_index = bit_index / 32U;
  const unsigned int bit_offset = bit_index % 32U;
  return ((GetWord(mask, static_cast<int>(word_index)) >> bit_offset) & 1U) != 0U;
}

std::vector<svBitVecVal> ZeroWords(unsigned int bit_width) {
  return std::vector<svBitVecVal>(WordCount(bit_width), 0);
}

uint8_t GetByte(const std::vector<svBitVecVal>& words, unsigned int byte_index) {
  const unsigned int bit_index = byte_index * 8U;
  const unsigned int word_index = bit_index / 32U;
  const unsigned int bit_offset = bit_index % 32U;
  if (word_index >= words.size()) {
    return 0;
  }
  return static_cast<uint8_t>((words[word_index] >> bit_offset) & 0xffU);
}

uint8_t GetByteFromWordsPtr(const svBitVecVal* words, unsigned int byte_index) {
  const unsigned int bit_index = byte_index * 8U;
  const unsigned int word_index = bit_index / 32U;
  const unsigned int bit_offset = bit_index % 32U;
  const uint32_t word = GetWord(words, static_cast<int>(word_index));
  return static_cast<uint8_t>((word >> bit_offset) & 0xffU);
}

void SetByte(std::vector<svBitVecVal>* words, unsigned int byte_index, uint8_t value) {
  const unsigned int bit_index = byte_index * 8U;
  const unsigned int word_index = bit_index / 32U;
  const unsigned int bit_offset = bit_index % 32U;
  if (words == nullptr || word_index >= words->size()) {
    return;
  }

  const svBitVecVal clear_mask = ~(static_cast<svBitVecVal>(0xffU) << bit_offset);
  (*words)[word_index] = ((*words)[word_index] & clear_mask) |
                         (static_cast<svBitVecVal>(value) << bit_offset);
}

Ddr5InstanceState& GetInstanceLocked(uint16_t bfm_id) {
  return g_instances[bfm_id];
}

std::vector<svBitVecVal> ReadDataLocked(uint16_t bfm_id, uint64_t addr) {
  Ddr5InstanceState& state = GetInstanceLocked(bfm_id);
  const auto it = state.storage.find(addr);
  if (it == state.storage.end()) {
    return ZeroWords(state.config.data_width);
  }
  return it->second;
}

void ReadDataToBufferLocked(uint16_t bfm_id, uint64_t addr, svBitVecVal* out) {
  Ddr5InstanceState& state = GetInstanceLocked(bfm_id);
  const unsigned int word_count = static_cast<unsigned int>(WordCount(state.config.data_width));
  if (out == nullptr) {
    return;
  }

  const auto it = state.storage.find(addr);
  if (it == state.storage.end()) {
    std::fill(out, out + word_count, static_cast<svBitVecVal>(0));
    return;
  }
  std::copy(it->second.begin(), it->second.end(), out);
}

void WriteDataLocked(uint16_t bfm_id, uint64_t addr, const svBitVecVal* data,
                     const svBitVecVal* mask) {
  Ddr5InstanceState& state = GetInstanceLocked(bfm_id);
  const unsigned int data_width = state.config.data_width;
  const unsigned int byte_count = std::max(1U, (data_width + 7U) / 8U);
  std::vector<svBitVecVal>& current = state.storage[addr];
  if (current.empty()) {
    current = ZeroWords(data_width);
  }

  if (mask == nullptr) {
    if (data == nullptr) {
      return;
    }
    std::copy(data, data + current.size(), current.begin());
    return;
  }

  for (unsigned int byte_index = 0; byte_index < byte_count; ++byte_index) {
    if (MaskBitSet(mask, byte_index)) {
      SetByte(&current, byte_index, GetByteFromWordsPtr(data, byte_index));
    }
  }
}

bool PopPendingReadLocked(uint16_t bfm_id, ReadRequest* request) {
  Ddr5InstanceState& state = GetInstanceLocked(bfm_id);
  if (state.pending_reads.empty()) {
    return false;
  }
  if (request != nullptr) {
    *request = state.pending_reads.front();
  }
  state.pending_reads.pop_front();
  return true;
}

void ReplyReadData(uint16_t bfm_id, uint64_t addr, bool valid) {
  thread_local std::vector<svBitVecVal> reply_words;
  const svBitVecVal* reply_ptr = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Ddr5InstanceState& state = GetInstanceLocked(bfm_id);
    const unsigned int word_count = static_cast<unsigned int>(WordCount(state.config.data_width));
    reply_words.assign(word_count, 0);

    if (valid) {
      ReadDataToBufferLocked(bfm_id, addr, reply_words.data());
    }
    reply_ptr = reply_words.data();
  }
  s2h_ddr5_read_data_reply(reply_ptr, valid ? 1U : 0U);
}

}  // namespace

extern "C" void h2s_ddr5_init_bfm(const svBitVecVal* bfm_id,
                                   unsigned int addr_width,
                                   unsigned int data_width,
                                   unsigned int data_byte_width) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Ddr5InstanceState& state = GetInstanceLocked(ReadU16(bfm_id));
  state.config.addr_width = addr_width;
  state.config.data_width = data_width;
  state.config.data_byte_width = data_byte_width;
  state.pending_reads.clear();
  state.pending_reads.shrink_to_fit();
}

extern "C" void h2s_ddr5_write(const svBitVecVal* bfm_id,
                                const svBitVecVal* addr,
                                const svBitVecVal* data,
                                const svBitVecVal* mask) {
  std::lock_guard<std::mutex> lock(g_mutex);
  WriteDataLocked(ReadU16(bfm_id), ReadU64(addr), data, mask);
}

extern "C" void h2s_ddr5_read(const svBitVecVal* bfm_id,
                               const svBitVecVal* addr,
                               svBitVecVal* data) {
  const uint16_t id = ReadU16(bfm_id);
  const uint64_t address = ReadU64(addr);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    ReadDataToBufferLocked(id, address, data);
  }
}

extern "C" void h2s_ddr5_read_request(const svBitVecVal* bfm_id,
                                       const svBitVecVal* addr) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint16_t id = ReadU16(bfm_id);
  GetInstanceLocked(id).pending_reads.push_back({id, ReadU64(addr)});
}

extern "C" void h2s_ddr5_read_data_request(const svBitVecVal* bfm_id,
                                            const svBitVecVal* left_space) {
  const uint16_t id = ReadU16(bfm_id);
  if (ReadU16(left_space) == 0U) {
    ReplyReadData(id, 0, false);
    return;
  }

  ReadRequest request;
  bool has_request = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    has_request = PopPendingReadLocked(id, &request);
  }
  ReplyReadData(id, request.addr, has_request);
}

extern "C" void h2s_ddr5_read_data_blocking(const svBitVecVal* bfm_id) {
  const uint16_t id = ReadU16(bfm_id);
  ReadRequest request;
  bool has_request = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    has_request = PopPendingReadLocked(id, &request);
  }
  ReplyReadData(id, request.addr, has_request);
}
