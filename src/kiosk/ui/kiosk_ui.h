#ifndef DOSBOX_KIOSK_UI_H
#define DOSBOX_KIOSK_UI_H

#include "../kiosk_types.h"
#include <string>

struct SDL_Window;
typedef union SDL_Event SDL_Event;

class KioskUI {
public:
    static KioskUI& GetInstance();

    bool Initialize(SDL_Window* window = nullptr, void* gl_context = nullptr);
    bool InitializeContext();
    bool InitializeBackend(SDL_Window* window, void* gl_context);
    void Shutdown();

    bool IsInitialized() const { return m_context_initialized; }
    bool IsBackendInitialized() const { return m_backend_initialized; }

    bool ProcessEvent(const SDL_Event* event);
    void NewFrame();
    void RenderFrame();

    std::string GetImGuiVersion() const;

private:
    KioskUI() = default;
    ~KioskUI();
    KioskUI(const KioskUI&) = delete;
    KioskUI& operator=(const KioskUI&) = delete;

    bool m_context_initialized = false;
    bool m_backend_initialized = false;
    SDL_Window* m_window = nullptr;
};

#endif // DOSBOX_KIOSK_UI_H
