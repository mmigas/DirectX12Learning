#include "Window.hpp"

Window::Window(HINSTANCE hInstance, const std::wstring& title, int width, int height) : m_hInstance(hInstance),
    m_hWnd(nullptr),
    m_title(title),
    m_width(width),
    m_height(height),
    m_isRunning(false) {
    m_className = L"WDX12FrameworkWindowClass";
    m_isClassRegistered = false;
}

Window::~Window() {
    destroy();
}

bool Window::create() {
    if (!m_isClassRegistered) {
        WNDCLASSEXW wc;
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = staticWindowProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = sizeof(Window*);
        wc.hInstance = m_hInstance;
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszMenuName = nullptr;
        wc.lpszClassName = m_className.c_str();
        wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
        if (!RegisterClassExW(&wc)) {
            MessageBoxW(nullptr, L"Window Registration Failed!",
                        L"Error", MB_ICONEXCLAMATION | MB_OK);
            return false;
        }
    }

    RECT windowRect = {0, 0, m_width, m_height};
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    int adjustWidth = windowRect.right - windowRect.left;
    int adjustHeight = windowRect.bottom - windowRect.top;

    m_hWnd = CreateWindowExW(
        0, // Optional window styles
        m_className.c_str(), // Window class name
        m_title.c_str(), // Window title
        WS_OVERLAPPEDWINDOW, // Default window style
        CW_USEDEFAULT,CW_USEDEFAULT, // Position (let Windows decide)
        adjustWidth, // Calculated window width
        adjustHeight, // Calculated window height
        nullptr, // Parent window
        nullptr, // Menu
        m_hInstance, // Application instance handle
        this); // Pass 'this' pointer - IMPORTANT for StaticWndProc

    if (!m_hWnd) {
        MessageBoxW(nullptr, L"Window Creation Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        destroy();
        return false;
    }
    return true;
}

void Window::show(int nCmdShow) {
    if (m_hWnd) {
        ShowWindow(m_hWnd, nCmdShow);
        UpdateWindow(m_hWnd);
    }
}

void Window::destroy() {
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
}

void Window::setTitle(const std::wstring& title) const {
    if (m_hWnd) {
        SetWindowTextW(m_hWnd, title.c_str());
    }
}

float Window::getAndResetMouseWheelDelta() {
    float delta = m_mouseWheelDelta;
    m_mouseWheelDelta = 0.0f; // Reset for next frame
    return delta;
}

void Window::getAndResetMouseDelta(float& deltaX, float& deltaY) {
    deltaX = m_mouseDeltaX;
    deltaY = m_mouseDeltaY;
    m_mouseDeltaX = 0.0f; // Reset for next frame
    m_mouseDeltaY = 0.0f;
}

bool Window::wasSpaceBarPressed() {
    bool pressed = m_spaceBarPressed;
    m_spaceBarPressed = false; // Reset flag after checking
    return pressed;
}


LRESULT Window::staticWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Window* pWindow = nullptr;

    if (message == WM_NCCREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        pWindow = static_cast<Window*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWindow));
        pWindow->m_hWnd = hwnd;
    } else {
        pWindow = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (pWindow) {
        return pWindow->handleMessage(message, wParam, lParam);
    } else {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

LRESULT Window::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_DESTROY:
            PostQuitMessage(0);
            m_hWnd = nullptr;
            return 0;
        case WM_CLOSE:
        case CTRL_C_EVENT:
            DestroyWindow(m_hWnd);
            return 0;
        case WM_PAINT: //Temp: directx 12 will override this
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(m_hWnd, &ps);
            FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));
            EndPaint(m_hWnd, &ps);
        }
            return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                m_width = LOWORD(lParam);
                m_height = HIWORD(lParam);
            }
            //Trigger swap chain resize here
            return 0;
        case WM_GETMINMAXINFO: //

        {
            LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
            lpMMI->ptMinTrackSize.x = 320;
            lpMMI->ptMinTrackSize.y = 240;
        }
            return 0;

        case WM_MOUSEWHEEL: {
            float wheelDelta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
            m_mouseWheelDelta += wheelDelta; // Accumulate scroll
            return 0;
        }

        case WM_LBUTTONDOWN: {
            m_leftMouseButtonDown = true;
            SetCapture(m_hWnd); // Capture mouse
            // Store starting position for delta calculation on first move
            m_lastMousePos.x = LOWORD(lParam);
            m_lastMousePos.y = HIWORD(lParam);
            return 0;
        }

        case WM_LBUTTONUP: {
            m_leftMouseButtonDown = false;
            ReleaseCapture(); // Release mouse
            return 0;
        }

        case WM_MOUSEMOVE: {
            // Only accumulate delta if button is down (for orbiting)
            if (m_leftMouseButtonDown) {
                POINT currentMousePos;
                currentMousePos.x = LOWORD(lParam);
                currentMousePos.y = HIWORD(lParam);

                // Calculate delta from last position
                float deltaX = static_cast<float>(currentMousePos.x - m_lastMousePos.x);
                float deltaY = static_cast<float>(currentMousePos.y - m_lastMousePos.y);

                // Accumulate deltas
                m_mouseDeltaX += deltaX;
                m_mouseDeltaY += deltaY;

                // Update last position for next delta calculation
                m_lastMousePos = currentMousePos;
            }
            return 0;
        }

        case WM_KEYDOWN: {
            // Check for space bar press
            if (wParam == VK_SPACE && !m_spaceBarDown) { // Check !m_spaceBarDown to only trigger once per press
                m_spaceBarPressed = true; // Set flag for Application to query
                m_spaceBarDown = true; // Mark key as down
            }
            // Handle other keydowns later (e.g., VK_ESCAPE)
            // else if (wParam == VK_ESCAPE) { /* ... */ }
            return 0; // Indicate message was handled
        }
        case WM_KEYUP: {
            // Check for space bar release
            if (wParam == VK_SPACE) {
                m_spaceBarDown = false; // Mark key as up
            }
            return 0; // Indicate message was handled
        }
        default:
            // Handle any messages we didn't explicitly handle
            return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }
    return 0; // Should not be reached if default handles messages
}
