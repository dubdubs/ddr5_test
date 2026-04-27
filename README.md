# ddr5_test

`SW` 侧共享内存客户端 + BoostMQ 进程通信示例。

## 设计

- 共享内存（由服务端创建）：
  - `test_sw2hw`
  - `test_hw2sw`
- 消息队列：
  - `test_cmd_sw2hw`（SW -> HW）
  - `test_ack_hw2sw`（HW -> SW）

`sw_memory_client` 运行在被动模式，不主动发请求，只接收服务端命令并响应；  
`mock_hw_server` 用于本地联调，会创建并清理共享内存和消息队列。

## 构建

```bash
cd ddr5_test
make
```

## 运行（两个进程）

终端1（服务端，创建共享内存并发压测命令）：

```bash
cd ddr5_test
./mock_hw_server test 100000 64 0x6
```

终端2（SW memory 客户端，被动响应）：

```bash
cd ddr5_test
./sw_memory_client test
```

调试模式示例（命令行参数）：

```bash
./sw_memory_client test --debug-check --debug-trace --debug-bytes 64
```

说明：

- 服务端先启动并创建共享内存与消息队列，然后等待你按 Enter 开始压测。
- 客户端不主动发任何请求，只被动处理服务端下发请求。
- BoostMQ 使用两段收发：先收/发 `PayloadBase`，再按 `PayloadBase.length` 收/发 body。
- body 按 `DDR5Payload` 兼容头解析（`command/mask_enable/data_size/ddr5_info/addr/data_ptr`），
  后跟数据（写请求）或读返回数据（读响应）。
- `sw_memory_client` 会周期打印总吞吐和读写分离指标：
  - 总体：`qps`、`MiB/s`、`avg_lat_us`、`max_lat_us`
  - 读路径：`read_qps`、`read_MiB/s`、`read_avg_lat_us`
  - 写路径：`write_qps`、`write_MiB/s`、`write_avg_lat_us`
- 调试开关（默认关闭）：
  - `--debug-check`：开启读写数据一致性检查，并统计 `check_fail`
  - `--debug-trace`：打印每笔读写数据预览（十六进制）
  - `--debug-bytes <N>`：限制每笔预览的最大字节数（默认 `32`）
  - 兼容保留环境变量方式：`DDR5_SW_MEM_DEBUG_CHECK` / `DDR5_SW_MEM_DEBUG_TRACE` / `DDR5_SW_MEM_DEBUG_BYTES`

参数说明：

- `mock_hw_server <ipc_name> <rounds> <bytes_per_req> <comp_id>`
- `sw_memory_client [ipc_name] [--debug-check] [--debug-trace] [--debug-bytes <N>]`

## BridgeDispatcher 联调约定

当 `HW` 端使用 `BridgeDispatcher`（BoostMQ）时，`sw_memory_client` 按以下方向工作：

- 从 `ipc_name + "_boost_sw2hw"` 接收 `HELLO_REQ/MEM_REQ`
- 向 `ipc_name + "_boost_hello"` 发送 `HELLO_RSP`
- 向 `ipc_name + "_boost_hw2sw"` 发送 `MEM_RSP`

注意事项：

- `BridgeDispatcher` 队列名必须与上述后缀严格一致，任何拼写差异都会导致连接失败。
- 常见错误示例：`"_boosst_sw2hw"`、`"boost_hello"`（缺少前导下划线）。
- 双方都采用两段消息：先 `PayloadBase`，再发送/接收 body。
- 队列 `max_msg_size` 需覆盖实际消息体大小（至少不小于 `PayloadBase`，并能容纳内存请求/响应 body）。

## TlmDispatcher 兼容客户端

新增 `dispatcher_sw_client`，用于对接你给的 `TlmDispatcher` BoostMQ 命名规则：

- `ipc_name + "_boost_sw2hw"`
- `ipc_name + "_boost_hw2sw"`
- `ipc_name + "_boost_hello"`

`dispatcher_sw_client` 的主要用途：

- 作为 **交互式联调工具**，手动验证 `TlmDispatcher` 收发链路是否打通。
- 发送 `hello` 握手并等待 hello 队列响应，快速确认 `comp_id/type` 是否匹配。
- 支持 `sendhex/recv` 直接收发裸字节，便于排查协议封包、长度和超时问题。
- 在共享内存存在时支持 `shmwrite/shmread`，用于端到端联调数据路径。

运行：

```bash
cd ddr5_test
./dispatcher_sw_client <ipc_name>
```

例如 `ipc_name=test`：

```bash
./dispatcher_sw_client test
```

命令示例：

```text
disp> hello 0x6 0x1001
disp> sendhex 01 02 03 04
disp> recv 2000
disp> shmwrite 0 11 22 33 44
disp> shmread 0 4
```

## 维护约定

- 每次对 `ddr5_test` 下代码做功能变更或优化时，必须同步更新本 README（行为变化、参数、命令、协议差异都要更新）。
- 若仅做重构且行为不变，也要至少检查 README 是否仍与代码一致。
