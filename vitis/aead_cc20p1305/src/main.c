/* KV260 Cortex-A53 standalone test, RFC 8439 section 2.8.2.
 * PS/DDR/PL clock initialization and FPGA programming belong to the JTAG
 * launch configuration. Run this program as the only owner of the IP.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xparameters.h"
#include "xil_io.h"
#include "xil_mmu.h"
#include "xil_printf.h"
#include "xiltimer.h"

#ifndef XPAR_AEAD_AXI4L_0_BASEADDR
#error "Select the KV260 standalone platform containing aead_axi4l_0"
#endif

#define AEAD_BASE ((UINTPTR)XPAR_AEAD_AXI4L_0_BASEADDR)
#define TIMEOUT_TICKS ((uint64_t)COUNTS_PER_SECOND * 5U)

enum {
    CONTROL = 0x00, STATUS = 0x04, KEY0 = 0x08, NONCE0 = 0x28,
    INPUT_DATA = 0x34, STREAM_CTRL = 0x38, OUTPUT_META = 0x38, OUTPUT_DATA = 0x3c,
    START = 1U << 0, ENCRYPT = 1U << 1, SOFT_RESET = 1U << 3,
    READY = 1U << 0, BUSY = 1U << 1, DONE = 1U << 2, AUTH_OK = 1U << 3,
    INPUT_FREE = 1U << 4, OUTPUT_VALID = 1U << 5, ERROR = 1U << 6,
    IS_AAD = 1U << 8, IS_TAG = 1U << 8, LAST = 1U << 9,
    PUSH = 1U << 16, POP = 1U << 17, META_VALID = 1U << 16
};

/* Debugger watch variables: result -1 running, 0 PASS, 1 FAIL.
 * Set pause_before_start to 1 before run_case(), then clear it in the
 * debugger after arming ILA. The transfer timeout starts after this pause.
 */
volatile int g_aead_result = -1;
volatile unsigned g_aead_test = 0;
volatile uint32_t g_aead_status = 0;
volatile unsigned g_aead_sent = 0;
volatile unsigned g_aead_received = 0;
volatile unsigned g_aead_pause_before_start = 0;

struct test_case {
    const char *name;
    const uint8_t *key, *nonce, *aad, *input, *expected, *tag;
    size_t aad_bytes, payload_bytes;
    bool encrypt, auth_ok;
};

static uint32_t read_register(unsigned offset)
{
    uint32_t value = Xil_In32(AEAD_BASE + offset);
    dsb();
    return value;
}

static void write_register(unsigned offset, uint32_t value)
{
    Xil_Out32(AEAD_BASE + offset, value);
    /* Complete INPUT_DATA before PUSH and output reads before POP. */
    dsb();
}

static XTime now(void)
{
    XTime value;
    XTime_GetTime(&value);
    return value;
}

static bool timed_out(XTime begin)
{
    return (uint64_t)(now() - begin) >= TIMEOUT_TICKS;
}

static size_t words(size_t bytes)
{
    return (bytes + 3U) / 4U;
}

static size_t section_words(size_t bytes)
{
    return bytes == 0U ? 1U : words(bytes);
}

static uint32_t pack_word(const uint8_t *bytes, size_t count)
{
    uint32_t value = 0;
    size_t index;
    for (index = 0; index < count; ++index) {
        value |= (uint32_t)bytes[index] << (8U * index);
    }
    return value;
}

static void input_word(const struct test_case *test, size_t index,
                       uint32_t *data, uint32_t *control)
{
    const uint8_t *bytes;
    size_t length, offset, count;
    *control = 0;
    if (index < section_words(test->aad_bytes)) {
        bytes = test->aad;
        length = test->aad_bytes;
        *control = IS_AAD;
    }
    else {
        index -= section_words(test->aad_bytes);
        if (index < section_words(test->payload_bytes)) {
            bytes = test->input;
            length = test->payload_bytes;
        }
        else {
            index -= section_words(test->payload_bytes);
            bytes = test->tag;
            length = 16;
        }
    }
    offset = index * 4U;
    count = length > offset ? length - offset : 0;
    if (count > 4U) {
        count = 4;
    }
    *data = count == 0U ? 0U : pack_word(bytes + offset, count);
    *control |= PUSH | (uint32_t)count | (offset + count == length ? LAST : 0U);
}

