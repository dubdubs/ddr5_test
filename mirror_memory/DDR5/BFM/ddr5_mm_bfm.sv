module ddr5_mm_bfm #(
    parameter [7:0] DDR5_ID = 0,
    parameter [1:0] CHANNEL_ID = 0,
    parameter [0:0] SUBCHANNEL_ID = 0,
    parameter [2:0] RANK_ID = 0,
    parameter ADDR_WIDTH = 64,
    parameter DATA_WIDTH = 256,
    // NOTE: Must be a parameter as it's used in port declaration
    parameter DATA_BYTE_WIDTH = DATA_WIDTH/8,
    parameter FIFO_AWIDTH = 8,  // FIFO depth of 256
    parameter MAX_OUTSTANDING_READS = 16,
    parameter BLOCKING_READ_THRESHOLD = 0,
    parameter [31:0] POLLING_INTERVAL = 16,
    parameter ENABLE_DDR5_ERROR_INJECTION = 0
)(
    // Write Port
    input  wire                           mm_clk,
    input  wire                           mm_rstn,
    input  wire                           mm_wen,
    input  wire [ADDR_WIDTH-1:0]         mm_waddr,
    input  wire [DATA_WIDTH-1:0]         mm_wdata,
    input  wire [DATA_BYTE_WIDTH-1:0]    mm_wmask,
    
    // Read Port
    input  wire                           mm_ren,
    input  wire [ADDR_WIDTH-1:0]         mm_raddr,
    input  wire                          mm_rready,
    output reg  [DATA_WIDTH-1:0]         mm_rdata,
    output reg                           mm_rvalid,

    input  wire [31:0]                  mm_rlat
);

localparam FIFO_DEPTH = (1 << FIFO_AWIDTH);
localparam OUTSTANDING_READS_AWIDTH = $clog2(MAX_OUTSTANDING_READS);

