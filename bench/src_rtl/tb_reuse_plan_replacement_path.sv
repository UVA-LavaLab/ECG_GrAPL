module tb_reuse_plan_replacement_path;
    localparam int WAYS = 4;
    localparam int ADDR_BITS = 16;
    localparam int EPOCH_BITS = 4;
    localparam int CONTEXT_BITS = 4;
    localparam int RECENCY_BITS = 2;

    logic clk = 1'b0;
    logic reset;
    logic online_enable;
    logic record_miss;
    logic [5:0] set_index;
    logic [2:0] online_variant;
    logic [2:0] winner_arm;
    logic [1:0] top_victim;
    logic [WAYS*3-1:0] top_aged_rrpv;
    logic top_invalid;
    logic [2:0] top_effective_variant;
    logic [2:0] top_winner;

    logic [2:0] variant;
    logic [CONTEXT_BITS-1:0] request_context;
    logic [EPOCH_BITS-1:0] current_epoch;
    logic region_valid;
    logic [ADDR_BITS-1:0] region_base;
    logic [ADDR_BITS-1:0] region_upper;
    logic [WAYS-1:0] valid;
    logic [WAYS*ADDR_BITS-1:0] line_addr;
    logic [WAYS-1:0] metadata_valid;
    logic [WAYS*CONTEXT_BITS-1:0] metadata_context;
    logic [WAYS*2-1:0] metadata_tier;
    logic [WAYS*EPOCH_BITS-1:0] metadata_epoch1;
    logic [WAYS*EPOCH_BITS-1:0] metadata_epoch2;
    logic [WAYS*3-1:0] rrpv;
    logic [WAYS*RECENCY_BITS-1:0] recency;
    logic [$clog2(WAYS)-1:0] victim;
    logic [WAYS*3-1:0] aged_rrpv;
    logic invalid_victim;
    logic [WAYS-1:0] property_line;
    logic [WAYS-1:0] stamped;
    logic [WAYS*EPOCH_BITS-1:0] distance;

    always #1 clk <= ~clk;

    reuse_plan_online_selector #(
        .SET_INDEX_BITS(6)
    ) online (
        .clk_i(clk),
        .reset_i(reset),
        .enable_i(online_enable),
        .record_miss_i(record_miss),
        .set_index_i(set_index),
        .variant_o(online_variant),
        .winner_arm_o(winner_arm)
    );

    reuse_plan_static_replacement_path #(
        .WAYS(WAYS),
        .ADDR_BITS(ADDR_BITS),
        .EPOCH_REGIONS(1),
        .EPOCH_BITS(EPOCH_BITS),
        .CONTEXT_BITS(CONTEXT_BITS),
        .RECENCY_BITS(RECENCY_BITS)
    ) static_path (
        .variant_i(variant),
        .request_context_i(request_context),
        .current_epoch_i(current_epoch),
        .epoch_region_valid_i(region_valid),
        .epoch_region_base_i(region_base),
        .epoch_region_upper_i(region_upper),
        .valid_i(valid),
        .line_addr_i(line_addr),
        .metadata_valid_i(metadata_valid),
        .metadata_context_i(metadata_context),
        .metadata_tier_i(metadata_tier),
        .metadata_epoch1_i(metadata_epoch1),
        .metadata_epoch2_i(metadata_epoch2),
        .rrpv_i(rrpv),
        .recency_i(recency),
        .victim_o(victim),
        .aged_rrpv_o(aged_rrpv),
        .invalid_victim_o(invalid_victim),
        .property_o(property_line),
        .stamped_o(stamped),
        .distance_o(distance)
    );

    reuse_plan_replacement_path #(
        .WAYS(WAYS),
        .ADDR_BITS(ADDR_BITS),
        .EPOCH_REGIONS(1),
        .EPOCH_BITS(EPOCH_BITS),
        .CONTEXT_BITS(CONTEXT_BITS),
        .RECENCY_BITS(RECENCY_BITS),
        .SET_INDEX_BITS(6)
    ) complete_path (
        .clk_i(clk),
        .reset_i(reset),
        .online_enable_i(online_enable),
        .record_miss_i(record_miss),
        .set_index_i(set_index),
        .static_variant_i(variant),
        .request_context_i(request_context),
        .current_epoch_i(current_epoch),
        .epoch_region_valid_i(region_valid),
        .epoch_region_base_i(region_base),
        .epoch_region_upper_i(region_upper),
        .valid_i(valid),
        .line_addr_i(line_addr),
        .metadata_valid_i(metadata_valid),
        .metadata_context_i(metadata_context),
        .metadata_tier_i(metadata_tier),
        .metadata_epoch1_i(metadata_epoch1),
        .metadata_epoch2_i(metadata_epoch2),
        .rrpv_i(rrpv),
        .recency_i(recency),
        .victim_o(top_victim),
        .aged_rrpv_o(top_aged_rrpv),
        .invalid_victim_o(top_invalid),
        .effective_variant_o(top_effective_variant),
        .online_winner_arm_o(top_winner)
    );

    integer way;
    integer miss;
    initial begin
        variant = 3'd1;
        online_enable = 1'b0;
        record_miss = 1'b0;
        set_index = 6'd10;
        request_context = 4'd7;
        current_epoch = 4'd14;
        region_valid = 1'b1;
        region_base = 16'd100;
        region_upper = 16'd200;
        valid = '1;
        metadata_valid = '1;
        metadata_tier = '0;
        rrpv = '0;
        recency = '0;
        for (way = 0; way < WAYS; way = way + 1) begin
            line_addr[way*ADDR_BITS +: ADDR_BITS] =
                ADDR_BITS'(110 + way);
            metadata_context[way*CONTEXT_BITS +: CONTEXT_BITS] = 4'd7;
        end
        metadata_epoch1[0 +: 4] = 4'd15;
        metadata_epoch2[0 +: 4] = 4'd0;
        metadata_epoch1[4 +: 4] = 4'd2;
        metadata_epoch2[4 +: 4] = 4'd3;
        metadata_epoch1[8 +: 4] = 4'd8;
        metadata_epoch2[8 +: 4] = 4'd9;
        metadata_epoch1[12 +: 4] = 4'd0;
        metadata_epoch2[12 +: 4] = 4'd1;
        #1;
        if (victim != 2) $fatal("static epoch distance");
        if (distance[0 +: 4] != 1 || distance[8 +: 4] != 10)
            $fatal("circular epoch distance");
        if (property_line != 4'b1111 || stamped != 4'b1111)
            $fatal("property/context qualification");
        if (top_effective_variant != 3'd1 || top_victim != victim)
            $fatal("complete static variant mux");

        metadata_context[2*CONTEXT_BITS +: CONTEXT_BITS] = 4'd6;
        #1;
        if (stamped[2] || victim != 1) $fatal("context mismatch fallback");

        line_addr[3*ADDR_BITS +: ADDR_BITS] = 16'd300;
        #1;
        if (property_line[3] || stamped[3] || victim != 3)
            $fatal("record qualification");

        reset = 1'b1;
        online_enable = 1'b1;
        @(posedge clk);
        @(negedge clk);
        reset = 1'b0;
        #1;
        if (online_variant != 3'd2 || winner_arm != 0)
            $fatal("online default RRIP");

        for (miss = 0; miss < 1024; miss = miss + 1) begin
            @(negedge clk);
            set_index = 6'd0;
            record_miss = 1'b1;
            @(posedge clk);
        end
        @(negedge clk);
        record_miss = 1'b0;
        set_index = 6'd10;
        #1;
        if (winner_arm != 1 || online_variant != 3'd0)
            $fatal("online window winner");
        if (top_winner != 1 || top_effective_variant != 3'd0)
            $fatal("complete online variant mux");

        set_index = 6'd3;
        #1;
        if (online_variant != 3'd5)
            $fatal("online leader mapping");

        $display("ReusePlan replacement path tests passed");
        $finish;
    end
endmodule
