# gather_scatter.s - indexed accesses.
# Gather: out[i] = table[idx[i]] with vluxei32 (byte offsets = idx * 4).
# Scatter: rev[7 - i] = out[i] with an ordered indexed store (vsoxei32).
# Results: out = {80, 10, 40, 40, 70, 0, 20, 50}, rev = {50, 20, 0, 70, 40, 40, 10, 80}
.data
table: .word 0, 10, 20, 30, 40, 50, 60, 70, 80, 90
idx:   .word 8, 1, 4, 4, 7, 0, 2, 5
rpos:  .word 7, 6, 5, 4, 3, 2, 1, 0
out:   .word 0, 0, 0, 0, 0, 0, 0, 0
rev:   .word 0, 0, 0, 0, 0, 0, 0, 0
.text
    vsetivli t0, 8, e32, m2, ta, ma
    la   a1, idx
    vle32.v v2, (a1)
    vsll.vi v2, v2, 2            # element indices -> byte offsets
    la   a2, table
    vluxei32.v v4, (a2), v2      # gather
    la   a3, out
    vse32.v v4, (a3)
    la   a1, rpos
    vle32.v v6, (a1)
    vsll.vi v6, v6, 2
    la   a4, rev
    vsoxei32.v v4, (a4), v6      # scatter
