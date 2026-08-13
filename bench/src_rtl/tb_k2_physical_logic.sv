module tb_k2_physical_logic;
    localparam int WAYS = 4;
    localparam int RRPV_BITS = 3;
    localparam int RECENCY_BITS = 2;
    localparam int TIER_BITS = 2;
    localparam int DIST_BITS = 15;

    logic [2:0] variant;
    logic [WAYS-1:0] valid;
    logic [WAYS-1:0] property_line;
    logic [WAYS-1:0] stamped;
    logic [WAYS*RRPV_BITS-1:0] rrpv;
    logic [WAYS*RECENCY_BITS-1:0] recency;
    logic [WAYS*TIER_BITS-1:0] tier;
    logic [WAYS*DIST_BITS-1:0] distance;
    logic [$clog2(WAYS)-1:0] victim;
    logic [WAYS*RRPV_BITS-1:0] aged_rrpv;
    logic invalid_victim;

    logic [48:0] ecc_data;
    logic [55:0] ecc_code;
    logic [55:0] corrupted;
    logic [48:0] corrected_data;
    logic single_error;
    logic double_error;

    k2_victim_select #(
        .WAYS(WAYS),
        .RRPV_BITS(RRPV_BITS),
        .RECENCY_BITS(RECENCY_BITS),
        .TIER_BITS(TIER_BITS),
        .DIST_BITS(DIST_BITS)
    ) selector (
        .variant_i(variant),
        .valid_i(valid),
        .property_i(property_line),
        .stamped_i(stamped),
        .rrpv_i(rrpv),
        .recency_i(recency),
        .tier_i(tier),
        .distance_i(distance),
        .victim_o(victim),
        .aged_rrpv_o(aged_rrpv),
        .invalid_victim_o(invalid_victim)
    );

    k2_secded_49_encode encoder (
        .data_i(ecc_data),
        .code_o(ecc_code)
    );
    k2_secded_49_decode decoder (
        .code_i(corrupted),
        .data_o(corrected_data),
        .single_error_corrected_o(single_error),
        .double_error_detected_o(double_error)
    );

    task automatic set_way(
        input int way,
        input logic [RRPV_BITS-1:0] way_rrpv,
        input logic [RECENCY_BITS-1:0] way_recency,
        input logic way_property,
        input logic way_stamped,
        input logic [TIER_BITS-1:0] way_tier,
        input logic [DIST_BITS-1:0] way_distance
    );
        rrpv[way*RRPV_BITS +: RRPV_BITS] = way_rrpv;
        recency[way*RECENCY_BITS +: RECENCY_BITS] = way_recency;
        property_line[way] = way_property;
        stamped[way] = way_stamped;
        tier[way*TIER_BITS +: TIER_BITS] = way_tier;
        distance[way*DIST_BITS +: DIST_BITS] = way_distance;
    endtask

    task automatic reset_ways;
        valid = '1;
        property_line = '0;
        stamped = '0;
        rrpv = '0;
        recency = '0;
        tier = '0;
        distance = '0;
    endtask

    initial begin
        reset_ways();
        valid[2] = 1'b0;
        variant = 3'd2;
        #1;
        if (!invalid_victim || victim != 2) $fatal("invalid priority");

        reset_ways();
        set_way(0, 1, 3, 0, 0, 0, 0);
        set_way(1, 2, 2, 0, 0, 0, 0);
        set_way(2, 2, 0, 0, 0, 0, 0);
        set_way(3, 0, 1, 1, 1, 1, 7);
        variant = 3'd2;
        #1;
        if (victim != 2) $fatal("rrip record recency");
        if (aged_rrpv[0 +: 3] != 6 ||
            aged_rrpv[3 +: 3] != 7 ||
            aged_rrpv[6 +: 3] != 7 ||
            aged_rrpv[9 +: 3] != 5) $fatal("rrip aging");

        reset_ways();
        set_way(0, 2, 3, 1, 1, 1, 4);
        set_way(1, 2, 2, 1, 1, 1, 9);
        set_way(2, 1, 0, 1, 1, 3, 20);
        set_way(3, 0, 1, 1, 0, 3, 30);
        variant = 3'd2;
        #1;
        if (victim != 1) $fatal("rrip property distance");

        reset_ways();
        set_way(0, 7, 3, 1, 1, 1, 20);
        set_way(1, 0, 0, 0, 0, 0, 0);
        set_way(2, 7, 2, 0, 0, 0, 0);
        set_way(3, 7, 1, 1, 1, 3, 30);
        variant = 3'd1;
        #1;
        if (victim != 1) $fatal("epoch record recency");

        reset_ways();
        set_way(0, 0, 3, 1, 1, 1, 4);
        set_way(1, 0, 2, 1, 1, 1, 9);
        set_way(2, 0, 0, 1, 1, 1, 7);
        set_way(3, 0, 1, 1, 1, 1, 5);
        variant = 3'd3;
        #1;
        if (victim != 1) $fatal("epoch-only farthest stamped");

        stamped = '0;
        #1;
        if (victim != 2) $fatal("epoch-only recency fallback");

        reset_ways();
        set_way(0, 7, 3, 1, 1, 2, 20);
        set_way(1, 7, 2, 1, 1, 3, 10);
        set_way(2, 7, 0, 1, 1, 3, 10);
        set_way(3, 6, 1, 0, 0, 0, 0);
        variant = 3'd5;
        #1;
        if (victim != 2) $fatal("degree tier distance recency");

        property_line[1] = 1'b0;
        #1;
        if (victim != 1) $fatal("degree records first");

        variant = 3'd0;
        #1;
        if (victim != 0) $fatal("grasp first max rrpv");

        variant = 3'd6;
        #1;
        if (victim != 2) $fatal("lru oldest");

        reset_ways();
        property_line = 4'b1101;
        variant = 3'd4;
        #1;
        if (victim != 1) $fatal("shortcircuit first record");

        property_line = '1;
        stamped = '1;
        distance = '0;
        tier = '0;
        distance[0 +: 15] = 8;
        distance[15 +: 15] = 8;
        tier[0 +: 2] = 1;
        tier[2 +: 2] = 3;
        #1;
        if (victim != 1) $fatal("shortcircuit property tier");

        variant = 3'd7;
        #1;
        if (victim != 0) $fatal("undefined variant defaults to rrip");

        ecc_data = 49'h1A5A_1234_5678;
        #1;
        corrupted = ecc_code;
        #1;
        if (corrected_data != ecc_data || single_error || double_error)
            $fatal("ecc no-error path");

        corrupted = ecc_code ^ (56'b1 << 10);
        #1;
        if (corrected_data != ecc_data || !single_error || double_error)
            $fatal("ecc single correction");

        corrupted = ecc_code ^ (56'b1 << 10) ^ (56'b1 << 11);
        #1;
        if (!double_error) $fatal("ecc double detection");

        corrupted = ecc_code ^ (56'b1 << 55);
        #1;
        if (corrected_data != ecc_data || !single_error || double_error)
            $fatal("ecc overall parity correction");

        $display("K2 physical RTL tests passed");
        $finish;
    end
endmodule
