# FFmpeg Bink codec adaptation

The Bink video tables and portable decoder kernels in `src/media` were adapted
from FFmpeg tag `n4.4`, commit
`09c358362008e2d04cec8239526c6827543da4cf`. The scaled-block correction was
carried forward from FFmpeg commit
`e1c1b6c55835d13647636340d478d370aa48fb8d`.

The imported code has been reduced to a direct C++20 decoder with
sl_open-owned fixed frame storage. It does not link to FFmpeg and does not
use the FFmpeg public or internal APIs. The derived files retain their original
copyright notices and are covered by the accompanying LGPL 2.1-or-later
license. This notice does not license the sl_open project as a whole.

Derived files:

- `src/media/bink_audio.cpp`
- `src/media/bink_data.hpp`
- `src/media/bink_dsp.cpp`
- `src/media/bink_dsp.hpp`
- `src/media/bink_video.cpp`
