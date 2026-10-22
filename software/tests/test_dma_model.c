/* Register-level transport model. Cryptographic data comes from independent
 * generator fixtures; this is not an AXI DMA IP or RTL simulation. */
#define main dma_program_main
#include "../aead_dma_uio.c"
#undef main
#include <assert.h>

enum fault { GOOD, BAD_DATA, BAD_LENGTH, BAD_AUTH, DMA_ERROR, CORE_ERROR, STUCK };
struct model {
    uint32_t registers[(DMA + 0x1000) / 4];
    struct fixture *test;
    volatile uint32_t *ram;
    unsigned starts, resets;
    bool tx_armed, rx_armed;
    bool corrupt_configuration;
    enum fault fault;
};

static uint32_t read_model(void *context, unsigned offset)
{
    struct model *model = context;
    return model->registers[offset / 4] ^ (model->corrupt_configuration && offset == KEY0 ? 1u : 0u);
}

static void write_model(void *context, unsigned offset, uint32_t value)
{
    struct model *model = context;
    struct fixture *test = model->test;
    uint32_t *regs = model->registers;
    size_t index;
    if (offset == TX_SR || offset == RX_SR) {
        regs[offset / 4] &= ~value;
        return;
    }
    regs[offset / 4] = value;
    if (offset == TX_CR && (value & RESET)) {
        ++model->resets;
        regs[TX_CR / 4] = regs[RX_CR / 4] = 0;
        regs[TX_SR / 4] = regs[RX_SR / 4] = HALTED;
        model->tx_armed = model->rx_armed = false;
    }
    if (offset == RX_LEN) {
        assert(value == test->output_words * 4 && value != 0);
        assert((((uint64_t)regs[(RX_ADDR + 4) / 4] << 32) | regs[RX_ADDR / 4]) ==
               UINT64_C(0x800000000) + RX_OFFSET);
        model->rx_armed = true;
    }
    if (offset == TX_LEN) {
        assert(!test->output_words || model->rx_armed);
        assert(value == test->input_words * 4 && value != 0);
        assert((((uint64_t)regs[(TX_ADDR + 4) / 4] << 32) | regs[TX_ADDR / 4]) == UINT64_C(0x800000000));
        model->tx_armed = true;
    }
    if (offset == CONTROL && (value & SOFT_RESET)) {
        regs[STATUS / 4] = READY;
    }
    else if (offset == CONTROL && (value & START)) {
        ++model->starts;
        assert(!test->input_words || model->tx_armed);
        assert(!test->output_words || model->rx_armed);
        assert(value == (START | (test->encrypt ? ENCRYPT : 0)));
        for (index = 0; index < 8; ++index) assert(regs[KEY0 / 4 + index] == test->key[index]);
        for (index = 0; index < 3; ++index) assert(regs[NONCE0 / 4 + index] == test->nonce[index]);
        assert(regs[AAD_BYTES / 4] == test->aad_bytes && regs[PAYLOAD_BYTES / 4] == test->payload_bytes);
        for (index = 0; index < test->input_words; ++index) assert(model->ram[index] == test->input[index]);
        for (index = 0; index < test->output_words; ++index) model->ram[RX_OFFSET / 4 + index] = test->expected[index];
        regs[TX_SR / 4] = regs[RX_SR / 4] = IOC | IDLE;
        regs[STATUS / 4] = READY | DONE | (test->auth_ok ? AUTH_OK : 0);
        regs[CYCLES / 4] = 12345;
        model->tx_armed = model->rx_armed = false;
        switch (model->fault) {
        case BAD_DATA: model->ram[RX_OFFSET / 4] ^= 1; break;
        case BAD_LENGTH: regs[RX_LEN / 4] -= 4; break;
        case BAD_AUTH: regs[STATUS / 4] ^= AUTH_OK; break;
        case DMA_ERROR: regs[TX_SR / 4] |= 0x10; break;
        case CORE_ERROR: regs[STATUS / 4] |= ERROR; break;
        case STUCK: regs[STATUS / 4] = BUSY; regs[TX_SR / 4] = regs[RX_SR / 4] = 0; break;
        default: break;
        }
    }
}

int main(int argc, char **argv)
{
    struct fixture test = {0};
    struct model model = {0};
    struct hw hw = {read_model, write_model, &model, NULL, UINT64_C(0x800000000), false};
    uint32_t cycles;
    size_t index;
    enum fault fault;
    assert(argc == 2 && load_fixture(argv[1], &test) == 0);
    hw.ram = calloc(1, RAM_BYTES);
    assert(hw.ram);
    model.test = &test;
    model.ram = hw.ram;
    model.registers[STATUS / 4] = READY;
    for (index = 0; index < test.input_words; ++index) hw.ram[index] = test.input[index];
    for (index = 0; index < 3; ++index) {
        assert(run_transfer(&hw, &test, 1000, &cycles) == 0 && cycles == 12345);
    }
    assert(model.starts == 3 && model.resets == 0);
    hw.diagnostics = true;
    assert(run_transfer(&hw, &test, 1000, &cycles) == 0);
    assert(model.starts == 4 && model.resets == 0);
    if (test.input_words) {
        hw.ram[0] ^= 1;
        assert(run_transfer(&hw, &test, 1000, &cycles) == 1);
        assert(model.starts == 4 && model.resets == 0);
        hw.ram[0] ^= 1;
    }
    model.corrupt_configuration = true;
    assert(run_transfer(&hw, &test, 1000, &cycles) == 1);
    assert(model.starts == 4 && model.resets == 0);
    model.corrupt_configuration = false;
    hw.diagnostics = false;
    if (test.input_words && test.output_words) {
        for (fault = BAD_DATA; fault <= STUCK; ++fault) {
            model.fault = fault;
            assert(run_transfer(&hw, &test, 1, &cycles) == 1);
            assert(model.resets == (unsigned)fault);
        }
    }
    model.registers[STATUS / 4] = BUSY;
    index = model.starts;
    assert(run_transfer(&hw, &test, 1, &cycles) == 1);
    assert(model.starts == index);
    free((void *)hw.ram);
    free(test.input);
    free(test.expected);
    puts("PASS DMA host ordering, 64-bit addresses, payload verification and recovery");
    return 0;
}
