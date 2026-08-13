module tb_k2_request_path;
    logic clk = 1'b0;
    logic reset;
    logic clear;
    logic merge;
    logic incoming_is_k2;
    logic incoming_conflicted;
    logic [7:0] incoming_requestor;
    logic [31:0] incoming_dest;
    logic [94:0] incoming_payload;
    logic saw_target;
    logic saw_k2;
    logic conflicted;
    logic selected_valid;
    logic [7:0] requestor;
    logic [31:0] dest;
    logic [94:0] payload;

    logic [7:0] rank_i;
    logic [7:0] rank_o;
    logic [7:0] stored_rank;
    logic rank_access;

    logic write_epoch;
    logic write_context;
    logic [14:0] csr_epoch_i;
    logic [15:0] csr_context_i;
    logic [14:0] csr_epoch_o;
    logic [15:0] csr_context_o;

    logic pipeline_valid_i;
    logic [94:0] pipeline_payload_i;
    logic pipeline_valid_o;
    logic [94:0] pipeline_payload_o;

    logic [7:0] sequence_allocate;
    logic [8*32-1:0] allocated_sequences;
    logic [31:0] sequence_base;

    always #1 clk <= ~clk;

    k2_request_state_slot #(
        .REQUESTOR_BITS(8)
    ) slot (
        .clk_i(clk),
        .reset_i(reset),
        .clear_i(clear),
        .merge_i(merge),
        .incoming_is_k2_i(incoming_is_k2),
        .incoming_conflicted_i(incoming_conflicted),
        .incoming_requestor_i(incoming_requestor),
        .incoming_dest_i(incoming_dest),
        .incoming_payload_i(incoming_payload),
        .saw_target_o(saw_target),
        .saw_k2_o(saw_k2),
        .conflicted_o(conflicted),
        .selected_valid_o(selected_valid),
        .requestor_o(requestor),
        .dest_o(dest),
        .payload_o(payload)
    );

    k2_recency_rank_update #(
        .WAYS(4),
        .RANK_BITS(2)
    ) rank_update (
        .access_i(1'b1),
        .accessed_way_i(2'd1),
        .rank_i(rank_i),
        .rank_o(rank_o)
    );

    k2_recency_rank_state #(
        .WAYS(4),
        .RANK_BITS(2)
    ) rank_state (
        .clk_i(clk),
        .reset_i(reset),
        .access_i(rank_access),
        .accessed_way_i(2'd1),
        .rank_o(stored_rank)
    );

    k2_csr_state csr_state (
        .clk_i(clk),
        .reset_i(reset),
        .write_epoch_i(write_epoch),
        .write_context_i(write_context),
        .epoch_i(csr_epoch_i),
        .context_i(csr_context_i),
        .epoch_o(csr_epoch_o),
        .context_o(csr_context_o)
    );

    k2_sequence_allocator allocator (
        .clk_i(clk),
        .reset_i(reset),
        .allocate_i(sequence_allocate),
        .sequence_o(allocated_sequences),
        .base_sequence_o(sequence_base)
    );

    k2_request_pipeline_stage pipeline_stage (
        .clk_i(clk),
        .reset_i(reset),
        .valid_i(pipeline_valid_i),
        .payload_i(pipeline_payload_i),
        .valid_o(pipeline_valid_o),
        .payload_o(pipeline_payload_o)
    );

    function automatic [94:0] make_payload(
        input [1:0] tier,
        input [14:0] epoch1,
        input [14:0] epoch2,
        input [14:0] current_epoch,
        input [15:0] context_value,
        input [31:0] sequence_value
    );
        make_payload = {
            sequence_value, context_value, current_epoch, epoch2, epoch1, tier
        };
    endfunction

    task automatic merge_request(
        input logic is_k2,
        input logic bad_context,
        input logic [7:0] req,
        input logic [31:0] request_dest,
        input logic [94:0] request_payload
    );
        @(negedge clk);
        incoming_is_k2 = is_k2;
        incoming_conflicted = bad_context;
        incoming_requestor = req;
        incoming_dest = request_dest;
        incoming_payload = request_payload;
        merge = 1'b1;
        @(posedge clk);
        @(negedge clk);
        merge = 1'b0;
    endtask

    initial begin
        reset = 1'b1;
        clear = 1'b0;
        merge = 1'b0;
        incoming_is_k2 = 1'b0;
        incoming_conflicted = 1'b0;
        incoming_requestor = '0;
        incoming_dest = '0;
        incoming_payload = '0;
        rank_access = 1'b0;
        write_epoch = 1'b0;
        write_context = 1'b0;
        csr_epoch_i = '0;
        csr_context_i = '0;
        pipeline_valid_i = 1'b0;
        pipeline_payload_i = '0;
        sequence_allocate = '0;
        @(posedge clk);
        @(negedge clk);
        reset = 1'b0;

        merge_request(
            1'b1, 1'b0, 8'd3, 32'd10,
            make_payload(2'd1, 15'd2, 15'd3, 15'd1, 16'd7, 32'd1));
        if (!selected_valid || conflicted || payload[94:63] != 1)
            $fatal("initial K2 selection");

        merge_request(
            1'b1, 1'b0, 8'd3, 32'd11,
            make_payload(2'd2, 15'd4, 15'd5, 15'd2, 16'd7, 32'd2));
        if (conflicted || dest != 11 || payload[94:63] != 2)
            $fatal("newer sequence replacement");

        merge_request(
            1'b1, 1'b0, 8'd3, 32'd12,
            make_payload(2'd3, 15'd4, 15'd5, 15'd2, 16'd7, 32'd2));
        if (!conflicted) $fatal("equal sequence payload conflict");

        clear = 1'b1;
        @(posedge clk);
        @(negedge clk);
        clear = 1'b0;
        merge_request(1'b0, 1'b0, 8'd0, 32'd0, '0);
        merge_request(
            1'b1, 1'b0, 8'd3, 32'd10,
            make_payload(2'd1, 15'd2, 15'd3, 15'd1, 16'd7, 32'd1));
        if (!conflicted) $fatal("ordinary then K2 conflict");

        clear = 1'b1;
        @(posedge clk);
        @(negedge clk);
        clear = 1'b0;
        merge_request(
            1'b1, 1'b1, 8'd3, 32'd10,
            make_payload(2'd1, 15'd2, 15'd3, 15'd1, 16'd0, 32'd1));
        if (!conflicted || !selected_valid)
            $fatal("invalid context conflict");

        rank_i = {2'd3, 2'd2, 2'd1, 2'd0};
        #1;
        if (rank_o != {2'd2, 2'd1, 2'd3, 2'd0})
            $fatal("recency rank update");

        @(negedge clk);
        rank_access = 1'b1;
        write_epoch = 1'b1;
        write_context = 1'b1;
        csr_epoch_i = 15'd9;
        csr_context_i = 16'd12;
        pipeline_valid_i = 1'b1;
        pipeline_payload_i =
            make_payload(2'd2, 15'd3, 15'd4, 15'd5, 16'd12, 32'd20);
        sequence_allocate = 8'b00001011;
        #0;
        if (allocated_sequences[0 +: 32] != 0 ||
            allocated_sequences[32 +: 32] != 1 ||
            allocated_sequences[3*32 +: 32] != 2)
            $fatal("superscalar sequence allocation");
        @(posedge clk);
        @(negedge clk);
        rank_access = 1'b0;
        write_epoch = 1'b0;
        write_context = 1'b0;
        pipeline_valid_i = 1'b0;
        sequence_allocate = '0;
        #1;
        if (stored_rank != {2'd2, 2'd1, 2'd3, 2'd0})
            $fatal("registered recency rank state");
        if (csr_epoch_o != 9 || csr_context_o != 12)
            $fatal("CSR state writes");
        if (!pipeline_valid_o || pipeline_payload_o != pipeline_payload_i)
            $fatal("pipeline payload propagation");
        if (sequence_base != 3)
            $fatal("sequence allocator advance");

        $display("K2 request path tests passed");
        $finish;
    end
endmodule
