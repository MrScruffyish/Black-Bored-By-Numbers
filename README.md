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
- `bc` (simple two-operand results are cross-checked against `bc` while generating)
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
                         [--max-scene-seconds S] [--threads N] [--expand] [--show-expanded]
                         [--syntax-check] [--stats] [--dry] [--only-show-results]
```

Both the input file and the output file are required, except with
`--only-show-results`, `--show-expanded`, `--syntax-check` or `--stats`, where no
output file is needed. The output may also be given with `-o out.mp4`. For
example:

```sh
./board examples.txt examples.mp4 --expand
```

- `--dry` parses the input and verifies the arithmetic without rendering.
- `--expand` enables the small algorithm preprocessor described under
  "Algorithms and the expander". Without it, `\repeat`, `\if` and
  `\begin{algorithm}` are not interpreted.
- `--show-expanded` implies `--expand` and prints the expanded input lines to
  stdout, one per line, then exits without rendering. This is useful for seeing
  what `\repeat`, `\if` and the other expander directives produced. It cannot
  be combined with `--only-show-results`.
- `--syntax-check` parses the input and reports every problem it finds, one per
  line, prefixed with the line number. It prints `syntax OK: N lines` and exits
  with status 0 when the file is valid, or lists the issues and exits with
  status 1 otherwise. It does not render anything and needs no output file,
  font or `ffmpeg`. It may be combined with `--expand`, but not with
  `--show-expanded` or `--only-show-results`.
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
- `--threads N` sets how many frames are rasterised in parallel. `--threads 1`
  renders serially. `--threads 0` (the default) picks one fewer than the
  available CPU cores, up to 8. Frames are still written to `ffmpeg` in order,
  so the output is identical. Leave some cores for `ffmpeg`: on a 4-core
  machine, `--threads 2` or `3` is usually a good choice.
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

### Shell completion

`board-completion.bash` completes the options and the input/output filenames in
bash. With the `bash-completion` package installed, install it as a per-command
completion script:

```sh
mkdir -p ~/.local/share/bash-completion/completions
cp board-completion.bash ~/.local/share/bash-completion/completions/board
```

Then `./board <TAB>` lists the options. The file name in the completions
directory must be `board` without the `.bash` suffix, because bash-completion
falls back to the command's basename when the command is written with a path.
Without bash-completion, source the script from `~/.bashrc` instead:

```sh
source /path/to/board-completion.bash
```

Completion covers every option, the `--format` presets, `--camera fit|follow`,
the common `--encoder` values, `.txt` input files and `.mp4` output files.

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
./board examples.txt examples.mp4 --expand --speed 2
```

The order is: zoom-based auto-speed, then the per-scene cap, then the global
`--speed`/`--minutes` scale. Use `--auto-speed 0` to disable the zoom-based
part, and `--max-scene-seconds 0` to disable the per-scene cap.

### Rendering performance

Rendering is split into independent whole frames. Each worker rasterises a
complete frame into its own buffer, and a writer feeds the completed frames to
`ffmpeg` in order. That means:

- `--threads N` controls the number of frame workers (`0` = automatic).
- `--threads 1` is the old single-threaded path.
- The output is frame-for-frame identical to serial rendering.
- Only pixels touched by chalk are colour-converted; the empty board is
  pre-filled, which makes the colour pass much cheaper.

`ffmpeg` also uses several cores, so do not give all cores to `board`. For
example:

```sh
./board nrlarge.txt nrlarge.mp4 --expand --max-scene-seconds 10 --threads 2
```


## Tests

```sh
make test
```

This builds `board` and runs the regression cases in `tests/cases/`. Each case
is a `NAME.txt` input with expected combined stdout/stderr in `NAME.expected`.
An optional `NAME.args` file supplies extra flags (one per line), and
`NAME.status` sets the expected exit status (default `0`). The tests use
`--only-show-results`, `--show-expanded` and `--syntax-check`, so they need no
font or `ffmpeg`.


# Input and Language

Input is a text file with one expression per line.  Comments start with # and
from there on, the rest of the line is ignored.


## Numbers
A decimal comma or a decimal point may be used (e.g. `1,5` or `1.5` are equal),
and spaces may be used as thousands separators.

## Simple operators
The operators `+ - * /` are supported:

```text
5   # This is a valid statement, there is no calculation, just setting the value 5.
1 234,5 + 678,75
5 002,3 - 87,65
4,71 * 13,6
9 175,5 / 25
```

