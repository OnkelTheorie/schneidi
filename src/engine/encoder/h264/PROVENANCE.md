# Provenance of the s264 encoder

The H.264 encoder in this directory was written for schneidi from the specification and from
published literature. No code, tables or thresholds were copied from another encoder.

## Sources

- **ITU-T Rec. H.264 / ISO/IEC 14496-10** (Advanced Video Coding): bitstream syntax, CAVLC and
  CABAC (context tables, range coder), integer transforms and quantisation, intra prediction,
  inter prediction and sub-pixel interpolation, the deblocking filter, B pictures and direct modes.
  Tables that come from the standard (zig-zag scans, CABAC initialisation, CAVLC code tables,
  dequantisation scales, deblocking thresholds) are taken from the specification text.
- **Published encoding techniques**, implemented from their descriptions:
  - Lagrangian rate-distortion optimisation of mode and motion decisions
    (Sullivan & Wiegand, "Rate-distortion optimization for video compression", 1998;
    Wiegand et al., "Rate-constrained coder control and comparison of video coding standards", 2003),
    including the lambda-QP relation used by the H.264 reference software (JM).
  - Trellis (rate-distortion optimised) quantisation with CABAC bit costs, a dynamic programme over
    the coefficients in reverse scan order.
  - SATD (Hadamard-transformed differences) as the cost of motion and mode estimates.
  - Macroblock-tree / temporal propagation of block costs through the motion vectors in a
    lookahead, turned into QP offsets.
  - Variance-based adaptive quantisation.
  - Two-pass rate control from a first-pass complexity log.
  - Psycho-visual rate-distortion: penalising the loss of texture energy in the mode decision
    (here measured coefficient by coefficient on Hadamard magnitudes of source and reconstruction).
  - Dropping residual groups that do not pay for themselves in rate-distortion terms (the
    thresholds and the bit model in `planInterLuma` were fitted by measurement on our own test
    material).

  - Speed shortcuts of the mode decision (round 6, 14.11.2026), all our own and tuned by BD-rate
    measurement on our test material: sub-pel refinement that measures the four direct neighbours
    and only the two diagonals beside the best one (a reduced search pattern of the kind described
    throughout the fast-motion-estimation literature); skipping work that the 8x8 trellis plan of
    the 16x16 prediction shows to be pointless (late skip, no split search, no 4x4 plan for empty
    quadrants); scene-change detection that stops once its threshold comparison is decided; the
    first pass analysing consecutive pictures side by side.

- Two-pass rate control (14.11.): the bit model's k learned per class of picture cost, a limit
  on how far a picture's QP may leave the whole-clip plan, and complexity compression (a
  picture's quantiser scale following a power of its smoothed complexity, the "qcomp" idea
  described in public rate-control literature and encoder documentation); all own code, tuned
  by measurement.

## Other encoders

x264 (and OpenH264) were used only as external benchmark binaries: their output was measured
(bitrate, PSNR, SSIM, VMAF, speed) to compare against. Their source code was not used as a
template, and no part of it is included here. Comments that mention them only report such
measurements.
