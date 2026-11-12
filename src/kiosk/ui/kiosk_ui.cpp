#include "config.h"

#if defined(C_KIOSK)

#include "kiosk_ui.h"
#include "../kiosk_main.h"
#include "../kiosk_db.h"
#include "logging.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#include "SDL.h"
#include <cmath>
#include <vector>

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

void KioskUI::ShutdownBackend() {
    if (m_backend_initialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        m_backend_initialized = false;
        m_window = nullptr;
    }
}

void KioskUI::Shutdown() {
    ShutdownBackend();
    if (m_context_initialized) {
        ImGui::DestroyContext();
        m_context_initialized = false;
        m_cache_loaded = false;
        m_cached_items.clear();
        m_cached_machines.clear();
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

void KioskUI::RenderCountdownBanner(float remaining_sec) {
    if (!m_context_initialized) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport) {
        ImVec2 pos(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                   viewport->WorkPos.y + viewport->WorkSize.y - 40.0f);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    }
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##KioskCountdownBanner", nullptr, flags)) {
        int secs = static_cast<int>(std::ceil(remaining_sec));
        if (secs < 0) secs = 0;
        ImGui::Text("Auto-booting in %d second%s... Press any key or controller button to enter Kiosk Menu",
                    secs, (secs == 1 ? "" : "s"));
    }
    ImGui::End();
}

void KioskUI::RenderInteractiveShellPlaceholder() {
    if (!m_context_initialized) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport) {
        ImVec2 center = viewport->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(520.0f, 360.0f), ImGuiCond_Always);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("DOSBox-X Kiosk Edition - Launcher", nullptr, flags)) {
        ImGui::TextWrapped("Select a machine profile, software title, or boot standard DOSBox-X.");
        ImGui::Separator();

        if (ImGui::Button("Boot Default DOSBox-X Shell", ImVec2(-1.0f, 36.0f))) {
            KioskManager::GetInstance().RequestDefaultDosboxLaunch();
        }

        if (!m_cache_loaded) {
            m_cached_items = KioskDatabase::GetInstance().GetAllCatalogItems();
            m_cached_machines = KioskDatabase::GetInstance().GetAllMachines();
            m_cache_loaded = true;
        }
        const std::vector<CatalogItemRecord>& items = m_cached_items;
        const std::vector<MachineRecord>& profiles = m_cached_machines;
        const float list_height = (!items.empty() && !profiles.empty()) ? 100.0f : 180.0f;

        if (!items.empty()) {
            ImGui::Spacing();
            ImGui::Text("Catalog Titles:");
            ImGui::BeginChild("##CatalogItemsList", ImVec2(0.0f, list_height), true);
            for (size_t i = 0; i < items.size(); ++i) {
                const CatalogItemRecord& item = items[i];
                std::string label = item.title + "##item_" + item.id;
                if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 28.0f))) {
                    KioskManager::GetInstance().RequestLaunchItem(item.id);
                }
            }
            ImGui::EndChild();
        }

        if (!profiles.empty()) {
            ImGui::Spacing();
            ImGui::Text("Machine Profiles:");
            ImGui::BeginChild("##MachineProfilesList", ImVec2(0.0f, list_height), true);
            for (size_t i = 0; i < profiles.size(); ++i) {
                const MachineRecord& prof = profiles[i];
                std::string label = prof.display_name + "##prof_" + prof.id;
                if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 28.0f))) {
                    KioskManager::GetInstance().RequestLaunchMachine(prof.id);
                }
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

void KioskUI::RenderRuntimeOverlayPlaceholder() {
    if (!m_context_initialized) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport) {
        ImVec2 center = viewport->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(360.0f, 200.0f), ImGuiCond_Always);
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("Kiosk In-Game Overlay", nullptr, flags)) {
        ImGui::TextWrapped("Emulation is currently paused.");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Resume Game", ImVec2(-1.0f, 40.0f))) {
            KioskManager::GetInstance().ResumeEmulation();
        }
        ImGui::Spacing();
        if (ImGui::Button("Quit to Kiosk Menu", ImVec2(-1.0f, 40.0f))) {
            KioskManager::GetInstance().AbortSessionToShell();
        }
    }
    ImGui::End();
}

void KioskUI::CompositeActiveState() {
    if (!m_context_initialized || !m_backend_initialized || !m_window) return;

    KioskState state = KioskManager::GetInstance().GetState();
    if (state != KioskState::STATE_COUNTDOWN_IDLE &&
        state != KioskState::STATE_SHELL_INTERACTIVE &&
        state != KioskState::STATE_RUNTIME_OVERLAY) {
        return;
    }

    NewFrame();
    if (state == KioskState::STATE_COUNTDOWN_IDLE) {
        RenderCountdownBanner(KioskManager::GetInstance().GetWatchdogRemainingSec());
    } else if (state == KioskState::STATE_SHELL_INTERACTIVE) {
        RenderInteractiveShellPlaceholder();
    } else if (state == KioskState::STATE_RUNTIME_OVERLAY) {
        RenderRuntimeOverlayPlaceholder();
    }
    RenderFrame();
}

std::string KioskUI::GetImGuiVersion() const {
    return IMGUI_VERSION;
}

#endif // defined(C_KIOSK)