Parentheses `()` and multiple operations may be used on one line. `*` and `/`
bind more tightly than `+` and `-`, and parentheses override that. The whole
expression is worked out on one board: each operation is drawn with the normal
column working, left to right in evaluation order, wrapping to a new row when
the row gets too wide. The original expression is the title and the final result
is repeated at the bottom:

```text
(1+1)*(2+2.1)
2 + 3 * 4
```

Each division is worked to a finite decimal. `\meta div_digits=K` keeps at most
`K` significant digits in total; otherwise the division keeps at most
`2 * max(A, B)` significant fractional digits, where `A` and `B` are that
division's operands. Leading zeros after the comma are free and integer digits
are always kept in full. The displayed quotient is what the following operations
use, so the worked board and the result always agree. `+`, `-`, and `*` are
exact. A quotient that would need more digits is truncated rather than rounded;
for example `1/8 * 2` works `1 / 8 = 0,12` and then `0,12 * 2 = 0,24`.


Long multiplication is drawn with the usual partial products. Leading zero
digits are not drawn in those rows, so `0,0012 * 0,0012` shows `24` and `12`
rather than rows of zeros; the result still shows the zeros its value needs
(`0,00000144`).

A meta command can be appended to the end of any line. It is stripped before
parsing and applies to that line only, e.g.:

```text
1 / 3 \meta div_digits=10
```

`div_digits=N` makes a line containing division keep at most `N` significant
digits in its result. Leading zeros do not count and integer digits are always
kept in full. It overrides both the default `2 * N` rule and `--decimals`.

Square roots are written `\sqrt{N}`, using the same command as LaTeX, and are
computed and drawn with the longhand square-root method, which is laid out like
long division: the root above the radicand, the trial divisors on the left and
the products subtracted underneath. A line containing only `\sqrt{NUMBER}` is
arithmetic and is calculated. Inside `$...$` or a `\begin{latex}` block the
same command is rendered as a formula instead. An optional variable name works
as for the other operations:

```text
\sqrt{2}
\sqrt{152,29}
```

`N` may use a decimal comma or point. The digits are paired outwards from the
decimal point, and an odd fractional tail is padded with a zero. For example,
`\sqrt{152,29}` works on the pairs `1`, `52`, `29`, so the root has two integer
digits and starts `12,34...`. The default result is `12,34058345`. An exact
root stops as soon as the remainder is zero, so `\sqrt{152,2756}` is `12,34`.

By default the root keeps at most `2 * D` significant digits, where `D` is the
number of digits in the radicand (leading zeros excluded), mirroring the
division rule. Leading zeros do not count and integer digits are always kept
in full. Digits beyond the limit are truncated, not rounded. A per-line
`\meta sqrt_digits=K` overrides this:

```text
\sqrt{2} \meta sqrt_digits=20
\sqrt{152,29} \meta sqrt_digits=6
```

The second line gives `12,3405`. Write `\sqrt`, not `sqrt`: the backslash marks
the special command, exactly as in LaTeX.



## Variables

A variable is a name that stores the value shown on the board. Assign with `=`,
and use the bare name wherever a number is allowed:

```text
5           # a value on its own
a = 5       # a = 5
a           # 5
b = a       # b = 5
age = 1+2   # works 1+2, then shows age = 3
x = age + 1 # x is now 4
```

Assignments and bare values are shown on one line. An assignment whose
right-hand side is a calculation is also worked out on the board, then the
final assignment is shown. Names are case-sensitive and may not contain spaces
around `_`: `x_ 2` is an error.

Values are stored as the decimal that was displayed, not as an exact fraction.
So after `q = 1/3` (which displays `0,33`), `q * 3` is `0,99`.

### Index

Variable names can carry a LaTeX-style index (`x_2`, `q_{i+1}`). An index made
only of digits is part of the name: `x_01` is an error, and `x_{123}` is the
same variable as `x_123`.

An index containing names is evaluated when every name in it already has a
value; otherwise the whole index is literal text:

```text
x_123 = 1+4
q_{n+1} = 2       # n is undefined, so this is the literal name q_{n+1}
x_123 * q_{n+1}   # 5 * 2 = 10
```

With index variables you can iterate:

```text
s_0 = 0
i = 0
s_{i+1} = s_i + i  # i is 0, so this assigns s_1 = 0
i = i + 1
s_{i+1} = s_i + i  # s_2 = s_1 + 1 = 1
i = i + 1
s_{i+1} = s_i + i  # s_3 = s_2 + 2 = 3
```

