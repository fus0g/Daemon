// SPDX-License-Identifier: GPL-3.0-or-later
// Single translation unit that provides the stb_truetype implementation.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// Image decoding for the Markdown preview: only the formats that matter, from memory (no stdio).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"
