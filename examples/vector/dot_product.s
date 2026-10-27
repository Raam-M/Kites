# dot_product.s - int32 dot product with vmul.vv and vredsum.vs, strip-mined over 20 elements.
# The running sum is carried in s0 between iterations.
# Result: s0 = sum(x[i] * y[i]) = 770
.data
x: .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
y: .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
.text
    li   a0, 20
    la   a1, x
    la   a2, y
    li   s0, 0
loop:
    vsetvli t0, a0, e32, m4, ta, ma
    vle32.v v8, (a1)
    vle32.v v12, (a2)
    vmul.vv v8, v8, v12
    vmv.s.x v16, s0          # v16[0] = running sum
    vredsum.vs v16, v8, v16  # v16[0] = s0 + sum(products)
    vmv.x.s s0, v16
    sub  a0, a0, t0
    slli t1, t0, 2
    add  a1, a1, t1
    add  a2, a2, t1
    bnez a0, loop
