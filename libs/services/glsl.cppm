// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.services.glsl - GLSL shaders: the file types and their stages, and an IntelliSense that needs no language server:
// completion (keywords, types, built-in functions and variables of the stage, the names declared in the file, struct and block
// members, swizzles), parameter hints, hover text, an outline, go to definition, and error checking with glslc or
// glslangValidator when one is installed.
export module daemon.services.glsl;
import std;
import daemon.services.lsp;

export namespace ide::glsl {

// Every extension shaders come with (without the dot, lower case).
const std::vector<std::string>& extensions();
bool isGlslPath(std::string_view path);

// "vert", "frag", "comp", "geom", "tesc", "tese", "mesh", "task", "rgen", ... from the extension (also `shader.vert.glsl`) or a
// `#pragma shader_stage(...)` line; empty when the file does not say (an include file, a plain .glsl).
std::string stageOf(std::string_view path, std::string_view text);

// Position inside a text: 0-based line, byte column.
std::vector<LspCompletion> complete(std::string_view text, int line, int col, std::string_view stage);
std::optional<LspSignatureHelp> signature(std::string_view text, int line, int col);
std::string hover(std::string_view text, int line, int col, std::string_view stage);
std::vector<LspSymbol> symbols(std::string_view text);
std::optional<std::pair<int, int>> definition(std::string_view text, int line, int col);   // -> line, byte column

// Error checking. `compiler()` is the program that will be used ("" when none is installed).
std::string compiler();
enum class Target { Auto, Vulkan, OpenGL };
// Compiles `text` (blocking: call it from a worker thread) and returns what the compiler complained about. `error` is set when
// the check itself could not run.
std::vector<LspDiag> diagnose(std::string_view text, std::string_view stage, Target target, const std::string& includeDir, std::string& error);

// Pieces exposed for tests.
struct Token { std::string text; int line = 0, col = 0; bool ident = false, number = false, pp = false; };
std::vector<Token> tokenize(std::string_view text);

}  // namespace ide::glsl
