# strlen.s - string length with a fault-only-first load (RVV spec, Appendix B).
# vle8ff.v may shorten vl instead of faulting past the end of memory; vmseq/vfirst
# locate the terminating zero.
# Result: a0 = 43
.data
message: .string "The quick brown fox jumps over the lazy dog"
.text
    la   a0, message
    mv   a3, a0              # save start
loop:
    vsetvli a1, x0, e8, m8, ta, ma
    vle8ff.v v8, (a3)
    csrr a1, vl              # bytes actually read
    vmseq.vi v0, v8, 0       # v0[i] = (byte == 0)
    vfirst.m a2, v0          # index of the first zero, or -1
    add  a3, a3, a1
    bltz a2, loop
    add  a0, a0, a1          # start + bump
    add  a3, a3, a2          # pointer to the zero byte + bump
    sub  a0, a3, a0          # length
