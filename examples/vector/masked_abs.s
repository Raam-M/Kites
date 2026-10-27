# masked_abs.s - in-place absolute value using a mask: negate only the negative elements.
# vmslt.vx builds the mask v0 (element < 0), then vrsub.vx (0 - x) runs under v0.t.
# Result: values = {5, 3, 0, 7, 2, 9, 4, 1}
.data
values: .word -5, 3, 0, -7, 2, -9, 4, -1
.text
    li   a0, 8
    la   a1, values
loop:
    vsetvli t0, a0, e32, m2, ta, mu
    vle32.v v8, (a1)
    vmslt.vx v0, v8, x0          # v0[i] = (x[i] < 0)
    vrsub.vx v8, v8, x0, v0.t    # x[i] = 0 - x[i] where v0[i] = 1
    vse32.v v8, (a1)
    sub  a0, a0, t0
    slli t1, t0, 2
    add  a1, a1, t1
    bnez a0, loop
