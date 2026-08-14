module reuse_plan_victim_select #(
    parameter int WAYS = 16,
    parameter int RRPV_BITS = 3,
    parameter int RECENCY_BITS = $clog2(WAYS),
    parameter int TIER_BITS = 2,
    parameter int DIST_BITS = 15,
    parameter int INDEX_BITS = $clog2(WAYS)
) (
    input  logic [2:0] variant_i,
    input  logic [WAYS-1:0] valid_i,
    input  logic [WAYS-1:0] property_i,
    input  logic [WAYS-1:0] stamped_i,
    input  logic [WAYS*RRPV_BITS-1:0] rrpv_i,
    input  logic [WAYS*RECENCY_BITS-1:0] recency_i,
    input  logic [WAYS*TIER_BITS-1:0] tier_i,
    input  logic [WAYS*DIST_BITS-1:0] distance_i,
    output logic [INDEX_BITS-1:0] victim_o,
    output logic [WAYS*RRPV_BITS-1:0] aged_rrpv_o,
    output logic invalid_victim_o
);
    localparam logic [2:0] GRASP_ONLY = 3'd0;
    localparam logic [2:0] EPOCH_FIRST = 3'd1;
    localparam logic [2:0] RRIP_FIRST = 3'd2;
    localparam logic [2:0] EPOCH_ONLY = 3'd3;
    localparam logic [2:0] SHORTCIRCUIT = 3'd4;
    localparam logic [2:0] DEGREE_FIRST = 3'd5;
    localparam logic [2:0] LRU_ONLY = 3'd6;
    localparam logic [RRPV_BITS-1:0] RRPV_MAX = {RRPV_BITS{1'b1}};

    logic [RRPV_BITS-1:0] rrpv [0:WAYS-1];
    logic [RECENCY_BITS-1:0] recency [0:WAYS-1];
    logic [TIER_BITS-1:0] tier [0:WAYS-1];
    logic [DIST_BITS-1:0] distance [0:WAYS-1];

    integer i;
    logic found;
    logic [INDEX_BITS-1:0] selected;
    logic [RRPV_BITS-1:0] max_rrpv;
    logic [RRPV_BITS-1:0] age_delta;
    logic [RECENCY_BITS-1:0] best_recency;
    logic [TIER_BITS-1:0] best_tier;
    logic [DIST_BITS-1:0] best_distance;
    logic rrip_variant;

    always_comb begin
        for (i = 0; i < WAYS; i = i + 1) begin
            rrpv[i] = rrpv_i[i*RRPV_BITS +: RRPV_BITS];
            recency[i] = recency_i[i*RECENCY_BITS +: RECENCY_BITS];
            tier[i] = tier_i[i*TIER_BITS +: TIER_BITS];
            distance[i] = distance_i[i*DIST_BITS +: DIST_BITS];
            aged_rrpv_o[i*RRPV_BITS +: RRPV_BITS] = rrpv[i];
        end

        victim_o = '0;
        invalid_victim_o = 1'b0;
        found = 1'b0;
        selected = '0;
        max_rrpv = '0;
        age_delta = '0;
        best_recency = '0;
        best_tier = '0;
        best_distance = '0;
        rrip_variant = (
            variant_i == GRASP_ONLY ||
            variant_i == RRIP_FIRST ||
            variant_i == DEGREE_FIRST ||
            variant_i > LRU_ONLY);

        for (i = 0; i < WAYS; i = i + 1) begin
            if (!found && !valid_i[i]) begin
                found = 1'b1;
                selected = i[INDEX_BITS-1:0];
                invalid_victim_o = 1'b1;
            end
        end

        if (!found) begin
            max_rrpv = rrpv[0];
            for (i = 1; i < WAYS; i = i + 1)
                if (rrpv[i] > max_rrpv) max_rrpv = rrpv[i];

            if (rrip_variant) begin
                age_delta = RRPV_MAX - max_rrpv;
                for (i = 0; i < WAYS; i = i + 1)
                    aged_rrpv_o[i*RRPV_BITS +: RRPV_BITS] =
                        rrpv[i] + age_delta;
            end

            case (variant_i)
                GRASP_ONLY: begin
                    for (i = 0; i < WAYS; i = i + 1)
                        if (!found && rrpv[i] == max_rrpv) begin
                            found = 1'b1;
                            selected = i[INDEX_BITS-1:0];
                        end
                end

                LRU_ONLY: begin
                    found = 1'b1;
                    selected = '0;
                    best_recency = recency[0];
                    for (i = 1; i < WAYS; i = i + 1)
                        if (recency[i] < best_recency) begin
                            selected = i[INDEX_BITS-1:0];
                            best_recency = recency[i];
                        end
                end

                SHORTCIRCUIT: begin
                    for (i = 0; i < WAYS; i = i + 1)
                        if (!found && !property_i[i]) begin
                            found = 1'b1;
                            selected = i[INDEX_BITS-1:0];
                        end
                    if (!found) begin
                        found = 1'b1;
                        selected = '0;
                        best_distance = stamped_i[0] ? distance[0] : '0;
                        best_tier = tier[0];
                        for (i = 1; i < WAYS; i = i + 1)
                            if ((stamped_i[i] ? distance[i] : '0) >
                                    best_distance ||
                                ((stamped_i[i] ? distance[i] : '0) ==
                                    best_distance &&
                                 tier[i] > best_tier)) begin
                                selected = i[INDEX_BITS-1:0];
                                best_distance =
                                    stamped_i[i] ? distance[i] : '0;
                                best_tier = tier[i];
                            end
                    end
                end

                EPOCH_FIRST, EPOCH_ONLY: begin
                    for (i = 0; i < WAYS; i = i + 1)
                        if (!property_i[i] &&
                            (!found || recency[i] < best_recency)) begin
                            found = 1'b1;
                            selected = i[INDEX_BITS-1:0];
                            best_recency = recency[i];
                        end
                    if (!found) begin
                        for (i = 0; i < WAYS; i = i + 1)
                            if (stamped_i[i] &&
                                (!found || distance[i] > best_distance)) begin
                                found = 1'b1;
                                selected = i[INDEX_BITS-1:0];
                                best_distance = distance[i];
                            end
                    end
                    if (!found) begin
                        found = 1'b1;
                        selected = '0;
                        best_recency = recency[0];
                        for (i = 1; i < WAYS; i = i + 1)
                            if (recency[i] < best_recency) begin
                                selected = i[INDEX_BITS-1:0];
                                best_recency = recency[i];
                            end
                    end
                end

                DEGREE_FIRST: begin
                    for (i = 0; i < WAYS; i = i + 1)
                        if (rrpv[i] == max_rrpv && !property_i[i] &&
                            (!found || recency[i] < best_recency)) begin
                            found = 1'b1;
                            selected = i[INDEX_BITS-1:0];
                            best_recency = recency[i];
                        end
                    if (!found) begin
                        for (i = 0; i < WAYS; i = i + 1)
                            if (rrpv[i] == max_rrpv && property_i[i] &&
                                (!found || tier[i] > best_tier ||
                                 (tier[i] == best_tier &&
                                  (stamped_i[i] ? distance[i] : '0) >
                                    best_distance) ||
                                 (tier[i] == best_tier &&
                                  (stamped_i[i] ? distance[i] : '0) ==
                                    best_distance &&
                                  recency[i] < best_recency))) begin
                                found = 1'b1;
                                selected = i[INDEX_BITS-1:0];
                                best_tier = tier[i];
                                best_distance =
                                    stamped_i[i] ? distance[i] : '0;
                                best_recency = recency[i];
                            end
                    end
                end

                default: begin
                    for (i = 0; i < WAYS; i = i + 1)
                        if (rrpv[i] == max_rrpv && !property_i[i] &&
                            (!found || recency[i] < best_recency)) begin
                            found = 1'b1;
                            selected = i[INDEX_BITS-1:0];
                            best_recency = recency[i];
                        end
                    if (!found) begin
                        for (i = 0; i < WAYS; i = i + 1)
                            if (rrpv[i] == max_rrpv && property_i[i] &&
                                (!found ||
                                 (stamped_i[i] ? distance[i] : '0) >
                                    best_distance)) begin
                                found = 1'b1;
                                selected = i[INDEX_BITS-1:0];
                                best_distance =
                                    stamped_i[i] ? distance[i] : '0;
                            end
                    end
                end
            endcase
        end

        victim_o = selected;
    end
endmodule
