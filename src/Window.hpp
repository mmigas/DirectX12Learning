#pragma once

#include <memory>
#include <string>
#include <windows.h>

class Window {
public:
    Window(HINSTANCE hInstance, const std::wstring& title, int width, int height);

    ~Window();

    bool create();

    void show(int nCmdShow);

    void destroy();

    // --Getters--
    HWND getWindowHandle() const {
        return m_hWnd;
    }

    int getWidth() const {
        return m_width;
    }

    int getHeight() const {
        return m_height;
    }

    const std::wstring& getTitle() const {
        return m_title;
    }

    void setTitle(const std::wstring& title) const;

    float getAndResetMouseWheelDelta(); // Gets accumulated delta and resets internal state
    void getAndResetMouseDelta(float& deltaX, float& deltaY); // Gets accumulated delta and resets

    bool isLeftMouseButtonDown() const {
        return m_leftMouseButtonDown;
    }
    bool wasSpaceBarPressed();
private:
    HWND m_hWnd;
    HINSTANCE m_hInstance; // Not sure if this should be a copy or a reference
    std::unique_ptr<Window> m_window;
    bool m_isRunning;
    std::wstring m_title;
    std::wstring m_className;
    int m_width;
    int m_height;
    bool m_isClassRegistered = false;

    float m_mouseWheelDelta = 0.0f; // Accumulates scroll delta between frames
    float m_mouseDeltaX = 0.0f; // Accumulates mouse X delta between frames
    float m_mouseDeltaY = 0.0f; // Accumulates mouse Y delta between frames
    POINT m_lastMousePos = {}; // Last known mouse position (for calculating delta)
    bool m_leftMouseButtonDown = false; // Is the left button currently pressed?

    bool m_spaceBarPressed = false; // Flag to indicate a press occurred since last check
    bool m_spaceBarDown = false;    // Tracks if key is currently held down
    
    static LRESULT CALLBACK staticWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
};
