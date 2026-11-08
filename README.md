# Black-Bored-By-Numbers

Generates chalk-on-blackboard videos of column arithmetic (addition, subtraction,
multiplication and long division), including operands with 1000+ digits. Each
frame rasterises only the strokes visible to the camera. By default the camera
stays at the board origin and only zooms out, fitting everything written so far,
so the view never pans back and forth (long division in particular stays stable).

## Dependencies

- A C++17 compiler
- FreeType 2 development headers (`pkg-config --cflags freetype2`)
- `ffmpeg` (encoding)
- `bc` (the results are cross-checked against `bc` while generating)
- `Courier New` (default: `/usr/share/fonts/truetype/msttcorefonts/cour.ttf`),
  a fixed-width font with a plain zero. If Courier New is not installed, the
  program falls back to Nimbus Mono PS, FreeMono, Noto Mono or DejaVu Sans Mono.
  Use `--font PATH` to choose another fixed-width font.

On Debian/Ubuntu:

```sh
sudo apt install g++ libfreetype6-dev pkg-config ffmpeg bc fonts-urw-base35
```

Courier New comes from `ttf-mscorefonts-installer` (contrib). Without it the
free fallback (Nimbus Mono PS) is used.

## Build

```sh
make
```

## Usage

```sh
./board ops.txt out.mp4 [--minutes M | --speed S] [--decimals N] [--font PATH] [--encoder h264_nvenc]
                         [--camera fit|follow] [--zoom-lead S] [--auto-speed R] [--max-auto-speed X] [--dry]
                         [--only-show-results]
```

Both the input file and the output file are required, except with
`--only-show-results` where no output file is needed. The output may also be
given with `-o out.mp4`. For example:

```sh
./board examples.txt examples.mp4
```

- `--dry` parses the input and verifies the arithmetic without rendering.
- `--only-show-results` parses the input, computes each arithmetic result and
  prints just the numeric values to stdout, one per line, in input order,
  without thousand separators (the decimal comma is kept). Formula lines are
  printed unchanged. Nothing is rendered, so no output file, font or `ffmpeg`
  is needed.
- `--speed` scales the animation speed (larger is faster).
- `--minutes` instead targets a total video length.
- `--camera fit` (default) keeps the camera fixed at the origin and only zooms
  out until all writing fits. `--camera follow` restores the old behaviour that
  pans to follow recent writing.
- `--zoom-lead S` sets how many seconds ahead the zoom-out is eased (default
  `0.6`). Larger values zoom out earlier and more gently.
- `--auto-speed R` speeds up scenes whose final zoom is large, because very small
  writing (huge operands) is not readable in detail anyway. The boost is
  `finalZoom^R`, capped by `--max-auto-speed`, and applied on top of `--speed`.
  Default `R = 0.5`; use `--auto-speed 0` to disable it.
- `--max-auto-speed X` caps that boost (default `8`). With `--minutes`, the
  automatic boost redistributes time between scenes but still targets the
  requested total length.
- `--decimals N` additionally caps the number of quotient decimals in division.
  By default the quotient keeps at most `2 * max(A, B)` significant decimals,
  where `A` and `B` are the digit counts of the operands (leading zeros
  excluded). Leading zeros right after the comma do not count, so `1 / 3`
  becomes `0,33` instead of running forever, and `0,001 / 0,5` is still
  `0,002`. Integer digits are always kept in full. A per-line
  `\meta div_digits=N` overrides this (see below).

Input is one operation per line. Decimal comma and spaces as thousands
separators are supported, as are `+ - * x × − / : ÷` and optional variable
names with a LaTeX-style index (`x_2`, `x_ 2`, `q_{i+1}`):

```text
s = 1 234,5 + 678,75
d = 5 002,3 - 87,65
a = 4,71 * 13,6
q_{i+1} = 9 175,5 / 25
```

A meta command can be appended to the end of any line. It is stripped before
parsing and applies to that line only, e.g.:

```text
x = 1 / 3 \meta div_digits=10
```

`div_digits=N` makes the division on that line keep at most `N` significant
digits in the quotient. Leading zeros do not count and integer digits are
always kept in full. It overrides both the default `2 * max(A, B)` rule and
`--decimals`.

Square roots are written `sqrt{N}` (no backslash) and are computed and drawn
with the longhand square-root method, which is laid out like long division:
the root above the radicand, the trial divisors on the left and the products
subtracted underneath. An optional variable name works as for the other
operations:

```text
a = sqrt{2}
sqrt{152,29}
```

By default the root keeps at most `2 * D` significant digits, where `D` is the
number of digits in the radicand (leading zeros excluded), mirroring the
division rule. Leading zeros do not count and integer digits are always kept
in full. A per-line `\meta sqrt_digits=K` overrides this:

```text
sqrt{2} \meta sqrt_digits=20
```

Lines containing a backslash (or wrapped in `$...$`) are rendered as math
formulas instead. A small LaTeX subset is supported: `\frac{}{}`, `\sqrt{}`,
`\left...\right`, `_`/`^` scripts, groups, and symbols such as `\approx`,
`\times`, `\cdot`, `\pi` and `\infty`:

```text
π \approx 3,141 6
x_{n+1} = \frac{1}{2}\left(x_n + \frac{N}{x_n}\right)
```

To force one or more lines to be rendered as formulas without being parsed as
arithmetic operations, wrap them in `\begin`/`\end`. The environment name is
optional, and if both markers have one they must match:

```text
\begin{latex}
x_ 2 = 2,3333 / 2
\frac{a}{b} = \sqrt{x}
\end{latex}
```

Each non-empty line inside the block becomes its own formula scene; the
`\begin`/`\end` markers themselves are not rendered. Blank lines and comments
are ignored, and the contents are not checked against `bc`.

See `examples.txt` for a complete example.
