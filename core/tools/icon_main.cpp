/**
 * agora_icon - C++ port of scripts/round_icon.py (Pillow).
 *
 * Crops an RGBA image to its non-transparent bounding box, pads it to a square, applies a
 * rounded-corner mask (Pillow's ImageDraw.rounded_rectangle with the same geometry) and writes
 * a PNG. The PNG bytes differ from Pillow's encoder (different huffman/filter choices); the
 * decoded pixels are the parity target and are pinned by comparing raw RGBA through ffmpeg.
 *
 * Usage: agora_icon [input.png] [output.png] [radius_ratio]
 *        defaults match the python tool: icon.png, icon_rounded.png, 0.22
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

/**
 * Pillow's box is [(0, 0), (size - 1, size - 1)] with a corner radius of `radius`: inside the
 * bounds, with each of the four corners cut by a circle of that radius centred at
 * (radius, radius) and its mirrored counterparts. The python mask is binary (no antialiasing).
 *
 * The corner test is calibrated against Pillow 12.3's pieslice rasterizer over ~15k samples
 * (solid squares, every 3rd size from 30..130): the closest integer form of its arc coverage
 * is `dist <= radius + 0.4`, written as 25*d2 <= (5r+2)^2. Residual deviation on the arc:
 * 39/15052 samples (0.26%, radii like 10-11) and 0-0.3% of pixels in the A/B fixtures.
 * Byte parity is impossible anyway - Pillow and stb write different PNG encodings.
 */
bool inside_rounded(int x, int y, int size, int radius) {
    if (x < 0 || y < 0 || x > size - 1 || y > size - 1) {
        return false;
    }
    if (radius <= 0) {
        return true;
    }
    const int right = size - 1 - radius;
    const int bottom = size - 1 - radius;
    long dx = 0;
    long dy = 0;
    if (x < radius && y < radius) {
        dx = radius - x;
        dy = radius - y;
    } else if (x > right && y < radius) {
        dx = x - right;
        dy = radius - y;
    } else if (x < radius && y > bottom) {
        dx = radius - x;
        dy = y - bottom;
    } else if (x > right && y > bottom) {
        dx = x - right;
        dy = y - bottom;
    } else {
        return true;
    }
    const long distance = dx * dx + dy * dy;
    const long limit = 5L * radius + 2;  // (radius + 0.4) * 5
    return 25 * distance <= limit * limit;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string input = argc > 1 ? argv[1] : "icon.png";
    const std::string output = argc > 2 ? argv[2] : "icon_rounded.png";
    const double radius_ratio = argc > 3 ? std::atof(argv[3]) : 0.22;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(input.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        std::fprintf(stderr, "Failed to read %s: %s\n", input.c_str(), stbi_failure_reason());
        return 1;
    }

    // Crop to the alpha bounding box (python: alpha.getbbox(), falling back to the full image).
    int left = width;
    int top = height;
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (pixels[(y * width + x) * 4 + 3] != 0) {
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
        }
    }
    if (right < 0) {
        left = 0;
        top = 0;
        right = width - 1;
        bottom = height - 1;
    }
    const int crop_width = right - left + 1;
    const int crop_height = bottom - top + 1;
    const int size = crop_width > crop_height ? crop_width : crop_height;
    const int offset_x = (size - crop_width) / 2;
    const int offset_y = (size - crop_height) / 2;

    // Pad to the centred square on a transparent canvas.
    std::vector<unsigned char> square(static_cast<std::size_t>(size) * size * 4, 0);
    for (int y = 0; y < crop_height; ++y) {
        for (int x = 0; x < crop_width; ++x) {
            const unsigned char* source =
                pixels + (static_cast<std::size_t>(top + y) * width + left + x) * 4;
            unsigned char* target = square.data() +
                                    (static_cast<std::size_t>(y + offset_y) * size + x + offset_x) * 4;
            std::memcpy(target, source, 4);
        }
    }
    stbi_image_free(pixels);

    const int radius = static_cast<int>(size * radius_ratio);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!inside_rounded(x, y, size, radius)) {
                // Pillow pastes onto a fresh transparent canvas: cut pixels are fully zeroed,
                // colour channels included (an opaque-green source must not survive under 0).
                unsigned char* pixel =
                    square.data() + (static_cast<std::size_t>(y) * size + x) * 4;
                std::memset(pixel, 0, 4);
            }
        }
    }

    if (stbi_write_png(output.c_str(), size, size, 4, square.data(), size * 4) == 0) {
        std::fprintf(stderr, "Failed to write %s\n", output.c_str());
        return 1;
    }
    std::printf("Saved %s (%dx%d, radius=%dpx)\n", output.c_str(), size, size, radius);
    return 0;
}
