// Regression tests for local stb patches. No Windows SDK, V8 or external assets.
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Record allocation attempts without allocating huge buffers in boundary tests.
static size_t last_allocation = 0;
static void* TestMalloc(size_t size)
{
    last_allocation = size;
    return size > 64 * 1024 * 1024 ? nullptr : std::malloc(size);
}
static void* TestRealloc(void* data, size_t size)
{
    last_allocation = size;
    return size > 64 * 1024 * 1024 ? nullptr : std::realloc(data, size);
}

#define STBI_MALLOC TestMalloc
#define STBI_REALLOC TestRealloc
#define STBI_FREE std::free
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"

static int failures = 0;
static int checks = 0;
#define EXPECT(condition) do { ++checks; if (!(condition)) { \
    ++failures; std::printf("not ok - line %d: %s\n", __LINE__, #condition); } } while (0)

static void Test16BitConversion()
{
    auto* pixel = static_cast<stbi__uint16*>(std::malloc(3 * sizeof(stbi__uint16)));
    pixel[0] = 123; pixel[1] = 456; pixel[2] = 789;
    auto* rgba = stbi__convert_format16(pixel, 3, 4, 1, 1);
    EXPECT(rgba != nullptr);
    if (rgba) {
        EXPECT(rgba[0] == 123 && rgba[1] == 456 && rgba[2] == 789 && rgba[3] == 65535);
        std::free(rgba);
    }

    // The original unsigned product wraps to zero for these dimensions.
    // Conversion must reject the size before allocating or reading input pixels.
    const unsigned int dimensions[][2] = {{65536, 8192}, {UINT_MAX, 1}, {1, UINT_MAX}};
    for (const auto& size : dimensions) {
        pixel = static_cast<stbi__uint16*>(std::malloc(3 * sizeof(stbi__uint16)));
        last_allocation = 0;
        rgba = stbi__convert_format16(pixel, 3, 4, size[0], size[1]);
        EXPECT(rgba == nullptr);
        EXPECT(last_allocation == 0);
        std::free(rgba);
    }
}

static void TestAnimatedGif()
{
    // Original fixture: 1x1, black/red palette. Frame 2 restores frame 1;
    // transparent frame 3 must therefore show red, not read before the buffer.
    std::vector<stbi_uc> gif = {
        'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0,
        0, 0, 0, 255, 0, 0
    };
    const stbi_uc controls[] = {4, 12, 5};
    const stbi_uc pixels[] = {0x4c, 0x44, 0x44};
    for (int i = 0; i < 3; ++i) {
        gif.insert(gif.end(), {0x21, 0xf9, 4, controls[i], 1, 0, 0, 0,
            0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, pixels[i], 1, 0});
    }
    gif.push_back(0x3b);
    int* delays = nullptr;
    int width = 0, height = 0, frames = 0, components = 0;
    stbi_uc* rgba = stbi_load_gif_from_memory(gif.data(), static_cast<int>(gif.size()),
        &delays, &width, &height, &frames, &components, 4);
    EXPECT(rgba != nullptr);
    EXPECT(width == 1 && height == 1 && frames == 3 && components == 4);
    if (rgba && frames == 3) {
        EXPECT(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
        EXPECT(rgba[4] == 0 && rgba[5] == 0 && rgba[6] == 0 && rgba[7] == 255);
        EXPECT(rgba[8] == 255 && rgba[9] == 0 && rgba[10] == 0 && rgba[11] == 255);
    }
    EXPECT(delays != nullptr);
    if (delays && frames == 3) EXPECT(delays[0] == 10 && delays[1] == 10 && delays[2] == 10);
    stbi_image_free(rgba);
    stbi_image_free(delays);

    // Oversized canvas is rejected before any pixel buffer is allocated.
    gif[6] = gif[7] = gif[8] = gif[9] = 255;
    last_allocation = 0;
    rgba = stbi_load_gif_from_memory(gif.data(), static_cast<int>(gif.size()),
        &delays, &width, &height, &frames, &components, 4);
    EXPECT(rgba == nullptr);
    EXPECT(last_allocation == 0);
    stbi_image_free(rgba);
    stbi_image_free(delays);
}

static void TestVorbisSeekBounds()
{
    unsigned char bytes[16] = {};
    stb_vorbis decoder = {};
    decoder.stream_start = bytes;
    decoder.stream_end = bytes + sizeof(bytes);
    for (unsigned int offset : {0U, 15U, 16U, 17U, UINT_MAX, 0U}) {
        const bool valid = offset < sizeof(bytes);
        EXPECT(set_file_offset(&decoder, offset) == static_cast<int>(valid));
        EXPECT(decoder.eof == static_cast<int>(!valid));
        EXPECT(decoder.stream == (valid ? bytes + offset : decoder.stream_end));
    }
    decoder.stream_end = decoder.stream_start;
    EXPECT(set_file_offset(&decoder, 0) == 0);
    EXPECT(decoder.eof == 1 && decoder.stream == decoder.stream_end);
}

int main()
{
    Test16BitConversion();
    TestAnimatedGif();
    TestVorbisSeekBounds();
    std::printf("stb safety: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
