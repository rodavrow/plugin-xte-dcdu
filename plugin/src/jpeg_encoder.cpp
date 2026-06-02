// XTE-DCDU: libjpeg-turbo wrapper
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jpeg_encoder.hpp"
#include "log.hpp"

#include <turbojpeg.h>

#include <cstring>

namespace xtedcdu {

size_t encode_jpeg_rgb(const uint8_t* rgb, int w, int h, int quality,
                       std::vector<uint8_t>& out) {
    if (!rgb || w <= 0 || h <= 0) return 0;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;

    tjhandle tj = tjInitCompress();
    if (!tj) {
        XTED_ERR("jpeg: tjInitCompress failed");
        return 0;
    }

    unsigned char* jpeg_buf = nullptr;
    unsigned long  jpeg_size = 0;

    // 4:4:4 (no chroma subsampling) + accurate DCT: small image of high-
    // contrast text. 4:2:0 causes visible color fringing around letter
    // edges; FASTDCT adds extra blocking. Size cost vs 4:2:0/FASTDCT is
    // ~40% but the absolute payload is still tiny (269x201 q=90 ~10KB).
    int rc = tjCompress2(tj,
                         rgb, w, /*pitch*/ 0, h,
                         TJPF_RGB,
                         &jpeg_buf, &jpeg_size,
                         TJSAMP_444,
                         quality,
                         TJFLAG_ACCURATEDCT);
    if (rc != 0) {
        XTED_ERR("jpeg: tjCompress2 failed: %s", tjGetErrorStr2(tj));
        if (jpeg_buf) tjFree(jpeg_buf);
        tjDestroy(tj);
        return 0;
    }

    out.assign(jpeg_buf, jpeg_buf + jpeg_size);
    tjFree(jpeg_buf);
    tjDestroy(tj);
    return out.size();
}

} // namespace xtedcdu
