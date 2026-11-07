#include "config.h"

#if defined(C_KIOSK)

#include "kiosk_main.h"
#include "kiosk_db.h"
#include "machine/archetypes.h"
#include "machine/config_synthesizer.h"
#include "machine/boot_injector.h"
#include "ui/kiosk_ui.h"
#include "control.h"
#include "dosbox.h"
#include "logging.h"
#include "SDL.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

extern void Kiosk_PumpPreBootFrame(void);

KioskManager::KioskManager()
    : m_state(KioskState::DISABLED),
      m_watchdog_timer(30.0f),
      m_initialized(false),
      m_kiosk_active(false),
      m_skip_countdown(false),
      m_exit_requested(false),
      m_last_shutdown_reason(KioskShutdownReason::NONE),
      m_completed_sessions(0) {
    // Ensure dependent singletons are constructed before KioskManager so their
    // static lifetimes outlive KioskManager::~KioskManager() at process exit.
    (void)KioskDatabase::GetInstance();
    (void)KioskUI::GetInstance();
    (void)BootInjector::GetInstance();
}

KioskManager& KioskManager::GetInstance() {
    static KioskManager instance;
    return instance;
}

KioskManager::~KioskManager() {
    Shutdown();
}

void KioskManager::CleanupSessionArtifactsLocked() {
    if (!m_active_dispatch_image_path.empty()) {
        BootInjector::GetInstance().CleanupDispatchVolume(m_active_dispatch_image_path);
        m_active_dispatch_image_path.clear();
    }
    if (!m_session_conf_path.empty()) {
        std::remove(m_session_conf_path.c_str());
        m_session_conf_path.clear();
    }
}

bool KioskManager::Initialize(const std::string& db_path, const std::string& base_conf) {
    LOG_MSG("KIOSK: Initializing DOSBox-X Kiosk Subsystems...");

    // 1. Initialize SQLite Catalog Database (preserve existing open DB if db_path is empty during tests)
    if (!db_path.empty() || !KioskDatabase::GetInstance().IsOpen()) {
        if (!KioskDatabase::GetInstance().Open(db_path)) {
            LOG_MSG("KIOSK: Warning: Failed to open catalog DB '%s', falling back to in-memory database", db_path.c_str());
            if (!KioskDatabase::GetInstance().Open(":memory:")) {
                LOG_MSG("KIOSK: Error: Failed to open in-memory fallback catalog database");
            }
        }
    }

    // 2. Load global Kiosk configuration from catalog_db
    KioskConfigRecord loaded_cfg;
    if (!KioskDatabase::GetInstance().GetConfig(loaded_cfg)) {
        loaded_cfg = KioskConfigRecord();
    }

    // 3. Initialize Dear ImGui UI Context (headless phase)
    if (!KioskUI::GetInstance().IsInitialized()) {
        if (!KioskUI::GetInstance().Initialize(nullptr, nullptr)) {
            LOG_MSG("KIOSK: Warning: ImGui headless initialization failed");
        }
    }
    KioskUI::GetInstance().InvalidateCatalogCache();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CleanupSessionArtifactsLocked();
        m_config = loaded_cfg;
        m_base_conf_path = base_conf;
        m_watchdog_timer = m_skip_countdown.load() ? 0.0f : static_cast<float>(m_config.boot_timeout_sec);
        m_launch_ctx = LaunchContext();
        m_last_synthesized_conf.clear();
        m_initialized = true;
    }

    m_exit_requested.store(false);
    m_last_shutdown_reason.store(KioskShutdownReason::NONE);
    m_completed_sessions.store(0);
    m_state.store(KioskState::STATE_KIOSK_BOOT);

    LOG_MSG("KIOSK: Subsystems initialized successfully (SQLite %s, ImGui %s).",
            KioskDatabase::GetInstance().GetSQLiteVersion().c_str(),
            KioskUI::GetInstance().GetImGuiVersion().c_str());

    if (m_skip_countdown.load()) {
        m_state.store(KioskState::STATE_SHELL_INTERACTIVE);
        LOG_MSG("KIOSK: Entering STATE_SHELL_INTERACTIVE (--kiosk-skip-countdown active)");
    } else if (loaded_cfg.boot_timeout_sec > 0) {
        m_state.store(KioskState::STATE_COUNTDOWN_IDLE);
        LOG_MSG("KIOSK: Entering STATE_COUNTDOWN_IDLE (Watchdog: %ds, Action: %s)",
                loaded_cfg.boot_timeout_sec, loaded_cfg.default_boot_action.c_str());
    } else {
        TriggerDefaultBootAction();
    }

    return true;
}

