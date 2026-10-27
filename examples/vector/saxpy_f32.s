# saxpy_f32.s - y = a * x + y for 10 single-precision values (RVV spec, Appendix B).
# vfmacc.vf computes vd = (f[rs1] * vs2) + vd with one rounding (fused multiply-add).
# Result: y[i] = 2.5 * x[i] + y[i]
.data
alpha: .float 2.5
x: .float 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0
y: .float 0.5, 0.5, 0.5, 0.5, 0.5, -1.0, -1.0, -1.0, -1.0, -1.0
.text
    la   t0, alpha
    flw  fa0, 0(t0)
    li   a0, 10
    la   a1, x
    la   a2, y
saxpy:
    vsetvli a4, a0, e32, m8, ta, ma
    vle32.v v0, (a1)
    sub  a0, a0, a4
    slli a4, a4, 2
    add  a1, a1, a4
    vle32.v v8, (a2)
    vfmacc.vf v8, fa0, v0
    vse32.v v8, (a2)
    add  a2, a2, a4
    bnez a0, saxpy
