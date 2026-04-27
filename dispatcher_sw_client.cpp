#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace bip = boost::interprocess;
namespace bpt = boost::posix_time;

namespace {

struct PayloadBase {
  std::uint32_t comp_id;
  std::uint32_t type;
  std::uint32_t length;
};

constexpr std::uint32_t kHybridHelloReqDefault = 0x1001;

std::vector<std::string> Split(const std::string& line) {
  std::istringstream iss(line);
  std::vector<std::string> out;
  std::string w;
  while (iss >> w) {
    out.push_back(w);
  }
  return out;
}

std::uint32_t ParseU32(const std::string& s) {
  std::size_t pos = 0;
  unsigned long v = std::stoul(s, &pos, 0);
  if (pos != s.size() || v > 0xFFFFFFFFUL) {
    throw std::runtime_error("invalid u32: " + s);
  }
  return static_cast<std::uint32_t>(v);
}

std::uint8_t ParseByte(const std::string& s) {
  std::size_t pos = 0;
  unsigned long v = std::stoul(s, &pos, 0);
  if (pos != s.size() || v > 0xFFUL) {
    throw std::runtime_error("invalid byte: " + s);
  }
  return static_cast<std::uint8_t>(v);
}

std::unique_ptr<bip::message_queue> OpenQueueByCandidates(
    const std::vector<std::string>& candidates,
    std::string* selected_name) {
  for (const auto& n : candidates) {
    try {
      auto q = std::make_unique<bip::message_queue>(bip::open_only, n.c_str());
      if (selected_name) {
        *selected_name = n;
      }
      return q;
    } catch (const bip::interprocess_exception&) {
    }
  }
  std::ostringstream oss;
  oss << "cannot open any queue in candidates: ";
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (i) {
      oss << ", ";
    }
    oss << candidates[i];
  }
  throw std::runtime_error(oss.str());
}

