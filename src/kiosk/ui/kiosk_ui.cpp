#include "config.h"
#include "kiosk_ui.h"
#include "logging.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#include "SDL.h"

KioskUI& KioskUI::GetInstance() {
    static KioskUI instance;
    return instance;
}

KioskUI::~KioskUI() {
    Shutdown();
}

bool KioskUI::InitializeContext() {
    if (m_context_initialized) return true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    m_context_initialized = true;
    LOG_MSG("KIOSK: Dear ImGui context initialized successfully (v%s)", IMGUI_VERSION);
    return true;
}

bool KioskUI::InitializeBackend(SDL_Window* window, void* gl_context) {
    if (!m_context_initialized) {
        if (!InitializeContext()) return false;
    }
    if (m_backend_initialized) return true;

    if (!window || !gl_context) {
        LOG_MSG("KIOSK: Cannot initialize UI backends without window and GL context");
        return false;
    }

    if (!ImGui_ImplSDL2_InitForOpenGL(window, gl_context)) {
        LOG_MSG("KIOSK: ImGui_ImplSDL2_InitForOpenGL failed");
        return false;
    }

    // Pass nullptr to allow auto-detection of GLSL version across desktop GL and GLES
    if (!ImGui_ImplOpenGL3_Init(nullptr)) {
        LOG_MSG("KIOSK: ImGui_ImplOpenGL3_Init failed");
        ImGui_ImplSDL2_Shutdown();
        return false;
    }

    m_window = window;
    m_backend_initialized = true;
    LOG_MSG("KIOSK: Dear ImGui SDL2/OpenGL backends initialized successfully");
    return true;
}

bool KioskUI::Initialize(SDL_Window* window, void* gl_context) {
    if (!InitializeContext()) return false;
    if (window && gl_context) {
        return InitializeBackend(window, gl_context);
    }
    return true;
}

void KioskUI::Shutdown() {
    if (m_backend_initialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        m_backend_initialized = false;
        m_window = nullptr;
    }
    if (m_context_initialized) {
        ImGui::DestroyContext();
        m_context_initialized = false;
        LOG_MSG("KIOSK: Dear ImGui shutdown completed.");
    }
}

bool KioskUI::ProcessEvent(const SDL_Event* event) {
    if (!m_backend_initialized || !m_window || !event) return false;
    return ImGui_ImplSDL2_ProcessEvent(event);
}

void KioskUI::NewFrame() {
    if (!m_context_initialized) return;
    if (m_backend_initialized && m_window) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
    }
    ImGui::NewFrame();
}

void KioskUI::RenderFrame() {
    if (!m_context_initialized) return;
    ImGui::Render();
    if (m_backend_initialized && m_window) {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
}

std::string KioskUI::GetImGuiVersion() const {
    return IMGUI_VERSION;
}
