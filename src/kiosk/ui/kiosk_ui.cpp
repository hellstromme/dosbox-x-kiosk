#include "config.h"

#if defined(C_KIOSK)

#include "kiosk_ui.h"
#include "input_router.h"
#include "view_watchdog.h"
#include "view_carousel.h"
#include "view_wizard.h"
#include "runtime_overlay.h"
#include "../kiosk_main.h"
#include "../kiosk_db.h"
#include "logging.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#include "SDL.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

KioskUI& KioskUI::GetInstance() {
    static KioskUI instance;
    return instance;
}

KioskUI::KioskUI() {
    ApplyTheme("crt_amber");
}

KioskUI::~KioskUI() {
    Shutdown();
}

void KioskUI::ApplyTheme(const std::string& theme_name) {
    if (theme_name == "crt_green") {
        m_theme.name         = "crt_green";
        m_theme.accent       = ImVec4(0.22f, 0.92f, 0.42f, 1.00f);
        m_theme.accent_dim   = ImVec4(0.10f, 0.30f, 0.16f, 0.92f);
        m_theme.bg_dark      = ImVec4(0.04f, 0.07f, 0.05f, 1.00f);
        m_theme.card_bg      = ImVec4(0.08f, 0.12f, 0.09f, 0.96f);
        m_theme.card_border  = ImVec4(0.18f, 0.42f, 0.24f, 1.00f);
        m_theme.text_primary = ImVec4(0.90f, 0.98f, 0.92f, 1.00f);
        m_theme.text_muted   = ImVec4(0.56f, 0.72f, 0.60f, 1.00f);
    } else if (theme_name == "classic_blue") {
        m_theme.name         = "classic_blue";
        m_theme.accent       = ImVec4(0.32f, 0.68f, 1.00f, 1.00f);
        m_theme.accent_dim   = ImVec4(0.14f, 0.28f, 0.48f, 0.92f);
        m_theme.bg_dark      = ImVec4(0.05f, 0.08f, 0.14f, 1.00f);
        m_theme.card_bg      = ImVec4(0.09f, 0.13f, 0.22f, 0.96f);
        m_theme.card_border  = ImVec4(0.22f, 0.38f, 0.62f, 1.00f);
        m_theme.text_primary = ImVec4(0.92f, 0.96f, 1.00f, 1.00f);
        m_theme.text_muted   = ImVec4(0.60f, 0.70f, 0.84f, 1.00f);
    } else {
        // Default: crt_amber
        m_theme.name         = "crt_amber";
        m_theme.accent       = ImVec4(1.00f, 0.69f, 0.12f, 1.00f);
        m_theme.accent_dim   = ImVec4(0.38f, 0.24f, 0.06f, 0.92f);
        m_theme.bg_dark      = ImVec4(0.06f, 0.05f, 0.04f, 1.00f);
        m_theme.card_bg      = ImVec4(0.11f, 0.10f, 0.09f, 0.96f);
        m_theme.card_border  = ImVec4(0.36f, 0.28f, 0.16f, 1.00f);
        m_theme.text_primary = ImVec4(0.98f, 0.94f, 0.86f, 1.00f);
        m_theme.text_muted   = ImVec4(0.72f, 0.65f, 0.54f, 1.00f);
    }

    if (m_context_initialized) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 4.0f;
        style.Colors[ImGuiCol_WindowBg]        = m_theme.bg_dark;
        style.Colors[ImGuiCol_ChildBg]         = m_theme.card_bg;
        style.Colors[ImGuiCol_Border]          = m_theme.card_border;
        style.Colors[ImGuiCol_Text]            = m_theme.text_primary;
        style.Colors[ImGuiCol_Button]          = m_theme.accent_dim;
        style.Colors[ImGuiCol_ButtonHovered]   = m_theme.accent;
        style.Colors[ImGuiCol_ButtonActive]    = m_theme.accent;
        style.Colors[ImGuiCol_Header]          = m_theme.accent_dim;
        style.Colors[ImGuiCol_HeaderHovered]   = m_theme.accent;
        style.Colors[ImGuiCol_CheckMark]       = m_theme.accent;
        style.Colors[ImGuiCol_SliderGrab]      = m_theme.accent;
        style.Colors[ImGuiCol_FrameBg]         = ImVec4(0.14f, 0.14f, 0.16f, 0.95f);
        style.Colors[ImGuiCol_FrameBgHovered]  = m_theme.accent_dim;
    }
}