void KioskManager::RunMainLoop() {
    LOG_MSG("KIOSK: Entering Kiosk Main Loop (State: %d)", static_cast<int>(m_state.load()));

    const bool headless_or_auto_exit =
        (control != nullptr && (control->opt_exit || control->opt_test || control->opt_nogui)) ||
        !KioskUI::GetInstance().IsBackendInitialized();

    if (headless_or_auto_exit) {
        KioskState cur = m_state.load();
        if (cur == KioskState::STATE_COUNTDOWN_IDLE || cur == KioskState::STATE_SHELL_INTERACTIVE) {
            TriggerDefaultBootAction();
        }
    } else {
        uint32_t last_ticks = SDL_GetTicks();
        while (!m_exit_requested.load()) {
            KioskState cur = m_state.load();
            if (cur != KioskState::STATE_COUNTDOWN_IDLE && cur != KioskState::STATE_SHELL_INTERACTIVE) {
                break;
            }

            uint32_t now_ticks = SDL_GetTicks();
            float delta_sec = (now_ticks >= last_ticks)
                                  ? (static_cast<float>(now_ticks - last_ticks) / 1000.0f)
                                  : 0.016f;
            last_ticks = now_ticks;

            if (cur == KioskState::STATE_COUNTDOWN_IDLE) {
                UpdateWatchdog(delta_sec);
            }

            Kiosk_PumpPreBootFrame();
        }
    }

    if (m_state.load() == KioskState::STATE_SYNTHESIZE_LAUNCH && !m_exit_requested.load()) {
        std::string conf;
        SynthesizeAndPrepareLaunch(conf);
    }
}

void KioskManager::Shutdown() {
    bool was_init = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CleanupSessionArtifactsLocked();
        was_init = m_initialized;
        m_initialized = false;
    }

    if (was_init) {
        LOG_MSG("KIOSK: Shutting down Kiosk Subsystems...");
        KioskUI::GetInstance().Shutdown();
        KioskDatabase::GetInstance().Close();
        m_state.store(KioskState::DISABLED);
        m_kiosk_active.store(false);
        m_skip_countdown.store(false);
    }
}

KioskConfigRecord KioskManager::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

float KioskManager::GetWatchdogRemainingSec() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_watchdog_timer;
}

void KioskManager::SetWatchdogRemainingSec(float sec) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_watchdog_timer = (sec < 0.0f) ? 0.0f : sec;
}

void KioskManager::UpdateWatchdog(float delta_sec) {
    if (m_state.load() != KioskState::STATE_COUNTDOWN_IDLE) {
        return;
    }
    if (delta_sec <= 0.0f) {
        return;
    }

    bool expired = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_watchdog_timer -= delta_sec;
        if (m_watchdog_timer <= 0.0f) {
            m_watchdog_timer = 0.0f;
            expired = true;
        }
    }

    if (expired) {
        LOG_MSG("KIOSK: Watchdog timer expired; triggering default boot action.");
        TriggerDefaultBootAction();
    }
}

void KioskManager::CancelWatchdog() {
    if (m_state.load() == KioskState::STATE_COUNTDOWN_IDLE) {
        LOG_MSG("KIOSK: Watchdog countdown cancelled by user input; transitioning to STATE_SHELL_INTERACTIVE.");
        m_state.store(KioskState::STATE_SHELL_INTERACTIVE);
    }
}

