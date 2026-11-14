// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.services.notebook — C++ notebooks: the .ipynb model, a cell planner and a kernel that compiles every cell as a
// C++20 module with GCC and loads it into a long-lived runner process, so variables, functions and types persist between
// cells and `import std;` and user modules work. No Jupyter involved: the files are ordinary nbformat 4 documents.
//
// How a cell runs
//   * The cell is split into top-level chunks. Declarations (types, functions, templates, global variables) are exported from
//     a module `nb_<n>`; statements and a trailing expression go into an entry function of their own module.
//   * `#include`s, macros, `using namespace`, and `import`s are remembered and repeated in the cells that follow, because
//     modules do not export them.
//   * Each module is compiled to a shared object and dlopen'ed (RTLD_GLOBAL) by the runner; later cells `import` the earlier
//     ones, so the compiler transfers the types and the linker resolves the symbols.
//   * A cell that is itself a module unit (`export module geometry;`) is passed through unchanged and can be imported.
export module daemon.services.notebook;
import std;
import daemon.base.json;
import daemon.base.process;

export namespace ide::nb {

// ------------------------------------------------------------------------------------------------ the document

struct Output {
    enum class Kind { Stream, Result, Error } kind = Kind::Stream;
    std::string name;   // stream: "stdout" / "stderr"; error: the exception name
    std::string text;   // stream text, the text/plain value of a result, or the error message with its traceback
};

struct Cell {
    enum class Type { Code, Markdown, Raw } type = Type::Code;
    std::string source;
    std::vector<Output> outputs;
    int execCount = 0;      // 0: never run
    std::string id;
    json::Value metadata = json::object();
};

struct Notebook {
    std::vector<Cell> cells;
    json::Value metadata = json::object();   // kept as read, so a notebook written by Jupyter survives a round trip
    int nbformat = 4, nbformatMinor = 5;

    bool isCpp() const;   // the kernelspec / language says C++ (an empty notebook is C++)
};

// Notebook files: `.ipynb` (Jupyter's name) and `.dcxxnb` (Daemon's own; the same nbformat-4 JSON, so a rename is all it takes
// to open one in Jupyter and the other way round).
bool isNotebookPath(std::string_view path);

// An empty text gives a new, empty C++ notebook.
std::expected<Notebook, std::string> parseNotebook(std::string_view text);
std::string serializeNotebook(const Notebook& nb);
std::string newCellId();

// ------------------------------------------------------------------------------------------------ planning

struct Chunk {
    enum class Kind { Include, Import, Using, Decl, Stmt, Expr } kind = Kind::Stmt;
    std::string text;
    int line = 1;              // 1-based line of the first character in the cell
    int col = 0;               // byte column of the first character on that line
    bool exported = false;     // Decl: gets `export` in front
};

// Splits a cell into top-level chunks and classifies them (exposed for tests).
std::vector<Chunk> splitChunks(std::string_view source);

struct Segment {
    enum class Kind { Decls, Stmts, Module } kind = Kind::Decls;
    std::vector<Chunk> chunks;
    std::string moduleName;   // Kind::Module: the name the user gave
    std::string text;         // Kind::Module: the whole cell
};

struct Plan {
    std::vector<Segment> segments;
    std::vector<std::string> flags;   // from `%flags ...` lines
    std::vector<std::string> libs;    // from `%link ...` lines
    std::string error;
};

Plan planCell(std::string_view source);

// GCC's output for one mistake can run to hundreds of lines (every overload candidate, every header in the include chain).
// This keeps the errors and warnings, the first few notes that point into the cell, and says how much was left out.
std::string condenseDiagnostics(std::string_view raw);

// ------------------------------------------------------------------------------------------------ IntelliSense support

// The notebook as one C++ file for a language server: statements are wrapped in functions so that names declared in earlier
// cells are visible, `import std;` becomes a standard header, user module cells are inlined. Every chunk is kept at the same
// column, so a position inside a cell maps to a position in the file and back.
struct VirtualDoc {
    struct Span { int cell = -1, cellLine = 0, lines = 0, vline = 0, col = 0; };
    std::string text;
    std::vector<Span> spans;
};
struct CellPos { int cell = -1, line = 0, col = 0; };

// `cells`: (notebook cell index, source) of the code cells, in order.
VirtualDoc buildVirtualDoc(const std::vector<std::pair<int, std::string>>& cells);
std::optional<std::pair<int, int>> toVirtual(const VirtualDoc& vd, int cell, int line, int col);   // -> (line, byte column)
std::optional<CellPos> fromVirtual(const VirtualDoc& vd, int vline, int vcol);

// ------------------------------------------------------------------------------------------------ the kernel

struct Event {
    enum class Type { Stream, Result, Error, Started, Finished, Status } type = Type::Stream;
    int cell = -1;
    std::string text;
    std::string name;     // stream name / error name
    int execCount = 0;    // Started
    bool ok = true;       // Finished
};

class Session {
public:
    enum class State { Idle, Starting, Busy, Dead };

    explicit Session(std::string notebookPath);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Queues a cell for execution (runs strictly in order on a worker thread). `index` is echoed in the events.
    void request(int index, std::string source);

    // Events since the last call (main thread).
    std::vector<Event> poll();

    // Stops what is running and forgets everything the kernel knew (a runaway cell cannot be interrupted otherwise).
    void interrupt();
    // Fresh kernel, empty state (also what happens after a crash, on the next request).
    void restart();

    State state() const;
    std::string statusLine() const;      // e.g. "g++ 16.2.1 - idle"
    bool pending() const;                // something is queued or running

    // Overrides for tests / power users: DAEMON_NB_CXX, DAEMON_NB_FLAGS.
    static std::string compilerName();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Runs every code cell of the notebook in order and fills in the outputs (command line, tests). Returns the number of cells
// that failed.
int runAll(Notebook& nb, const std::string& notebookPath, std::ostream* log = nullptr);

}  // namespace ide::nb
