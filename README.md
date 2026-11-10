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
                         [--format 360p|480p|720p|1080p|1440p|2160p|WxH]
                         [--camera fit|follow] [--zoom-lead S] [--auto-speed R] [--max-auto-speed X]
                         [--max-scene-seconds S] [--expand] [--show-expanded] [--stats] [--dry]
                         [--only-show-results]
```

Both the input file and the output file are required, except with
`--only-show-results`, `--show-expanded` or `--stats`, where no output file is
needed. The output may also be given with `-o out.mp4`. For example:

```sh
./board examples.txt examples.mp4
```

- `--dry` parses the input and verifies the arithmetic without rendering.
- `--expand` enables the small algorithm preprocessor described under
  "Algorithms and the expander". Without it, `\let`, `\calc`, `\repeat`,
  `\if` and `\begin{algorithm}` are not interpreted.
- `--show-expanded` implies `--expand` and prints the expanded input lines to
  stdout, one per line, then exits without rendering. This is useful for seeing
  what `\let`, `\calc`, `\repeat`, `\if` and `${...}` produced. It cannot be
  combined with `--only-show-results`.
- `--only-show-results` parses the input, computes each arithmetic result and
  prints just the numeric values to stdout, one per line, in input order,
  without thousand separators (the decimal comma is kept). Formula lines are
  printed unchanged. Nothing is rendered, so no output file, font or `ffmpeg`
  is needed.
- `--speed S` scales the whole timeline uniformly (`--speed 2` is twice as
  fast). Use the per-scene options below when long operations should be sped up
  more than short ones.
- `--minutes M` instead targets a total video length, distributing the time
  across scenes.
- `--camera fit` (default) keeps the camera fixed at the origin and only zooms
  out until all writing fits. `--camera follow` restores the old behaviour that
  pans to follow recent writing.
- `--zoom-lead S` sets how many seconds ahead the zoom-out is eased (default
  `0.6`). Larger values zoom out earlier and more gently.
- `--auto-speed R` speeds up scenes whose final zoom is large, because very small
  writing (huge operands) is not readable in detail anyway. The boost is
  `finalZoom^R`, capped by `--max-auto-speed`, and applied on top of `--speed`.
  Default `R = 0.5`; use `--auto-speed 0` to disable it. Larger `R` compresses
  big scenes more aggressively.
- `--max-auto-speed X` caps the zoom-based boost (default `8`). It does not
  limit `--max-scene-seconds`.
- `--max-scene-seconds S` guarantees that no single scene runs longer than `S`
  seconds (default `30`; `0` disables the cap). Long operations are sped up
  individually until they fit, while short scenes keep their natural pace. This
  is the main control for stopping one huge division or square root from
  dominating the video. It is applied before `--speed`/`--minutes`.
- `--stats` prints a per-scene table (strokes, raw seconds, zoom, boost, final
  seconds) and exits without rendering. This makes it easy to see which
  operation is expensive and to tune the speed options. It needs a font but no
  output file or `ffmpeg`.
- `--format NAME` sets the output resolution. Presets are `360p` (640x360),
  `480p` (854x480), `720p` (1280x720, the default), `1080p` (1920x1080),
  `1440p` (2560x1440) and `2160p`/`4k` (3840x2160). A custom `WxH` such as
  `--format 1920x800` is also accepted. Odd dimensions are rounded up to the
  next even value because the encoder uses yuv420p. The layout is scaled from
  the 1280x720 design by the smaller of `W/1280` and `H/720`, so text keeps its
  apparent size at higher resolutions.
- `--decimals N` additionally caps the number of quotient decimals in division.
  By default the quotient keeps at most `2 * max(A, B)` significant decimals,
  where `A` and `B` are the digit counts of the operands (leading zeros
  excluded). Leading zeros right after the comma do not count, so `1 / 3`
  becomes `0,33` instead of running forever, and `0,001 / 0,5` is still
  `0,002`. Integer digits are always kept in full. A per-line
  `\meta div_digits=N` overrides this (see below).

### Controlling speed and length

There are two levels of control:

- `--speed` and `--minutes` scale the whole video.
- `--auto-speed`, `--max-auto-speed` and `--max-scene-seconds` act per scene.
  One expanded line (or one input line) is one scene.

For a long algorithm, `--max-scene-seconds` is usually the option you want. It
leaves short scenes at their natural pace and compresses only the operations
that would otherwise run too long:

```sh
./board examples-expand.txt examples-expand.mp4 --expand --max-scene-seconds 10
```

For a 10-step Newton-Raphson run with 1024-digit precision, the default cap
makes a huge difference:

| Command | Video length |
| --- | ---: |
| `--expand` (default cap: 30 s per scene) | 12.9 min |
| `--expand --max-scene-seconds 20` | 9.8 min |
| `--expand --max-scene-seconds 10` | 5.9 min |
| `--expand --max-scene-seconds 0` | 867.7 min |

Use `--stats` to see where the time goes before rendering:

```sh
./board examples-expand.txt --expand --stats
```

It prints one row per scene and exits without rendering or needing an output
file. A very long division can produce a row like this:

```text
scene  strokes     raw s     zoom    boost   final s
   37   949153 301851.60   394.38 10061.72     30.00  q