bool KioskManager::IsIntentionalUserInput(const SDL_Event& event) {
    switch (event.type) {
        case SDL_KEYDOWN:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEWHEEL:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_JOYBUTTONDOWN:
            return true;

        case SDL_MOUSEMOTION:
            return (std::abs(event.motion.xrel) >= 5 || std::abs(event.motion.yrel) >= 5);

        case SDL_CONTROLLERAXISMOTION:
            return (std::abs(static_cast<int>(event.caxis.value)) > 8000);

        case SDL_JOYAXISMOTION:
            return (std::abs(static_cast<int>(event.jaxis.value)) > 8000);

        default:
            return false;
    }
}

bool KioskManager::HandlePreBootInputEvent(const SDL_Event& event) {
    if (event.type == SDL_QUIT) {
        RequestApplianceExit();
        return true;
    }

    KioskState cur = m_state.load();
    if (cur == KioskState::STATE_COUNTDOWN_IDLE) {
        if (IsIntentionalUserInput(event)) {
            CancelWatchdog();
            return true;
        }
    }
    return false;
}

LaunchContext KioskManager::ResolveDefaultBootContext() const {
    KioskConfigRecord cfg;
    if (!KioskDatabase::GetInstance().IsOpen() || !KioskDatabase::GetInstance().GetConfig(cfg)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        cfg = m_config;
    }

    LaunchContext ctx;
    KioskDatabase& db = KioskDatabase::GetInstance();

    if (cfg.default_boot_action == "SPECIFIC_MACHINE" && !cfg.default_target_uuid.empty()) {
        MachineRecord mach;
        if (db.GetMachine(cfg.default_target_uuid, mach)) {
            ctx.machine_id = mach.id;
            ctx.is_real_hdd_boot = (mach.boot_mode == "REAL_HDD_BOOT");
            return ctx;
        }
        LOG_MSG("KIOSK: Default target machine '%s' not found; falling back to DEFAULT_DOSBOX.",
                cfg.default_target_uuid.c_str());
    } else if (cfg.default_boot_action == "SPECIFIC_ITEM" && !cfg.default_target_uuid.empty()) {
        CatalogItemRecord item;
        if (db.GetItem(cfg.default_target_uuid, item)) {
            ctx.catalog_item_id = item.id;
            ctx.machine_id = item.machine_id;
            MachineRecord mach;
            if (db.GetMachine(item.machine_id, mach)) {
                ctx.is_real_hdd_boot = (mach.boot_mode == "REAL_HDD_BOOT");
            }
            return ctx;
        }
        LOG_MSG("KIOSK: Default target item '%s' not found; falling back to DEFAULT_DOSBOX.",
                cfg.default_target_uuid.c_str());
    } else if (cfg.default_boot_action == "LAST_PLAYED") {
        auto all_items = db.GetAllCatalogItems();
        const CatalogItemRecord* best_item = nullptr;
        for (const auto& item : all_items) {
            if (!item.last_launched_at.empty()) {
                if (!best_item || item.last_launched_at > best_item->last_launched_at) {
                    best_item = &item;
                }
            }
        }
        if (!best_item && !all_items.empty()) {
            best_item = &all_items[0];
        }
        if (best_item) {
            ctx.catalog_item_id = best_item->id;
            ctx.machine_id = best_item->machine_id;
            MachineRecord mach;
            if (db.GetMachine(best_item->machine_id, mach)) {
                ctx.is_real_hdd_boot = (mach.boot_mode == "REAL_HDD_BOOT");
            }
            return ctx;
        }

        auto machines = db.GetAllMachines();
        if (!machines.empty()) {
            ctx.machine_id = machines[0].id;
            ctx.is_real_hdd_boot = (machines[0].boot_mode == "REAL_HDD_BOOT");
            return ctx;
        }
    }

    // DEFAULT_DOSBOX fallback
    ctx.machine_id.clear();
    ctx.catalog_item_id.clear();
    ctx.is_real_hdd_boot = false;
    return ctx;
}

