#pragma once
#include "GuestMemory.hpp"
#include "EmuCallbacks.hpp"
#include <cstring>

namespace HLE::SoftFloat {

    inline void RegisterAll(GuestMemory& memory, EmuCallbacks& callback) {
        // Helper to patch a function entry with an SVC
        auto patchArm = [&](uint32_t addr, uint32_t swi) {
            memory.Write32(addr, 0xEF000000 | (swi & 0x00FFFFFF));   // ARM SVC
            memory.Write32(addr + 4, 0xE12FFF1E);     // BX LR
        };

        auto patchThumb = [&](uint32_t addr, uint8_t swi) {
            memory.Write16(addr, 0xDF00 | swi);   // THUMB SVC
            memory.Write16(addr + 2, 0x4770);     // BX LR (Thumb)
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
        __aeabi_uidivmod .text	005357D4	00000016              R	.	.	.	.	.	.	.	.	.
        __aeabi_idivmod	 .text	00535884	00000012              R	.	.	.	.	.	.	.	.	.
        __aeabi_cdrcmple .text	00535DA0	0000001C              R	.	.	.	.	.	.	.	.	.
        __aeabi_cdcmple	 .text	00535DBC	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_dcmpeq	 .text	00535DD0	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_dcmplt	 .text	00535DE4	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_dcmple	 .text	00535DF8	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_dcmpge	 .text	00535E0C	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_dcmpgt	 .text	00535E20	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_frsub	   .text	00535FBC	00000008			        R	.	.	.	.	.	.	.	.	.
        __aeabi_fadd     .text	00535FC8	00000190			        R	.	.	.	.	.	.	.	.	.
        __aeabi_cfrcmple .text	00536578	00000010              R	.	.	.	.	.	.	.	.	.
        __aeabi_cfcmple	 .text	00536588	00000014	00000014    R	.	.	.	.	.	.	.	.	.
        __aeabi_fcmpeq	 .text	0053659C	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_fcmplt	 .text	005365B0	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_fcmple	 .text	005365C4	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_fcmpge	 .text	005365D8	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_fcmpgt	 .text	005365EC	00000014	00000008    R	.	.	.	.	.	.	.	.	.
        __aeabi_ldivmod	 .text	005366F0	00000044	00000010    R	.	.	.	.	.	.	.	.	.
        __aeabi_uldivmod .text	00536734	0000003C	00000010    R	.	.	.	.	.	.	.	.	.
        __aeabi_ldiv0	   .text	00536770	00000010	00000008    R	.	.	.	.	.	.	.	.	.

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
    }
}
