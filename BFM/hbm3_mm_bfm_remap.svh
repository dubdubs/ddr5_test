function [64-1:0] remap_addr(input [ADDR_WIDTH-1:0] addr,
                                     input int unsigned hbm_id,
                                     input int unsigned channel_id,
                                     input int unsigned pc_id);

    return addr;
endfunction

function [DATA_WIDTH-1:0] remap_data(input [DATA_WIDTH-1:0] data,
                                     input int unsigned hbm_id,
                                     input int unsigned channel_id,
                                     input int unsigned pc_id);
    return data;
endfunction

function [DATA_BYTE_WIDTH-1:0] remap_wmask(input [DATA_BYTE_WIDTH-1:0] wmask,
                                            input int unsigned hbm_id,
                                            input int unsigned channel_id,
                                            input int unsigned pc_id);
    return wmask;
endfunction
