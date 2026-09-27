// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The JPEG decoder: ITU-T T.81 with the JFIF, Adobe and EXIF conventions a
// photograph actually carries, written for this library and depending on
// nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <cworks/task.hpp>

#include "limits.hpp"
#include "raster_image.hpp"

namespace cplot {

/// The coding process a JPEG frame declares (T.81 Table B.1). Only the three
/// Huffman-coded DCT processes are decoded; the rest are refused.
enum class JpegCoding {
    Baseline,           ///< SOF0: sequential, 8-bit, two tables of each kind
    ExtendedSequential, ///< SOF1: sequential, four tables of each kind
    Progressive,        ///< SOF2: spectral selection and successive approximation
};

/// What the components of a frame mean, as JFIF, the Adobe APP14 marker and
/// the component identifiers together say.
enum class JpegColorModel {
    Gray,  ///< one component
    YCbCr, ///< three components, JFIF / T.871 YCbCr
    Rgb,   ///< three components stored untransformed
    Cmyk,  ///< four components stored untransformed
    Ycck,  ///< four components, YCbCr for C, M, Y plus K (Adobe transform 2)
};

/// What a JPEG stream is, as far as a consumer that does not look at its
/// pixels needs to know — enough to embed it unchanged in a document format
/// that decodes JPEG itself.
struct JpegInfo {
    int width = 0;      ///< samples per line of the frame, as stored
    int height = 0;     ///< lines of the frame, as stored
    int components = 0; ///< 1, 3 or 4
    JpegCoding coding = JpegCoding::Baseline;
    JpegColorModel color_model = JpegColorModel::Gray;
    /// The four components follow the Adobe convention — 0 is full ink —
    /// rather than T.81's. True exactly when a four-component stream carries
    /// the Adobe APP14 marker; such samples are inverted before use.
    bool inverted_cmyk = false;
    /// EXIF orientation (TIFF tag 274): 1 = as stored, 2–8 the seven
    /// mirrorings and rotations. 1 when the stream has no EXIF, or a value
    /// outside the defined range. decode_jpeg() applies it.
    int orientation = 1;
    /// Bytes of an embedded ICC profile (APP2 `ICC_PROFILE`), 0 when there
    /// is none. The decoder does not colour-manage: the profile is reported
    /// so a caller can say so, and is otherwise ignored.
    std::size_t icc_profile_bytes = 0;
    /// Bytes from SOI through EOI. Anything after EOI — a phone's appended
    /// video, a second picture — is not part of this stream.
    std::size_t stream_length = 0;

