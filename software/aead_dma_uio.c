#define _POSIX_C_SOURCE 200809L
/* Lab Linux host: AXI DMA simple mode, existing MY_IP and reserved ddr_high UIO.
 * Input/output fixtures come from script/gen_vectors.py, not a hardware oracle.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    CONTROL = 0, STATUS = 4, KEY0 = 8, NONCE0 = 40, AAD_BYTES = 52, PAYLOAD_BYTES = 56, CYCLES = 60,
    START = 1, ENCRYPT = 2, SOFT_RESET = 8, READY = 1, BUSY = 2, DONE = 4, AUTH_OK = 8, ERROR = 64,
    DMA = 0x10000, TX_CR = DMA, TX_SR = DMA + 4, TX_ADDR = DMA + 0x18, TX_LEN = DMA + 0x28,
    RX_CR = DMA + 0x30, RX_SR = DMA + 0x34, RX_ADDR = DMA + 0x48, RX_LEN = DMA + 0x58,
    HALTED = 1, IDLE = 2, RESET = 4, SG = 8, IOC = 0x1000, DMA_ERRORS = 0x770,
    RX_OFFSET = 0x40000, RAM_BYTES = 0x80000, MAX_WORDS = 32774
};

struct fixture {
    uint32_t key[8], nonce[3], aad_bytes, payload_bytes;
    uint32_t *input, *expected;
    size_t input_words, output_words;
    bool encrypt, auth_ok;
};

struct hw {
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    void *context;
    volatile uint32_t *ram;
    uint64_t ram_phys;
    bool diagnostics;
};

static uint64_t nanoseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static uint32_t mmio_read(void *context, unsigned offset)
{
    uint32_t value = ((volatile uint32_t *)context)[offset / 4];
    __sync_synchronize();
    return value;
}

static void mmio_write(void *context, unsigned offset, uint32_t value)
{
    ((volatile uint32_t *)context)[offset / 4] = value;
    __sync_synchronize();
}

static int hex_line(FILE *file, unsigned digits, char *buffer)
{
    unsigned index;
    if (!fgets(buffer, 80, file) || strcspn(buffer, "\r\n") != digits) {
        return -1;
    }
    buffer[digits] = 0;
    for (index = 0; index < digits; ++index) {
        if (!isxdigit((unsigned char)buffer[index])) {
            return -1;
        }
    }
    return 0;
}

static FILE *case_file(const char *directory, const char *name)
{
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path)) {
        return NULL;
    }
    return fopen(path, "r");
}

static int word_file(const char *directory, const char *name, uint32_t *words, size_t count)
{
    FILE *file = case_file(directory, name);
    char text[80];
    size_t index;
    int character, result = -1;
    if (!file) {
        return -1;
    }
    for (index = 0; index < count; ++index) {
        if (hex_line(file, 8, text)) {
            goto out;
        }
        words[index] = (uint32_t)strtoul(text, NULL, 16);
    }
    while ((character = fgetc(file)) != EOF) {
        if (!isspace((unsigned char)character)) {
            goto out;
        }
    }
    result = 0;
out:
    fclose(file);
    return result;
}

static int load_fixture(const char *directory, struct fixture *test)
{
    FILE *file = case_file(directory, "rtl_config.hex");
    char text[80], chunk[9];
    uint32_t config[6], *control = NULL;
    unsigned index;
    size_t source, packed;
    int result = -1;
    if (!file) {
        return -1;
    }
    for (index = 0; index < 2; ++index) {
        unsigned word, count = index == 0 ? 8 : 3;
        uint32_t *destination = index == 0 ? test->key : test->nonce;
        if (hex_line(file, count * 8, text)) {
            goto close;
        }
        for (word = 0; word < count; ++word) {
            memcpy(chunk, text + (count - word - 1) * 8, 8);
            chunk[8] = 0;
            destination[word] = (uint32_t)strtoul(chunk, NULL, 16);
        }
    }
    if (hex_line(file, 32, text)) {
        goto close;
    }
    for (index = 0; index < 6; ++index) {
        if (hex_line(file, 8, text)) {
            goto close;
        }
        config[index] = (uint32_t)strtoul(text, NULL, 16);
    }
    if (config[0] > 1 || config[1] > 1 || (config[0] && config[1]) ||
        config[2] > 65536 || config[3] > 65536 || config[4] > MAX_WORDS || config[5] > MAX_WORDS ||
        config[4] != (config[2] ? (config[2] + 3) / 4 : 1) +
                     (config[3] ? (config[3] + 3) / 4 : 1) + (config[0] ? 0 : 4) ||
        config[5] != (config[3] + 3) / 4 + (config[0] ? 4 : 0)) {
        goto close;
    }
    test->encrypt = config[0] != 0;
    test->auth_ok = config[1] != 0;
    test->aad_bytes = config[2];
    test->payload_bytes = config[3];
    test->output_words = config[5];
    test->input = calloc(config[4] + 1, sizeof(uint32_t));
    test->expected = calloc(config[5] + 1, sizeof(uint32_t));
    control = calloc(config[4] + 1, sizeof(uint32_t));
    if (!test->input || !test->expected || !control ||
        word_file(directory, "rtl_input.hex", test->input, config[4]) ||
        word_file(directory, "rtl_input_control.hex", control, config[4]) ||
        word_file(directory, "rtl_expected.hex", test->expected, config[5])) {
        goto close;
    }
    packed = 0;
    for (source = 0; source < config[4]; ++source) {
        if ((control[source] & 7) > 4 || (control[source] & 0xfffffcf8u) ||
            (!(control[source] & 512) && (control[source] & 7) != 4)) {
            goto close;
        }
        if (control[source] & 7) {
            test->input[packed++] = test->input[source];
        }
    }
    test->input_words = packed;
    if (packed != (test->aad_bytes + 3) / 4 + (test->payload_bytes + 3) / 4 + (test->encrypt ? 0 : 4)) {
        goto close;
    }
    result = 0;
close:
    free(control);
    fclose(file);
    return result;
}

static int dma_reset(const struct hw *hw, unsigned timeout_ms)
{
    uint64_t deadline = nanoseconds() + (uint64_t)timeout_ms * 1000000;
    hw->write(hw->context, TX_CR, RESET); /* Either reset resets both channels. */
    while ((hw->read(hw->context, TX_CR) | hw->read(hw->context, RX_CR)) & RESET) {
        if (nanoseconds() >= deadline) {
            return -1;
        }
    }
    return 0;
}

