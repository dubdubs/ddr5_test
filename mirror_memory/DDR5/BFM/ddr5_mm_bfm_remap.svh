// DDR5 controller default address remap derived from
// docs/UVIP-DDR5-Controller-UG-002-00021-0J_251209.pdf, Section 2.8.
//
// The controller first drops the byte-within-command offset and then maps the
// remaining UIF address bits to DRAM fields through csr*Pos registers.
//
// This mirror-memory remap models the default register values from Table 2-35
// and Table 2-36:
// - Col[10:4]  <- UIF[6:0]
// - Row[17:0]  <- UIF[24:7]
// - Ba[4:0]    <- UIF[29:25]
// - Cid[2:0]   <- UIF[32:30]
//
// `RANK_ID` is already provided by the BFM instance, so Cs bits are not
// reconstructed from the address here. Unmapped upper UIF bits are preserved
// in the packed key so high-address regions still remain unique.

localparam int unsigned BEAT_BYTES =
    (DATA_WIDTH / 8);
localparam int unsigned BEAT_LSB =
    (BEAT_BYTES <= 1) ? 0 : $clog2(BEAT_BYTES);

localparam int unsigned COL_BITS = 7;   // Col[10:4]
localparam int unsigned ROW_BITS = 18;  // Row[17:0]
localparam int unsigned BA_BITS = 5;    // BA[1:0] + BG[2:0]
localparam int unsigned CID_BITS = 3;

function automatic int unsigned get_col_pos(input int unsigned index);
begin
    get_col_pos = index;
end
endfunction

function automatic int unsigned get_row_pos(input int unsigned index);
begin
    get_row_pos = 7 + index;
end
endfunction

function automatic int unsigned get_ba_pos(input int unsigned index);
begin
    get_ba_pos = 25 + index;
end
endfunction

function automatic int unsigned get_cid_pos(input int unsigned index);
begin
    get_cid_pos = 30 + index;
end
endfunction

function automatic [COL_BITS-1:0] extract_col_addr(input [63:0] uif_addr);
    integer i;
begin
    extract_col_addr = '0;
    for (i = 0; i < COL_BITS; i = i + 1) begin
        extract_col_addr[i] = uif_addr[get_col_pos(i)];
    end
end
endfunction

function automatic [ROW_BITS-1:0] extract_row_addr(input [63:0] uif_addr);
    integer i;
begin
    extract_row_addr = '0;
    for (i = 0; i < ROW_BITS; i = i + 1) begin
        extract_row_addr[i] = uif_addr[get_row_pos(i)];
    end
end
endfunction

function automatic [BA_BITS-1:0] extract_ba_addr(input [63:0] uif_addr);
    integer i;
begin
    extract_ba_addr = '0;
    for (i = 0; i < BA_BITS; i = i + 1) begin
        extract_ba_addr[i] = uif_addr[get_ba_pos(i)];
    end
end
endfunction

function automatic [CID_BITS-1:0] extract_cid_addr(input [63:0] uif_addr);
    integer i;
begin
    extract_cid_addr = '0;
    for (i = 0; i < CID_BITS; i = i + 1) begin
        extract_cid_addr[i] = uif_addr[get_cid_pos(i)];
    end
end
endfunction

function automatic [63:0] remap_addr(
    input [ADDR_WIDTH-1:0] addr,
    input [7:0] ddr5_id,
    input [1:0] channel_id,
    input [0:0] subchannel_id,
    input [2:0] rank_id
);
    reg [63:0] uif_addr;
    reg [63:0] packed_addr;
    reg [COL_BITS-1:0] col_addr;
    reg [ROW_BITS-1:0] row_addr;
    reg [BA_BITS-1:0] ba_addr;
    reg [CID_BITS-1:0] cid_addr;
begin
    uif_addr = '0;
    packed_addr = '0;

    // One mirror-memory entry stores one DATA_WIDTH beat.
    if (BEAT_LSB < ADDR_WIDTH) begin
        uif_addr[ADDR_WIDTH - BEAT_LSB - 1:0] = addr[ADDR_WIDTH-1:BEAT_LSB];
    end

    col_addr = extract_col_addr(uif_addr);
    row_addr = extract_row_addr(uif_addr);
    ba_addr = extract_ba_addr(uif_addr);
    cid_addr = extract_cid_addr(uif_addr);

    packed_addr[63:56] = ddr5_id;
    packed_addr[55:54] = channel_id;
    packed_addr[53] = subchannel_id;
    packed_addr[52:50] = rank_id;
    packed_addr[49:33] = uif_addr[49:33];
    packed_addr[32:30] = cid_addr;
    packed_addr[29:25] = ba_addr;
    packed_addr[24:7] = row_addr;
    packed_addr[6:0] = col_addr;

    remap_addr = packed_addr;
end
endfunction

function automatic [DATA_WIDTH-1:0] remap_data(
    input [DATA_WIDTH-1:0] data,
    input [7:0] ddr5_id,
    input [1:0] channel_id,
    input [0:0] subchannel_id,
    input [2:0] rank_id
);
begin
    remap_data = data;
end
endfunction

function automatic [DATA_BYTE_WIDTH-1:0] remap_wmask(
    input [DATA_BYTE_WIDTH-1:0] mask,
    input [7:0] ddr5_id,
    input [1:0] channel_id,
    input [0:0] subchannel_id,
    input [2:0] rank_id
);
begin
    remap_wmask = mask;
end
endfunction