static void expected_word(const struct test_case *test, size_t index,
                          uint32_t *data, uint32_t *metadata)
{
    const uint8_t *bytes = test->expected;
    size_t length = test->payload_bytes, offset, count;
    *metadata = META_VALID;
    if (index >= words(length)) {
        index -= words(length);
        bytes = test->tag;
        length = 16;
        *metadata |= IS_TAG;
    }
    offset = index * 4U;
    count = length - offset;
    if (count > 4U) {
        count = 4;
    }
    *data = pack_word(bytes + offset, count);
    *metadata |= (uint32_t)count | (offset + count == length ? LAST : 0U);
}

static int run_case(const struct test_case *test)
{
    size_t sent = 0, received = 0, index;
    const size_t input_count = section_words(test->aad_bytes) +
        section_words(test->payload_bytes) + (test->encrypt ? 0U : 4U);
    const size_t output_count = words(test->payload_bytes) + (test->encrypt ? 4U : 0U);
    XTime begin = now();
    uint32_t status;
    bool started = false;

    g_aead_sent = 0;
    g_aead_received = 0;
    xil_printf("Running %s\r\n", test->name);
    for (;;) {
        status = read_register(STATUS);
        g_aead_status = status;
        if (status & ERROR) {
            xil_printf("FAIL: ERROR before START\r\n");
            goto fail;
        }
        if (status & READY) {
            break;
        }
        if (timed_out(begin)) {
            xil_printf("FAIL: timeout waiting for START_READY\r\n");
            goto fail;
        }
    }
    for (index = 0; index < 8U; ++index) {
        write_register(KEY0 + (unsigned)index * 4U, pack_word(test->key + index * 4U, 4));
    }
    for (index = 0; index < 3U; ++index) {
        write_register(NONCE0 + (unsigned)index * 4U, pack_word(test->nonce + index * 4U, 4));
    }
    if (g_aead_pause_before_start != 0U) {
        xil_printf("Arm ILA, then set g_aead_pause_before_start=0 in debugger.\r\n");
        while (g_aead_pause_before_start != 0U) {
            /* Debugger-controlled pause; no UART input is required. */
        }
    }
    begin = now();
    write_register(CONTROL, START | (test->encrypt ? ENCRYPT : 0U));
    started = true;
    for (;;) {
        uint32_t data, metadata, expected, expected_metadata;
        status = read_register(STATUS);
        g_aead_status = status;
        if (status & ERROR) {
            xil_printf("FAIL: wrapper ERROR\r\n");
            goto fail;
        }
        /* Drain while feeding: either held slot can stall the native core.
         * Decrypt plaintext is checked privately, never printed or released.
         */
        if (status & OUTPUT_VALID) {
            metadata = read_register(OUTPUT_META);
            data = read_register(OUTPUT_DATA);
            if (received >= output_count) {
                xil_printf("FAIL: extra output word\r\n");
                goto fail;
            }
            expected_word(test, received, &expected, &expected_metadata);
            if (data != expected || metadata != expected_metadata) {
                xil_printf("FAIL: word %u, data %08x/%08x, meta %08x/%08x (actual/expected)\r\n",
                           (unsigned)received, (unsigned)data, (unsigned)expected,
                           (unsigned)metadata, (unsigned)expected_metadata);
                goto fail;
            }
            write_register(STREAM_CTRL, POP);
            ++received;
            g_aead_received = (unsigned)received;
        }
        if ((status & INPUT_FREE) && sent < input_count) {
            input_word(test, sent, &data, &metadata);
            write_register(INPUT_DATA, data);
            write_register(STREAM_CTRL, metadata);
            ++sent;
            g_aead_sent = (unsigned)sent;
        }
        /* DONE may precede software's POP of the final held output. Wait for
         * START_READY as well, then verify sticky completion on another read.
         */
        if ((status & DONE) && sent == input_count && received == output_count) {
            if ((status & BUSY) || ((status & AUTH_OK) != 0U) != test->auth_ok) {
                xil_printf("FAIL: unexpected BUSY or AUTH_OK at DONE\r\n");
                goto fail;
            }
            status = read_register(STATUS);
            g_aead_status = status;
            if ((status & (READY | DONE)) == (READY | DONE) &&
                !(status & (BUSY | OUTPUT_VALID | ERROR)) &&
                ((status & AUTH_OK) != 0U) == test->auth_ok) {
                xil_printf("PASS %s: %u payload bytes, AUTH_OK=%u\r\n",
                           test->name, (unsigned)test->payload_bytes, (unsigned)test->auth_ok);
                return 0;
            }
        }
        if (timed_out(begin)) {
            xil_printf("FAIL: transfer timeout\r\n");
            goto fail;
        }
    }

fail:
    xil_printf("STATUS=%08x, sent=%u/%u, received=%u/%u\r\n",
               (unsigned)g_aead_status, (unsigned)sent, (unsigned)input_count,
               (unsigned)received, (unsigned)output_count);
    if (started) {
        write_register(CONTROL, SOFT_RESET);
    }
    return 1;
}