void HexDump(const std::uint8_t* data, std::size_t len) {
  for (std::size_t i = 0; i < len; ++i) {
    if (i % 16 == 0) {
      std::cout << "\n[" << std::setw(4) << std::setfill('0') << std::hex << i
                << "] ";
    }
    std::cout << std::setw(2) << std::setfill('0') << std::hex
              << static_cast<unsigned>(data[i]) << " ";
  }
  std::cout << std::dec << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string ipc_name = (argc > 1) ? argv[1] : "test";

    std::string queue_out_name;
    std::string queue_in_name;
    std::string queue_hello_name;

    auto mq_out = OpenQueueByCandidates(
        {ipc_name + "_boost_sw2hw"}, &queue_out_name);
    auto mq_in = OpenQueueByCandidates(
        {ipc_name + "_boost_hw2sw"}, &queue_in_name);
    auto mq_hello = OpenQueueByCandidates(
        {ipc_name + "_boost_hello"}, &queue_hello_name);

    std::unique_ptr<bip::shared_memory_object> shm_sw2hw;
    std::unique_ptr<bip::shared_memory_object> shm_hw2sw;
    std::unique_ptr<bip::mapped_region> region_sw2hw;
    std::unique_ptr<bip::mapped_region> region_hw2sw;
    std::uint8_t* sw2hw = nullptr;
    std::uint8_t* hw2sw = nullptr;

    try {
      shm_sw2hw = std::make_unique<bip::shared_memory_object>(
          bip::open_only, ddr5_test::kShmSwToHw, bip::read_write);
      shm_hw2sw = std::make_unique<bip::shared_memory_object>(
          bip::open_only, ddr5_test::kShmHwToSw, bip::read_write);
      region_sw2hw = std::make_unique<bip::mapped_region>(*shm_sw2hw, bip::read_write);
      region_hw2sw = std::make_unique<bip::mapped_region>(*shm_hw2sw, bip::read_write);
      sw2hw = static_cast<std::uint8_t*>(region_sw2hw->get_address());
      hw2sw = static_cast<std::uint8_t*>(region_hw2sw->get_address());
    } catch (const std::exception&) {
      std::cout << "[SW] shared memory not available now, queue test still works.\n";
    }

    std::cout << "[SW] dispatcher client connected.\n"
              << "  mq_out   : " << queue_out_name << "\n"
              << "  mq_in    : " << queue_in_name << "\n"
              << "  mq_hello : " << queue_hello_name << "\n";
    if (sw2hw && hw2sw) {
      std::cout << "  shm      : " << ddr5_test::kShmSwToHw << ", "
                << ddr5_test::kShmHwToSw << "\n";
    }

    std::cout
        << "commands:\n"
        << "  hello <comp_id> [msg_type=0x1001]\n"
        << "  sendhex <hex1> [hex2 ...]\n"
        << "  recv [timeout_ms=2000]\n"
        << "  shmwrite <offset> <hex1> [hex2 ...]\n"
        << "  shmread <offset> <len>\n"
        << "  quit\n";

    std::string line;
    while (std::cout << "disp> ", std::getline(std::cin, line)) {
      const auto args = Split(line);
      if (args.empty()) {
        continue;
      }

      if (args[0] == "quit" || args[0] == "exit") {
        break;
      }

      if (args[0] == "hello") {
        if (args.size() < 2 || args.size() > 3) {
          std::cout << "usage: hello <comp_id> [msg_type]\n";
          continue;
        }
        PayloadBase hello{};
        hello.comp_id = ParseU32(args[1]);
        hello.type = (args.size() == 3) ? ParseU32(args[2]) : kHybridHelloReqDefault;
        hello.length = 0;

        mq_out->send(&hello, sizeof(hello), 0);
        std::cout << "[SW] hello sent, comp_id=" << hello.comp_id
                  << ", type=0x" << std::hex << hello.type << std::dec << "\n";

        std::vector<std::uint8_t> buf(4096);
        std::size_t recved = 0;
        unsigned prio = 0;
        const auto timeout = bpt::microsec_clock::universal_time() + bpt::seconds(5);
        const bool ok =
            mq_hello->timed_receive(buf.data(), buf.size(), recved, prio, timeout);
        if (!ok) {
          std::cout << "[SW] hello timed out\n";
        } else {
          std::cout << "[SW] hello recv " << recved << " bytes";
          if (recved >= sizeof(PayloadBase)) {
            PayloadBase resp{};
            std::memcpy(&resp, buf.data(), sizeof(resp));
            std::cout << " (comp_id=" << resp.comp_id << ", type=0x"
                      << std::hex << resp.type << std::dec
                      << ", len=" << resp.length << ")";
          }
          std::cout << "\n";
          HexDump(buf.data(), recved);
        }
        continue;
      }

      if (args[0] == "sendhex") {
        if (args.size() < 2) {
          std::cout << "usage: sendhex <hex1> [hex2 ...]\n";
          continue;
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(args.size() - 1);
        for (std::size_t i = 1; i < args.size(); ++i) {
          payload.push_back(ParseByte(args[i]));
        }
        mq_out->send(payload.data(), payload.size(), 0);
        std::cout << "[SW] sent " << payload.size() << " bytes\n";
        continue;
      }

      if (args[0] == "recv") {
        const std::uint32_t timeout_ms = (args.size() > 1) ? ParseU32(args[1]) : 2000;
        std::vector<std::uint8_t> buf(4096);
        std::size_t recved = 0;
        unsigned prio = 0;
        const auto timeout = bpt::microsec_clock::universal_time() +
                             bpt::milliseconds(timeout_ms);
        const bool ok =
            mq_in->timed_receive(buf.data(), buf.size(), recved, prio, timeout);
        if (!ok) {
          std::cout << "[SW] recv timeout\n";
        } else {
          std::cout << "[SW] recv " << recved << " bytes\n";
          HexDump(buf.data(), recved);
        }
        continue;
      }

      if (args[0] == "shmwrite") {
        if (!sw2hw) {
          std::cout << "shared memory not opened\n";
          continue;
        }
        if (args.size() < 3) {
          std::cout << "usage: shmwrite <offset> <hex1> [hex2 ...]\n";
          continue;
        }
        const std::uint32_t offset = ParseU32(args[1]);
        const std::uint32_t n = static_cast<std::uint32_t>(args.size() - 2);
        if (offset + n > ddr5_test::kMemorySize) {
          std::cout << "out of range\n";
          continue;
        }
        for (std::size_t i = 2; i < args.size(); ++i) {
          sw2hw[offset + (i - 2)] = ParseByte(args[i]);
        }
        std::cout << "[SW] shmwrite ok\n";
        continue;
      }

      if (args[0] == "shmread") {
        if (!hw2sw) {
          std::cout << "shared memory not opened\n";
          continue;
        }
        if (args.size() != 3) {
          std::cout << "usage: shmread <offset> <len>\n";
          continue;
        }
        const std::uint32_t offset = ParseU32(args[1]);
        const std::uint32_t len = ParseU32(args[2]);
        if (offset + len > ddr5_test::kMemorySize) {
          std::cout << "out of range\n";
          continue;
        }
        HexDump(hw2sw + offset, len);
        continue;
      }

      std::cout << "unknown command\n";
    }
  } catch (const std::exception& e) {
    std::cerr << "[SW] fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
