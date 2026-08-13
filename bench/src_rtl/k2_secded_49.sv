module k2_secded_49_encode (
    input  logic [48:0] data_i,
    output logic [55:0] code_o
);
    logic [54:0] hamming;
    logic parity;
    integer pos;
    integer parity_index;
    integer data_index;

    always_comb begin
        hamming = '0;
        data_index = 0;
        for (pos = 1; pos <= 55; pos = pos + 1) begin
            if ((pos & (pos - 1)) != 0) begin
                hamming[pos - 1] = data_i[data_index];
                data_index = data_index + 1;
            end
        end
        for (parity_index = 0; parity_index < 6;
             parity_index = parity_index + 1) begin
            parity = 1'b0;
            for (pos = 1; pos <= 55; pos = pos + 1)
                if ((pos & (1 << parity_index)) != 0)
                    parity = parity ^ hamming[pos - 1];
            hamming[(1 << parity_index) - 1] = parity;
        end
        code_o[54:0] = hamming;
        code_o[55] = ^hamming;
    end
endmodule


module k2_secded_49_decode (
    input  logic [55:0] code_i,
    output logic [48:0] data_o,
    output logic single_error_corrected_o,
    output logic double_error_detected_o
);
    logic [54:0] corrected;
    logic [5:0] syndrome;
    logic parity;
    logic overall_mismatch;
    integer pos;
    integer parity_index;
    integer data_index;

    always_comb begin
        corrected = code_i[54:0];
        syndrome = '0;
        for (parity_index = 0; parity_index < 6;
             parity_index = parity_index + 1) begin
            parity = 1'b0;
            for (pos = 1; pos <= 55; pos = pos + 1)
                if ((pos & (1 << parity_index)) != 0)
                    parity = parity ^ code_i[pos - 1];
            syndrome[parity_index] = parity;
        end
        overall_mismatch = ^code_i;
        single_error_corrected_o = 1'b0;
        double_error_detected_o = 1'b0;
        if (overall_mismatch && syndrome != 0) begin
            corrected[syndrome - 1'b1] =
                ~corrected[syndrome - 1'b1];
            single_error_corrected_o = 1'b1;
        end else if (overall_mismatch) begin
            single_error_corrected_o = 1'b1;
        end else if (syndrome != 0) begin
            double_error_detected_o = 1'b1;
        end

        data_o = '0;
        data_index = 0;
        for (pos = 1; pos <= 55; pos = pos + 1) begin
            if ((pos & (pos - 1)) != 0) begin
                data_o[data_index] = corrected[pos - 1];
                data_index = data_index + 1;
            end
        end
    end
endmodule


module k2_secded_49_parallel16 (
    input  logic [16*49-1:0] data_i,
    input  logic [16*56-1:0] code_i,
    output logic [16*56-1:0] encoded_o,
    output logic [16*49-1:0] decoded_o,
    output logic [15:0] single_error_corrected_o,
    output logic [15:0] double_error_detected_o
);
    genvar way;
    generate
        for (way = 0; way < 16; way = way + 1) begin : codecs
            k2_secded_49_encode encoder (
                .data_i(data_i[way*49 +: 49]),
                .code_o(encoded_o[way*56 +: 56])
            );
            k2_secded_49_decode decoder (
                .code_i(code_i[way*56 +: 56]),
                .data_o(decoded_o[way*49 +: 49]),
                .single_error_corrected_o(single_error_corrected_o[way]),
                .double_error_detected_o(double_error_detected_o[way])
            );
        end
    endgenerate
endmodule