int main(void)
{
    static const uint8_t nonce[12] = {
        0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47
    };
    static const uint8_t aad[12] = {
        0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7
    };
    static const uint8_t plaintext[] =
        "Ladies and Gentlemen of the class of '99: If I could offer you "
        "only one tip for the future, sunscreen would be it.";
    static const uint8_t ciphertext[] = {
        0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb, 0x7b, 0x86, 0xaf, 0xbc,
        0x53, 0xef, 0x7e, 0xc2, 0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe,
        0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6, 0x3d, 0xbe, 0xa4, 0x5e,
        0x8c, 0xa9, 0x67, 0x12, 0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
        0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29, 0x05, 0xd6, 0xa5, 0xb6,
        0x7e, 0xcd, 0x3b, 0x36, 0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c,
        0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58, 0xfa, 0xb3, 0x24, 0xe4,
        0xfa, 0xd6, 0x75, 0x94, 0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
        0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d, 0xe5, 0x76, 0xd2, 0x65,
        0x86, 0xce, 0xc6, 0x4b, 0x61, 0x16
    };
    static const uint8_t tag[16] = {
        0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
        0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91
    };
    uint8_t key[32], bad_tag[16];
    unsigned index;

    g_aead_result = -1;
    g_aead_test = 0;
    xil_printf("\r\nKV260 bare-metal ChaCha20-Poly1305 RFC 8439 test\r\n");
    xil_printf("AXI-Lite base=%08x; next access requires PL clock/reset ready.\r\n",
               (unsigned)AEAD_BASE);
    /* This Cortex-A53 application owns the peripheral. Device mapping plus
     * barriers prevents caching/speculation of the stream slot registers.
     */
    Xil_SetTlbAttributes(AEAD_BASE, DEVICE_MEMORY);
    write_register(CONTROL, SOFT_RESET);
    for (index = 0; index < sizeof(key); ++index) {
        key[index] = (uint8_t)(0x80U + index);
    }
    memcpy(bad_tag, tag, sizeof(tag));
    bad_tag[0] ^= 1U;
    for (index = 0; index < 3U; ++index) {
        const char *names[] = {
            "RFC encrypt", "RFC decrypt", "RFC decrypt bad tag (rejected)"
        };
        struct test_case test = {
            names[index], key, nonce, aad,
            index == 0U ? plaintext : ciphertext,
            index == 0U ? ciphertext : plaintext,
            index == 2U ? bad_tag : tag,
            sizeof(aad), sizeof(plaintext) - 1U, index == 0U, index == 1U
        };
        g_aead_test = index + 1U;
        if (run_case(&test) != 0) {
            g_aead_result = 1;
            xil_printf("AEAD SELF-TEST FAIL\r\n");
            return 1;
        }
    }
    g_aead_result = 0;
    xil_printf("AEAD SELF-TEST PASS\r\n");
    return 0;
}
