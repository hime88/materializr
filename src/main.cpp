#include "app/Application.h"
#include "core/Verbose.h"

#include <OSD.hxx>

#include <cstdio>
#include <cstring>
#include <iostream>

namespace {

struct CliOptions {
    bool safeMode = false;
    bool wantHelp = false;
    bool verbose  = false;
    const char* logPath = "/tmp/materializr.log";
};

CliOptions parseArgs(int argc, char* argv[]) {
    CliOptions o;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--safe-mode")      == 0 ||
            std::strcmp(a, "--safe-graphics")  == 0 ||
            std::strcmp(a, "--low-graphics")   == 0) {
            o.safeMode = true;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            o.wantHelp = true;
        } else if (std::strcmp(a, "--verbose") == 0 || std::strcmp(a, "-v") == 0) {
            o.verbose = true;
        } else if (std::strcmp(a, "--log") == 0 && i + 1 < argc) {
            o.verbose = true;
            o.logPath = argv[++i];
        }
    }
    return o;
}

void printHelp() {
    std::cout <<
        "Materializr - parametric 3D CAD\n"
        "\n"
        "Usage: materializr [options]\n"
        "\n"
        "Options:\n"
        "  --safe-mode | --safe-graphics | --low-graphics\n"
        "      Bring the app up in a known-safe configuration: MSAA off,\n"
        "      mesh quality Low, default lights, autosave off, auto-open\n"
        "      last project off. The safe values are written to the settings\n"
        "      file, so subsequent normal launches stay recovered. Use this\n"
        "      if a previously-saved setting crashes the app at startup or\n"
        "      if a complex auto-opened project hangs a lower-core machine.\n"
        "\n"
        "  -v, --verbose\n"
        "      Redirect stderr to /tmp/materializr.log (truncated each run)\n"
        "      so [Resize], [Push/Pull] etc. diagnostics are captured to a\n"
        "      file that can be inspected after the session.\n"
        "\n"
        "  --log <path>\n"
        "      Implies --verbose and writes the log to <path> instead of the\n"
        "      default /tmp/materializr.log.\n"
        "\n"
        "  -h, --help\n"
        "      Print this help and exit.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    CliOptions opts = parseArgs(argc, argv);
    if (opts.wantHelp) {
        printHelp();
        return 0;
    }
    if (opts.verbose) {
        // Flip the per-op log gate so [Resize], etc. fprintf(stderr, ...)
        // calls actually emit. Without this they're no-ops.
        materializr::setVerbose(true);
        // Redirect stderr to a file so diagnostics (fprintf(stderr, ...) calls
        // scattered through the modeling ops) survive past the terminal
        // session. "w" truncates so each run starts clean. Line-buffered so a
        // crash mid-op still flushes recent traces.
        std::FILE* log = std::freopen(opts.logPath, "w", stderr);
        if (log) {
            std::setvbuf(log, nullptr, _IOLBF, 0);
            std::cout << "[verbose] stderr -> " << opts.logPath << std::endl;
            std::fprintf(stderr, "[verbose] materializr log opened\n");
        } else {
            std::cerr << "[verbose] failed to open log " << opts.logPath
                      << " (continuing with stderr to terminal)" << std::endl;
        }
    }
    // Convert OCCT internal faults (SIGSEGV/SIGFPE inside the kernel — e.g. a
    // NURBS-convert on degenerate geometry) into catchable Standard_Failure
    // exceptions, so an op's try/catch (with OCC_CATCH_SIGNALS) refuses the
    // operation instead of taking the whole app down.
    OSD::SetSignal(Standard_False);

    try {
        materializr::Application app(opts.safeMode);
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
