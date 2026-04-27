#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace bip = boost::interprocess;
namespace bpt = boost::posix_time;

namespace {

class CleanupGuard {
 public:
  explicit CleanupGuard(std::string ipc_name) : ipc_name_(std::move(ipc_name)) {}
  ~CleanupGuard() {
    bip::message_queue::remove(ddr5_test::MqSwToHwName(ipc_name_).c_str());
    bip::message_queue::remove(ddr5_test::MqHwToSwName(ipc_name_).c_str());
    bip::message_queue::remove(ddr5_test::MqHelloName(ipc_name_).c_str());
    bip::shared_memory_object::remove(ddr5_test::kShmSwToHw);
    bip::shared_memory_object::remove(ddr5_test::kShmHwToSw);
  }

 private:
  std::string ipc_name_;
};

void SendMemReqTwoPhase(bip::message_queue* mq_sw2hw,
                        std::uint32_t comp_id,
                        const ddr5_test::Ddr5PayloadWire& req,
                        const std::uint8_t* extra_data,
                        std::size_t extra_size) {
  ddr5_test::PayloadBase hdr{};
  hdr.comp_id = comp_id;
  hdr.type = ddr5_test::HYBRID_MEM_REQ;
  hdr.length = static_cast<std::uint32_t>(sizeof(req) + extra_size);

  std::vector<std::uint8_t> body(sizeof(req) + extra_size);
  std::memcpy(body.data(), &req, sizeof(req));
  if (extra_size > 0 && extra_data) {
    std::memcpy(body.data() + sizeof(req), extra_data, extra_size);
  }

  mq_sw2hw->send(&hdr, sizeof(hdr), 0);
  mq_sw2hw->send(body.data(), body.size(), 0);
}

ddr5_test::Ddr5PayloadWire RecvMemRspTwoPhase(bip::message_queue* mq_hw2sw,
                                              std::uint64_t expected_addr,
                                              std::uint8_t* read_data_out) {
  std::vector<std::uint8_t> buf(4096);
  std::size_t recv_size = 0;
  unsigned recv_prio = 0;
  mq_hw2sw->receive(buf.data(), buf.size(), recv_size, recv_prio);
  if (recv_size < sizeof(ddr5_test::PayloadBase)) {
    throw std::runtime_error("short response header message");
  }
  ddr5_test::PayloadBase hdr{};
  std::memcpy(&hdr, buf.data(), sizeof(hdr));
  if (hdr.type != ddr5_test::HYBRID_MEM_RSP ||
      hdr.length < sizeof(ddr5_test::Ddr5PayloadWire)) {
    throw std::runtime_error("bad mem response header");
  }

  if (hdr.length > 4096) {
    throw std::runtime_error("response body too large");
  }
  std::vector<std::uint8_t> body(4096);
  mq_hw2sw->receive(body.data(), body.size(), recv_size, recv_prio);
  if (recv_size < hdr.length) {
    throw std::runtime_error("bad mem response body");
  }
  body.resize(hdr.length);

  ddr5_test::Ddr5PayloadWire rsp{};
  std::memcpy(&rsp, body.data(), sizeof(rsp));
  if (rsp.addr != expected_addr) {
    throw std::runtime_error("response addr mismatch");
  }
  if ((rsp.ddr5_info & 0x8000U) != 0) {
    throw std::runtime_error("response status error");
  }

  if (rsp.command == static_cast<std::uint8_t>(ddr5_test::OpCode::kRead) &&
      read_data_out && rsp.data_size > 0) {
    if (body.size() < sizeof(rsp) + rsp.data_size) {
      throw std::runtime_error("short read data in response");
    }
    std::memcpy(read_data_out, body.data() + sizeof(rsp), rsp.data_size);
  }
  return rsp;
}

void HelloHandshake(bip::message_queue* mq_sw2hw,
                    bip::message_queue* mq_hello,
                    std::uint32_t comp_id) {
  ddr5_test::PayloadBase hello{};
  hello.comp_id = comp_id;
  hello.type = ddr5_test::HYBRID_HELLO_REQ;
  hello.length = 0;
  mq_sw2hw->send(&hello, sizeof(hello), 0);

  std::vector<std::uint8_t> buf(4096);
  std::size_t recv_size = 0;
  unsigned recv_prio = 0;
  const auto timeout = bpt::microsec_clock::universal_time() + bpt::seconds(5);
  const bool ok =
      mq_hello->timed_receive(buf.data(), buf.size(), recv_size, recv_prio, timeout);
  if (!ok || recv_size < sizeof(ddr5_test::PayloadBase)) {
    throw std::runtime_error("hello handshake timeout");
  }
  ddr5_test::PayloadBase rsp{};
  std::memcpy(&rsp, buf.data(), sizeof(rsp));
  if (rsp.type != ddr5_test::HYBRID_HELLO_RSP) {
    throw std::runtime_error("hello response type mismatch");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string ipc_name = (argc > 1) ? argv[1] : "test";
    const std::uint32_t rounds =
        (argc > 2) ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 100000;
    const std::uint32_t bytes_per_req =
        (argc > 3) ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 64;
    const std::uint32_t comp_id =
        (argc > 4) ? static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 0)) : 0x6;

    if (bytes_per_req == 0 || bytes_per_req > ddr5_test::kMemorySize) {
      throw std::runtime_error("invalid bytes_per_req");
    }

