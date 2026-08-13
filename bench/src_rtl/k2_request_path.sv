module k2_request_merge #(
    parameter int REQUESTOR_BITS = 16
) (
    input  logic state_saw_target_i,
    input  logic state_saw_k2_i,
    input  logic state_conflicted_i,
    input  logic state_selected_valid_i,
    input  logic [REQUESTOR_BITS-1:0] state_requestor_i,
    input  logic [31:0] state_dest_i,
    input  logic [94:0] state_payload_i,
    input  logic incoming_is_k2_i,
    input  logic incoming_conflicted_i,
    input  logic [REQUESTOR_BITS-1:0] incoming_requestor_i,
    input  logic [31:0] incoming_dest_i,
    input  logic [94:0] incoming_payload_i,
    output logic saw_target_o,
    output logic saw_k2_o,
    output logic conflicted_o,
    output logic selected_valid_o,
    output logic [REQUESTOR_BITS-1:0] requestor_o,
    output logic [31:0] dest_o,
    output logic [94:0] payload_o
);
    logic [15:0] state_context;
    logic [31:0] state_sequence;
    logic [15:0] incoming_context;
    logic [31:0] incoming_sequence;
    logic incoming_valid_context;
    logic same_payload;

    always_comb begin
        state_context = state_payload_i[62:47];
        state_sequence = state_payload_i[94:63];
        incoming_context = incoming_payload_i[62:47];
        incoming_sequence = incoming_payload_i[94:63];
        incoming_valid_context = (
            incoming_context != 16'd0 && !incoming_conflicted_i);
        same_payload = (
            state_dest_i == incoming_dest_i &&
            state_payload_i == incoming_payload_i);

        saw_target_o = 1'b1;
        saw_k2_o = state_saw_k2_i;
        conflicted_o = state_conflicted_i;
        selected_valid_o = state_selected_valid_i;
        requestor_o = state_requestor_i;
        dest_o = state_dest_i;
        payload_o = state_payload_i;

        if (!incoming_is_k2_i) begin
            if (state_saw_k2_i) conflicted_o = 1'b1;
        end else begin
            saw_k2_o = 1'b1;
            if (!state_conflicted_i) begin
                if (!incoming_valid_context) begin
                    selected_valid_o = 1'b1;
                    requestor_o = incoming_requestor_i;
                    dest_o = incoming_dest_i;
                    payload_o = incoming_payload_i;
                    conflicted_o = 1'b1;
                end else if (
                        state_saw_target_i && !state_selected_valid_i) begin
                    selected_valid_o = 1'b1;
                    requestor_o = incoming_requestor_i;
                    dest_o = incoming_dest_i;
                    payload_o = incoming_payload_i;
                    conflicted_o = 1'b1;
                end else if (!state_selected_valid_i) begin
                    selected_valid_o = 1'b1;
                    requestor_o = incoming_requestor_i;
                    dest_o = incoming_dest_i;
                    payload_o = incoming_payload_i;
                end else if (
                        state_requestor_i != incoming_requestor_i ||
                        state_context != incoming_context) begin
                    conflicted_o = 1'b1;
                end else if (incoming_sequence > state_sequence) begin
                    requestor_o = incoming_requestor_i;
                    dest_o = incoming_dest_i;
                    payload_o = incoming_payload_i;
                end else if (
                        incoming_sequence == state_sequence &&
                        !same_payload) begin
                    conflicted_o = 1'b1;
                end
            end
        end
    end
endmodule


module k2_request_state_slot #(
    parameter int REQUESTOR_BITS = 16
) (
    input  logic clk_i,
    input  logic reset_i,
    input  logic clear_i,
    input  logic merge_i,
    input  logic incoming_is_k2_i,
    input  logic incoming_conflicted_i,
    input  logic [REQUESTOR_BITS-1:0] incoming_requestor_i,
    input  logic [31:0] incoming_dest_i,
    input  logic [94:0] incoming_payload_i,
    output logic saw_target_o,
    output logic saw_k2_o,
    output logic conflicted_o,
    output logic selected_valid_o,
    output logic [REQUESTOR_BITS-1:0] requestor_o,
    output logic [31:0] dest_o,
    output logic [94:0] payload_o
);
    logic next_saw_target;
    logic next_saw_k2;
    logic next_conflicted;
    logic next_selected_valid;
    logic [REQUESTOR_BITS-1:0] next_requestor;
    logic [31:0] next_dest;
    logic [94:0] next_payload;

    k2_request_merge #(
        .REQUESTOR_BITS(REQUESTOR_BITS)
    ) merge_logic (
        .state_saw_target_i(saw_target_o),
        .state_saw_k2_i(saw_k2_o),
        .state_conflicted_i(conflicted_o),
        .state_selected_valid_i(selected_valid_o),
        .state_requestor_i(requestor_o),
        .state_dest_i(dest_o),
        .state_payload_i(payload_o),
        .incoming_is_k2_i(incoming_is_k2_i),
        .incoming_conflicted_i(incoming_conflicted_i),
        .incoming_requestor_i(incoming_requestor_i),
        .incoming_dest_i(incoming_dest_i),
        .incoming_payload_i(incoming_payload_i),
        .saw_target_o(next_saw_target),
        .saw_k2_o(next_saw_k2),
        .conflicted_o(next_conflicted),
        .selected_valid_o(next_selected_valid),
        .requestor_o(next_requestor),
        .dest_o(next_dest),
        .payload_o(next_payload)
    );

    always_ff @(posedge clk_i) begin
        if (reset_i || clear_i) begin
            saw_target_o <= 1'b0;
            saw_k2_o <= 1'b0;
            conflicted_o <= 1'b0;
            selected_valid_o <= 1'b0;
            requestor_o <= '0;
            dest_o <= '0;
            payload_o <= '0;
        end else if (merge_i) begin
            saw_target_o <= next_saw_target;
            saw_k2_o <= next_saw_k2;
            conflicted_o <= next_conflicted;
            selected_valid_o <= next_selected_valid;
            requestor_o <= next_requestor;
            dest_o <= next_dest;
            payload_o <= next_payload;
        end
    end