static void arm(const struct hw *hw, unsigned control, unsigned address, unsigned length,
                uint64_t physical, size_t bytes)
{
    hw->write(hw->context, control, 1); /* RUN, IRQ enables remain zero. */
    hw->write(hw->context, address, (uint32_t)physical);
    hw->write(hw->context, address + 4, (uint32_t)(physical >> 32));
    hw->write(hw->context, length, (uint32_t)bytes); /* Length is the trigger. */
}

static int check_ram(const struct hw *hw, const struct fixture *test, bool check_rx)
{
    size_t index;
    __sync_synchronize();
    for (index = 0; index < test->input_words; ++index) {
        uint32_t actual = hw->ram[index];
        if (actual != test->input[index]) {
            fprintf(stderr, "CPU TX readback failed BEFORE DMA: word=%zu addr=0x%" PRIx64
                    " actual=%08x expected=%08x\n", index, hw->ram_phys + index * 4,
                    actual, test->input[index]);
            return -1;
        }
    }
    if (check_rx) {
        for (index = 0; index < test->output_words; ++index) {
            uint32_t actual = hw->ram[RX_OFFSET / 4 + index];
            uint32_t expected = ~test->expected[index];
            if (actual != expected) {
                fprintf(stderr, "CPU RX readback failed BEFORE DMA: word=%zu addr=0x%" PRIx64
                        " actual=%08x expected=%08x\n", index, hw->ram_phys + RX_OFFSET + index * 4,
                        actual, expected);
                return -1;
            }
        }
    }
    return 0;
}

