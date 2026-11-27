# Media fixtures

Pixel coordinates are zero-based from the top left. Expected colors are
packed 8-bit RGB after compositing against black.

- `two-colors.gif`, `two-colors.svg`, `two-colors.bmp`, `two-colors.tiff` and
  `two-colors.webp` each contain one 8 × 4 frame with exact red (255, 0, 0) at
  (1, 2) and blue (0, 0, 255) at (6, 2). The GIF starts at 0 ms and has a
  100 ms frame delay. The SVG has no source timing.
- `alpha.png` contains one 8 × 4 frame. At row 2 the RGBA values at x = 1, 3,
  5 and 7 are (255, 32, 64, 255), (240, 126, 20, 128), (20, 240, 126, 0)
  and (0, 0, 255, 255). The resulting RGB values are (255, 32, 64),
  (120, 63, 10), black and blue. The build copies this fixture to a
  renamed file, a literal percent-pattern filename and numbered siblings.
- `alpha-edge.svg` contains one 8 × 4 frame. At row 1, x = 1 is clear,
  x = 3 is half-opacity orange (128, 64, 0), and x = 2 and 6 are the
  antialiased edges (64, 32, 0). Cairo rounding can differ by 1 per channel.
- `anamorphic.png` contains a 32 × 16 frame. FFmpeg reports 2:1 sample aspect
  from its pHYs metadata, giving a 64 × 16 native display. The equal-width
  vertical bars are red, green, blue and white from left to right.
- `circle-landscape.svg` and `circle-portrait.svg` contain a centered blue
  600 × 600 circle against solid red. Their 1920 × 1080 and 1080 × 1920 canvases
  exercise horizontal and vertical fitting in the live terminal check.
- `oriented.jpg` and `mirrored.jpg` contain one 16 × 12 frame each, with red,
  green, blue and white quadrants in reading order. EXIF orientation 6
  rotates the display clockwise to 12 × 16, with the four colors in the
  positions (9, 2), (9, 13), (2, 2), (2, 13). EXIF orientation 5 transposes
  the display and puts them at (2, 2), (2, 13), (9, 2), (9, 13). Tests allow
  12 levels per channel for JPEG compression.
- `limited-yuv.mkv` contains six reordered H.264 frames at 8 fps, with no
  declared frame count. They are 32 × 16 with 2:1 sample aspect ratio.
  Each has limited-range BT.709 YUV 4:4:4 red, green, blue and white bars at
  y = 12 and x = 4, 12, 20 and 28. At (24, 4), frames show red, green,
  blue, white, black and yellow in display order. PTS values are 0, 125,
  250, 375, 500 and 625 ms. Each duration is 125 ms. Tests allow 6 levels
  per channel for YUV quantization and H.264 compression.
- `covered.mp4` has the same six H.264 frames, an AAC audio stream and a
  separate attached JPEG cover stream. The video has a 90° counterclockwise
  display matrix, so its raster is 16 × 32, its sample aspect becomes 1:2
  and every sample (x, y) above moves to (y, 31 − x).
- `untimed.h264` contains the same six video frames copied as an elementary
  H.264 stream. Its frames lack timestamps. Its bitstream declares 8 fps.
- `variable-rate.mkv` contains four lossless 8 × 4 FFV1 frames filled with
  red, green, blue and white in that order. The source PTS values are 1000,
  1120, 1360 and 1400 ms. The stream has no frame durations and the 25 fps
  guessed rate supplies the final 40 ms interval.
- `once-disposal.gif` has an 8 × 4 canvas and a GIF extension requesting
  infinite looping. Its first frame fills the canvas red and remains under
  subsequent transparent pixels. The second draws a green checkerboard in
  (2, 0)–(5, 2) and restores the previous canvas before the third. The last
  draws blue pixels at (6, 1), (7, 1), (5, 2), (7, 2), (4, 3) and (5, 3),
  and requests background disposal. The frame delays are 2000, 120 and
  310 ms. The composed reference is tested at every pixel.
- `resize-motion.gif` contains three 480 × 300 frames with delays of 4000,
  320 and 300 ms. The first alternates single-pixel red and yellow vertical
  stripes, the second is solid green and the third is solid blue. The first
  SIXEL picture exceeds a pseudoterminal's output buffer so playback can be
  interrupted during an incomplete write.
- `audio-only.wav`, `corrupt.bin` and `secondary-input.ffconcat` exercise
  the CLI diagnostics for audio alone, an invalid bitstream and a manifest
  that would need to open a second local file.

Optional fixture tests for PPM, QOI and JPEG XL use the exact two-color raster
and have a separate `media-optional` CTest label. A missing FFmpeg demuxer
or decoder skips only its corresponding optional case.
