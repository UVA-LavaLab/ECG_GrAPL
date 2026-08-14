module reuse_plan_recency_rank_update #(
    parameter int WAYS = 16,
    parameter int RANK_BITS = $clog2(WAYS),
    parameter int INDEX_BITS = $clog2(WAYS)
) (
    input  logic access_i,
    input  logic [INDEX_BITS-1:0] accessed_way_i,
    input  logic [WAYS*RANK_BITS-1:0] rank_i,
    output logic [WAYS*RANK_BITS-1:0] rank_o
);
    localparam logic [RANK_BITS-1:0] NEWEST_RANK =
        RANK_BITS'(WAYS - 1);
    logic [RANK_BITS-1:0] accessed_rank;

    always_comb begin
        rank_o = rank_i;
        accessed_rank =
            rank_i[accessed_way_i*RANK_BITS +: RANK_BITS];
        if (access_i) begin
            for (integer way = 0; way < WAYS; way = way + 1) begin
                if (INDEX_BITS'(way) == accessed_way_i)
                    rank_o[way*RANK_BITS +: RANK_BITS] = NEWEST_RANK;
                else if (
                        rank_i[way*RANK_BITS +: RANK_BITS] >
                        accessed_rank)
                    rank_o[way*RANK_BITS +: RANK_BITS] =
                        rank_i[way*RANK_BITS +: RANK_BITS] - 1'b1;
            end
        end
    end
endmodule


module reuse_plan_recency_rank_state #(
    parameter int WAYS = 16,
    parameter int RANK_BITS = $clog2(WAYS),
    parameter int INDEX_BITS = $clog2(WAYS)
) (
    input  logic clk_i,
    input  logic reset_i,
    input  logic access_i,
    input  logic [INDEX_BITS-1:0] accessed_way_i,
    output logic [WAYS*RANK_BITS-1:0] rank_o
);
    logic [WAYS*RANK_BITS-1:0] next_rank;

    reuse_plan_recency_rank_update #(
        .WAYS(WAYS),
        .RANK_BITS(RANK_BITS),
        .INDEX_BITS(INDEX_BITS)
    ) update_logic (
        .access_i(access_i),
        .accessed_way_i(accessed_way_i),
        .rank_i(rank_o),
        .rank_o(next_rank)
    );

    always_ff @(posedge clk_i) begin
        if (reset_i) begin
            for (integer way = 0; way < WAYS; way = way + 1)
                rank_o[way*RANK_BITS +: RANK_BITS] <= RANK_BITS'(way);
        end else if (access_i) begin
            rank_o <= next_rank;
        end
    end
endmodule
