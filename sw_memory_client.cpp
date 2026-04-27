#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace bip = boost::interprocess;

namespace {

std::unique_ptr<bip::message_queue> OpenQueueByCandidates(
    const std::vector<std::string>& candidates, std::string* selected_name) {
  for (const auto& name : candidates) {
    try {
      auto q = std::make_unique<bip::message_queue>(bip::open_only, name.c_str());
      if (selected_name) {
        *selected_name = name;
      }
      return q;
    } catch (const bip::interprocess_exception&) {
    }
  }
  throw std::runtime_error("open queue failed for all candidates");
}

bool EnvFlagEnabled(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr) {
    return false;
  }
  const std::string s(v);
  return s == "1" || s == "true" || s == "TRUE" || s == "on" || s == "ON" ||
         s == "yes" || s == "YES";
}

std::size_t EnvByteLimit(const char* name, std::size_t default_v) {
  const char* v = std::getenv(name);
  if (v == nullptr) {
    return default_v;
  }
  try {
    const unsigned long parsed = std::stoul(v, nullptr, 0);
    return static_cast<std::size_t>(parsed);
  } catch (const std::exception&) {
    return default_v;
  }
}

struct CliOptions {
  std::string ipc_name = "test";
  bool debug_check = false;
  bool debug_trace = false;
  std::size_t debug_bytes = 32;
};

bool IsOptionToken(const std::string& arg) {
  return !arg.empty() && arg[0] == '-';
}

CliOptions ParseOptions(int argc, char** argv) {
  CliOptions opts;
  int i = 1;
  if (i < argc) {
    const std::string first(argv[i]);
    if (!IsOptionToken(first)) {
      opts.ipc_name = first;
      ++i;
    }
  }

  while (i < argc) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      std::cout
          << "usage: sw_memory_client [ipc_name] [--debug-check] [--debug-trace] "
             "[--debug-bytes N]\n";
      std::exit(EXIT_SUCCESS);
    }
    if (arg == "--debug-check") {
      opts.debug_check = true;
      ++i;
      continue;
    }
    if (arg == "--debug-trace") {
      opts.debug_trace = true;
      ++i;
      continue;
    }
    if (arg == "--debug-bytes") {
      if (i + 1 >= argc) {
        throw std::runtime_error("--debug-bytes requires a value");
      }
      const unsigned long parsed = std::stoul(argv[i + 1], nullptr, 0);
      opts.debug_bytes = static_cast<std::size_t>(parsed);
      i += 2;
      continue;
    }
    throw std::runtime_error("unknown argument: " + arg);
  }

  return opts;
}

std::string HexPreview(const std::uint8_t* data,
                       std::size_t len,
                       std::size_t max_bytes) {
  if (data == nullptr || len == 0) {
    return "<empty>";
  }
  const std::size_t n = std::min(len, max_bytes);
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < n; ++i) {
    if (i != 0) {
      oss << " ";
    }
    oss << std::setw(2) << static_cast<unsigned>(data[i]);
  }
  if (n < len) {
    oss << " ...(" << std::dec << len << " bytes total)";
  }
  return oss.str();
}

std::size_t FirstMismatch(const std::uint8_t* a,
                          const std::uint8_t* b,
                          std::size_t len) {
  for (std::size_t i = 0; i < len; ++i) {
    if (a[i] != b[i]) {
      return i;
    }
  }
  return len;
}

void SendHelloRsp(bip::message_queue* mq_hello, std::uint32_t comp_id) {
  ddr5_test::PayloadBase rsp{};
  rsp.comp_id = comp_id;
  rsp.type = ddr5_test::HYBRID_HELLO_RSP;
  rsp.length = 0;
  mq_hello->send(&rsp, sizeof(rsp), 0);
}