bool KioskManager::TriggerDefaultBootAction() {
    LaunchContext ctx = ResolveDefaultBootContext();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_launch_ctx = ctx;
    }
    m_state.store(KioskState::STATE_SYNTHESIZE_LAUNCH);
    LOG_MSG("KIOSK: Transitioned to STATE_SYNTHESIZE_LAUNCH (Machine: '%s', Item: '%s').",
            ctx.machine_id.c_str(), ctx.catalog_item_id.c_str());
    return true;
}

LaunchContext KioskManager::GetCurrentLaunchContext() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_launch_ctx;
}

void KioskManager::SetLaunchContext(const LaunchContext& ctx) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_launch_ctx = ctx;
}

bool KioskManager::RequestLaunchMachine(const std::string& machine_id, const std::string& custom_disk_path) {
    MachineRecord mach;
    if (!KioskDatabase::GetInstance().GetMachine(machine_id, mach)) {
        LOG_MSG("KIOSK: RequestLaunchMachine failed: Machine '%s' not found.", machine_id.c_str());
        return false;
    }

    LaunchContext ctx;
    ctx.machine_id = mach.id;
    ctx.catalog_item_id.clear();
    ctx.is_real_hdd_boot = (mach.boot_mode == "REAL_HDD_BOOT");
    ctx.custom_disk_path = custom_disk_path;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_launch_ctx = ctx;
    }
    m_state.store(KioskState::STATE_SYNTHESIZE_LAUNCH);
    return true;
}

bool KioskManager::RequestLaunchItem(const std::string& item_id) {
    CatalogItemRecord item;
    if (!KioskDatabase::GetInstance().GetItem(item_id, item)) {
        LOG_MSG("KIOSK: RequestLaunchItem failed: Item '%s' not found.", item_id.c_str());
        return false;
    }

    MachineRecord mach;
    bool has_mach = KioskDatabase::GetInstance().GetMachine(item.machine_id, mach);

    LaunchContext ctx;
    ctx.catalog_item_id = item.id;
    ctx.machine_id = item.machine_id;
    ctx.is_real_hdd_boot = has_mach && (mach.boot_mode == "REAL_HDD_BOOT");

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_launch_ctx = ctx;
    }
    m_state.store(KioskState::STATE_SYNTHESIZE_LAUNCH);
    return true;
}

bool KioskManager::RequestDefaultDosboxLaunch() {
    LaunchContext ctx;
    ctx.machine_id.clear();
    ctx.catalog_item_id.clear();
    ctx.is_real_hdd_boot = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_launch_ctx = ctx;
    }
    m_state.store(KioskState::STATE_SYNTHESIZE_LAUNCH);
    return true;
}

std::string KioskManager::GetDefaultSessionConfPath() const {
#if defined(_WIN32)
    const char* temp_dir = std::getenv("TEMP");
    if (!temp_dir || !*temp_dir) {
        temp_dir = std::getenv("TMP");
    }
    if (temp_dir && *temp_dir) {
        std::string path(temp_dir);
        if (path.back() != '\\' && path.back() != '/') {
            path += '\\';
        }
        return path + "kiosk_session.conf";
    }
    return "kiosk_session.conf";
#else
    const char* xdg_dir = std::getenv("XDG_RUNTIME_DIR");
    if (xdg_dir && *xdg_dir) {
        std::string path(xdg_dir);
        if (path.back() != '/') path += '/';
        return path + "kiosk_session.conf";
    }
    return "/tmp/kiosk_session.conf";
#endif
}

