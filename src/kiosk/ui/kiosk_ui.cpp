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

bool KioskUI::Initialize(SDL_Window* window, void* gl_context) {
    if (m_initialized) return true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();

    if (window && gl_context) {
        if (!ImGui_ImplSDL2_InitForOpenGL(window, gl_context)) {
            LOG_MSG("KIOSK: ImGui_ImplSDL2_InitForOpenGL failed");
            ImGui::DestroyContext();
            return false;
        }
        if (!ImGui_ImplOpenGL3_Init("#version 130")) {
            LOG_MSG("KIOSK: ImGui_ImplOpenGL3_Init failed");
            ImGui_ImplSDL2_Shutdown();
            ImGui::DestroyContext();
            return false;
        }
        m_window = window;
    }

    m_initialized = true;
    LOG_MSG("KIOSK: Dear ImGui initialized successfully (v%s)", IMGUI_VERSION);
    return true;
}

void KioskUI::Shutdown() {
    if (m_initialized) {
        if (m_window) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL2_Shutdown();
            m_window = nullptr;
        }
        ImGui::DestroyContext();
        m_initialized = false;
        LOG_MSG("KIOSK: Dear ImGui shutdown completed.");
    }
}

bool KioskUI::ProcessEvent(const SDL_Event* event) {
    if (!m_initialized || !event) return false;
    return ImGui_ImplSDL2_ProcessEvent(event);
}

void KioskUI::NewFrame() {
    if (!m_initialized) return;
    if (m_window) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
    }
    ImGui::NewFrame();
}

void KioskUI::RenderFrame() {
    if (!m_initialized) return;
    ImGui::Render();
    if (m_window) {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
}

std::string KioskUI::GetImGuiVersion() const {
    return IMGUI_VERSION;
}
