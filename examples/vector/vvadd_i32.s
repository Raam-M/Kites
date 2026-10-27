# vvadd_i32.s - strip-mined vector add: c[i] = a[i] + b[i] for n = 13 int32 elements.
# Each iteration processes vl = min(remaining, VLMAX) elements (VLMAX = 4 for e32, m1,
# VLEN = 128), so the last iteration handles a partial vector.
# Result: c = {11, 22, 33, ..., 143}
.data
a: .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
b: .word 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130
c: .word 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
.text
    li   a0, 13          # n
    la   a1, a
    la   a2, b
    la   a3, c
loop:
    vsetvli t0, a0, e32, m1, ta, ma   # t0 = vl
    vle32.v v0, (a1)
    vle32.v v1, (a2)
    vadd.vv v2, v0, v1
    vse32.v v2, (a3)
    sub  a0, a0, t0
    slli t1, t0, 2       # bytes processed
    add  a1, a1, t1
    add  a2, a2, t1
    add  a3, a3, t1
    bnez a0, loop
