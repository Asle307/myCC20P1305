# Radix-4 multiplier

Inputs are two unsigned 130-bit operands; output is their 260-bit product. START-to-DONE remains 67 clocks.

## Registers with one role each

| Register | Role |
|---|---|
| `r_operand_a` | Captured A, fixed for the operation |
| `r_operand_b_shift` | Captured B; shift right by two each iteration |
| `r_partial_3a` | 3A, computed once in SETUP |
| `r_product_high` / `r_product_low` | Product accumulation; both start at zero |

2A is wiring. Each ITER reads `r_operand_b_shift[1:0]` directly and selects 0, A, 2A or the registered 3A with a short case. There is no next-digit prefetch or selected-partial pipeline register. B and product storage are separate.

## Clock schedule

| Edge after accepting START | State before edge | Work |
|---:|---|---|
| 0 | IDLE | Capture A/B, clear product, enter SETUP |
| 1 | SETUP | Register 3A, enter WAIT_ITER |
| 2 | WAIT_ITER | Alignment slot, enter ITER |
| 3..67 | ITER | Process digits 0..64; last edge pulses DONE |

The alignment slot preserves the existing 67-cycle contract. State encodings remain IDLE=0, SETUP=1, ITER=2 and WAIT_ITER=3.

```text
partial = select(B[1:0], 0, A, 2A, 3A)
sum = high + partial
high_next = sum[131:2]
low_next = {sum[1:0], low[129:2]}
B_next = B >> 2
product = {high, low}
```

Only high participates in the 132-bit addition. The two low sum bits enter the high end of low and shift toward their final positions over later iterations. Since `high + digit*A < 4*2^130`, 132 bits suffice. Before DONE, product is intermediate and must be ignored.

## Small example

For an 8-bit illustration, A=13 and B=231 (`11_10_01_11`). Four iterations consume digits 3,1,2,3, from low to high. SETUP captures 3A=39.

| ITER | Digit | Sum | High after | Low after | B after |
|---:|---:|---:|---:|---|---:|
| 0 | 3 | 39 | 9 | `11000000` | 57 |
| 1 | 1 | 22 | 5 | `10110000` | 14 |
| 2 | 2 | 31 | 7 | `11101100` | 3 |
| 3 | 3 | 46 | 11 | `10111011` | 0 |

Final product is `00001011_10111011` = 3003. Low contains only generated product bits and initial zeros throughout; unconsumed digits remain in B.

## Ports and tradeoff

Pulse START while idle; both operands are captured. Inputs may change afterward. DONE pulses for one clock after the last iteration; product stays held until the next accepted request. Reset aborts control without wiping datapath registers.

The maintained verification suite exercises this datapath through the top-level AEAD core. There is no standalone multiplier testbench in the current suite.
