# ATRAC decoder licensing

The ATRAC3plus decoder in this directory is the project's own implementation. Its constant data
tables are not: they define the bitstream format and must be bit-exact, so they are transcribed
from FFmpeg's ATRAC3+ decoder (`libavcodec/atrac3plus_data.h` and `atrac3plusdsp.c`,
Copyright (c) 2010-2013 Maxim Poliakovski, part of the FFmpeg project), via the standalone
extraction in PPSSPP's `ext/at3_standalone`.

| File | License |
|------|---------|
| `atrac3plus_tables.cpp`, `atrac3plus_tables.h` | **GNU LGPL 2.1 or later** (derived from FFmpeg) |
| all other files in this directory | the project's license |

`atrac3plus_tables.cpp` carries the LGPL notice. The full license text is available at
<https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>.

The tables are linked into `vc3runtime.dll`. To meet the LGPL, the complete source of that DLL,
including these files, is distributed with this repository, so anyone can modify the tables (or
replace the decoder) and rebuild the DLL.

No other FFmpeg or PPSSPP code is included. The decoder logic (bitstream parser, inverse
quantisation, IMDCT, gain compensation, tone synthesis and the inverse PQF filter bank) was
written for this project, using FFmpeg's decoder as a reference for the format.