bool KioskManager::SynthesizeAndPrepareLaunch(std::string& out_synthesized_conf) {
    LaunchContext ctx;
    std::string base_conf;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CleanupSessionArtifactsLocked();
        ctx = m_launch_ctx;
        base_conf = m_base_conf_path;
    }

    KioskDatabase& db = KioskDatabase::GetInstance();
    MachineRecord machine;
    if (!ctx.machine_id.empty()) {
        if (!db.GetMachine(ctx.machine_id, machine)) {
            LOG_MSG("KIOSK: SynthesizeAndPrepareLaunch: Machine '%s' not found, using default 386DX profile.",
                    ctx.machine_id.c_str());
            machine.id = "default";
            machine.display_name = "Default DOSBox-X Machine";
            machine.archetype_id = KioskArchetypes::DX_386;
            machine.archetype_tier = 3;
            machine.boot_mode = "INTEGRATED_DOS";
        }
    } else {
        machine.id = "default";
        machine.display_name = "Default DOSBox-X Machine";
        machine.archetype_id = KioskArchetypes::DX_386;
        machine.archetype_tier = 3;
        machine.boot_mode = "INTEGRATED_DOS";
    }

    CatalogItemRecord item;
    const CatalogItemRecord* item_ptr = nullptr;
    if (!ctx.catalog_item_id.empty()) {
        if (db.GetItem(ctx.catalog_item_id, item)) {
            item_ptr = &item;
            db.IncrementPlayCount(item.id);
        }
    }

    std::vector<CaddyBindingRecord> caddy;
    if (item_ptr) {
        caddy = db.GetCaddyBindingsForItem(item_ptr->id);
    }
    if (caddy.empty() && !ctx.machine_id.empty()) {
        caddy = db.GetCaddyBindingsForMachine(ctx.machine_id);
    }
    if (!ctx.custom_disk_path.empty()) {
        CaddyBindingRecord custom_floppy;
        custom_floppy.machine_id = machine.id;
        custom_floppy.target_drive = "A";
        custom_floppy.slot_order = 0;
        custom_floppy.resolved_file_path = ctx.custom_disk_path;
        custom_floppy.resolved_media_type = "FLOPPY_35_1440K";
        caddy.insert(caddy.begin(), custom_floppy);
    }

    // Phase 3: Ephemeral Boot Injector for REAL_HDD_BOOT machines
    std::string active_dispatch_path;
    if (machine.boot_mode == "REAL_HDD_BOOT") {
        if (!machine.primary_hdd_path.empty()) {
            BootInjector::GetInstance().EnsureOneTimeAutoexecHook(machine.primary_hdd_path);
        }

        std::string dispatch_path = BootInjector::GetInstance().GetDefaultDispatchImagePath();
        if (item_ptr != nullptr) {
            // Mode A: Carousel Game/App Launch -> KIOSKGO.BAT + KIOSKEXT.COM
            if (BootInjector::GetInstance().BuildDispatchVolume(dispatch_path, machine, *item_ptr)) {
                active_dispatch_path = dispatch_path;
            }
        } else {
            // Mode B: Machine-Only Boot -> Idle floppy (KIOSKEXT.COM only, no KIOSKGO.BAT)
            if (BootInjector::GetInstance().BuildIdleDispatchVolume(dispatch_path)) {
                active_dispatch_path = dispatch_path;
            }
        }
    }

    out_synthesized_conf = ConfigSynthesizer::GetInstance().SynthesizeConfig(
        machine, item_ptr, caddy, base_conf, active_dispatch_path
    );

    std::string session_conf_path;
    if (!ctx.machine_id.empty() || !ctx.catalog_item_id.empty() || !base_conf.empty()) {
        std::string candidate_conf = GetDefaultSessionConfPath();
        if (ConfigSynthesizer::GetInstance().WriteConfigFile(out_synthesized_conf, candidate_conf)) {
            session_conf_path = candidate_conf;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_synthesized_conf = out_synthesized_conf;
        m_active_dispatch_image_path = active_dispatch_path;
        m_session_conf_path = session_conf_path;
    }

    m_state.store(KioskState::STATE_EMULATION_RUNNING);
    LOG_MSG("KIOSK: Configuration synthesized (%u bytes); entering STATE_EMULATION_RUNNING.",
            static_cast<unsigned int>(out_synthesized_conf.size()));
    return true;
}

std::string KioskManager::GetLastSynthesizedConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_synthesized_conf;
}