localparam [15:0] BFM_ID = {DDR5_ID, CHANNEL_ID, SUBCHANNEL_ID, RANK_ID, 2'b0};


`include "ddr5_mm_bfm_remap.svh"


`ifdef MM_DDR5_SINGLE_STORAGE_NONBLOCKING
    // FIFO definitions
    reg [DATA_WIDTH-1:0] read_fifo [0:FIFO_DEPTH-1];
    bit [FIFO_AWIDTH:0] fifo_rptr;
    bit [FIFO_AWIDTH:0] fifo_wptr;
    bit [FIFO_AWIDTH:0] left_space;

    // FIFO status
    wire fifo_empty;
    wire fifo_full;

    // mm_rready backpressure FIFO - 用于存储因mm_rready=0而无法输出的数据
    localparam RREADY_FIFO_AWIDTH = $clog2(MAX_OUTSTANDING_READS);
    localparam RREADY_FIFO_DEPTH = MAX_OUTSTANDING_READS;
    reg [DATA_WIDTH-1:0] rready_fifo [0:RREADY_FIFO_DEPTH-1];
    bit [RREADY_FIFO_AWIDTH:0] rready_fifo_rptr;
    bit [RREADY_FIFO_AWIDTH:0] rready_fifo_wptr;

    // RREADY FIFO status
    wire rready_fifo_empty;
    wire rready_fifo_full;

    // DDR5 data and address remap
    reg [64-1:0] remapped_raddr;
    reg [64-1:0] remapped_waddr;
    reg [DATA_WIDTH-1:0] remapped_wdata;
    reg [DATA_BYTE_WIDTH-1:0] remapped_wmask;

    import "DPI-C" context function void h2s_ddr5_init_bfm(
        input bit [15:0] bfm_id,
        input int unsigned ADDR_WIDTH,
        input int unsigned DATA_WIDTH,
        input int unsigned DATA_BYTE_WIDTH
    );

    // Import DPI-C functions
    (* is_nonblocking_dpi = 1 *)
    import "DPI-C" context function void h2s_ddr5_write(
        input bit [15:0] bfm_id,
        input bit [64-1:0] addr,
        input bit [DATA_WIDTH-1:0] data,
        input bit [DATA_BYTE_WIDTH-1:0] mask
    );

   import "DPI-C" context function void h2s_ddr5_read(
       input bit [15:0] bfm_id,
       input bit [63:0] addr,
       output bit [DATA_WIDTH-1:0] data
   );

    (* is_nonblocking_dpi = 1 *)
    import "DPI-C" context function void h2s_ddr5_read_request(
        input bit [15:0] bfm_id,
        input bit [64-1:0] addr
    );

    // Export callback function
    export "DPI-C" function s2h_ddr5_read_data_reply;

    // Import DPI-C function for requesting DDR5 read data
    (* is_nonblocking_dpi = 1 *)
    import "DPI-C" context function void h2s_ddr5_read_data_request(
        input bit [15:0] bfm_id,
        input bit [15:0] left_space
    );

    import "DPI-C" context function void h2s_ddr5_read_data_blocking(
        input bit [15:0] bfm_id
    );

    // Read transaction tracking (synthesizable)
    reg [63:0] cycle_counter;
    reg [31:0] pending_read_start_cycle[0:MAX_OUTSTANDING_READS-1];
    reg [OUTSTANDING_READS_AWIDTH:0] pending_req_count;
    reg [OUTSTANDING_READS_AWIDTH:0] pending_resp_count;
    wire [OUTSTANDING_READS_AWIDTH:0] num_pending_reads = pending_req_count - pending_resp_count;

    // Registers for the dedicated low-latency read pipeline
    reg [DATA_WIDTH-1:0] ll_read_data;
    reg ll_read_valid;
    reg [DATA_WIDTH-1:0] ll_rdata_hold;
    reg ll_rvalid_hold;


    // Counters for request_data tracking
    reg [FIFO_AWIDTH:0] request_data_send_count;    // Number of request_data sent
    reg [FIFO_AWIDTH:0] request_data_resp_count;    // Number of responses received

    // Counters for read requests and responses
    reg [15:0] read_req_count;   // Number of read requests sent
    reg [15:0] read_resp_count;  // Number of read responses received

    reg [31:0] polling_counter;
    reg [31:0] polling_interval_reg;


    reg timeout_detected;
    reg normal_request_needed;
    reg issue_poll_now;

    wire [FIFO_AWIDTH:0] pending_request_data_count;
    wire [FIFO_AWIDTH:0] available_space ;
    wire can_request_data;
`pragma protect begin

    // DPI-C callback function - Write to FIFO
    // This is now the ONLY place that modifies FIFO pointers and data
    function void s2h_ddr5_read_data_reply(input bit [DATA_WIDTH-1:0] data, input byte valid);
    begin
        // Always increment request_data_resp_count as this function is called
        // for every request_data (both from h2s_ddr5_read_data_request and h2s_ddr5_read_data_blocking)
        request_data_resp_count = request_data_resp_count + 1;

        if (valid) begin
            if (num_pending_reads > 0) begin
                if (!fifo_full) begin
                    read_fifo[fifo_wptr[FIFO_AWIDTH-1:0]] = data;
                    fifo_wptr = fifo_wptr + 1;
                end
                // A read completed, increment the response counter
                pending_resp_count = pending_resp_count + 1;
            end
        end
    end
    endfunction
    
    // Calculate FIFO available space
    function automatic [FIFO_AWIDTH:0] calculate_space(
        bit [FIFO_AWIDTH:0] rptr, 
        bit [FIFO_AWIDTH:0] wptr
    );
    begin
        if ((wptr[FIFO_AWIDTH] == ~rptr[FIFO_AWIDTH]) && 
            (rptr[FIFO_AWIDTH-1:0] >= wptr[FIFO_AWIDTH-1:0])) begin
            calculate_space = rptr[FIFO_AWIDTH-1:0] - wptr[FIFO_AWIDTH-1:0];
        end else begin
            calculate_space = FIFO_DEPTH - wptr[FIFO_AWIDTH-1:0] + rptr[FIFO_AWIDTH-1:0];
        end
    end
    endfunction

    // Calculate pending request_data count
    function automatic [FIFO_AWIDTH:0] calculate_pending_request_data_count(
        bit [FIFO_AWIDTH:0] send_count,
        bit [FIFO_AWIDTH:0] resp_count
    );
    begin
        if (send_count[FIFO_AWIDTH] == ~resp_count[FIFO_AWIDTH]) begin
            calculate_pending_request_data_count = FIFO_DEPTH + send_count[FIFO_AWIDTH-1:0] - resp_count[FIFO_AWIDTH-1:0];
        end else begin
            calculate_pending_request_data_count = send_count[FIFO_AWIDTH-1:0] - resp_count[FIFO_AWIDTH-1:0];
        end
    end
    endfunction

    // FIFO status calculation
    assign fifo_empty = (fifo_rptr == fifo_wptr);
    assign fifo_full = (fifo_rptr[FIFO_AWIDTH] == ~fifo_wptr[FIFO_AWIDTH]) &&
                      (fifo_rptr[FIFO_AWIDTH-1:0] == fifo_wptr[FIFO_AWIDTH-1:0]);

    // RREADY FIFO status calculation
    assign rready_fifo_empty = (rready_fifo_rptr == rready_fifo_wptr);
    assign rready_fifo_full = (rready_fifo_rptr[RREADY_FIFO_AWIDTH] == ~rready_fifo_wptr[RREADY_FIFO_AWIDTH]) &&
                             (rready_fifo_rptr[RREADY_FIFO_AWIDTH-1:0] == rready_fifo_wptr[RREADY_FIFO_AWIDTH-1:0]);

    // Calculate available space
    always @(*) begin
        left_space = calculate_space(fifo_rptr, fifo_wptr);
    end

    // FIFO read and output control with mm_rready backpressure handling
    always @(posedge mm_clk) begin
        if (!mm_rstn) begin
            fifo_rptr <= '0;
            rready_fifo_rptr <= '0;
            rready_fifo_wptr <= '0;
            mm_rvalid <= 1'b0;
            mm_rdata <= '0;
            ll_rvalid_hold <= 1'b0;
        end else begin
            // Stage 2 of the low-latency pipeline
            ll_rdata_hold <= ll_read_data;
            ll_rvalid_hold <= ll_read_valid;

            // Default to not valid
            mm_rvalid <= 1'b0;

            // 处理数据到RREADY FIFO的写入
            // Low-latency path has priority for writing to RREADY FIFO
            if (ll_rvalid_hold && !mm_rready && !rready_fifo_full) begin
                // 将low-latency数据写入RREADY FIFO
                rready_fifo[rready_fifo_wptr[RREADY_FIFO_AWIDTH-1:0]] <= ll_rdata_hold;
                rready_fifo_wptr <= rready_fifo_wptr + 1;
            end else if (!fifo_empty && !mm_rready && !rready_fifo_full) begin
                // 将FIFO数据写入RREADY FIFO
                rready_fifo[rready_fifo_wptr[RREADY_FIFO_AWIDTH-1:0]] <= read_fifo[fifo_rptr[FIFO_AWIDTH-1:0]];
                rready_fifo_wptr <= rready_fifo_wptr + 1;
                fifo_rptr <= fifo_rptr + 1; // 从原FIFO中移除数据
            end

            // 输出控制：优先级为 RREADY FIFO > Low-latency > Normal FIFO
            if (!rready_fifo_empty && mm_rready) begin
                // 从RREADY FIFO输出数据
                mm_rdata <= rready_fifo[rready_fifo_rptr[RREADY_FIFO_AWIDTH-1:0]];
                mm_rvalid <= 1'b1;
                rready_fifo_rptr <= rready_fifo_rptr + 1;
            end else if (ll_rvalid_hold && mm_rready) begin
                // 直接从low-latency path输出
                mm_rdata <= ll_rdata_hold;
                mm_rvalid <= 1'b1;
            end else if (!fifo_empty && mm_rready) begin
                // 从normal FIFO输出
                mm_rdata <= read_fifo[fifo_rptr[FIFO_AWIDTH-1:0]];
                mm_rvalid <= 1'b1;
                fifo_rptr <= fifo_rptr + 1;
            end
        end
    end

    // Write logic
    always @(posedge mm_clk) begin
        if (mm_wen && mm_rstn) begin
            $display("Write request: addr=%h, data=%h, mask=%h", mm_waddr, mm_wdata, mm_wmask);
            remapped_waddr = remap_addr(mm_waddr, DDR5_ID, CHANNEL_ID, SUBCHANNEL_ID, RANK_ID);
            remapped_wdata = remap_data(mm_wdata, DDR5_ID, CHANNEL_ID, SUBCHANNEL_ID, RANK_ID);
            remapped_wmask = remap_wmask(mm_wmask, DDR5_ID, CHANNEL_ID, SUBCHANNEL_ID, RANK_ID);
            h2s_ddr5_write(BFM_ID, remapped_waddr, remapped_wdata,
                        remapped_wmask);
        end
    end

    // Read request logic
    always @(posedge mm_clk) begin
        // Stage 1 of the low-latency pipeline
        ll_read_valid <= 1'b0; // Default to not valid

        if (mm_ren && mm_rstn) begin
            remapped_raddr = remap_addr(mm_raddr, DDR5_ID, CHANNEL_ID, SUBCHANNEL_ID, RANK_ID);
            // If threshold is enabled and latency is low, use blocking read
            if (BLOCKING_READ_THRESHOLD > 0 && mm_rlat < BLOCKING_READ_THRESHOLD) begin
                $display("[SV] Low-latency read request: addr=%h, cycle=%d. Using blocking call.", mm_raddr, cycle_counter);
                h2s_ddr5_read(BFM_ID, remapped_raddr, ll_read_data);
                ll_read_valid <= 1'b1;
            end else begin // Otherwise, use non-blocking read
                if (num_pending_reads < MAX_OUTSTANDING_READS) begin
                    $display("[SV] Read request: addr=%h, cycle=%d", mm_raddr, cycle_counter);
                    
                    // Store the start cycle for this transaction, using req_count as the pointer
                    pending_read_start_cycle[pending_req_count[OUTSTANDING_READS_AWIDTH-1:0]] = cycle_counter;
                    pending_req_count = pending_req_count + 1;
                    
                    h2s_ddr5_read_request(BFM_ID, remapped_raddr);
                end else begin
                    $display("[SV] WARNING: Cannot issue read request. Maximum outstanding reads reached.");
                end
            end
        end
    end


    // Calculate pending request_data count and available space
    
    // Calculate pending request_data count and available space
    assign pending_request_data_count = calculate_pending_request_data_count(request_data_send_count, request_data_resp_count);
    assign available_space = (left_space > pending_request_data_count) ?
                                          (left_space - pending_request_data_count) : '0;
    assign can_request_data = (available_space > 1);

    // Request data control logic
    always @(posedge mm_clk) begin
        if (!mm_rstn) begin
            read_req_count <= '0;
            read_resp_count <= '0;
            request_data_send_count <= request_data_resp_count;
            cycle_counter <= 0;
            polling_counter <= 0;
//            pending_req_count <= 0;
//            pending_resp_count <= 0;
        end else begin
            cycle_counter <= cycle_counter + 1;

            // Determine if we need to increment request_data_send_count
            // This ensures only one increment per clock cycle for synthesizability

            timeout_detected = (num_pending_reads > 0) &&
                              (cycle_counter + 64'h2 - pending_read_start_cycle[pending_resp_count[OUTSTANDING_READS_AWIDTH-1:0]] >= mm_rlat);

            normal_request_needed = (read_req_count != read_resp_count) && can_request_data && !timeout_detected;

            issue_poll_now = normal_request_needed && (polling_counter >= polling_interval_reg);

            // Timeout logic has priority over normal request
            if (timeout_detected) begin
                $display("[SV] Timeout detected for oldest read request! Issuing blocking call at cycle %d.", cycle_counter);

                // Call blocking function which will internally call s2h_ddr5_read_data_reply
                // This ensures s2h_ddr5_read_data_reply is the only place that modifies FIFO
                h2s_ddr5_read_data_blocking(BFM_ID);

                // Increment request_data_send_count for timeout case
                request_data_send_count <= request_data_send_count + 1;
            end else if (issue_poll_now) begin
                // Request data if there are pending read requests and available space
                h2s_ddr5_read_data_request(BFM_ID, left_space);
                request_data_send_count <= request_data_send_count + 1;
            end

            if (issue_poll_now || (read_req_count == read_resp_count)) begin
                polling_counter <= 0;
            end else if (read_req_count != read_resp_count) begin
                polling_counter <= polling_counter + 1;
            end

            // Increment count when sending new read request
            if (mm_ren) begin
                read_req_count <= read_req_count + 1;
            end

            // Increment count when receiving read response
            if (mm_rvalid) begin
                read_resp_count <= read_resp_count + 1;
            end
        end
    end


    // Initialization
    initial begin
        fifo_rptr = '0;
        fifo_wptr = '0;
        read_req_count = '0;
        read_resp_count = '0;
        request_data_send_count = '0;
        request_data_resp_count = '0;
        left_space = FIFO_DEPTH;
        cycle_counter = 0;
        pending_req_count = 0;
        pending_resp_count = 0;
        polling_interval_reg = POLLING_INTERVAL;
        if (ENABLE_DDR5_ERROR_INJECTION) begin
            $display("[DDR5 BFM] Error injection hook enabled for BFM_ID=%h.", BFM_ID);
        end
        h2s_ddr5_init_bfm(BFM_ID, ADDR_WIDTH, DATA_WIDTH, DATA_BYTE_WIDTH);
    end

`pragma protect end

`else // BLOCKING

    // read data buffer
    reg [DATA_WIDTH-1:0] read_data;
    reg [DATA_WIDTH-1:0] read_data_hold;  // hold last read data
    reg read_valid;

    reg done = 0;

    // mm_rready backpressure FIFO for blocking mode
    localparam RREADY_FIFO_AWIDTH = $clog2(MAX_OUTSTANDING_READS);
    localparam RREADY_FIFO_DEPTH = MAX_OUTSTANDING_READS;
    reg [DATA_WIDTH-1:0] rready_fifo [0:RREADY_FIFO_DEPTH-1];
    bit [RREADY_FIFO_AWIDTH:0] rready_fifo_rptr;
    bit [RREADY_FIFO_AWIDTH:0] rready_fifo_wptr;

    // RREADY FIFO status
    wire rready_fifo_empty;
    wire rready_fifo_full;
    // DDR5 data and address remap
    reg [63:0] remapped_raddr;
    reg [63:0] remapped_waddr;
    reg [DATA_WIDTH-1:0] remapped_wdata;
    reg [DATA_BYTE_WIDTH-1:0] remapped_wmask;

    // Import DPI-C functions
    import "DPI-C" context function void h2s_ddr5_write(
        input bit [15:0] bfm_id,
        input bit [63:0] addr,
        input bit [DATA_WIDTH-1:0] data,
        input bit [DATA_BYTE_WIDTH-1:0] mask
    );

    import "DPI-C" context function void h2s_ddr5_read(
        input bit [15:0] bfm_id,
        input bit [63:0] addr,
        output bit [DATA_WIDTH-1:0] data
    );

    import "DPI-C" context function void h2s_ddr5_init_bfm(
        input bit [15:0] bfm_id,
        input int unsigned ADDR_WIDTH,
        input int unsigned DATA_WIDTH,
        input int unsigned DATA_BYTE_WIDTH
    );

    // RREADY FIFO status calculation for blocking mode
    assign rready_fifo_empty = (rready_fifo_rptr == rready_fifo_wptr);
    assign rready_fifo_full = (rready_fifo_rptr[RREADY_FIFO_AWIDTH] == ~rready_fifo_wptr[RREADY_FIFO_AWIDTH]) &&
                             (rready_fifo_rptr[RREADY_FIFO_AWIDTH-1:0] == rready_fifo_wptr[RREADY_FIFO_AWIDTH-1:0]);

`pragma protect begin

    // Write logic
    always @(posedge mm_clk) begin
        if (!mm_rstn) begin
            done <= 0;
        end else if (mm_wen && mm_rstn) begin
            remapped_waddr = remap_addr(mm_waddr, DDR5_ID, CHANNEL_ID, SUBCHANNEL_ID, RANK_ID);
            remapped_wdata = remap_data(mm_wdata, DDR5_ID, CHANNEL_ID, SUBCHANNEL_ID, RANK_ID);
            remapped_wmask = remap_wmask(mm_wmask, DDR5_ID, CHANNEL_ID, SUBCHANNEL_ID, RANK_ID);
            h2s_ddr5_write(BFM_ID, remapped_waddr, remapped_wdata,
                        remapped_wmask);
            done <= 1;
        end else begin
            done <= 0;
        end
    end

    // Read request and data handling with mm_rready backpressure support
    always @(posedge mm_clk) begin
        if (!mm_rstn) begin
            mm_rvalid <= 1'b0;
            mm_rdata <= '0;
            read_valid <= 1'b0;
            read_data <= '0;
            read_data_hold <= '0;
            rready_fifo_rptr <= '0;
            rready_fifo_wptr <= '0;
        end else begin
            if (mm_ren) begin
                    remapped_raddr = remap_addr(mm_raddr, DDR5_ID, CHANNEL_ID, SUBCHANNEL_ID, RANK_ID);
                h2s_ddr5_read(BFM_ID, remapped_raddr, read_data);
                read_valid <= 1'b1;
                read_data_hold <= read_data;  // save current read data
            end else begin
                read_valid <= 1'b0;
            end

            // 处理数据到RREADY FIFO的写入
            if (read_valid && !mm_rready && !rready_fifo_full) begin
                // 将读数据写入RREADY FIFO
                rready_fifo[rready_fifo_wptr[RREADY_FIFO_AWIDTH-1:0]] <= read_data_hold;
                rready_fifo_wptr <= rready_fifo_wptr + 1;
            end

            // 输出控制：优先级为 RREADY FIFO > 当前读数据
            mm_rvalid <= 1'b0;  // 默认无效
            if (!rready_fifo_empty && mm_rready) begin
                // 从RREADY FIFO输出数据
                mm_rdata <= rready_fifo[rready_fifo_rptr[RREADY_FIFO_AWIDTH-1:0]];
                mm_rvalid <= 1'b1;
                rready_fifo_rptr <= rready_fifo_rptr + 1;
            end else if (read_valid && mm_rready) begin
                // 直接输出当前读数据
                mm_rdata <= read_data_hold;
                mm_rvalid <= 1'b1;
            end
        end
    end

    initial begin
        if (ENABLE_DDR5_ERROR_INJECTION) begin
            $display("[DDR5 BFM] Error injection hook enabled for BFM_ID=%h.", BFM_ID);
        end
        h2s_ddr5_init_bfm(BFM_ID, ADDR_WIDTH, DATA_WIDTH, DATA_BYTE_WIDTH);
    end

`pragma protect end

`endif

endmodule