    CleanupGuard guard(ipc_name);
    bip::message_queue::remove(ddr5_test::MqSwToHwName(ipc_name).c_str());
    bip::message_queue::remove(ddr5_test::MqHwToSwName(ipc_name).c_str());
    bip::message_queue::remove(ddr5_test::MqHelloName(ipc_name).c_str());
    bip::shared_memory_object::remove(ddr5_test::kShmSwToHw);
    bip::shared_memory_object::remove(ddr5_test::kShmHwToSw);

    bip::shared_memory_object shm_sw2hw(
        bip::create_only, ddr5_test::kShmSwToHw, bip::read_write);
    shm_sw2hw.truncate(ddr5_test::kMemorySize);
    bip::shared_memory_object shm_hw2sw(
        bip::create_only, ddr5_test::kShmHwToSw, bip::read_write);
    shm_hw2sw.truncate(ddr5_test::kMemorySize);

    bip::mapped_region region_sw2hw(shm_sw2hw, bip::read_write);
    bip::mapped_region region_hw2sw(shm_hw2sw, bip::read_write);
    auto* sw2hw = static_cast<std::uint8_t*>(region_sw2hw.get_address());
    auto* hw2sw = static_cast<std::uint8_t*>(region_hw2sw.get_address());
    std::memset(sw2hw, 0, ddr5_test::kMemorySize);
    std::memset(hw2sw, 0, ddr5_test::kMemorySize);

    bip::message_queue mq_sw2hw(
        bip::create_only, ddr5_test::MqSwToHwName(ipc_name).c_str(),
        ddr5_test::kQueueDepth, 4096);
    bip::message_queue mq_hw2sw(
        bip::create_only, ddr5_test::MqHwToSwName(ipc_name).c_str(),
        ddr5_test::kQueueDepth, 4096);
    bip::message_queue mq_hello(
        bip::create_only, ddr5_test::MqHelloName(ipc_name).c_str(),
        ddr5_test::kQueueDepth, 4096);

    std::cout << "[HW] ready (dispatcher + two-phase ddr5 payload)\n"
              << "  ipc_name=" << ipc_name << " comp_id=0x" << std::hex
              << comp_id << std::dec << "\n"
              << "  rounds=" << rounds << " bytes_per_req=" << bytes_per_req << "\n";
    std::cout << "Start sw_memory_client now, then press ENTER...";
    std::cin.get();

    HelloHandshake(&mq_sw2hw, &mq_hello, comp_id);
    std::cout << "[HW] hello handshake ok\n";

    std::vector<std::uint8_t> expect(bytes_per_req);
    for (std::uint32_t i = 0; i < bytes_per_req; ++i) {
      expect[i] = static_cast<std::uint8_t>(i & 0xFFU);
    }

    auto t0 = std::chrono::steady_clock::now();

    for (std::uint32_t i = 0; i < rounds; ++i) {
      const std::uint32_t offset =
          (i * bytes_per_req) % (ddr5_test::kMemorySize - bytes_per_req);
      std::memcpy(sw2hw + offset, expect.data(), bytes_per_req);

      ddr5_test::Ddr5PayloadWire wr{};
      wr.command = static_cast<std::uint8_t>(ddr5_test::OpCode::kWrite);
      wr.mask_enable = 0;
      wr.data_size = bytes_per_req;
      wr.ddr5_info = 0;
      wr.addr = offset;
      wr.data_ptr = 0;
      SendMemReqTwoPhase(&mq_sw2hw, comp_id, wr, expect.data(), expect.size());
      (void)RecvMemRspTwoPhase(&mq_hw2sw, wr.addr, nullptr);

      ddr5_test::Ddr5PayloadWire rd{};
      rd.command = static_cast<std::uint8_t>(ddr5_test::OpCode::kRead);
      rd.mask_enable = 0;
      rd.data_size = bytes_per_req;
      rd.ddr5_info = 0;
      rd.addr = offset;
      rd.data_ptr = 0;
      SendMemReqTwoPhase(&mq_sw2hw, comp_id, rd, nullptr, 0);
      std::vector<std::uint8_t> got(bytes_per_req);
      (void)RecvMemRspTwoPhase(&mq_hw2sw, rd.addr, got.data());

      if (std::memcmp(got.data(), expect.data(), bytes_per_req) != 0 ||
          std::memcmp(hw2sw + offset, expect.data(), bytes_per_req) != 0) {
        throw std::runtime_error("data mismatch");
      }
    }

    auto t1 = std::chrono::steady_clock::now();
    const double secs =
        std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
    const double reqs = static_cast<double>(rounds) * 2.0;
    const double bytes = static_cast<double>(rounds) * bytes_per_req * 2.0;

    std::cout << "[HW] done\n"
              << "  elapsed_s=" << secs << "\n"
              << "  req_total=" << reqs << " qps=" << (reqs / secs) << "\n"
              << "  throughput_MiBps=" << ((bytes / (1024.0 * 1024.0)) / secs) << "\n";

    ddr5_test::Ddr5PayloadWire ex{};
    ex.command = static_cast<std::uint8_t>(ddr5_test::OpCode::kExit);
    ex.mask_enable = 0;
    ex.data_size = 0;
    ex.ddr5_info = 0;
    ex.addr = 0;
    ex.data_ptr = 0;
    SendMemReqTwoPhase(&mq_sw2hw, comp_id, ex, nullptr, 0);
    (void)RecvMemRspTwoPhase(&mq_hw2sw, 0, nullptr);
  } catch (const std::exception& e) {
    std::cerr << "[HW] fatal: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
