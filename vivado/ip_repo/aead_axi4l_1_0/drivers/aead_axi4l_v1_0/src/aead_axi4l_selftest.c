#include "aead_axi4l.h"
#include "xil_io.h"

/* Idle-only register test. Preserve configuration and never write commands or
 * read-only STATUS/OUTPUT registers as the wizard's generic self-test did.
 * The caller must own the peripheral exclusively for the duration of this test.
 */
XStatus AEAD_AXI4L_Reg_SelfTest(void *baseaddr_p)
{
    UINTPTR base = (UINTPTR)baseaddr_p;
    u32 saved[12];
    u32 offsets[12];
    u32 index;
    u32 status;
    XStatus result = XST_SUCCESS;

    status = AEAD_AXI4L_mReadReg(base, AEAD_AXI4L_STATUS_OFFSET);
    if ((status & AEAD_AXI4L_START_READY) == 0U) {
        return XST_FAILURE;
    }
    for (index = 0U; index < 11U; ++index) {
        offsets[index] = AEAD_AXI4L_KEY0_OFFSET + 4U * index;
    }
    offsets[11] = AEAD_AXI4L_INPUT_DATA_OFFSET;
    for (index = 0U; index < 12U; ++index) {
        saved[index] = AEAD_AXI4L_mReadReg(base, offsets[index]);
        AEAD_AXI4L_mWriteReg(base, offsets[index], 0xA55A0000U ^ index);
        if (AEAD_AXI4L_mReadReg(base, offsets[index]) != (0xA55A0000U ^ index)) {
            result = XST_FAILURE;
        }
    }
    for (index = 0U; index < 12U; ++index) {
        AEAD_AXI4L_mWriteReg(base, offsets[index], saved[index]);
    }
    return result;
}
