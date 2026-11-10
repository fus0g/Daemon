// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal GLSL -> SPIR-V compiler used at build time when `glslc` is not installed.
// It talks to libshaderc_shared through its stable C API (declared here by hand so
// the shaderc development headers are not required).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

extern "C" {
typedef struct shaderc_compiler* shaderc_compiler_t;
typedef struct shaderc_compilation_result* shaderc_compilation_result_t;
typedef struct shaderc_compile_options* shaderc_compile_options_t;
shaderc_compiler_t shaderc_compiler_initialize(void);
void shaderc_compiler_release(shaderc_compiler_t);
shaderc_compile_options_t shaderc_compile_options_initialize(void);
void shaderc_compile_options_release(shaderc_compile_options_t);
void shaderc_compile_options_set_target_env(shaderc_compile_options_t, int target, uint32_t version);
void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t, int level);
shaderc_compilation_result_t shaderc_compile_into_spv(const shaderc_compiler_t, const char* src, size_t len,
                                                      int kind, const char* file, const char* entry,
                                                      const shaderc_compile_options_t);
void shaderc_result_release(shaderc_compilation_result_t);
size_t shaderc_result_get_length(const shaderc_compilation_result_t);
const char* shaderc_result_get_bytes(const shaderc_compilation_result_t);
int shaderc_result_get_compilation_status(const shaderc_compilation_result_t);
const char* shaderc_result_get_error_message(const shaderc_compilation_result_t);
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: spvc <in.vert|in.frag> <out.inc>\n"); return 2; }
    std::string in = argv[1];
    int kind = 0;  // shaderc_vertex_shader
    if (in.ends_with(".frag")) kind = 1;       // shaderc_fragment_shader
    else if (in.ends_with(".comp")) kind = 2;  // shaderc_compute_shader
    else if (!in.ends_with(".vert")) { std::fprintf(stderr, "unknown shader stage: %s\n", argv[1]); return 2; }

    std::ifstream f(in, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto* comp = shaderc_compiler_initialize();
    auto* opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opts, 0 /*vulkan*/, (1u << 22) | (3u << 12));  // Vulkan 1.3
    shaderc_compile_options_set_optimization_level(opts, 2 /*performance*/);
    auto* res = shaderc_compile_into_spv(comp, src.data(), src.size(), kind, argv[1], "main", opts);
    if (shaderc_result_get_compilation_status(res) != 0) {
        std::fprintf(stderr, "%s\n", shaderc_result_get_error_message(res));
        return 1;
    }
    size_t len = shaderc_result_get_length(res);
    const char* bytes = shaderc_result_get_bytes(res);
    std::FILE* out = std::fopen(argv[2], "w");
    if (!out) { std::perror("fopen"); return 1; }
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t w; std::memcpy(&w, bytes + i, 4);
        std::fprintf(out, "0x%08x,%s", w, (i / 4) % 8 == 7 ? "\n" : " ");
    }
    std::fputc('\n', out);
    std::fclose(out);
    shaderc_result_release(res);
    shaderc_compile_options_release(opts);
    shaderc_compiler_release(comp);
    return 0;
}