An evaluated index must be a non-negative integer. Index expressions accept the
same `+ - * /` and parentheses as ordinary arithmetic; a division must produce
an integer, otherwise the line is an error. A symbolic index such as `i+1`
stays literal and is rendered with LaTeX. Write `@` to force evaluation, for
example `s_@{i+1}`.

## LaTeX

Lines wrapped in `$...$`, or containing a LaTeX command (a backslash) or a
symbol such as `≈`, `≤` or `≥`, are rendered as math formulas instead of
arithmetic. A small LaTeX subset is supported: `\frac{}{}`, `\sqrt{}`,
`\left...\right`, `_`/`^` scripts, groups, and symbols such as `\approx`,
`\times`, `\cdot`, `\pi` and `\infty`:

```text
$π \approx 3,141 6$
$x_{n+1} = \frac{1}{2}\left(x_n + \frac{N}{x_n}\right)$
```

LaTeX text is literal: `x_i` stays `x_i` even when `i` has a value. Put `@`
before a name or `{expression}` to substitute its value, as in `$x_@i$` or
`$s_@{i+1}$`.

To force one or more lines to be rendered as formulas without being parsed as
arithmetic operations, wrap them in `\begin`/`\end`. The environment name is
optional, and if both markers have one they must match:

```text
\begin{latex}
x_2 = 2,3333 / 2
\frac{a}{b} = \sqrt{x}
\end{latex}
```

Each non-empty line inside the block becomes its own formula scene; the
`\begin`/`\end` markers themselves are not rendered. Blank lines and comments
are ignored, and the contents are not checked against `bc`.

## Value substitution

Inside LaTeX text, indices are literal. Put `@` before a name to force its
value into the formula:

```text
k = 1
$ a_k = 2 $    # writes  a_k = 2
$ a_@k = 2 $   # writes  a_1 = 2
```

Outside LaTeX the `@` is optional, because an index is evaluated automatically
when its names have values. Here `a_@k` is the variable `a_1`:

```text
a_@k = 5   # works because k is 1
a_@k       # 5
```

If a name in a forced index has no value, the line is an error.

## Algorithms and the expander

Hand-writing every intermediate line of an algorithm is tedious. With
`--expand`, a few directives are expanded into ordinary arithmetic and formula
lines before anything is rendered. The directives are textual, so the normal
parser still checks every arithmetic line. A `#` comment may follow any
directive; it is ignored by the expander.

Ordinary assignments work as everywhere else, and the expander remembers them
so that `\if` conditions and `\repeat` bounds can use them. Inside formula text,
put `@` before a name to substitute its value; in arithmetic lines the bare name
is enough. The value stored is the displayed, already-truncated result, so later
references see exactly what the viewer saw:

```text
N = 2
x = 1
q = N / x \meta div_digits=2
s = x + q
x = s / 2
$x_1 = @x$
```

`\repeat VAR = START..END [digits d1 d2 ...] ... \endrepeat` repeats a block.
`START` and `END` may be integers or names of variables that already have a
value. `VAR` is the current iteration number. In arithmetic lines the bare name
`VAR` is replaced by that number; in formula text use `@VAR` (or `@{...}`). The
optional digit list sets `div_digits` for division lines in that iteration (one
value for all iterations, or one per iteration); an explicit
`\meta div_digits=N` on a line still wins. The loop variable is restored after
the loop.

`\if{CONDITION} ... \else ... \endif` chooses a block at expansion time. The
condition is a numeric comparison evaluated with `bc`; both `,` and `.` work as
the decimal separator, and the usual operators `< <= > >= == !=` (also `=` and
`<>`) are available. Conditions may use variable names directly. Blocks may be
nested.

`\begin{algorithm}{TITLE} ... \end{algorithm}` renders `TITLE` as a formula
scene. The title may contain `@name` substitutions. Variables are global: an
assignment inside an algorithm is visible afterwards. The title is optional.

A complete Newton–Raphson example:

```text
N = 2
x = 1
\begin{algorithm}{Newton-Raphson for @N}
\repeat k = 1..4 digits 2 4 8 16
q = N / x
s = x + q
x = s / 2
$x_@k = @x$
\endrepeat
\end{algorithm}
```

Run it with `./board examples-expand.txt out.mp4 --expand`. With
`--only-show-results --expand`, the expanded arithmetic lines print their
numeric results and formula lines print unchanged (the algorithm title is a
formula line).

The older `\let`, `\calc` and `${...}` forms are no longer supported: write an
ordinary assignment and use a bare name or `@name` instead.

See `examples.txt` for a complete tour of the input language (run it with
`--expand`) and `examples-expand.txt` for the Newton–Raphson example on its
own.
