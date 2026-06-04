#pragma once
#include "GuestMemory.hpp"
#include "EmuCallbacks.hpp"
#include <cstring>

namespace HLE::SoftFloat {

    inline void RegisterAll(GuestMemory& memory, EmuCallbacks& callback) {
        // Helper to patch a function entry with an SVC
        auto patchArm = [&](uint32_t addr, uint32_t swi) {
            memory.Write32(memory.CODE_BASE + addr, 0xEF000000 | (swi & 0x00FFFFFF));
            memory.Write32(memory.CODE_BASE + addr + 4, 0xE12FFF1E);
        };

        auto patchThumb = [&](uint32_t addr, uint8_t swi) {
            memory.Write16(memory.CODE_BASE + addr, 0xDF00 | swi);
            memory.Write16(memory.CODE_BASE + addr + 2, 0x4770);
        };

        // Assign a base SVC number for soft-float
        const uint16_t ARM_BASE = 0x6969;
        const uint8_t THUMB_BASE = 0x0;

        // ---- Patch the entry points (addresses from your IDA dump) ----
        /*
        EXAMPLES:
        arm:
        patchArm(0x00535FC8, ARM_BASE + 0);
        patchArm(0x00535FBC, ARM_BASE + 1);

        thumb:
        patchThumb(0x00535FC8, THUMB_BASE + 0);
        patchThumb(0x00535FBC, THUMB_BASE + 1);
        */

        // Functions to patch:
        /* __aeabi
        | Func Name     | Seg |  Start  | Length  | Locals   |  Arguments
        __aeabi_uidivmod .text	004EF5F0	00000020			        R	.	.	.	.	.	.	.	.	.
        __aeabi_idivmod	 .text	004EF6EC	00000030			        R	.	.	.	.	.	.	.	.	.
        __aeabi_drsub	   .text	004EF70C	00000008			        R	.	.	.	.	.	.	.	.	.
        __aeabi_dadd	   .text	004EF718	000002AC	0000000C		R	.	.	.	.	.	.	.	.	.
        __aeabi_cdrcmple .text	004EFFCC	0000001C			        R	.	.	.	.	.	.	.	.	.
        __aeabi_cdcmple  .text	004EFFE8	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_dcmpeq   .text	004EFFFC	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_dcmplt	 .text	004F0010	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_dcmple	 .text	004F0024	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_dcmpge	 .text	004F0038	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_dcmpgt	 .text	004F004C	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_frsub	   .text	004F01E8	00000008			        R	.	.	.	.	.	.	.	.	.
        __aeabi_fadd	   .text	004F01F4	00000190			        R	.	.	.	.	.	.	.	.	.
        __aeabi_cfrcmple .text	004F07A4	00000010			        R	.	.	.	.	.	.	.	.	.
        __aeabi_cfcmple	 .text	004F07B4	00000014	00000014		R	.	.	.	.	.	.	.	.	.
        __aeabi_fcmpeq	 .text	004F07C8	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_fcmplt	 .text	004F07DC	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_fcmple	 .text	004F07F0	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_fcmpge	 .text	004F0804	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_fcmpgt	 .text	004F0818	00000014	00000008		R	.	.	.	.	.	.	.	.	.
        __aeabi_ldivmod	 .text	004F091C	00000044	00000010		R	.	.	.	.	.	.	.	.	.
        __aeabi_uldivmod .text	004F0960	0000003C	00000010		R	.	.	.	.	.	.	.	.	.
        __aeabi_ldiv0	   .text	004F099C	00000010	00000008		R	.	.	.	.	.	.	.	.	.

        softfloat:

        __subsf3	       .text	004F01F0	00000004              R	.	.	.	.	.	.	.	.	.
        __mulsf3	       .text	004F0438	00000198              R	.	.	.	.	.	.	.	.	.
        __divsf3	       .text	004F05D0	00000160              R	.	.	.	.	.	.	.	.	.
        __subdf3	       .text	004EF714	00000004              R	.	.	.	.	.	.	.	.	.
        __muldf3	       .text	004EFAC4	00000194	00000010    R	.	.	.	.	.	.	.	.	.
        __divdf3	       .text	004EFD30	0000018C	00000010    R	.	.	.	.	.	.	.	.	.
        __truncdfsf2     .text	004F0148	000000A0              R	.	.	.	.	.	.	.	.	.
        __gtsf2          .text	004F0730	00000008              R	.	.	.	.	.	.	.	.	.
        __ltsf2          .text	004F0738	00000008              R	.	.	.	.	.	.	.	.	.
        __nesf2	         .text	004F0740	00000064              R	.	.	.	.	.	.	.	.	.
        __unordsf2       .text	004F082C	00000038              R	.	.	.	.	.	.	.	.	.
        __extendsfdf2	   .text	004EFA10	00000040              R	.	.	.	.	.	.	.	.	.
        __gtdf2	         .text	004EFF34	00000008              R	.	.	.	.	.	.	.	.	.
        __ltdf2	         .text	004EFF3C	00000008              R	.	.	.	.	.	.	.	.	.
        __nedf2	         .text	004EFF44	00000088              R	.	.	.	.	.	.	.	.	.
        __unorddf2       .text	004F0060	00000038              R	.	.	.	.	.	.	.	.	.
        __fixdfsi	       .text	004F0098	0000005C              R	.	.	.	.	.	.	.	.	.
        __fixunsdfsi     .text	004F00F4	00000054			        R	.	.	.	.	.	.	.	.	.
        __fixsfsi	       .text	004F0864	0000005C			        R	.	.	.	.	.	.	.	.	.
        __fixunssfsi	   .text	004F08C0	00000054			        R	.	.	.	.	.	.	.	.	.
        __fixdfdi	       .text	004F0A2C	00000044	00000010		R	.	.	.	.	.	.	.	.	.
        __fixunsdfdi	   .text	004F0A70	00000064	00000018		R	.	.	.	.	.	.	.	.	.
        __floatunsisf	   .text	004F0384	00000008			        R	.	.	.	.	.	.	.	.	.
        __floatsisf	     .text	004F038C	00000020			        R	.	.	.	.	.	.	.	.	.
        __floatundisf	   .text	004F03AC	00000010			        R	.	.	.	.	.	.	.	.	.
        __floatdisf	     .text	004F03BC	0000007C			        R	.	.	.	.	.	.	.	.	.
        __floatunsidf	   .text	004EF9C4	00000024			        R	.	.	.	.	.	.	.	.	.
        __floatsidf	     .text	004EF9E8	00000028			        R	.	.	.	.	.	.	.	.	.
        __floatundidf	   .text	004EFA50	00000014			        R	.	.	.	.	.	.	.	.	.
        __floatdidf	     .text	004EFA64	00000060			        R	.	.	.	.	.	.	.	.	.
        */
        /*

        EXAMPLE callback:
        callback.AddFastDispatch(ARM_BASE + 0, "softfloat_fadd", [](Dynarmic::A32::Jit* cpu) {
            float a, b;
            std::memcpy(&a, &cpu->Regs()[0], 4);
            std::memcpy(&b, &cpu->Regs()[1], 4);
            float res = a + b;
            std::memcpy(&cpu->Regs()[0], &res, 4);
        });
        */

        // __aeabi_idivmod  -- ARM, 0x004EF6EC
        patchArm(0x004EF6EC, ARM_BASE + 0);
        callback.AddFastDispatch(ARM_BASE + 0, "__aeabi_idivmod", [](Dynarmic::A32::Jit* cpu) {
            int32_t num = static_cast<int32_t>(cpu->Regs()[0]);
            int32_t den = static_cast<int32_t>(cpu->Regs()[1]);
            if (den == 0) {
                if      (num > 0) cpu->Regs()[0] = 0x7FFFFFFFu;
                else if (num < 0) cpu->Regs()[0] = 0x80000000u;
                else              cpu->Regs()[0] = 0u;
                cpu->Regs()[1] = 0u;
                return;
            }
            if (num == INT32_MIN && den == -1) {
                cpu->Regs()[0] = 0x80000000u;
                cpu->Regs()[1] = 0u;
                return;
            }
            cpu->Regs()[0] = static_cast<uint32_t>(num / den);
            cpu->Regs()[1] = static_cast<uint32_t>(num % den);
        });

        // __aeabi_uidivmod -- ARM, 0x004EF5F0
        patchArm(0x004EF5F0, ARM_BASE + 1);
        callback.AddFastDispatch(ARM_BASE + 1, "__aeabi_uidivmod", [](Dynarmic::A32::Jit* cpu) {
            uint32_t num = cpu->Regs()[0];
            uint32_t den = cpu->Regs()[1];
            if (den == 0) {
                cpu->Regs()[0] = (num != 0) ? 0xFFFFFFFFu : 0u;
                cpu->Regs()[1] = 0u;
                return;
            }
            cpu->Regs()[0] = num / den;
            cpu->Regs()[1] = num % den;
        });
    }
}
