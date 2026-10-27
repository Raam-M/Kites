# prefix_sum.s - inclusive prefix sum of 8 elements in log2(8) = 3 slide-and-add steps.
# vslideup leaves elements below the offset unchanged, so v4 is zeroed before each slide.
# Result: out = {1, 3, 6, 10, 15, 21, 28, 36}
.data
in:  .word 1, 2, 3, 4, 5, 6, 7, 8
out: .word 0, 0, 0, 0, 0, 0, 0, 0
.text
    vsetivli t0, 8, e32, m2, ta, ma
    la   a1, in
    vle32.v v2, (a1)
    vmv.v.i v4, 0
    vslideup.vi v4, v2, 1
    vadd.vv v2, v2, v4
    vmv.v.i v4, 0
    vslideup.vi v4, v2, 2
    vadd.vv v2, v2, v4
    vmv.v.i v4, 0
    vslideup.vi v4, v2, 4
    vadd.vv v2, v2, v4
    la   a2, out
    vse32.v v2, (a2)
