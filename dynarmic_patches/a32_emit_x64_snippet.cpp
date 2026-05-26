// =============================================================================
// Instructions for patching src/dynarmic/backend/x64/a32_emit_x64.cpp
// =============================================================================
//
// TWO changes to make in a32_emit_x64.cpp:
//
//   1. Near the top of the file, add one #include after the existing A32
//      headers. Look for this group (around line ~28):
//
//          #include "dynarmic/frontend/A32/a32_location_descriptor.h"
//          #include "dynarmic/frontend/A32/a32_types.h"
//          #include "dynarmic/interface/A32/coprocessor.h"
//
//      Add this line in that block (order within the block doesn't matter,
//      but next to the existing A32 interface include is tidiest):
//
//          #include "dynarmic/interface/A32/config.h"
//
//   2. REPLACE the entire EmitA32CallSupervisor function (the existing one
//      starts at ~line 739 and ends at ~line 760) with the function body
//      below. Everything else in the file stays as-is.
//
// =============================================================================


void A32EmitX64::EmitA32CallSupervisor(A32EmitContext& ctx, IR::Inst* inst) {
    code.SwitchMxcsrOnExit();

    if (conf.enable_cycle_counting) {
        ctx.reg_alloc.HostCall(nullptr);
        code.mov(code.ABI_PARAM2, qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)]);
        code.sub(code.ABI_PARAM2, qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]);
        Devirtualize<&A32::UserCallbacks::AddTicks>(conf.callbacks).EmitCall(code);
        ctx.reg_alloc.EndOfAllocScope();
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[0].IsImmediate());
    const u32 imm = args[0].GetImmediateU32();

    auto fast_call = conf.callbacks->GetSupervisorFastCall(imm);
    if (fast_call) {
        ASSERT(fast_call->integer_params <= ABI_PARAM_COUNT);

        ctx.reg_alloc.HostCall(nullptr);

        for (std::uint32_t i = 0; i < fast_call->integer_params; i++) {
            // ARM register r[i] -> x64 ABI param. 32-bit load auto-zero-extends
            // to 64 by x86_64 semantics, which is the right thing for uint32_t
            // args; signed int32_t args end up zero-extended too but the C ABI
            // only looks at the low 32 anyway so it's a no-op for the callee.
            const auto addr = dword[r15 + offsetof(A32JitState, Reg) + sizeof(u32) * static_cast<size_t>(i)];
            code.mov(code.ABI_PARAMS[i].cvt32(), addr);
        }

        code.CallFunction(fast_call->function);

        using ReturnType = Dynarmic::A32::UserCallbacks::SupervisorFastCall::ReturnType;
        switch (fast_call->return_type) {
        case ReturnType::Integer32: {
            const auto r0 = dword[r15 + offsetof(A32JitState, Reg) + sizeof(u32) * 0];
            code.mov(r0, code.ABI_RETURN.cvt32());
        } break;

        case ReturnType::Integer64: {
            // AAPCS: low half in r0, high half in r1.
            const auto r0 = dword[r15 + offsetof(A32JitState, Reg) + sizeof(u32) * 0];
            const auto r1 = dword[r15 + offsetof(A32JitState, Reg) + sizeof(u32) * 1];
            code.mov(r0, code.ABI_RETURN.cvt32());
            code.shr(code.ABI_RETURN, 32);
            code.mov(r1, code.ABI_RETURN.cvt32());
        } break;

        case ReturnType::Void:
            break;
        }

        ctx.reg_alloc.EndOfAllocScope();
    } else {
        ctx.reg_alloc.HostCall(nullptr, {}, args[0]);
        Devirtualize<&A32::UserCallbacks::CallSVC>(conf.callbacks).EmitCall(code);
    }

    if (conf.enable_cycle_counting) {
        Devirtualize<&A32::UserCallbacks::GetTicksRemaining>(conf.callbacks).EmitCall(code);
        code.mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], code.ABI_RETURN);
        code.mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], code.ABI_RETURN);
        code.SwitchMxcsrOnEntry();
    }
}