    /// The picture's extent once `orientation` is applied: the stored
    /// dimensions, swapped for the four orientations that turn it on its side.
    int display_width() const noexcept { return orientation >= 5 ? height : width; }
    int display_height() const noexcept { return orientation >= 5 ? width : height; }
};

/// Validate a JPEG stream completely and describe it, without reconstructing
/// its pixels.
///
/// Every marker segment, table, frame and scan header is checked, and every
/// scan's entropy-coded data is decoded to its last coefficient — exactly the
/// work decode_jpeg() does before its inverse DCT. A stream this accepts,
/// decode_jpeg() accepts, so a backend that embeds the bytes unchanged and
/// one that draws the pixels always agree about whether the picture exists.
///
/// Refusals are structured: `InvalidFormat` for bytes that break T.81, JFIF
/// or the Adobe convention; `Unsupported` for a legal stream this decoder
/// does not implement (arithmetic coding, 12-bit samples, lossless and
/// hierarchical processes, two or more than four components, a height
/// deferred to a DNL marker); `ValidationFailed` for a picture of more than
/// `max_pixels` pixels, checked before anything is allocated. `cancellation`
/// is polled between rows of MCUs, and a request throws the structured
/// Cancelled error.
JpegInfo inspect_jpeg(std::string_view bytes, std::uint64_t max_pixels = kMaxPicturePixels,
                      const cworks::CancelToken* cancellation = nullptr);

/// Decode a JPEG stream into opaque RGBA pixels, with its EXIF orientation
/// applied.
///
/// Decodes baseline, extended-sequential and progressive Huffman-coded DCT
/// streams of 8-bit precision: grayscale, YCbCr, RGB, CMYK and YCCK (the
/// last two with the Adobe inversion), any sampling factors from 1 to 4
/// (4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1 and factors whose ratio is not an
/// integer alike), and restart intervals. It refuses what inspect_jpeg()
/// refuses, with the same codes.
///
/// The output is bit-exact on every platform — integers throughout, no
/// floating point anywhere between the bytes and the pixels:
///
/// - **Dequantization** multiplies each coefficient by its table entry and
///   saturates at [−4096, 4095], a range a conforming 8-bit stream never
///   reaches (its coefficients stay within ±2048).
/// - **Inverse DCT** is the separable T.81 A.3.3 definition evaluated as two
///   fixed-point matrix products. The basis T[x][u] = round(8192 · C(u)/2 ·
///   cos((2x+1)uπ/16)), with C(0) = 1/√2 and C(u) = 1 otherwise, is a fixed
///   table of integers. The column pass sums T[y][v]·F[v][u] and rounds away
///   10 bits, keeping three fractional bits; the row pass sums T[x][u]·G[y][u]
///   and rounds away 16; the level shift adds 128 and the result clamps to
///   [0, 255]. Every rounding is (s + 2^(n−1)) >> n, an arithmetic shift, and
///   no intermediate leaves the 32-bit range.
/// - **Upsampling** of a component with sampling factors (H, V) under the
///   frame's maxima (Hmax, Vmax) is bilinear interpolation between sample
///   centres (JFIF centred siting), computed exactly: output column x reads
///   the component at (2x+1)·H/(2·Hmax) − ½, and likewise vertically, so the
///   two weights have denominators 2·Hmax and 2·Vmax; indices clamp to the
///   component's own extent, and the weighted sum is rounded once, half up.
///   A full-resolution component is read as it is.
/// - **Colour conversion** is T.871's YCbCr→RGB with 16-bit fixed-point
///   constants: R = Y + (91881·Cr′ + 2^15) >> 16, G = Y + (−22554·Cb′ −
///   46802·Cr′ + 2^15) >> 16, B = Y + (116130·Cb′ + 2^15) >> 16, where Cb′ =
///   Cb − 128 and Cr′ = Cr − 128, each clamped to [0, 255]. YCCK converts
///   its first three components the same way and takes C, M, Y as 255 minus
///   the result. CMYK in the T.81 convention is inverted to the Adobe one
///   (255 = no ink), and each of R, G, B is then ⌊(2·c·k + 255) / 510⌋ of
///   its ink and K.
/// - **Orientation** is applied last, as the pure pixel permutation EXIF
///   defines.
///
/// Requires no optional dependency: JPEG decoding is always available.
RasterImage decode_jpeg(std::string_view bytes, std::uint64_t max_pixels = kMaxPicturePixels,
                        const cworks::CancelToken* cancellation = nullptr);

/// A JPEG picture carried by its encoded bytes.
///
/// This is how a JPEG enters a scene (<cplot/scene.hpp>, ImageItem): the
/// vector backends embed the stream exactly as it was authored — PDF as a
/// `/DCTDecode` image, SVG as a `data:image/jpeg` URI — and only a raster
/// backend decodes it, on first request.
///
/// A JpegPicture always holds a stream inspect_jpeg() accepted: it is built
/// only by from_bytes(), which validates, so every backend that meets one
/// draws it. Copies share the bytes and the decoded pixels; the class is
/// safe to use from any number of threads at once.
class JpegPicture {
public:
    /// Validate `bytes` with inspect_jpeg() under `max_pixels` and keep the
    /// stream — SOI through EOI; anything after EOI is dropped. Throws what
    /// inspect_jpeg() throws.
    static JpegPicture from_bytes(std::string bytes,
                                  std::uint64_t max_pixels = kMaxPicturePixels,
                                  const cworks::CancelToken* cancellation = nullptr);

    const JpegInfo& info() const noexcept;

    /// The stream, SOI through EOI, byte for byte as authored.
    std::string_view bytes() const noexcept;

    /// The decoded, oriented pixels (decode_jpeg()), computed on the first
    /// request and shared by every copy afterwards. `cancellation` governs
    /// that first decode only; a cancelled decode leaves nothing behind, and
    /// the next request starts over.
    const RasterImage& pixels(const cworks::CancelToken* cancellation = nullptr) const;

    /// Two pictures are equal when their streams are.
    friend bool operator==(const JpegPicture& a, const JpegPicture& b) noexcept;

private:
    struct State;
    explicit JpegPicture(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

} // namespace cplot
