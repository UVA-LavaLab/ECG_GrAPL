    .text
    .align 2

    # a0 = pointer to one 64-bit ReusePlan record
    # a1 = base of a uint32_t property array

    .globl baseline_u32_d32
    .type baseline_u32_d32, @function
baseline_u32_d32:
    ld      t0, 0(a0)          # common record load
    slli    t1, t0, 32         # destination extraction (low 32 bits)
    srli    t1, t1, 32
    slli    t1, t1, 2          # address generation: dest * 4
    add     t1, a1, t1         # address generation: base + offset
    lw      a0, 0(t1)          # ordinary property load
    ret
    .size baseline_u32_d32, .-baseline_u32_d32

    .globl reuse_plan_m_u32_d32
    .type reuse_plan_m_u32_d32, @function
reuse_plan_m_u32_d32:
    ld      t0, 0(a0)          # common record load
    slli    t1, t0, 32         # destination extraction (low 32 bits)
    srli    t1, t1, 32
    slli    t1, t1, 2          # address generation: dest * 4
    add     t1, a1, t1         # address generation: base + offset
    .insn r 0x0b, 0x2, 0x18, a0, t1, t0  # mode 6, width 0
    ret
    .size reuse_plan_m_u32_d32, .-reuse_plan_m_u32_d32

    .globl reuse_plan_i_u32_d32
    .type reuse_plan_i_u32_d32, @function
reuse_plan_i_u32_d32:
    ld      t0, 0(a0)          # common record load
    .insn r 0x0b, 0x2, 0x0c, a0, a1, t0  # mode 3, width 0
    ret
    .size reuse_plan_i_u32_d32, .-reuse_plan_i_u32_d32
