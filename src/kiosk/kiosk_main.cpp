#include "kiosk_main.h"
#include "kiosk_db.h"
#include "ui/kiosk_ui.h"
#include "logging.h"

KioskManager& KioskManager::GetInstance() {
    static KioskManager instance;
    return instance;
}

KioskManager::~KioskManager() {
    Shutdown();
}

bool KioskManager::Initialize(const std::string& db_path, const std::string& base_conf) {
    if (m_initialized) return true;

    LOG_MSG("KIOSK: Initializing DOSBox-X Kiosk Subsystems...");

    // 1. Initialize SQLite Catalog Database
    if (!KioskDatabase::GetInstance().Open(db_path)) {
        LOG_MSG("KIOSK: Warning: Failed to open catalog DB '%s', running in-memory fallback", db_path.c_str());
    }

    // 2. Initialize Dear ImGui UI Context
    if (!KioskUI::GetInstance().Initialize(nullptr, nullptr)) {
        LOG_MSG("KIOSK: Warning: ImGui headless initialization failed");
    }

    m_initialized = true;
    m_state = KioskState::STATE_KIOSK_BOOT;

    LOG_MSG("KIOSK: Subsystems initialized successfully (SQLite %s, ImGui %s).",
            KioskDatabase::GetInstance().GetSQLiteVersion().c_str(),
            KioskUI::GetInstance().GetImGuiVersion().c_str());

    return true;
}

void KioskManager::RunMainLoop() {
    LOG_MSG("KIOSK: Entering Kiosk Main Loop (State: %d)", static_cast<int>(m_state));
    // Full state execution will be implemented in Phase 2
}

void KioskManager::Shutdown() {
    if (m_initialized) {
        LOG_MSG("KIOSK: Shutting down Kiosk Subsystems...");
        KioskUI::GetInstance().Shutdown();
        KioskDatabase::GetInstance().Close();
        m_initialized = false;
        m_state = KioskState::DISABLED;
    }
}
