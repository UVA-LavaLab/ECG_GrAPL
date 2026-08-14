# ECG Next RISC-V Instruction Overlay

This directory contains the tracked gem5 ISA definitions for ECG metadata
delivery. The custom-0 instruction family is organized by role:

- `ecg.plan.load*` acquires a ReusePlan record with ordinary placement;
- `ecg.flow.load*` acquires a record with FlowThrough placement;
- `ecg.bind.load.*` binds a plan to a computed-address property load; and
- `ecg.bind.iload.*` combines indexed address generation and binding.

The compact format is configured once through the ECG record-format CSR.
Instruction decoding widens compact records to the canonical metadata layout
before attaching them to the corresponding memory request.

[`decoder_ecg_extract.isa`](decoder_ecg_extract.isa) is the decoder source and
[`formats/ecg.isa`](formats/ecg.isa) defines its bit fields. `setup_gem5.py`
installs these files into the local gem5 checkout; repository tests verify that
the installed copies match the tracked overlays.
