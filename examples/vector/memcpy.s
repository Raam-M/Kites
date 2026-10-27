# memcpy.s - byte copy with e8/m8 vectors (RVV spec, Appendix B).
# Copies the 37-byte string at src to dst; one iteration moves up to VLMAX = 128 bytes.
# Result: dst holds the same bytes as src (including the terminating zero).
.data
src: .string "Vectors make memcpy short and simple."
dst: .zero 40
.text
    la   a0, dst         # destination
    la   a1, src         # source
    li   a2, 38          # bytes to copy (37 characters + NUL)
    mv   a3, a0
loop:
    vsetvli t0, a2, e8, m8, ta, ma
    vle8.v  v0, (a1)
    add  a1, a1, t0
    sub  a2, a2, t0
    vse8.v  v0, (a3)
    add  a3, a3, t0
    bnez a2, loop
