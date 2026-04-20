#include "src/gui/app_window.h"

int main(int /*argc*/, char* /*argv*/[]) {
    jtag::gui::AppWindow app;
    if (!app.isValid()) {
        return 1;
    }
    app.run();
    return 0;
}