bool KioskUI::InitializeContext() {
    if (m_context_initialized) return true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr; // Do not pollute working directory with imgui.ini

    ImGui::StyleColorsDark();
    m_context_initialized = true;
    ApplyTheme(m_theme.name);

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
        m_settings_draft_loaded = false;
        m_cached_items.clear();
        m_cached_machines.clear();
        m_cached_os_dists.clear();
        m_cached_media.clear();
        LOG_MSG("KIOSK: Dear ImGui shutdown completed.");
    }
}

bool KioskUI::ProcessEvent(const SDL_Event* event) {
    if (!event) return false;
    bool handled = false;
    if (m_backend_initialized && m_window) {
        handled = ImGui_ImplSDL2_ProcessEvent(event);
    }

    if (KioskManager::GetInstance().GetState() == KioskState::STATE_SHELL_INTERACTIVE) {
        const bool want_text = m_context_initialized ? ImGui::GetIO().WantTextInput : false;
        const bool popup_or_active =
            m_context_initialized &&
            (ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
        const bool is_carousel_tab =
            (m_active_tab == KioskHubTab::GAMES ||
             m_active_tab == KioskHubTab::APPLICATIONS ||
             m_active_tab == KioskHubTab::MACHINES);
        // Single-owner navigation rule:
        // - On carousel tabs without an active widget/popup/text input, KioskInputRouter is the
        //   sole owner of directional & confirm input (ImGui keyboard/gamepad nav is disabled).
        // - On form/modal views (Wizard, Settings, Media Pool, or open modals), ImGui nav owns
        //   focused widget activation and KioskInputRouter suppresses confirm while focused.
        const bool router_owns_nav = is_carousel_tab && !want_text && !popup_or_active;
        const bool suppress_confirm =
            !router_owns_nav && m_context_initialized &&
            (ImGui::IsAnyItemFocused() || ImGui::GetIO().NavVisible);
        if (KioskInputRouter::GetInstance().ProcessSDLEvent(
                *event, want_text, 0, popup_or_active, suppress_confirm)) {
            handled = true;
        }
    }
    return handled;
}

void KioskUI::NewFrame() {
    if (!m_context_initialized) return;
    if (m_backend_initialized && m_window) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
    } else {
        // Headless fallback (unit tests without SDL2/OpenGL backend)
        ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
            io.DisplaySize = ImVec2(1280.0f, 720.0f);
        }
        if (io.DeltaTime <= 0.0f) {
            io.DeltaTime = 1.0f / 60.0f;
        }
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        if (io.Fonts && !io.Fonts->IsBuilt()) {
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
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

void KioskUI::InvalidateCatalogCache() {
    m_cache_loaded = false;
    m_settings_draft_loaded = false;
    ViewWatchdog::GetInstance().InvalidatePreview();
}

void KioskUI::EnsureCatalogCacheLoaded() {
    if (m_cache_loaded) return;

    KioskDatabase& db = KioskDatabase::GetInstance();
    if (db.IsOpen()) {
        if (!db.GetConfig(m_cached_config)) {
            m_cached_config = KioskManager::GetInstance().GetConfig();
        }
        m_cached_items = db.GetAllCatalogItems();
        m_cached_machines = db.GetAllMachines();
        m_cached_os_dists = db.GetAllOSDistributions();
        m_cached_media = db.GetAllMedia();
    } else {
        m_cached_config = KioskManager::GetInstance().GetConfig();
        m_cached_items.clear();
        m_cached_machines.clear();
        m_cached_os_dists.clear();
        m_cached_media.clear();
    }

    if (m_cached_config.theme_name != m_theme.name && !m_cached_config.theme_name.empty()) {
        ApplyTheme(m_cached_config.theme_name);
    }

    m_cache_loaded = true;
}

void KioskUI::SetActiveTab(KioskHubTab tab) {
    m_active_tab = tab;
    ViewCarousel::GetInstance().ResetTransientModals();
}

void KioskUI::ShowToast(const std::string& message, float duration_sec) {
    m_toast_message = message;
    m_toast_timer = (duration_sec > 0.0f) ? duration_sec : 2.5f;
}

void KioskUI::RenderCrtBackdrop() {
    if (!m_context_initialized) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 pos = vp ? vp->Pos : ImVec2(0.0f, 0.0f);
    const ImVec2 sz = vp ? vp->Size : ImVec2(1280.0f, 720.0f);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (!bg) return;

    // Deep tinted CRT base fill
    const ImU32 bg_col = ImGui::ColorConvertFloat4ToU32(m_theme.bg_dark);
    bg->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), bg_col);

    // Subtle retro grid lines
    const ImU32 grid_col = IM_COL32(255, 255, 255, 10);
    for (float gx = pos.x; gx < pos.x + sz.x; gx += 64.0f) {
        bg->AddLine(ImVec2(gx, pos.y), ImVec2(gx, pos.y + sz.y), grid_col, 1.0f);
    }
    for (float gy = pos.y; gy < pos.y + sz.y; gy += 64.0f) {
        bg->AddLine(ImVec2(pos.x, gy), ImVec2(pos.x + sz.x, gy), grid_col, 1.0f);
    }

    // Subtle horizontal CRT scanlines
    const ImU32 scan_col = IM_COL32(0, 0, 0, 32);
    for (float sy = pos.y; sy < pos.y + sz.y; sy += 4.0f) {
        bg->AddLine(ImVec2(pos.x, sy), ImVec2(pos.x + sz.x, sy), scan_col, 1.0f);
    }

    // Corner vignette framing
    const ImU32 vig_edge = IM_COL32(0, 0, 0, 120);
    const ImU32 vig_clear = IM_COL32(0, 0, 0, 0);
    bg->AddRectFilledMultiColor(
        pos, ImVec2(pos.x + sz.x, pos.y + 42.0f),
        vig_edge, vig_edge, vig_clear, vig_clear
    );
    bg->AddRectFilledMultiColor(
        ImVec2(pos.x, pos.y + sz.y - 42.0f), ImVec2(pos.x + sz.x, pos.y + sz.y),
        vig_clear, vig_clear, vig_edge, vig_edge
    );
}

