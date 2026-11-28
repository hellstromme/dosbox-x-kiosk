#ifndef DOSBOX_KIOSK_UI_H
#define DOSBOX_KIOSK_UI_H

#include "../kiosk_types.h"
#include "view_carousel.h"
#include "imgui.h"
#include <atomic>
#include <string>
#include <vector>

struct SDL_Window;
typedef union SDL_Event SDL_Event;

struct KioskThemePalette {
    std::string name;
    ImVec4 accent;
    ImVec4 accent_dim;
    ImVec4 bg_dark;
    ImVec4 card_bg;
    ImVec4 card_border;
    ImVec4 text_primary;
    ImVec4 text_muted;
};

class KioskUI {
public:
    static KioskUI& GetInstance();

    bool Initialize(SDL_Window* window = nullptr, void* gl_context = nullptr);
    bool InitializeContext();
    bool InitializeBackend(SDL_Window* window, void* gl_context);
    void ShutdownBackend();
    void Shutdown();

    bool IsInitialized() const { return m_context_initialized; }
    bool IsBackendInitialized() const { return m_backend_initialized; }

    bool ProcessEvent(const SDL_Event* event);
    void NewFrame();
    void RenderFrame();

    // Active state rendering & compositing
    void RenderCrtBackdrop();
    void RenderCountdownBanner(float remaining_sec);
    void RenderInteractiveShell();
    void RenderInteractiveShellPlaceholder(); // Backwards-compatible alias -> RenderInteractiveShell()
    void RenderRuntimeOverlayPlaceholder();
    void CompositeActiveState();
    void UpdateImGuiNavFlagsForState(KioskState state);

    // Theme & Visual Styling
    void ApplyTheme(const std::string& theme_name);
    const KioskThemePalette& GetThemePalette() const { return m_theme; }

    // Hub Navigation & Toast Feedback
    KioskHubTab GetActiveTab() const { return m_active_tab; }
    void SetActiveTab(KioskHubTab tab);
    void ShowToast(const std::string& message, float duration_sec = 3.0f);
    std::string GetActiveToastMessage() const { return (m_toast_timer > 0.0f) ? m_toast_message : ""; }

    // Zero-per-frame-SQLite catalog cache
    void InvalidateCatalogCache();
    void InvalidateSettingsDraft();
    void EnsureCatalogCacheLoaded();
    const KioskConfigRecord& GetCachedConfig() const { return m_cached_config; }
    const std::vector<CatalogItemRecord>& GetCachedItems() const { return m_cached_items; }
    const std::vector<MachineRecord>& GetCachedMachines() const { return m_cached_machines; }
    const std::vector<OSDistributionRecord>& GetCachedOSDistributions() const { return m_cached_os_dists; }
    const std::vector<MediaPoolRecord>& GetCachedMedia() const { return m_cached_media; }

    std::string GetImGuiVersion() const;

private:
    KioskUI();
    ~KioskUI();
    KioskUI(const KioskUI&) = delete;
    KioskUI& operator=(const KioskUI&) = delete;

    void ProcessPendingInputActions();
    void RenderSettingsTab();
    void RenderToastOverlay(float delta_sec);

    bool m_context_initialized = false;
    bool m_backend_initialized = false;
    std::atomic<uint64_t> m_cache_generation{1};
    uint64_t m_loaded_generation = 0;        // m_cache_generation + DB change generation at load
    uint64_t m_loaded_config_generation = 0; // DB config generation the settings draft was taken from
    SDL_Window* m_window = nullptr;

    KioskHubTab m_active_tab = KioskHubTab::GAMES;
    KioskThemePalette m_theme;

    // Cached SQLite records (reloaded only on invalidation)
    KioskConfigRecord m_cached_config;
    std::vector<CatalogItemRecord> m_cached_items;
    std::vector<MachineRecord> m_cached_machines;
    std::vector<OSDistributionRecord> m_cached_os_dists;
    std::vector<MediaPoolRecord> m_cached_media;

    // Settings editor draft state
    bool m_settings_draft_loaded = false;
    KioskConfigRecord m_settings_draft;
    char m_settings_hotkey_buf[32] = {0};

    // Toast notification state
    std::string m_toast_message;
    float m_toast_timer = 0.0f;
    uint32_t m_last_frame_ticks = 0;
};

#endif // DOSBOX_KIOSK_UI_H