std::string KioskManager::GetActiveDispatchImagePath() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_active_dispatch_image_path;
}

std::string KioskManager::GetSessionConfPath() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session_conf_path;
}

void KioskManager::EnterRuntimeOverlay() {
    if (m_state.load() != KioskState::STATE_EMULATION_RUNNING) {
        return;
    }
    LOG_MSG("KIOSK: Entering STATE_RUNTIME_OVERLAY (Emulation paused).");
    m_state.store(KioskState::STATE_RUNTIME_OVERLAY);
}

void KioskManager::ResumeEmulation() {
    if (m_state.load() != KioskState::STATE_RUNTIME_OVERLAY) {
        return;
    }
    LOG_MSG("KIOSK: Resuming STATE_EMULATION_RUNNING.");
    m_state.store(KioskState::STATE_EMULATION_RUNNING);
}

void KioskManager::AbortSessionToShell() {
    KioskState cur = m_state.load();
    if (cur != KioskState::STATE_RUNTIME_OVERLAY && cur != KioskState::STATE_EMULATION_RUNNING) {
        return;
    }
    LOG_MSG("KIOSK: User aborted active session to return to Kiosk Shell.");
    OnGuestShutdown(KioskShutdownReason::USER_OVERLAY_ABORT);
}

void KioskManager::OnGuestShutdown(KioskShutdownReason reason) {
    LOG_MSG("KIOSK: Guest session ended (Reason: %s). Returning to STATE_SHELL_INTERACTIVE.",
            KioskShutdownReasonToString(reason));
    m_last_shutdown_reason.store(reason);
    m_completed_sessions.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CleanupSessionArtifactsLocked();
        m_launch_ctx = LaunchContext();
    }
    KioskUI::GetInstance().InvalidateCatalogCache();
    m_state.store(KioskState::STATE_SHELL_INTERACTIVE);
}

bool KioskManager::RelaunchIntoInteractiveShell(
    const char* argv0,
    const std::string& db_path,
    const std::string& conf_path
) {
#if defined(_WIN32)
    (void)argv0;
    wchar_t module_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, module_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    std::wstring cmdline = L"\"";
    cmdline += module_path;
    cmdline += L"\" -kiosk-mode -kiosk-skip-countdown";

    auto append_utf8_arg = [&](const wchar_t* flag, const std::string& val) {
        if (val.empty()) return;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, NULL, 0);
        if (wlen > 1) {
            std::vector<wchar_t> wbuf(static_cast<size_t>(wlen), 0);
            MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, wbuf.data(), wlen);
            cmdline += L" ";
            cmdline += flag;
            cmdline += L" \"";
            cmdline += wbuf.data();
            cmdline += L"\"";
        }
    };

    append_utf8_arg(L"-kiosk-db", db_path);
    append_utf8_arg(L"-kiosk-conf", conf_path);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    if (!CreateProcessW(module_path, mutable_cmd.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    if (!argv0 || !*argv0) return false;
    std::vector<std::string> arg_strings;
    arg_strings.push_back(argv0);
    arg_strings.push_back("-kiosk-mode");
    arg_strings.push_back("-kiosk-skip-countdown");
    if (!db_path.empty()) {
        arg_strings.push_back("-kiosk-db");
        arg_strings.push_back(db_path);
    }
    if (!conf_path.empty()) {
        arg_strings.push_back("-kiosk-conf");
        arg_strings.push_back(conf_path);
    }

    std::vector<char*> argv_ptrs;
    for (size_t i = 0; i < arg_strings.size(); ++i) {
        argv_ptrs.push_back(const_cast<char*>(arg_strings[i].c_str()));
    }
    argv_ptrs.push_back(nullptr);

    execv(argv0, argv_ptrs.data());
    return false;
#endif
}

void KioskManager::RequestApplianceExit() {
    LOG_MSG("KIOSK: Appliance exit requested.");
    m_exit_requested.store(true);
    m_state.store(KioskState::DISABLED);
}

#endif // defined(C_KIOSK)
