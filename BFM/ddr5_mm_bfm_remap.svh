// Identity remap for the abstract DDR5 memory-model interface.
//
// The DDR controller and digital PHY have already translated DDR5 protocol
// details into simple memory-mapped read/write accesses before this BFM sees
// them.  Keep address/data/mask untouched here; use BFM_ID to distinguish
// logical DDR5 storage instances on the DPI side.
function automatic [63:0] remap_addr(
    input [ADDR_WIDTH-1:0] addr,
    input [7:0] ddr5_id,
    input [1:0] channel_id,
    input [0:0] subchannel_id,
    input [2:0] rank_id
);
    reg [63:0] normalized_addr;
begin
    normalized_addr = '0;
    normalized_addr[ADDR_WIDTH-1:0] = addr;
    remap_addr = normalized_addr;
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
