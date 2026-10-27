# rgb_to_grey.s - segment loads: convert 10 packed RGB pixels to 8-bit grey.
# vlseg3e8.v splits interleaved R,G,B bytes into v0, v1, v2. The weighted sum
# 77*R + 150*G + 29*B is accumulated at 16 bits (widening multiply-add) and narrowed
# with a shift right by 8.
# Result: grey[i] = (77*R + 150*G + 29*B) >> 8
.data
rgb:  .byte 255, 0, 0,  0, 255, 0,  0, 0, 255,  255, 255, 255,  0, 0, 0
      .byte 128, 128, 128,  200, 100, 50,  10, 20, 30,  90, 180, 45,  33, 66, 99
grey: .zero 10
.text
    li   a0, 10
    la   a1, rgb
    la   a2, grey
    li   t1, 77
    li   t2, 150
    li   t3, 29
    vsetvli t0, a0, e8, m1, ta, ma
    vlseg3e8.v v0, (a1)          # v0 = R, v1 = G, v2 = B
    vwmulu.vx v4, v0, t1         # v4..v5 (16-bit) = 77*R
    vwmaccu.vx v4, t2, v1        # += 150*G
    vwmaccu.vx v4, t3, v2        # += 29*B
    vnsrl.wi v6, v4, 8           # >> 8, back to 8 bits
    vse8.v v6, (a2)