void SendMemRspTwoPhase(bip::message_queue* mq_hw2sw,
                        std::uint32_t comp_id,
                        const ddr5_test::Ddr5PayloadWire& wire,
                        const std::uint8_t* extra_data,
                        std::size_t extra_size,
                        std::size_t max_body_size) {
  if (sizeof(wire) + extra_size > max_body_size) {
    throw std::runtime_error("response body exceeds mq_hw2sw max_msg_size");
  }

  ddr5_test::PayloadBase hdr{};
  hdr.comp_id = comp_id;
  hdr.type = ddr5_test::HYBRID_MEM_RSP;
  hdr.length = static_cast<std::uint32_t>(sizeof(wire) + extra_size);

  std::vector<std::uint8_t> body(sizeof(wire) + extra_size);
  std::memcpy(body.data(), &wire, sizeof(wire));
  if (extra_size > 0 && extra_data) {
    std::memcpy(body.data() + sizeof(wire), extra_data, extra_size);
  }
  mq_hw2sw->send(&hdr, sizeof(hdr), 0);
  mq_hw2sw->send(body.data(), body.size(), 0);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const CliOptions opts = ParseOptions(argc, argv);
    const std::string ipc_name = opts.ipc_name;

    bip::shared_memory_object shm_sw2hw(
        bip::open_only, ddr5_test::kShmSwToHw, bip::read_write);
    bip::shared_memory_object shm_hw2sw(
        bip::open_only, ddr5_test::kShmHwToSw, bip::read_write);

    bip::mapped_region region_sw2hw(shm_sw2hw, bip::read_write);
    bip::mapped_region region_hw2sw(shm_hw2sw, bip::read_write);
    auto* sw2hw = static_cast<std::uint8_t*>(region_sw2hw.get_address());
    auto* hw2sw = static_cast<std::uint8_t*>(region_hw2sw.get_address());

    std::string mq_sw2hw_name;
    std::string mq_hw2sw_name;
    std::string mq_hello_name;
    auto mq_sw2hw = OpenQueueByCandidates(
        {ddr5_test::MqSwToHwName(ipc_name)}, &mq_sw2hw_name);
    auto mq_hw2sw = OpenQueueByCandidates(
        {ddr5_test::MqHwToSwName(ipc_name)},
        &mq_hw2sw_name);
    auto mq_hello = OpenQueueByCandidates(
        {ddr5_test::MqHelloName(ipc_name)},
        &mq_hello_name);
    const std::size_t sw2hw_max_msg = mq_sw2hw->get_max_msg_size();
    const std::size_t hw2sw_max_msg = mq_hw2sw->get_max_msg_size();
    const std::size_t hello_max_msg = mq_hello->get_max_msg_size();
    if (sw2hw_max_msg < sizeof(ddr5_test::PayloadBase) ||
        hw2sw_max_msg < sizeof(ddr5_test::PayloadBase) ||
        hello_max_msg < sizeof(ddr5_test::PayloadBase)) {
      throw std::runtime_error("queue max_msg_size is smaller than PayloadBase");
    }
    const bool debug_check =
        opts.debug_check || EnvFlagEnabled("DDR5_SW_MEM_DEBUG_CHECK");
    const bool debug_trace =
        opts.debug_trace || EnvFlagEnabled("DDR5_SW_MEM_DEBUG_TRACE");
    const std::size_t debug_bytes = (opts.debug_bytes != 32)
                                        ? opts.debug_bytes
                                        : EnvByteLimit("DDR5_SW_MEM_DEBUG_BYTES", 32);

    std::uint64_t req_count = 0;
    std::uint64_t byte_count = 0;
    std::uint64_t read_req_count = 0;
    std::uint64_t read_byte_count = 0;
    std::uint64_t write_req_count = 0;
    std::uint64_t write_byte_count = 0;
    std::uint64_t total_latency_ns = 0;
    std::uint64_t read_latency_ns = 0;
    std::uint64_t write_latency_ns = 0;
    std::uint64_t max_latency_ns = 0;
    std::uint64_t check_fail_count = 0;
    auto start = std::chrono::steady_clock::now();

    std::cout << "[SW-MEM] ready (passive, dispatcher-aligned)\n"
              << "  ipc_name : " << ipc_name << "\n"
              << "  mq_sw2hw : " << mq_sw2hw_name << "\n"
              << "  mq_hw2sw : " << mq_hw2sw_name << "\n"
              << "  mq_hello : " << mq_hello_name << "\n"
              << "  direction: recv from mq_sw2hw, send to mq_hw2sw/mq_hello\n"
              << "  max_msg  : sw2hw=" << sw2hw_max_msg
              << " hw2sw=" << hw2sw_max_msg
              << " hello=" << hello_max_msg << "\n"
              << "  debug    : check=" << (debug_check ? "on" : "off")
              << " trace=" << (debug_trace ? "on" : "off")
              << " bytes=" << debug_bytes << "\n";

    std::vector<std::uint8_t> hdr_buf(sw2hw_max_msg);
    while (true) {
      std::size_t recv_size = 0;
      unsigned recv_prio = 0;
      mq_sw2hw->receive(hdr_buf.data(), hdr_buf.size(), recv_size, recv_prio);
      if (recv_size < sizeof(ddr5_test::PayloadBase)) {
        continue;
      }

      ddr5_test::PayloadBase hdr{};
      std::memcpy(&hdr, hdr_buf.data(), sizeof(hdr));

      if (hdr.type == ddr5_test::HYBRID_HELLO_REQ) {
        std::cout << "[SW-MEM] recv hello req comp_id=0x" << std::hex
                  << hdr.comp_id << std::dec << "\n";
        SendHelloRsp(mq_hello.get(), hdr.comp_id);
        std::cout << "[SW-MEM] hello rsp sent\n";
        continue;
      }

      if (hdr.type != ddr5_test::HYBRID_MEM_REQ || hdr.length < sizeof(ddr5_test::Ddr5PayloadWire)) {
        continue;
      }

      if (hdr.length > sw2hw_max_msg) {
        continue;
      }
      std::vector<std::uint8_t> body(sw2hw_max_msg);
      mq_sw2hw->receive(body.data(), body.size(), recv_size, recv_prio);
      if (recv_size < hdr.length) {
        continue;
      }
      body.resize(hdr.length);

      ddr5_test::Ddr5PayloadWire req{};
      std::memcpy(&req, body.data(), sizeof(req));

      ddr5_test::Ddr5PayloadWire rsp = req;
      rsp.data_ptr = 0;

      const bool range_ok =
          req.addr <= ddr5_test::kMemorySize &&
          req.data_size <= ddr5_test::kMemorySize &&
          req.addr + req.data_size <= ddr5_test::kMemorySize;
      if (!range_ok) {
        rsp.ddr5_info = static_cast<std::uint16_t>(0x8000U);
        rsp.data_size = 0;
        SendMemRspTwoPhase(
            mq_hw2sw.get(), hdr.comp_id, rsp, nullptr, 0, hw2sw_max_msg);
        continue;
      }

      const auto op = static_cast<ddr5_test::OpCode>(req.command);
      if (op == ddr5_test::OpCode::kWrite) {
        const auto req_begin = std::chrono::steady_clock::now();
        if (body.size() < sizeof(req) + req.data_size) {
          rsp.ddr5_info = static_cast<std::uint16_t>(0x8001U);
          rsp.data_size = 0;
          SendMemRspTwoPhase(
              mq_hw2sw.get(), hdr.comp_id, rsp, nullptr, 0, hw2sw_max_msg);
          continue;
        }
        const auto* wr_data = body.data() + sizeof(req);
        std::uint8_t* mem_ptr = sw2hw + req.addr;
        std::vector<std::uint8_t> before_write;
        if (debug_check && req.data_size > 0 && req.mask_enable != 0) {
          before_write.assign(mem_ptr, mem_ptr + req.data_size);
        }
        if (req.mask_enable == 0) {
          std::memcpy(mem_ptr, wr_data, req.data_size);
        } else {
          if (body.size() < sizeof(req) + req.data_size + req.data_size) {
            rsp.ddr5_info = static_cast<std::uint16_t>(0x8002U);
            rsp.data_size = 0;
            SendMemRspTwoPhase(
                mq_hw2sw.get(), hdr.comp_id, rsp, nullptr, 0, hw2sw_max_msg);
            continue;
          }
          const auto* mask = wr_data + req.data_size;
          for (std::uint32_t i = 0; i < req.data_size; ++i) {
            if (mask[i] == 0xFFU) {
              mem_ptr[i] = wr_data[i];
            }
          }
        }
        if (debug_check && req.data_size > 0) {
          bool ok = true;
          std::size_t bad_idx = req.data_size;
          if (req.mask_enable == 0) {
            bad_idx = FirstMismatch(mem_ptr, wr_data, req.data_size);
            ok = (bad_idx == req.data_size);
          } else {
            const auto* mask = wr_data + req.data_size;
            for (std::size_t i = 0; i < req.data_size; ++i) {
              const std::uint8_t expected =
                  (mask[i] == 0xFFU) ? wr_data[i] : before_write[i];
              if (mem_ptr[i] != expected) {
                ok = false;
                bad_idx = i;
                break;
              }
            }
          }
          if (!ok) {
            ++check_fail_count;
            std::cerr << "[SW-MEM][CHECK][WRITE] mismatch comp_id=0x"
                      << std::hex << hdr.comp_id << std::dec
                      << " addr=" << req.addr
                      << " size=" << req.data_size
                      << " bad_index=" << bad_idx << "\n";
          }
        }
        if (debug_trace) {
          std::cout << "[SW-MEM][TRACE][WRITE] comp_id=0x"
                    << std::hex << hdr.comp_id << std::dec
                    << " addr=" << req.addr
                    << " size=" << req.data_size
                    << " mask=" << static_cast<unsigned>(req.mask_enable)
                    << " data=" << HexPreview(wr_data, req.data_size, debug_bytes);
          if (req.mask_enable != 0 && body.size() >= sizeof(req) + req.data_size * 2U) {
            const auto* mask = wr_data + req.data_size;
            std::cout << " mask_data=" << HexPreview(mask, req.data_size, debug_bytes);
          }
          std::cout << " mem_after=" << HexPreview(mem_ptr, req.data_size, debug_bytes)
                    << "\n";
        }
        rsp.ddr5_info = 0;
        rsp.data_size = 0;
        SendMemRspTwoPhase(
            mq_hw2sw.get(), hdr.comp_id, rsp, nullptr, 0, hw2sw_max_msg);
        const auto req_end = std::chrono::steady_clock::now();
        const auto latency_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    req_end - req_begin)
                    .count());
        req_count++;
        byte_count += req.data_size;
        write_req_count++;
        write_byte_count += req.data_size;
        total_latency_ns += latency_ns;
        write_latency_ns += latency_ns;
        if (latency_ns > max_latency_ns) {
          max_latency_ns = latency_ns;
        }
      } else if (op == ddr5_test::OpCode::kRead) {
        const auto req_begin = std::chrono::steady_clock::now();
        std::memcpy(hw2sw + req.addr, sw2hw + req.addr, req.data_size);
        if (debug_check && req.data_size > 0) {
          const std::size_t bad_idx =
              FirstMismatch(hw2sw + req.addr, sw2hw + req.addr, req.data_size);
          if (bad_idx != req.data_size) {
            ++check_fail_count;
            std::cerr << "[SW-MEM][CHECK][READ] mismatch comp_id=0x"
                      << std::hex << hdr.comp_id << std::dec
                      << " addr=" << req.addr
                      << " size=" << req.data_size
                      << " bad_index=" << bad_idx << "\n";
          }
        }
        if (debug_trace) {
          std::cout << "[SW-MEM][TRACE][READ] comp_id=0x"
                    << std::hex << hdr.comp_id << std::dec
                    << " addr=" << req.addr
                    << " size=" << req.data_size
                    << " data=" << HexPreview(hw2sw + req.addr, req.data_size, debug_bytes)
                    << "\n";
        }
        rsp.ddr5_info = 0;
        SendMemRspTwoPhase(
            mq_hw2sw.get(), hdr.comp_id, rsp, hw2sw + req.addr, req.data_size,
            hw2sw_max_msg);
        const auto req_end = std::chrono::steady_clock::now();
        const auto latency_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    req_end - req_begin)
                    .count());
        req_count++;
        byte_count += req.data_size;
        read_req_count++;
        read_byte_count += req.data_size;
        total_latency_ns += latency_ns;
        read_latency_ns += latency_ns;
        if (latency_ns > max_latency_ns) {
          max_latency_ns = latency_ns;
        }
      } else if (op == ddr5_test::OpCode::kExit) {
        rsp.ddr5_info = 0;
        rsp.data_size = 0;
        SendMemRspTwoPhase(
            mq_hw2sw.get(), hdr.comp_id, rsp, nullptr, 0, hw2sw_max_msg);
        break;
      } else {
        rsp.ddr5_info = static_cast<std::uint16_t>(0x8003U);
        rsp.data_size = 0;
        SendMemRspTwoPhase(
            mq_hw2sw.get(), hdr.comp_id, rsp, nullptr, 0, hw2sw_max_msg);
      }

      if (req_count > 0 && req_count % 100000 == 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
                .count();
        const double secs = (ms <= 0) ? 1e-6 : static_cast<double>(ms) / 1000.0;
        const double qps = static_cast<double>(req_count) / secs;
        const double mibps =
            (static_cast<double>(byte_count) / (1024.0 * 1024.0)) / secs;
        const double read_qps = static_cast<double>(read_req_count) / secs;
        const double read_mibps =
            (static_cast<double>(read_byte_count) / (1024.0 * 1024.0)) / secs;
        const double write_qps = static_cast<double>(write_req_count) / secs;
        const double write_mibps =
            (static_cast<double>(write_byte_count) / (1024.0 * 1024.0)) / secs;
        const double avg_latency_us =
            (req_count == 0)
                ? 0.0
                : (static_cast<double>(total_latency_ns) /
                   static_cast<double>(req_count) / 1000.0);
        const double read_avg_latency_us =
            (read_req_count == 0)
                ? 0.0
                : (static_cast<double>(read_latency_ns) /
                   static_cast<double>(read_req_count) / 1000.0);
        const double write_avg_latency_us =
            (write_req_count == 0)
                ? 0.0
                : (static_cast<double>(write_latency_ns) /
                   static_cast<double>(write_req_count) / 1000.0);
        const double max_latency_us =
            static_cast<double>(max_latency_ns) / 1000.0;
        std::cout << "[SW-MEM] req=" << req_count
                  << " bytes=" << byte_count
                  << " qps=" << qps
                  << " MiB/s=" << mibps
                  << " avg_lat_us=" << avg_latency_us
                  << " max_lat_us=" << max_latency_us
                  << " read_qps=" << read_qps
                  << " read_MiB/s=" << read_mibps
                  << " read_avg_lat_us=" << read_avg_latency_us
                  << " write_qps=" << write_qps
                  << " write_MiB/s=" << write_mibps
                  << " write_avg_lat_us=" << write_avg_latency_us
                  << " check_fail=" << check_fail_count << "\n";
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "[SW-MEM] fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
