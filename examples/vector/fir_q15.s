# fir_q15.s - 4-tap FIR filter in Q15 fixed point over 8 outputs.
# y[i] = sat(sum_k h[k] * x[i + k]) using vsmul (Q15 multiply with rounding, vxrm = rnu)
# and vsadd (saturating add); vxsat records whether any result saturated.
.data
h:  .halfword 16384, -8192, 4096, 32767
x:  .halfword 1000, -2000, 3000, 32767, -32768, 12345, -12345, 0, 500, 600, 700
y:  .halfword 0, 0, 0, 0, 0, 0, 0, 0
.text
    csrwi vxrm, 0                # round to nearest, ties up
    vsetivli t0, 8, e16, m1, ta, ma
    vmv.v.i v6, 0                # accumulator
    la   a1, x
    la   a2, h
    li   s1, 4                   # taps
tap:
    lh   t1, 0(a2)               # h[k]
    vle16.v v2, (a1)             # x[k .. k+7]
    vsmul.vx v4, v2, t1
    vsadd.vv v6, v6, v4
    addi a1, a1, 2
    addi a2, a2, 2
    addi s1, s1, -1
    bnez s1, tap
    la   a3, y
    vse16.v v6, (a3)