endmodule


module k2_csr_state (
    input  logic clk_i,
    input  logic reset_i,
    input  logic write_epoch_i,
    input  logic write_context_i,
    input  logic [14:0] epoch_i,
    input  logic [15:0] context_i,
    output logic [14:0] epoch_o,
    output logic [15:0] context_o
);
    always_ff @(posedge clk_i) begin
        if (reset_i) begin
            epoch_o <= '0;
            context_o <= '0;
        end else begin
            if (write_epoch_i) epoch_o <= epoch_i;
            if (write_context_i) context_o <= context_i;
        end
    end
endmodule


module k2_sequence_allocator #(
    parameter int LANES = 8,
    parameter int SEQUENCE_BITS = 32
) (
    input  logic clk_i,
    input  logic reset_i,
    input  logic [LANES-1:0] allocate_i,
    output logic [LANES*SEQUENCE_BITS-1:0] sequence_o,
    output logic [SEQUENCE_BITS-1:0] base_sequence_o
);
    logic [SEQUENCE_BITS-1:0] sequence_q;
    logic [SEQUENCE_BITS-1:0] allocated_count;
    logic [SEQUENCE_BITS-1:0] lane_offset;

    always_comb begin
        allocated_count = '0;
        lane_offset = '0;
        for (integer lane = 0; lane < LANES; lane = lane + 1) begin
            sequence_o[lane*SEQUENCE_BITS +: SEQUENCE_BITS] =
                sequence_q + lane_offset;
            if (allocate_i[lane]) begin
                lane_offset = lane_offset + 1'b1;
                allocated_count = allocated_count + 1'b1;
            end
        end
        base_sequence_o = sequence_q;
    end

    always_ff @(posedge clk_i) begin
        if (reset_i)
            sequence_q <= '0;
        else
            sequence_q <= sequence_q + allocated_count;
    end
endmodule


module k2_request_pipeline_stage (
    input  logic clk_i,
    input  logic reset_i,
    input  logic valid_i,
    input  logic [94:0] payload_i,
    output logic valid_o,
    output logic [94:0] payload_o
);
    always_ff @(posedge clk_i) begin
        if (reset_i) begin
            valid_o <= 1'b0;
            payload_o <= '0;
        end else begin
            valid_o <= valid_i;
            if (valid_i) payload_o <= payload_i;
        end
    end
endmodule