static void dump_state(const struct hw *hw, const struct fixture *test)
{
    static const struct { const char *name; unsigned offset; } fields[] = {
        {"CONTROL", CONTROL}, {"STATUS", STATUS}, {"CYCLES", CYCLES},
        {"KEY0", KEY0}, {"NONCE0", NONCE0}, {"AAD_BYTES", AAD_BYTES}, {"PAYLOAD_BYTES", PAYLOAD_BYTES},
        {"TX_CR", TX_CR}, {"TX_SR", TX_SR}, {"TX_ADDR_LO", TX_ADDR},
        {"TX_ADDR_HI", TX_ADDR + 4}, {"TX_LEN", TX_LEN},
        {"RX_CR", RX_CR}, {"RX_SR", RX_SR}, {"RX_ADDR_LO", RX_ADDR},
        {"RX_ADDR_HI", RX_ADDR + 4}, {"RX_LEN", RX_LEN}
    };
    size_t index;
    fprintf(stderr, "--- diagnostic snapshot BEFORE reset ---\n");
    for (index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index) {
        fprintf(stderr, "%-14s = 0x%08x\n", fields[index].name,
                hw->read(hw->context, fields[index].offset));
    }
    for (index = 0; index < test->input_words && index < 8; ++index) {
        fprintf(stderr, "TX[%zu] = %08x expected=%08x\n", index, hw->ram[index], test->input[index]);
    }
    for (index = 0; index < test->output_words && index < 8; ++index) {
        fprintf(stderr, "RX[%zu] = %08x expected=%08x initial=%08x\n", index,
                hw->ram[RX_OFFSET / 4 + index], test->expected[index], ~test->expected[index]);
    }
}

