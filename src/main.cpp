#include <cstring>

#include "src/gui/app_window.h"
#include "src/tools/daemon_runner.h"

int main(int argc, char* argv[]) {
    // If the first argument is --mcp or --daemon, run in headless daemon mode.
    // The flag is consumed here; remaining args are forwarded to runDaemon.
    if (argc >= 2 && (std::strcmp(argv[1], "--mcp") == 0 ||
                      std::strcmp(argv[1], "--daemon") == 0)) {
        return runDaemon(argc - 2, argv + 2);
    }
    jtag::gui::AppWindow app;
    if (!app.isValid()) {
        return 1;
    }
    app.run();
    return 0;
}
