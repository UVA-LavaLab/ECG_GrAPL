module reuse_plan_online_selector #(
    parameter int SET_INDEX_BITS = 13,
    parameter int WINDOW_MISSES = 1024,
    parameter int COUNTER_BITS = $clog2(WINDOW_MISSES + 1)
) (
    input  logic clk_i,
    input  logic reset_i,
    input  logic enable_i,
    input  logic record_miss_i,
    input  logic [SET_INDEX_BITS-1:0] set_index_i,
    output logic [2:0] variant_o,
    output logic [2:0] winner_arm_o
);
    localparam logic [2:0] ARM_RRIP = 3'd0;
    localparam logic [2:0] ARM_GRASP = 3'd1;
    localparam logic [2:0] ARM_EPOCH = 3'd2;
    localparam logic [2:0] ARM_DEGREE = 3'd3;
    localparam logic [2:0] ARM_LRU = 3'd4;
    localparam int SAMPLE_BITS = $clog2(WINDOW_MISSES);
    localparam logic [SAMPLE_BITS-1:0] WINDOW_LAST =
        SAMPLE_BITS'(WINDOW_MISSES - 1);

    logic [COUNTER_BITS-1:0] misses_q [0:4];
    logic [SAMPLE_BITS-1:0] sampled_misses_q;
    logic [2:0] winner_q;
    logic leader_valid;
    logic [2:0] leader_arm;
    logic [2:0] selected_arm;
    logic [COUNTER_BITS-1:0] window_count [0:4];
    logic [COUNTER_BITS-1:0] best_count;
    logic [2:0] best_arm;
    integer comb_arm;
    integer seq_arm;

    function automatic logic [2:0] arm_variant(input logic [2:0] value);
        case (value)
            ARM_GRASP: arm_variant = 3'd0;
            ARM_EPOCH: arm_variant = 3'd1;
            ARM_DEGREE: arm_variant = 3'd5;
            ARM_LRU: arm_variant = 3'd6;
            default: arm_variant = 3'd2;
        endcase
    endfunction

    always_comb begin
        leader_valid = set_index_i[5:0] < 6'd5;
        leader_arm = set_index_i[2:0];
        selected_arm = leader_valid ? leader_arm : winner_q;
        variant_o = arm_variant(selected_arm);
        winner_arm_o = winner_q;

        for (comb_arm = 0; comb_arm < 5; comb_arm = comb_arm + 1)
            window_count[comb_arm] = misses_q[comb_arm];
        if (leader_valid && record_miss_i)
            window_count[leader_arm] = misses_q[leader_arm] + 1'b1;

        best_arm = winner_q;
        best_count = window_count[winner_q];
        for (comb_arm = 0; comb_arm < 5; comb_arm = comb_arm + 1)
            if (window_count[comb_arm] < best_count) begin
                best_arm = comb_arm[2:0];
                best_count = window_count[comb_arm];
            end
    end

    always_ff @(posedge clk_i) begin
        if (reset_i) begin
            for (seq_arm = 0; seq_arm < 5; seq_arm = seq_arm + 1)
                misses_q[seq_arm] <= '0;
            sampled_misses_q <= '0;
            winner_q <= ARM_RRIP;
        end else if (enable_i && record_miss_i && leader_valid) begin
            if (sampled_misses_q == WINDOW_LAST) begin
                for (seq_arm = 0; seq_arm < 5; seq_arm = seq_arm + 1)
                    misses_q[seq_arm] <= '0;
                sampled_misses_q <= '0;
                winner_q <= best_arm;
            end else begin
                misses_q[leader_arm] <= misses_q[leader_arm] + 1'b1;
                sampled_misses_q <= sampled_misses_q + 1'b1;
            end
        end
    end
endmodule
