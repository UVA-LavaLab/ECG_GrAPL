#ifndef __ARCH_RISCV_ECG_RECORD_HH__
#define __ARCH_RISCV_ECG_RECORD_HH__

#include "arch/riscv/regs/misc.hh"
#include "cpu/thread_context.hh"
#include "mem/cache/replacement_policies/ecg_record_control.hh"
#include "mem/cache/replacement_policies/ecg_record_native.h"

namespace gem5
{
namespace RiscvISA
{

template<typename ExecutionContext>
ecg_record::NativeConfiguration
readEcgRecordConfiguration(ExecutionContext* context)
{
    ecg_record::NativeConfiguration configuration;
    configuration.layout_descriptor =
        context->readMiscReg(MISCREG_ECG_RECORD_LAYOUT);
    configuration.record_base =
        context->readMiscReg(MISCREG_ECG_REF32_BASE);
    configuration.property_base =
        context->readMiscReg(MISCREG_ECG_RECORD_PROPERTY_BASE);
    configuration.record_count =
        context->readMiscReg(MISCREG_ECG_RECORD_COUNT);
    configuration.vertex_count =
        context->readMiscReg(MISCREG_ECG_RECORD_VERTICES);
    configuration.iteration_base =
        context->readMiscReg(MISCREG_ECG_RECORD_ITERATION_BASE);
    configuration.generation =
        context->readMiscReg(MISCREG_ECG_RECORD_GENERATION);
    configuration.context =
        context->readMiscReg(MISCREG_ECG_CONTEXT);
    configuration.control =
        context->readMiscReg(MISCREG_ECG_RECORD_CONTROL);
    configuration.property_descriptor =
        context->readMiscReg(MISCREG_ECG_RECORD_PROPERTY);
    return configuration;
}

template<typename ExecutionContext>
ecg_record::Status
readEcgRecordPending(ExecutionContext* context, uint64_t& pending)
{
    const auto* control = context->tcBase()->getCpuPtr()->ecgRecordControl;
    if (!control)
        return ecg_record::Status::INVALID_LAYOUT;
    pending = control->pendingWork();
    return ecg_record::Status::OK;
}

} // namespace RiscvISA
} // namespace gem5

#endif
