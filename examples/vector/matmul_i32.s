# matmul_i32.s - 4x4 int32 matrix multiply, C = A * B, computed twice:
#   cv: vectorised - each row of C is accumulated with vmacc.vx (A[i][k] * row k of B)
#   cs: scalar triple loop, as a reference computed by the same program
# Result: cv == cs == A * B
.data
A:  .word 1, 2, 3, 4,  5, 6, 7, 8,  9, 10, 11, 12,  13, 14, 15, 16
B:  .word 1, 0, 2, -1,  3, 1, 0, 2,  -2, 4, 1, 0,  0, -3, 2, 5
cv: .zero 64
cs: .zero 64
.text
    # ---- vector version
    vsetivli t0, 4, e32, m1, ta, ma
    la   s1, A
    la   s3, cv
    li   s4, 0               # i
vrow:
    vmv.v.i v1, 0            # accumulator for row i
    la   s2, B
    li   s5, 0               # k
vk:
    lw   t1, 0(s1)           # A[i][k]
    vle32.v v2, (s2)         # row k of B
    vmacc.vx v1, t1, v2      # acc += A[i][k] * B[k][:]
    addi s1, s1, 4
    addi s2, s2, 16
    addi s5, s5, 1
    li   t2, 4
    blt  s5, t2, vk
    vse32.v v1, (s3)
    addi s3, s3, 16
    addi s4, s4, 1
    li   t2, 4
    blt  s4, t2, vrow

    # ---- scalar reference
    li   s4, 0               # i
srow:
    li   s6, 0               # j
scol:
    li   s7, 0               # sum
    li   s5, 0               # k
sk:
    slli t3, s4, 4           # &A[i][k] = A + 16*i + 4*k
    slli t4, s5, 2
    add  t3, t3, t4
    la   t5, A
    add  t3, t3, t5
    lw   t1, 0(t3)
    slli t3, s5, 4           # &B[k][j] = B + 16*k + 4*j
    slli t4, s6, 2
    add  t3, t3, t4
    la   t5, B
    add  t3, t3, t5
    lw   t2, 0(t3)
    mul  t1, t1, t2
    add  s7, s7, t1
    addi s5, s5, 1
    li   t2, 4
    blt  s5, t2, sk
    slli t3, s4, 4           # &cs[i][j]
    slli t4, s6, 2
    add  t3, t3, t4
    la   t5, cs
    add  t3, t3, t5
    sw   s7, 0(t3)
    addi s6, s6, 1
    li   t2, 4
    blt  s6, t2, scol
    addi s4, s4, 1
    li   t2, 4
    blt  s4, t2, srow
