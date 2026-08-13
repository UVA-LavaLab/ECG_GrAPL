module k2_static_replacement_path #(
    parameter int WAYS = 16,
    parameter int ADDR_BITS = 48,
    parameter int EPOCH_REGIONS = 2,
    parameter int EPOCH_BITS = 15,
    parameter int CONTEXT_BITS = 16,
    parameter int RRPV_BITS = 3,
    parameter int RECENCY_BITS = $clog2(WAYS),
    parameter int TIER_BITS = 2,
    parameter int INDEX_BITS = $clog2(WAYS)
) (
    input  logic [2:0] variant_i,
    input  logic [CONTEXT_BITS-1:0] request_context_i,
    input  logic [EPOCH_BITS-1:0] current_epoch_i,
    input  logic [EPOCH_REGIONS-1:0] epoch_region_valid_i,
    input  logic [EPOCH_REGIONS*ADDR_BITS-1:0] epoch_region_base_i,
    input  logic [EPOCH_REGIONS*ADDR_BITS-1:0] epoch_region_upper_i,
    input  logic [WAYS-1:0] valid_i,
    input  logic [WAYS*ADDR_BITS-1:0] line_addr_i,
    input  logic [WAYS-1:0] metadata_valid_i,
    input  logic [WAYS*CONTEXT_BITS-1:0] metadata_context_i,
    input  logic [WAYS*TIER_BITS-1:0] metadata_tier_i,
    input  logic [WAYS*EPOCH_BITS-1:0] metadata_epoch1_i,
    input  logic [WAYS*EPOCH_BITS-1:0] metadata_epoch2_i,
    input  logic [WAYS*RRPV_BITS-1:0] rrpv_i,
    input  logic [WAYS*RECENCY_BITS-1:0] recency_i,
    output logic [INDEX_BITS-1:0] victim_o,
    output logic [WAYS*RRPV_BITS-1:0] aged_rrpv_o,
    output logic invalid_victim_o,
    output logic [WAYS-1:0] property_o,
    output logic [WAYS-1:0] stamped_o,
    output logic [WAYS*EPOCH_BITS-1:0] distance_o
);
    logic [WAYS-1:0] property_bits;
    logic [WAYS-1:0] stamped;
    logic [WAYS*EPOCH_BITS-1:0] distance;
    logic [ADDR_BITS-1:0] line_addr;
    logic [ADDR_BITS-1:0] region_base;
    logic [ADDR_BITS-1:0] region_upper;
    logic [CONTEXT_BITS-1:0] metadata_context;
    logic [EPOCH_BITS-1:0] epoch1;
    logic [EPOCH_BITS-1:0] epoch2;
    logic [EPOCH_BITS-1:0] distance1;
    logic [EPOCH_BITS-1:0] distance2;
    integer way;
    integer region;

    always_comb begin
        property_bits = '0;
        stamped = '0;
        distance = '0;
        for (way = 0; way < WAYS; way = way + 1) begin
            line_addr = line_addr_i[way*ADDR_BITS +: ADDR_BITS];
            for (region = 0; region < EPOCH_REGIONS; region = region + 1) begin
                region_base =
                    epoch_region_base_i[region*ADDR_BITS +: ADDR_BITS];
                region_upper =
                    epoch_region_upper_i[region*ADDR_BITS +: ADDR_BITS];
                if (epoch_region_valid_i[region] &&
                    line_addr >= region_base && line_addr < region_upper)
                    property_bits[way] = 1'b1;
            end
            metadata_context =
                metadata_context_i[way*CONTEXT_BITS +: CONTEXT_BITS];
            stamped[way] = (
                property_bits[way] && metadata_valid_i[way] &&
                request_context_i != '0 &&
                metadata_context == request_context_i);
            epoch1 = metadata_epoch1_i[way*EPOCH_BITS +: EPOCH_BITS];
            epoch2 = metadata_epoch2_i[way*EPOCH_BITS +: EPOCH_BITS];
            distance1 = epoch1 - current_epoch_i;
            distance2 = epoch2 - current_epoch_i;
            distance[way*EPOCH_BITS +: EPOCH_BITS] =
                distance2 < distance1 ? distance2 : distance1;
        end
        property_o = property_bits;
        stamped_o = stamped;
        distance_o = distance;
    end

    k2_victim_select #(
        .WAYS(WAYS),
        .RRPV_BITS(RRPV_BITS),
        .RECENCY_BITS(RECENCY_BITS),
        .TIER_BITS(TIER_BITS),
        .DIST_BITS(EPOCH_BITS),
        .INDEX_BITS(INDEX_BITS)
    ) ranking (
        .variant_i(variant_i),
        .valid_i(valid_i),
        .property_i(property_bits),
        .stamped_i(stamped),
        .rrpv_i(rrpv_i),
        .recency_i(recency_i),
        .tier_i(metadata_tier_i),
        .distance_i(distance),
        .victim_o(victim_o),
        .aged_rrpv_o(aged_rrpv_o),
        .invalid_victim_o(invalid_victim_o)
    );
