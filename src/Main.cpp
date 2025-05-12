#include <Windows.h>

#include "Application.hpp"

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
#if defined(DEBUG) || defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try {
        // Initialize the application
        Application app(hInstance);
        if (!app.init()) {
            return 1;
        }

        int exitCode = app.run();

        app.shutdown();

        return exitCode;
    } catch (const std::exception& e) {
        MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(e.what()), L"Error", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, LPCWSTR("Unknown error occurred"), L"Error", MB_OK | MB_ICONERROR);
    }
    return 0;
}