```

`raw s` is the scene's natural duration, `boost` is the per-scene speed-up, and
`final s` is its duration in the output video.

To target a specific total length instead, use `--minutes`:

```sh
./board examples-expand.txt examples-expand.mp4 --expand --minutes 5
```

Combine it with `--max-scene-seconds` when you also want a per-scene upper
bound. `--speed` is a simpler uniform multiplier:

```sh
./board examples.txt examples.mp4 --speed 2
```

The order is: zoom-based auto-speed, then the per-scene cap, then the global
`--speed`/`--minutes` scale. Use `--auto-speed 0` to disable the zoom-based
part, and `--max-scene-seconds 0` to disable the per-scene cap.

Input is one operation per line. A decimal comma or a decimal point may be
used (e.g. `1,5` or `1.5`), and spaces may be used as thousands separators.
The operators `+ - * x × − / : ÷` are supported, as are optional variable
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

## Algorithms and the expander

Hand-writing every intermediate line of an algorithm is tedious. With
`--expand`, a few directives are expanded into ordinary arithmetic and formula
lines before anything is rendered. The directives are textual, so the normal
parser still checks every arithmetic line against `bc`.

`\let NAME = VALUE` defines a numeric variable. `VALUE` may use either decimal
separator and may contain `${NAME}` references. Values are stored without
thousand separators and with a decimal comma. `${NAME}` substitutes a variable
anywhere in a later line, including inside math formulas. The `$` in `${...}`
is independent of the `$...$` math delimiters: `$N = ${N}$` expands to
`$N = 2$` before the formula is parsed.

`\calc [NAME =] A OP B [label=VAR] [\meta ...]` renders one arithmetic line and
stores its displayed result. If the left-hand side `NAME` is present it is both
shown and used as the variable name; `label=VAR` stores under `VAR` instead (or
when there is no shown name). For example:

```text
\let N = 2
\let x = 1
\calc q = ${N} / ${x} \meta div_digits=2
\calc s = ${x} + ${q}
\calc x = ${s} / 2 label=x
$x_1 = ${x}$
```

`\repeat VAR = START..END [digits d1 d2 ...] ... \endrepeat` repeats a block.
`${VAR}` is the current iteration number. The optional digit list sets
`div_digits` for division lines in that iteration (one value for all
iterations, or one per iteration); an explicit `\meta div_digits=N` on a line
still wins. The loop variable is restored after the loop.

`\if{CONDITION} ... \else ... \endif` chooses a block at expansion time. The
condition is a numeric comparison evaluated with `bc`; both `,` and `.` work as
the decimal separator, and the usual operators `< <= > >= == !=` (also `=` and
`<>`) are available. Blocks may be nested.

`\begin{algorithm}{TITLE} ... \end{algorithm}` renders `TITLE` as a formula
scene and gives the body its own variable scope: variables defined or changed
inside do not leak out. The title is optional.

A complete Newton–Raphson example:

```text
\let N = 2
\let x = 1
\begin{algorithm}{Newton-Raphson for ${N}}
\repeat k = 1..4 digits 2 4 8 16
\calc q = ${N} / ${x}
\calc s = ${x} + ${q}
\calc x = ${s} / 2 label=x
$x_${k} = ${x}$
\endrepeat
\end{algorithm}
```

Run it with `./board examples-expand.txt out.mp4 --expand`. With
`--only-show-results --expand`, the expanded arithmetic lines print their
numeric results and formula lines print unchanged (the algorithm title is a
formula line).

See `examples.txt` for a complete arithmetic example and
`examples-expand.txt` for the algorithm example above.

## Tests

```sh
make test
```

This builds `board` and runs the regression cases in `tests/cases/`. Each case
is a `NAME.txt` input with expected combined stdout/stderr in `NAME.expected`.
An optional `NAME.args` file supplies extra flags (one per line), and
`NAME.status` sets the expected exit status (default `0`). The tests use
`--only-show-results` and `--show-expanded`, so they need no font or `ffmpeg`.