static int run_transfer(const struct hw *hw, const struct fixture *test,
                        unsigned timeout_ms, uint32_t *cycles)
{
    size_t index;
    uint64_t deadline = nanoseconds() + (uint64_t)timeout_ms * 1000000;
    uint32_t status, tx_status, rx_status;
    uint32_t mode = test->encrypt ? ENCRYPT : 0;
    status = hw->read(hw->context, STATUS);
    if (!(status & READY) || (status & (BUSY | ERROR))) {
        fprintf(stderr, "AEAD not ready; existing session left untouched\n");
        return 1;
    }
    for (index = 0; index < test->output_words; ++index) {
        hw->ram[RX_OFFSET / 4 + index] = ~test->expected[index];
    }
    __sync_synchronize();
    for (index = 0; index < 8; ++index) {
        hw->write(hw->context, KEY0 + (unsigned)index * 4, test->key[index]);
    }
    for (index = 0; index < 3; ++index) {
        hw->write(hw->context, NONCE0 + (unsigned)index * 4, test->nonce[index]);
    }
    hw->write(hw->context, AAD_BYTES, test->aad_bytes);
    hw->write(hw->context, PAYLOAD_BYTES, test->payload_bytes);
    if (hw->diagnostics) {
        if (check_ram(hw, test, true)) {
            // No DMA has been armed yet; preserve the evidence without
            // starting or resetting a transfer on an invalid CPU mapping.
            dump_state(hw, test);
            return 1;
        }
        for (index = 0; index < 13; ++index) {
            uint32_t expected = index < 8 ? test->key[index] : index < 11 ? test->nonce[index - 8] :
                                index == 11 ? test->aad_bytes : test->payload_bytes;
            uint32_t actual = hw->read(hw->context, KEY0 + (unsigned)index * 4);
            if (actual != expected) {
                fprintf(stderr, "Configuration readback failed BEFORE DMA: offset=0x%02x "
                        "actual=%08x expected=%08x\n", KEY0 + (unsigned)index * 4, actual, expected);
                dump_state(hw, test);
                return 1;
            }
        }
        printf("DIAG CPU TX/RX readback and all key/nonce/length registers passed\n");
        fflush(stdout);
    }
    hw->write(hw->context, TX_SR, 0x7000);
    hw->write(hw->context, RX_SR, 0x7000);
    /* Receive first; transmit may hold VALID while the AEAD core is idle.
     * START last keeps CPU setup outside the PL stopwatch. */
    if (test->output_words) {
        arm(hw, RX_CR, RX_ADDR, RX_LEN, hw->ram_phys + RX_OFFSET, test->output_words * 4);
    }
    if (test->input_words) {
        arm(hw, TX_CR, TX_ADDR, TX_LEN, hw->ram_phys, test->input_words * 4);
    }
    hw->write(hw->context, CONTROL, START | mode);
    for (;;) {
        status = hw->read(hw->context, STATUS);
        tx_status = test->input_words ? hw->read(hw->context, TX_SR) : IOC | IDLE;
        rx_status = test->output_words ? hw->read(hw->context, RX_SR) : IOC | IDLE;
        if ((status & (ERROR | 128)) || ((tx_status | rx_status) & DMA_ERRORS)) {
            fprintf(stderr, "AEAD/DMA error: STATUS=%08x TX=%08x RX=%08x\n", status, tx_status, rx_status);
            goto abort;
        }
        if ((status & DONE) && (tx_status & (IOC | IDLE)) == (IOC | IDLE) &&
            (rx_status & (IOC | IDLE)) == (IOC | IDLE)) {
            break;
        }
        if (nanoseconds() >= deadline) {
            fprintf(stderr, "DMA/AEAD timeout: STATUS=%08x TX=%08x RX=%08x\n", status, tx_status, rx_status);
            goto abort;
        }
    }
    *cycles = hw->read(hw->context, CYCLES);
    if (!*cycles || (status & (READY | BUSY)) != READY || !!(status & AUTH_OK) != test->auth_ok ||
        (test->output_words && hw->read(hw->context, RX_LEN) != test->output_words * 4)) {
        fprintf(stderr, "Completion/auth/cycles/received-length mismatch: STATUS=%08x "
                "AUTH_OK=%u expected=%u CYCLES=%u RX_LEN=%u expected=%zu\n", status,
                (unsigned)!!(status & AUTH_OK), (unsigned)test->auth_ok, *cycles,
                test->output_words ? hw->read(hw->context, RX_LEN) : 0, test->output_words * 4);
        goto abort;
    }
    __sync_synchronize();
    for (index = 0; index < test->output_words; ++index) {
        uint32_t value = hw->ram[RX_OFFSET / 4 + index];
        if (value != test->expected[index]) {
            fprintf(stderr, "Output word %zu: %08x, expected %08x\n", index, value, test->expected[index]);
            goto abort;
        }
    }
    if (hw->diagnostics) {
        dump_state(hw, test);
    }
    return 0;
abort:
    dump_state(hw, test);
    if (dma_reset(hw, timeout_ms)) {
        fprintf(stderr, "DMA reset timed out\n");
    }
    hw->write(hw->context, CONTROL, SOFT_RESET | mode);
    return 1;
}