endmodule


module k2_replacement_path #(
    parameter int WAYS = 16,
    parameter int ADDR_BITS = 48,
    parameter int EPOCH_REGIONS = 2,
    parameter int EPOCH_BITS = 15,
    parameter int CONTEXT_BITS = 16,
    parameter int RRPV_BITS = 3,
    parameter int RECENCY_BITS = $clog2(WAYS),
    parameter int TIER_BITS = 2,
    parameter int SET_INDEX_BITS = 13,
    parameter int INDEX_BITS = $clog2(WAYS)
) (
    input  logic clk_i,
    input  logic reset_i,
    input  logic online_enable_i,
    input  logic record_miss_i,
    input  logic [SET_INDEX_BITS-1:0] set_index_i,
    input  logic [2:0] static_variant_i,
    input  logic [CONTEXT_BITS-1:0] request_context_i,
    input  logic [EPOCH_BITS-1:0] current_epoch_i,
    input  logic [EPOCH_REGIONS-1:0] epoch_region_valid_i,
    input  logic [EPOCH_REGIONS*ADDR_BITS-1:0] epoch_region_base_i,
    input  logic [EPOCH_REGIONS*ADDR_BITS-1:0] epoch_region_upper_i,
    input  logic [WAYS-1:0] valid_i,
    input  logic [WAYS*ADDR_BITS-1:0] line_addr_i,
    input  logic [WAYS-1:0] metadata_valid_i,
    input  logic [WAYS*CONTEXT_BITS-1:0] metadata_context_i,
    input  logic [WAYS*TIER_BITS-1:0] metadata_tier_i,
    input  logic [WAYS*EPOCH_BITS-1:0] metadata_epoch1_i,
    input  logic [WAYS*EPOCH_BITS-1:0] metadata_epoch2_i,
    input  logic [WAYS*RRPV_BITS-1:0] rrpv_i,
    input  logic [WAYS*RECENCY_BITS-1:0] recency_i,
    output logic [INDEX_BITS-1:0] victim_o,
    output logic [WAYS*RRPV_BITS-1:0] aged_rrpv_o,
    output logic invalid_victim_o,
    output logic [2:0] effective_variant_o,
    output logic [2:0] online_winner_arm_o
);
    logic [2:0] online_variant;
    logic [WAYS-1:0] unused_property;
    logic [WAYS-1:0] unused_stamped;
    logic [WAYS*EPOCH_BITS-1:0] unused_distance;

    k2_online_selector #(
        .SET_INDEX_BITS(SET_INDEX_BITS)
    ) online_selector (
        .clk_i(clk_i),
        .reset_i(reset_i),
        .enable_i(online_enable_i),
        .record_miss_i(record_miss_i),
        .set_index_i(set_index_i),
        .variant_o(online_variant),
        .winner_arm_o(online_winner_arm_o)
    );

    always_comb
        effective_variant_o =
            online_enable_i ? online_variant : static_variant_i;

    k2_static_replacement_path #(
        .WAYS(WAYS),
        .ADDR_BITS(ADDR_BITS),
        .EPOCH_REGIONS(EPOCH_REGIONS),
        .EPOCH_BITS(EPOCH_BITS),
        .CONTEXT_BITS(CONTEXT_BITS),
        .RRPV_BITS(RRPV_BITS),
        .RECENCY_BITS(RECENCY_BITS),
        .TIER_BITS(TIER_BITS),
        .INDEX_BITS(INDEX_BITS)
    ) static_path (
        .variant_i(effective_variant_o),
        .request_context_i(request_context_i),
        .current_epoch_i(current_epoch_i),
        .epoch_region_valid_i(epoch_region_valid_i),
        .epoch_region_base_i(epoch_region_base_i),
        .epoch_region_upper_i(epoch_region_upper_i),
        .valid_i(valid_i),
        .line_addr_i(line_addr_i),
        .metadata_valid_i(metadata_valid_i),
        .metadata_context_i(metadata_context_i),
        .metadata_tier_i(metadata_tier_i),
        .metadata_epoch1_i(metadata_epoch1_i),
        .metadata_epoch2_i(metadata_epoch2_i),
        .rrpv_i(rrpv_i),
        .recency_i(recency_i),
        .victim_o(victim_o),
        .aged_rrpv_o(aged_rrpv_o),
        .invalid_victim_o(invalid_victim_o),
        .property_o(unused_property),
        .stamped_o(unused_stamped),
        .distance_o(unused_distance)
    );
endmodule
