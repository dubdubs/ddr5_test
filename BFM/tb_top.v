`ifdef UMISIM
reg [63:0] clk_cnt = 64'd0;
reg rstn;
wire [1:0] test_status;
assign hw_top.rst_n = rstn;
assign test_status = hw_top.test_status;

// ---------------- perf by event trigger ----------------
localparam int TARGET_TXN = 30;  

reg perf_started;
reg perf_done;
reg [63:0] perf_start_clk;
reg [63:0] perf_end_clk;

reg [63:0] wr_cnt, rd_req_cnt, rd_rsp_cnt, txn_done_cnt;
reg [63:0] rd_lat_sum, rd_lat_max;

// 记录每个读请求发起时刻（简单FIFO）
localparam int LATQ_DEPTH = 128;
reg [63:0] rd_ts_q [0:LATQ_DEPTH-1];
reg [6:0]  rd_ts_wptr, rd_ts_rptr;
reg [7:0]  rd_ts_count;

initial begin
  rstn = 0;
  perf_started = 0;
  perf_done = 0;
  wr_cnt = 0;
  rd_req_cnt = 0;
  rd_rsp_cnt = 0;
  txn_done_cnt = 0;
  rd_lat_sum = 0;
  rd_lat_max = 0;
  rd_ts_wptr = 0;
  rd_ts_rptr = 0;
  rd_ts_count = 0;
end

always @(posedge hw_top.clk) begin
  clk_cnt <= clk_cnt + 1;

  // 复位释放
  if (clk_cnt == 300) begin
    rstn <= 1;
  end

  // 事件触发开始统计：复位后第一次看到读/写请求
  if (!perf_started && rstn && (hw_top.mm_wen || hw_top.mm_ren)) begin
    perf_started <= 1;
    perf_start_clk <= clk_cnt;
    $display("[PERF] start @ clk=%0d", clk_cnt);
  end

  if (perf_started && !perf_done) begin
    // 写请求计数（这里把写请求视作完成事务）
    if (hw_top.mm_wen) begin
      wr_cnt <= wr_cnt + 1;
      txn_done_cnt <= txn_done_cnt + 1;
    end

    // 读请求计数 + 入时间戳队列
    if (hw_top.mm_ren) begin
      rd_req_cnt <= rd_req_cnt + 1;
      if (rd_ts_count < LATQ_DEPTH) begin
        rd_ts_q[rd_ts_wptr] <= clk_cnt;
        rd_ts_wptr <= rd_ts_wptr + 1'b1;
        rd_ts_count <= rd_ts_count + 1'b1;
      end
    end

    // 读响应计数 + 读延迟统计
    if (hw_top.mm_rvalid && hw_top.mm_rready) begin
      reg [63:0] lat;
      rd_rsp_cnt <= rd_rsp_cnt + 1;
      txn_done_cnt <= txn_done_cnt + 1;
      if (rd_ts_count != 0) begin
        lat = clk_cnt - rd_ts_q[rd_ts_rptr];
        rd_ts_rptr <= rd_ts_rptr + 1'b1;
        rd_ts_count <= rd_ts_count - 1'b1;
        rd_lat_sum <= rd_lat_sum + lat;
        if (lat > rd_lat_max) rd_lat_max <= lat;
      end
    end

    // 按目标事务数结束
    if (txn_done_cnt >= TARGET_TXN) begin
      perf_done <= 1;
      perf_end_clk <= clk_cnt;

      $display("========== PERF RESULT ==========");
      $display("start_clk=%0d end_clk=%0d total_cycles=%0d",
               perf_start_clk, clk_cnt, (clk_cnt - perf_start_clk + 1));
      $display("wr_cnt=%0d rd_req_cnt=%0d rd_rsp_cnt=%0d txn_done_cnt=%0d",
               wr_cnt, rd_req_cnt, rd_rsp_cnt, txn_done_cnt);

      if ((clk_cnt - perf_start_clk + 1) != 0) begin
        $display("effective_tput(txn/cycle)=%.6f",
                 txn_done_cnt * 1.0 / (clk_cnt - perf_start_clk + 1));
      end

      if (rd_rsp_cnt != 0) begin
        $display("rd_lat_avg(cycle)=%.3f", rd_lat_sum * 1.0 / rd_rsp_cnt);
        $display("rd_lat_max(cycle)=%0d", rd_lat_max);
      end
      $display("=================================");
    end
  end
end

// 原有 test_done 逻辑
reg [7:0] stop_cnt = 8'd0;
always @(posedge hw_top.clk) begin
  if (hw_top.test_done) begin
    stop_cnt <= stop_cnt + 1'b1;
  end else begin
    stop_cnt <= 8'd0;
  end

  if (stop_cnt == 8'hff) begin
    report_result(test_status);
  end
end
`endif
