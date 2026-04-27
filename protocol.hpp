#ifndef DDR5_TEST_PROTOCOL_HPP_
#define DDR5_TEST_PROTOCOL_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace ddr5_test {

constexpr const char* kShmSwToHw = "test_sw2hw";
constexpr const char* kShmHwToSw = "test_hw2sw";

constexpr std::size_t kMemorySize = 1024 * 1024;
constexpr std::size_t kQueueDepth = 128;

enum class OpCode : std::uint8_t {
  kWrite = 1,
  kRead = 2,
  kPing = 3,
  kExit = 255,
};

enum MsgType : std::uint32_t {
  HYBRID_HELLO_REQ = 0x1001,
  HYBRID_HELLO_RSP = 0x1002,
  HYBRID_MEM_REQ = 0x2001,
  HYBRID_MEM_RSP = 0x2002,
};

#pragma pack(push, 1)
struct PayloadBase {
  std::uint32_t comp_id;
  std::uint32_t type;
  std::uint32_t length;
};

struct MemRequest {
  std::uint32_t op;
  std::uint32_t offset;
  std::uint32_t size;
  std::uint32_t token;
};

struct MemResponse {
  std::uint32_t status;
  std::uint32_t token;
  std::uint32_t size;
};

/*
 * Wire header compatible with:
 * struct DDR5Payload : PayloadBase { ... uint8_t* data; }
 * For IPC transfer, data pointer value is treated as opaque placeholder.
 */
struct Ddr5PayloadWire {
  std::uint8_t command;      // 0: read, 1: write
  std::uint8_t mask_enable;  // 0: all 0xff, 1: mask provided
  std::uint32_t data_size;
  std::uint16_t ddr5_info;
  std::uint64_t addr;
  std::uint64_t data_ptr;
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable<PayloadBase>::value,
              "PayloadBase must be POD");
static_assert(std::is_trivially_copyable<MemRequest>::value,
              "MemRequest must be POD");
static_assert(std::is_trivially_copyable<MemResponse>::value,
              "MemResponse must be POD");
static_assert(std::is_trivially_copyable<Ddr5PayloadWire>::value,
              "Ddr5PayloadWire must be POD");

inline std::string MqSwToHwName(const std::string& ipc_name) {
  return ipc_name + "_boost_sw2hw";
}

inline std::string MqHwToSwName(const std::string& ipc_name) {
  return ipc_name + "_boost_hw2sw";
}

inline std::string MqHelloName(const std::string& ipc_name) {
  return ipc_name + "_boost_hello";
}

}  // namespace ddr5_test

#endif  // DDR5_TEST_PROTOCOL_HPP_
