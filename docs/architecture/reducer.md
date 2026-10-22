# Reduction modulo 2^130 - 5

For `p = 2^130 - 5`, replace each high part by five times that part. The reducer has two clock stages and one shared 133-bit bank.

```text
fold1 = low130(product) + 5*high130(product)
k = 5*fold1[132:130]
a = low130(fold1) + k
b = low130(fold1) + (k+5)
residue = b[130] ? low130(b) : low130(a)
```

Stage 1 captures fold1 at accepted START. Stage 2 computes fold2 and canonical correction together. The two candidates a and b are parallel additions from the same stored low part. Only the six-bit increment k+5 is formed before b's wide adder; there is no serial pair of 131-bit adders.

## Bounds and correction

`fold1 < 6*2^130`, so its high part is at most 5 and k is at most 25. Thus `a < 2^130+25` and `b = a+5 < 2^131`. Bit 130 of b is one exactly when `a >= 2^130-5 = p`. In that case its low 130 bits equal a-p; otherwise a already lies in `0..p-1`.

The unbiased candidate only needs low 130 bits. If it overflows, b's carry selects the corrected candidate. The final bank is zero-extended canonical residue, with no correction logic after its output register.

## Clock schedule

| Edge relative to accepted START | Result |
|---:|---|
| 0 | Capture fold1; valid stays low |
| 1 | Capture canonical residue; valid rises |
| 2 or later | Consumer transfers valid/ready result |

Control is IDLE → FINISH → HOLD → IDLE. HOLD retains `r_fold[129:0]` under stalls. Reset clears control/valid and aborts the request without wiping data. Output before valid may show intermediate fold data.

With immediate consumption, new requests can be accepted every three clocks. `p1305_pair` retains a reducer per lane: single/pair result-valid is 69 clocks after arithmetic START, OTK setup 70, pair update 73 and final singleton 72. No lane arbitration is introduced.

## Verification

The maintained suite verifies reduction through the AEAD top against independent Python ciphertext/tag results, including boundary messages, stalls and reset/recovery. There are no standalone 260-bit modulo or three-clock request-spacing tests in the maintained native suite; the clock schedule above remains the implementation contract.
