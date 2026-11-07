# Black-Bored-By-Numbers

Generates chalk-on-blackboard videos of column arithmetic (addition, subtraction,
multiplication and long division), including operands with 1000+ digits. Each
frame rasterises only the strokes visible to the camera, and the camera zooms
out to fit what was recently written.

## Dependencies

- A C++17 compiler
- FreeType 2 development headers (`pkg-config --cflags freetype2`)
- `ffmpeg` (encoding)
- `bc` (the results are cross-checked against `bc` while generating)
- `DejaVuSansMono.ttf` (default: `/usr/share/fonts/truetype/dejavu/`)

On Debian/Ubuntu:

```sh
sudo apt install g++ libfreetype6-dev pkg-config ffmpeg bc fonts-dejavu-core
```

## Build

```sh
make
```

## Usage

```sh
./board ops.txt out.mp4 [--minutes M | --speed S] [--decimals N] [--encoder h264_nvenc] [--dry]
```

Both the input file and the output file are required. The output may also be
given with `-o out.mp4`. For example:

```sh
./board examples.txt examples.mp4
```

- `--dry` parses the input and verifies the arithmetic without rendering.
- `--speed` scales the animation speed (larger is faster).
- `--minutes` instead targets a total video length.
- `--decimals` sets the maximum number of quotient decimals in division (default 4).

Input is one operation per line. Decimal comma and spaces as thousands
separators are supported, as are `+ - * x × − / : ÷` and optional variable
names with a LaTeX-style index:

```text
s = 1 234,5 + 678,75
d = 5 002,3 - 87,65
a = 4,7 * 13,6
q_{i+1} = 9 175,5 / 25
```

See `examples.txt` for a complete example.