static int map_number(const char *device, const char *field, uint64_t *value)
{
    const char *name = strrchr(device, '/');
    char path[256], text[128], *end;
    FILE *file;
    name = name ? name + 1 : device;
    if (strncmp(name, "uio", 3) || !name[3] || strspn(name + 3, "0123456789") != strlen(name + 3)) {
        return -1;
    }
    if (snprintf(path, sizeof(path), "/sys/class/uio/%s/maps/map0/%s", name, field) >= (int)sizeof(path)) {
        return -1;
    }
    file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    if (!fgets(text, sizeof(text), file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    errno = 0;
    *value = strtoull(text, &end, 0);
    return errno || end == text || (*end && *end != '\n') ? -1 : 0;
}

static void *map_device(const char *device, uint64_t expected, size_t bytes, int *descriptor)
{
    uint64_t base, size, offset;
    struct stat info;
    void *mapping = MAP_FAILED;
    int fd;
    if (map_number(device, "addr", &base) || map_number(device, "size", &size) ||
        map_number(device, "offset", &offset) || base != expected || size < bytes || offset) {
        fprintf(stderr, "Unexpected %s map; require base 0x%" PRIx64 ", size >= 0x%zx, offset 0\n",
                device, expected, bytes);
        return MAP_FAILED;
    }
    fd = open(device, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror(device);
        return MAP_FAILED;
    }
    if (!fstat(fd, &info) && S_ISCHR(info.st_mode)) {
        mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    if (mapping == MAP_FAILED) {
        close(fd);
    }
    else {
        /* Keep the UIO open reference, including runtime-PM ownership, until
         * the transfer and mapping are finished. */
        *descriptor = fd;
    }
    return mapping;
}

static int number(const char *text, unsigned minimum, unsigned maximum, unsigned *value)
{
    unsigned long parsed;
    char *end;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || !*text || strspn(text, "0123456789") != strlen(text) || *end ||
        parsed < minimum || parsed > maximum) {
        return -1;
    }
    *value = (unsigned)parsed;
    return 0;
}

static void usage(const char *program)
{
    printf("Usage: %s --case DIRECTORY [--device /dev/uio12] [--memory /dev/uio13]\n"
           "       [--iterations 1] [--warmup 0] [--clock-hz 199999998] [--timeout-ms 5000]\n"
           "       [--inspect] [--diagnose]\n"
           "Requires NEW AXIS/DMA bitstream, AXI DMA at 0xA0010000 (64-bit addresses),\n"
           "and the lab's reserved, uncached ddr_high map at 0x800000000.\n"
           "--inspect validates fixture packing without accessing hardware.\n"
           "--diagnose adds CPU RAM/config readback and register/buffer snapshots.\n", program);
}

int main(int argc, char **argv)
{
    static const struct option arguments[] = {
        {"case", required_argument, NULL, 'c'}, {"device", required_argument, NULL, 'd'},
        {"memory", required_argument, NULL, 'r'}, {"iterations", required_argument, NULL, 'n'},
        {"warmup", required_argument, NULL, 'w'}, {"clock-hz", required_argument, NULL, 'f'},
        {"timeout-ms", required_argument, NULL, 't'}, {"inspect", no_argument, NULL, 'i'},
        {"diagnose", no_argument, NULL, 'D'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    const char *directory = NULL, *device = "/dev/uio12", *memory = "/dev/uio13";
    struct fixture test = {0};
    struct hw hw = {mmio_read, mmio_write, NULL, NULL, UINT64_C(0x800000000), false};
    unsigned iterations = 1, warmup = 0, clock_hz = 0, timeout_ms = 5000, iteration;
    uint64_t sum_cycles = 0, sum_ns = 0;
    uint32_t min_cycles = UINT32_MAX, max_cycles = 0, tx_status, rx_status;
    void *registers = MAP_FAILED, *ram = MAP_FAILED;
    bool inspect = false;
    int option, result = 2, register_fd = -1, memory_fd = -1;
    while ((option = getopt_long(argc, argv, "c:d:r:n:w:f:t:iDh", arguments, NULL)) != -1) {
        switch (option) {
        case 'c': directory = optarg; break;
        case 'd': device = optarg; break;
        case 'r': memory = optarg; break;
        case 'i': inspect = true; break;
        case 'D': hw.diagnostics = true; break;
        case 'n': if (number(optarg, 1, 1000000, &iterations)) goto invalid; break;
        case 'w': if (number(optarg, 0, 1000000, &warmup)) goto invalid; break;
        case 'f': if (number(optarg, 1, 1000000000, &clock_hz)) goto invalid; break;
        case 't': if (number(optarg, 1, 600000, &timeout_ms)) goto invalid; break;
        case 'h': usage(argv[0]); return 0;
        default: goto invalid;
        }
    }
    if (!directory || optind != argc) {
        goto invalid;
    }
    if (load_fixture(directory, &test)) {
        fprintf(stderr, "Invalid/incomplete native fixture: %s\n", directory);
        goto out;
    }
    printf("%s: %s, AAD=%u, payload=%u, DMA TX=%zu bytes, RX=%zu bytes, expected AUTH_OK=%u\n",
           directory, test.encrypt ? "encrypt" : "decrypt", test.aad_bytes, test.payload_bytes,
           test.input_words * 4, test.output_words * 4, (unsigned)test.auth_ok);
    if (inspect) {
        result = 0;
        goto out;
    }
    registers = map_device(device, UINT64_C(0xa0000000), DMA + 0x1000, &register_fd);
    if (registers == MAP_FAILED) {
        goto out;
    }
    hw.context = registers;
    if ((hw.read(hw.context, CONTROL) & 0xffffff00u) != 0x41453200u) {
        fprintf(stderr, "Wrong peripheral: install the aead_axi_dma bitstream first\n");
        goto out;
    }
    tx_status = hw.read(hw.context, TX_SR);
    rx_status = hw.read(hw.context, RX_SR);
    if (!(hw.read(hw.context, STATUS) & READY) || (hw.read(hw.context, STATUS) & ERROR) ||
        ((tx_status | rx_status) & SG) || !(tx_status & (IDLE | HALTED)) || !(rx_status & (IDLE | HALTED))) {
        fprintf(stderr, "AEAD/DMA busy, errored, or not simple mode; left untouched\n");
        goto out;
    }
    ram = map_device(memory, hw.ram_phys, RAM_BYTES, &memory_fd);
    if (ram == MAP_FAILED) {
        goto out;
    }
    hw.ram = ram;
    if (hw.diagnostics) {
        printf("DIAG register base=0xa0000000 DMA=0xa0010000 "
               "TX physical=0x%" PRIx64 " RX physical=0x%" PRIx64 "\n",
               hw.ram_phys, hw.ram_phys + RX_OFFSET);
    }
    if (dma_reset(&hw, timeout_ms)) {
        fprintf(stderr, "Initial DMA reset timeout\n");
        goto out;
    }
    for (iteration = 0; iteration < test.input_words; ++iteration) {
        hw.ram[iteration] = test.input[iteration];
    }
    __sync_synchronize();
    result = 1;
    for (iteration = 0; iteration < warmup + iterations; ++iteration) {
        uint32_t cycles;
        uint64_t begin = nanoseconds(), elapsed;
        if (run_transfer(&hw, &test, timeout_ms, &cycles)) {
            goto out;
        }
        elapsed = nanoseconds() - begin;
        if (iteration >= warmup) {
            sum_cycles += cycles;
            sum_ns += elapsed;
            if (cycles < min_cycles) min_cycles = cycles;
            if (cycles > max_cycles) max_cycles = cycles;
        }
    }
    printf("PASS %u measured messages, %u warm-ups; every output word and AUTH_OK checked\n"
           "PL cycles/message: mean=%.2f min=%u max=%u (includes DMA stream stalls)\n"
           "Host verified transfer: %.3f Mbit/s, %.3f us/message\n", iterations, warmup,
           (double)sum_cycles / iterations, min_cycles, max_cycles,
           (double)test.payload_bytes * iterations * 8000.0 / (double)sum_ns,
           (double)sum_ns / iterations / 1000.0);
    if (clock_hz) {
        printf("PL at %u Hz: %.3f Mbit/s, %.3f us/message\n", clock_hz,
               (double)test.payload_bytes * iterations * 8.0 * clock_hz / (double)sum_cycles / 1e6,
               (double)sum_cycles / iterations / clock_hz * 1e6);
    }
    result = 0;
out:
    if (ram != MAP_FAILED) munmap(ram, RAM_BYTES);
    if (registers != MAP_FAILED) munmap(registers, DMA + 0x1000);
    if (memory_fd >= 0) close(memory_fd);
    if (register_fd >= 0) close(register_fd);
    free(test.input);
    free(test.expected);
    return result;
invalid:
    usage(argv[0]);
    return 2;
}