void KioskUI::UpdateImGuiNavFlagsForState(KioskState state) {
    if (!m_context_initialized) return;
    ImGuiIO& io = ImGui::GetIO();

    bool allow_imgui_nav = true;
    if (state == KioskState::STATE_SHELL_INTERACTIVE) {
        const bool is_carousel_tab =
            (m_active_tab == KioskHubTab::GAMES ||
             m_active_tab == KioskHubTab::APPLICATIONS ||
             m_active_tab == KioskHubTab::MACHINES);
        allow_imgui_nav =
            !is_carousel_tab || io.WantTextInput ||
            ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    }

    if (allow_imgui_nav) {
        io.ConfigFlags |= (ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    } else {
        io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    }
}

void KioskUI::RenderCountdownBanner(float remaining_sec) {
    if (!m_context_initialized) return;
    UpdateImGuiNavFlagsForState(KioskState::STATE_COUNTDOWN_IDLE);
    EnsureCatalogCacheLoaded();
    RenderCrtBackdrop();
    ViewWatchdog::GetInstance().Render(remaining_sec);
}

void KioskUI::ProcessPendingInputActions() {
    KioskInputRouter& router = KioskInputRouter::GetInstance();
    const bool popup_open = m_context_initialized && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    const bool suppress_repeat =
        m_context_initialized &&
        (ImGui::IsAnyItemActive() || popup_open);
    router.UpdateRepeat(SDL_GetTicks(), suppress_repeat);

    KioskInputAction action = KioskInputAction::NONE;
    while (router.PollAction(action)) {
        // Reload cache before each queued action so consecutive mutations (e.g., double favorite)
        // observe the latest persisted state instead of stale cached records.
        EnsureCatalogCacheLoaded();

        // While a modal popup is open, only route ACTION_BACK to dismiss the modal without
        // switching hub tabs or dispatching actions to the underlying carousel.
        if (popup_open) {
            if (action == KioskInputAction::ACTION_BACK) {
                ViewCarousel::GetInstance().HandleAction(m_active_tab, action);
            }
            continue;
        }

        if (action == KioskInputAction::NAV_TAB_PREV) {
            int idx = static_cast<int>(m_active_tab);
            idx = (idx - 1 + 6) % 6;
            SetActiveTab(static_cast<KioskHubTab>(idx));
            continue;
        }
        if (action == KioskInputAction::NAV_TAB_NEXT) {
            int idx = static_cast<int>(m_active_tab);
            idx = (idx + 1) % 6;
            SetActiveTab(static_cast<KioskHubTab>(idx));
            continue;
        }

        if (m_active_tab == KioskHubTab::NEW_MACHINE) {
            ViewWizard::GetInstance().HandleAction(action);
        } else if (m_active_tab != KioskHubTab::SETTINGS) {
            ViewCarousel::GetInstance().HandleAction(m_active_tab, action);
        }
    }
}

void KioskUI::RenderSettingsTab() {
    if (!m_settings_draft_loaded) {
        m_settings_draft = m_cached_config;
        std::snprintf(m_settings_hotkey_buf, sizeof(m_settings_hotkey_buf), "%s", m_settings_draft.overlay_hotkey.c_str());
        m_settings_draft_loaded = true;
    }

    ImGui::Text("Global Kiosk Appliance Settings");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderInt("Auto-Boot Watchdog Timeout (seconds)", &m_settings_draft.boot_timeout_sec, 0, 120, "%d s");

    static const char* kBootActions[4] = {
        "DEFAULT_DOSBOX", "LAST_PLAYED", "SPECIFIC_MACHINE", "SPECIFIC_ITEM"
    };
    int boot_idx = 0;
    for (int i = 0; i < 4; ++i) {
        if (m_settings_draft.default_boot_action == kBootActions[i]) boot_idx = i;
    }
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::Combo("Default Auto-Boot Action", &boot_idx, kBootActions, 4)) {
        m_settings_draft.default_boot_action = kBootActions[boot_idx];
        if (m_settings_draft.default_boot_action == "SPECIFIC_MACHINE") {
            bool valid = false;
            for (size_t i = 0; i < m_cached_machines.size(); ++i) {
                if (m_cached_machines[i].id == m_settings_draft.default_target_uuid) valid = true;
            }
            if (!valid) {
                m_settings_draft.default_target_uuid = m_cached_machines.empty() ? "" : m_cached_machines[0].id;
            }
        } else if (m_settings_draft.default_boot_action == "SPECIFIC_ITEM") {
            bool valid = false;
            for (size_t i = 0; i < m_cached_items.size(); ++i) {
                if (m_cached_items[i].id == m_settings_draft.default_target_uuid) valid = true;
            }
            if (!valid) {
                m_settings_draft.default_target_uuid = m_cached_items.empty() ? "" : m_cached_items[0].id;
            }
        } else {
            m_settings_draft.default_target_uuid.clear();
        }
    }

    if (m_settings_draft.default_boot_action == "SPECIFIC_MACHINE") {
        std::string cur_mach = "(None Available)";
        for (size_t i = 0; i < m_cached_machines.size(); ++i) {
            if (m_cached_machines[i].id == m_settings_draft.default_target_uuid) {
                cur_mach = m_cached_machines[i].display_name;
                break;
            }
        }
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::BeginCombo("Target Machine", cur_mach.c_str())) {
            for (size_t i = 0; i < m_cached_machines.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(m_cached_machines[i].display_name.c_str(),
                                      m_cached_machines[i].id == m_settings_draft.default_target_uuid)) {
                    m_settings_draft.default_target_uuid = m_cached_machines[i].id;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    } else if (m_settings_draft.default_boot_action == "SPECIFIC_ITEM") {
        std::string cur_item = "(None Available)";
        for (size_t i = 0; i < m_cached_items.size(); ++i) {
            if (m_cached_items[i].id == m_settings_draft.default_target_uuid) {
                cur_item = m_cached_items[i].title;
                break;
            }
        }
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::BeginCombo("Target Catalog Item", cur_item.c_str())) {
            for (size_t i = 0; i < m_cached_items.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(m_cached_items[i].title.c_str(),
                                      m_cached_items[i].id == m_settings_draft.default_target_uuid)) {
                    m_settings_draft.default_target_uuid = m_cached_items[i].id;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    static const char* kCaddyViews[2] = {"SKEUOMORPHIC", "LIST"};
    int caddy_idx = (m_settings_draft.default_caddy_view == "LIST") ? 1 : 0;
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::Combo("Default Disk Caddy View", &caddy_idx, kCaddyViews, 2)) {
        m_settings_draft.default_caddy_view = kCaddyViews[caddy_idx];
    }

    static const char* kThemes[3] = {"crt_amber", "crt_green", "classic_blue"};
    int theme_idx = 0;
    for (int i = 0; i < 3; ++i) {
        if (m_settings_draft.theme_name == kThemes[i]) theme_idx = i;
    }
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::Combo("CRT Interface Theme", &theme_idx, kThemes, 3)) {
        m_settings_draft.theme_name = kThemes[theme_idx];
        ApplyTheme(m_settings_draft.theme_name);
    }

    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputText("Runtime Overlay Hotkey", m_settings_hotkey_buf, sizeof(m_settings_hotkey_buf))) {
        m_settings_draft.overlay_hotkey = m_settings_hotkey_buf;
    }

    ImGui::Checkbox("Auto-mount removable USB DOCUMENTS folder as Drive E:", &m_settings_draft.auto_mount_usb_docs);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Save Settings", ImVec2(150.0f, 36.0f))) {
        bool target_valid = true;
        if (m_settings_draft.default_boot_action == "SPECIFIC_MACHINE") {
            target_valid = false;
            for (size_t i = 0; i < m_cached_machines.size(); ++i) {
                if (m_cached_machines[i].id == m_settings_draft.default_target_uuid) target_valid = true;
            }
            if (!target_valid) ShowToast("Please select a valid Target Machine before saving.");
        } else if (m_settings_draft.default_boot_action == "SPECIFIC_ITEM") {
            target_valid = false;
            for (size_t i = 0; i < m_cached_items.size(); ++i) {
                if (m_cached_items[i].id == m_settings_draft.default_target_uuid) target_valid = true;
            }
            if (!target_valid) ShowToast("Please select a valid Target Catalog Item before saving.");
        }

        if (target_valid) {
            if (KioskDatabase::GetInstance().UpdateConfig(m_settings_draft)) {
                KioskManager::GetInstance().ReloadConfig();
                InvalidateCatalogCache();
                ShowToast("Kiosk settings saved.");
            } else {
                ShowToast("Failed to save Kiosk settings.");
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Exit Kiosk to Host OS", ImVec2(190.0f, 36.0f))) {
        KioskManager::GetInstance().RequestApplianceExit();
    }
}

void KioskUI::RenderToastOverlay(float delta_sec) {
    if (m_toast_timer <= 0.0f || m_toast_message.empty()) return;
    m_toast_timer -= delta_sec;
    if (m_toast_timer <= 0.0f) {
        m_toast_timer = 0.0f;
        m_toast_message.clear();
        return;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 pos = vp ? ImVec2(vp->WorkPos.x + vp->WorkSize.x - 24.0f, vp->WorkPos.y + vp->WorkSize.y - 54.0f)
                          : ImVec2(1240.0f, 660.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleColor(ImGuiCol_Border, m_theme.accent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    if (ImGui::Begin("##KioskToast", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, m_theme.accent);
        ImGui::TextUnformatted(m_toast_message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void KioskUI::RenderInteractiveShell() {
    if (!m_context_initialized) return;

    UpdateImGuiNavFlagsForState(KioskState::STATE_SHELL_INTERACTIVE);
    EnsureCatalogCacheLoaded();
    ProcessPendingInputActions();
    RenderCrtBackdrop();

    const uint32_t now_ticks = SDL_GetTicks();
    const float delta_sec = (m_last_frame_ticks > 0 && now_ticks >= m_last_frame_ticks)
                                ? static_cast<float>(now_ticks - m_last_frame_ticks) / 1000.0f
                                : 0.016f;
    m_last_frame_ticks = now_ticks;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 vp_pos = vp ? vp->WorkPos : ImVec2(0.0f, 0.0f);
    const ImVec2 vp_size = vp ? vp->WorkSize : ImVec2(1280.0f, 720.0f);

    ImGui::SetNextWindowPos(vp_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp_size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.76f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 16.0f));
    if (ImGui::Begin("##KioskBigPictureHub", nullptr, flags)) {
        // --- 1. Top Navigation Header Bar (wraps cleanly on 640x480 viewports) ---
        const float win_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        ImGui::PushStyleColor(ImGuiCol_Text, m_theme.accent);
        ImGui::SetWindowFontScale(1.25f);
        ImGui::Text("DOSBOX-X KIOSK");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        static const char* kTabNames[6] = {
            "Games", "Applications", "Machines", "Media Pool", "+ New Machine", "Settings"
        };
        const float pad_x = ImGui::GetStyle().FramePadding.x * 2.0f;
        for (int i = 0; i < 6; ++i) {
            const float btn_w = ImGui::CalcTextSize(kTabNames[i]).x + pad_x + 4.0f;
            const float gap = (i == 0) ? 18.0f : 6.0f;
            if (ImGui::GetItemRectMax().x + gap + btn_w <= win_right) {
                ImGui::SameLine(0.0f, gap);
            }
            const bool active = (static_cast<int>(m_active_tab) == i);
            ImGui::PushStyleColor(ImGuiCol_Button, active ? m_theme.accent : m_theme.card_bg);
            ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.08f, 0.08f, 0.08f, 1.0f) : m_theme.text_primary);
            if (ImGui::Button(kTabNames[i], ImVec2(0.0f, 30.0f))) {
                SetActiveTab(static_cast<KioskHubTab>(i));
            }
            ImGui::PopStyleColor(2);
        }

        if (ImGui::GetItemRectMax().x + 12.0f + 95.0f <= win_right) {
            ImGui::SameLine(0.0f, 12.0f);
        }
        if (ImGui::Button("Quick DOS", ImVec2(95.0f, 30.0f))) {
            KioskManager::GetInstance().RequestDefaultDosboxLaunch();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- 2. Central Hub Content Region ---
        const float footer_reserve = 38.0f;
        const float body_h = (std::max)(120.0f, ImGui::GetContentRegionAvail().y - footer_reserve);
        if (ImGui::BeginChild("##KioskHubBody", ImVec2(-1.0f, body_h), false)) {
            switch (m_active_tab) {
                case KioskHubTab::GAMES:        ViewCarousel::GetInstance().RenderGamesTab(); break;
                case KioskHubTab::APPLICATIONS: ViewCarousel::GetInstance().RenderApplicationsTab(); break;
                case KioskHubTab::MACHINES:     ViewCarousel::GetInstance().RenderMachinesTab(); break;
                case KioskHubTab::MEDIA_POOL:   ViewCarousel::GetInstance().RenderMediaPoolTab(); break;
                case KioskHubTab::NEW_MACHINE:  ViewWizard::GetInstance().Render(); break;
                case KioskHubTab::SETTINGS:     RenderSettingsTab(); break;
            }
        }
        ImGui::EndChild();

        // --- 3. Bottom Control Hint Footer (adapts to narrow 640x480 viewports) ---
        ImGui::Separator();
        const char* hint_text = (vp_size.x >= 960.0f)
            ? "Arrows/D-Pad: Browse  |  Q/E or LB/RB: Tabs  |  Enter/A: Launch  |  M/Start: Actions  |  C/X: Caddy  |  F12+D: Disks"
            : "Arrows/D-Pad: Browse | Enter/A: Launch | M/Start: Actions | F12+D: Disks";
        ImGui::PushStyleColor(ImGuiCol_Text, m_theme.text_muted);
        ImGui::TextUnformatted(hint_text);
        ImGui::PopStyleColor();
        const float exit_btn_w = 88.0f;
        if (ImGui::GetItemRectMax().x + 12.0f + exit_btn_w <= win_right) {
            ImGui::SameLine((std::max)(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f,
                                       ImGui::GetWindowContentRegionMax().x - exit_btn_w));
        }
        if (ImGui::SmallButton("Exit Kiosk")) {
            KioskManager::GetInstance().RequestApplianceExit();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    RenderToastOverlay(delta_sec);
}

void KioskUI::RenderInteractiveShellPlaceholder() {
    RenderInteractiveShell();
}

void KioskUI::RenderRuntimeOverlayPlaceholder() {
    if (!m_context_initialized) return;
    UpdateImGuiNavFlagsForState(KioskState::STATE_RUNTIME_OVERLAY);
    RuntimeOverlay::GetInstance().Render();
}

void KioskUI::CompositeActiveState() {
    if (!m_context_initialized || !m_backend_initialized || !m_window) return;

    KioskState state = KioskManager::GetInstance().GetState();
    if (state != KioskState::STATE_COUNTDOWN_IDLE &&
        state != KioskState::STATE_SHELL_INTERACTIVE &&
        state != KioskState::STATE_RUNTIME_OVERLAY) {
        return;
    }

    UpdateImGuiNavFlagsForState(state);
    NewFrame();
    if (state == KioskState::STATE_COUNTDOWN_IDLE) {
        RenderCountdownBanner(KioskManager::GetInstance().GetWatchdogRemainingSec());
    } else if (state == KioskState::STATE_SHELL_INTERACTIVE) {
        RenderInteractiveShell();
    } else if (state == KioskState::STATE_RUNTIME_OVERLAY) {
        RenderRuntimeOverlayPlaceholder();
    }
    RenderFrame();
}

std::string KioskUI::GetImGuiVersion() const {
    return IMGUI_VERSION;
}

#endif // defined(C_KIOSK)
