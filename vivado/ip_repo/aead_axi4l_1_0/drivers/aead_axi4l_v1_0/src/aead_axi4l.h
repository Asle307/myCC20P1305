
#ifndef AEAD_AXI4L_H
#define AEAD_AXI4L_H


/****************** Include Files ********************/
#include "xil_types.h"
#include "xstatus.h"
#include "xil_io.h"

#define AEAD_AXI4L_S00_AXI_SLV_REG0_OFFSET 0
#define AEAD_AXI4L_S00_AXI_SLV_REG1_OFFSET 4
#define AEAD_AXI4L_S00_AXI_SLV_REG2_OFFSET 8
#define AEAD_AXI4L_S00_AXI_SLV_REG3_OFFSET 12
#define AEAD_AXI4L_S00_AXI_SLV_REG4_OFFSET 16
#define AEAD_AXI4L_S00_AXI_SLV_REG5_OFFSET 20
#define AEAD_AXI4L_S00_AXI_SLV_REG6_OFFSET 24
#define AEAD_AXI4L_S00_AXI_SLV_REG7_OFFSET 28
#define AEAD_AXI4L_S00_AXI_SLV_REG8_OFFSET 32
#define AEAD_AXI4L_S00_AXI_SLV_REG9_OFFSET 36
#define AEAD_AXI4L_S00_AXI_SLV_REG10_OFFSET 40
#define AEAD_AXI4L_S00_AXI_SLV_REG11_OFFSET 44
#define AEAD_AXI4L_S00_AXI_SLV_REG12_OFFSET 48
#define AEAD_AXI4L_S00_AXI_SLV_REG13_OFFSET 52
#define AEAD_AXI4L_S00_AXI_SLV_REG14_OFFSET 56
#define AEAD_AXI4L_S00_AXI_SLV_REG15_OFFSET 60


/* Functional register map; see docs/interfaces/aead_axi4l.md. */
#define AEAD_AXI4L_CONTROL_OFFSET       0x00U
#define AEAD_AXI4L_STATUS_OFFSET        0x04U
#define AEAD_AXI4L_KEY0_OFFSET          0x08U
#define AEAD_AXI4L_NONCE0_OFFSET        0x28U
#define AEAD_AXI4L_INPUT_DATA_OFFSET    0x34U
#define AEAD_AXI4L_STREAM_CTRL_OFFSET   0x38U
#define AEAD_AXI4L_OUTPUT_META_OFFSET   0x38U
#define AEAD_AXI4L_OUTPUT_DATA_OFFSET   0x3CU

#define AEAD_AXI4L_START                (1U << 0)
#define AEAD_AXI4L_MODE_ENCRYPT         (1U << 1)
#define AEAD_AXI4L_CLEAR_DONE           (1U << 2)
#define AEAD_AXI4L_SOFT_RESET           (1U << 3)
#define AEAD_AXI4L_CLEAR_ERROR          (1U << 4)
#define AEAD_AXI4L_START_READY          (1U << 0)
#define AEAD_AXI4L_BUSY                 (1U << 1)
#define AEAD_AXI4L_DONE                 (1U << 2)
#define AEAD_AXI4L_AUTH_OK              (1U << 3)
#define AEAD_AXI4L_INPUT_FREE           (1U << 4)
#define AEAD_AXI4L_OUTPUT_VALID         (1U << 5)
#define AEAD_AXI4L_ERROR                (1U << 6)
#define AEAD_AXI4L_START_PENDING        (1U << 7)
#define AEAD_AXI4L_BYTE_COUNT_MASK      0x07U
#define AEAD_AXI4L_IS_AAD_OR_TAG        (1U << 8)
#define AEAD_AXI4L_IS_AAD               (1U << 8)
#define AEAD_AXI4L_IS_TAG               (1U << 8)
#define AEAD_AXI4L_LAST                 (1U << 9)
#define AEAD_AXI4L_PUSH                 (1U << 16)
#define AEAD_AXI4L_POP                  (1U << 17)
#define AEAD_AXI4L_META_VALID           (1U << 16)

/**************************** Type Definitions *****************************/
/**
 *
 * Write a value to a AEAD_AXI4L register. A 32 bit write is performed.
 * If the component is implemented in a smaller width, only the least
 * significant data is written.
 *
 * @param   BaseAddress is the base address of the AEAD_AXI4Ldevice.
 * @param   RegOffset is the register offset from the base to write to.
 * @param   Data is the data written to the register.
 *
 * @return  None.
 *
 * @note
 * C-style signature:
 * 	void AEAD_AXI4L_mWriteReg(u32 BaseAddress, unsigned RegOffset, u32 Data)
 *
 */
#define AEAD_AXI4L_mWriteReg(BaseAddress, RegOffset, Data) \
    Xil_Out32((BaseAddress) + (RegOffset), (u32)(Data))

/**
 *
 * Read a value from a AEAD_AXI4L register. A 32 bit read is performed.
 * If the component is implemented in a smaller width, only the least
 * significant data is read from the register. The most significant data
 * will be read as 0.
 *
 * @param   BaseAddress is the base address of the AEAD_AXI4L device.
 * @param   RegOffset is the register offset from the base to write to.
 *
 * @return  Data is the data from the register.
 *
 * @note
 * C-style signature:
 * 	u32 AEAD_AXI4L_mReadReg(u32 BaseAddress, unsigned RegOffset)
 *
 */
#define AEAD_AXI4L_mReadReg(BaseAddress, RegOffset) \
    Xil_In32((BaseAddress) + (RegOffset))

/************************** Function Prototypes ****************************/
/**
 *
 * Run an idle-only key/nonce/input staging readback test. Register values
 * are restored before return. This does not start or reset the AEAD core.
 *
 * If the hardware system is not built correctly, this function may never
 * return to the caller.
 *
 * @param   baseaddr_p is the base address of the AEAD_AXI4L instance to be worked on.
 *
 * @return
 *
 *    - XST_SUCCESS   if all self-test code passed
 *    - XST_FAILURE   if any self-test code failed
 *
 * @note    Caching must be turned off for this function to work.
 * @note    Self test may fail if data memory and device are not on the same bus.
 *
 */
XStatus AEAD_AXI4L_Reg_SelfTest(void * baseaddr_p);

#endif // AEAD_AXI4L_H
