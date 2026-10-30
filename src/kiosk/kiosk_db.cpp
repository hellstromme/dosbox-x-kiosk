#include "config.h"

#if defined(C_KIOSK)

#include "kiosk_db.h"
#include "sqlite3.h"
#include "logging.h"
#include <cstdio>
#include <cstring>

namespace {

// Helper to bind nullable std::string
void BindTextOrNull(sqlite3_stmt* stmt, int index, const std::string& val) {
    if (val.empty()) {
        sqlite3_bind_null(stmt, index);
    } else {
        sqlite3_bind_text(stmt, index, val.c_str(), -1, SQLITE_TRANSIENT);
    }
}

// Helper to read string from column safely
std::string ColumnTextSafe(sqlite3_stmt* stmt, int col) {
    const unsigned char* text = sqlite3_column_text(stmt, col);
    return text ? reinterpret_cast<const char*>(text) : "";
}

} // anonymous namespace

KioskDatabase& KioskDatabase::GetInstance() {
    static KioskDatabase instance;
    return instance;
}

KioskDatabase::~KioskDatabase() {
    Close();
}

bool KioskDatabase::ExecuteSqlInternal(const char* sql) {
    if (!m_db) return false;
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_MSG("KIOSK: SQL error (%d): %s", rc, err_msg ? err_msg : "unknown");
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool KioskDatabase::IsOpen() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_db != nullptr;
}

bool KioskDatabase::Open(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return OpenInternal(db_path);
}

bool KioskDatabase::OpenInternal(const std::string& db_path) {
    if (m_db) {
        CloseInternal();
    }

    std::string path = db_path.empty() ? ":memory:" : db_path;
    int rc = sqlite3_open(path.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        LOG_MSG("KIOSK: Failed to open SQLite database '%s': %s", path.c_str(), sqlite3_errmsg(m_db));
        CloseInternal();
        if (path != ":memory:") {
            LOG_MSG("KIOSK: Falling back to in-memory database (:memory:)");
            return OpenInternal(":memory:");
        }
        return false;
    }

    // Enable foreign keys
    ExecuteSqlInternal("PRAGMA foreign_keys = ON;");

    // Enable WAL mode if opening a real file on disk (WAL is not applicable to :memory:)
    if (path != ":memory:") {
        ExecuteSqlInternal("PRAGMA journal_mode = WAL;");
    }

    LOG_MSG("KIOSK: Opened SQLite database '%s' (SQLite v%s)", path.c_str(), sqlite3_libversion());

    if (!InitializeSchema()) {
        LOG_MSG("KIOSK: Failed to initialize schema in '%s'", path.c_str());
        CloseInternal();
        if (path != ":memory:") {
            LOG_MSG("KIOSK: Falling back to in-memory database (:memory:) after schema failure");
            return OpenInternal(":memory:");
        }
        return false;
    }

    SeedDefaultData();
    return true;
}

void KioskDatabase::Close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    CloseInternal();
}

void KioskDatabase::CloseInternal() {
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

std::string KioskDatabase::GetSQLiteVersion() const {
    return sqlite3_libversion();
}

bool KioskDatabase::InitializeSchema() {
    if (!m_db) return false;

    const char* schema_sql =
        "-- 1. Kiosk Global Configuration\n"
        "CREATE TABLE IF NOT EXISTS kiosk_config (\n"
        "    id INTEGER PRIMARY KEY CHECK (id = 1),\n"
        "    boot_timeout_sec INTEGER NOT NULL DEFAULT 30,\n"
        "    default_boot_action TEXT NOT NULL DEFAULT 'DEFAULT_DOSBOX'\n"
        "        CHECK (default_boot_action IN ('DEFAULT_DOSBOX', 'LAST_PLAYED', 'SPECIFIC_MACHINE', 'SPECIFIC_ITEM')),\n"
        "    default_target_uuid TEXT,\n"
        "    overlay_hotkey TEXT NOT NULL DEFAULT 'F12+D',\n"
        "    default_caddy_view TEXT NOT NULL DEFAULT 'SKEUOMORPHIC'\n"
        "        CHECK (default_caddy_view IN ('SKEUOMORPHIC', 'LIST')),\n"
        "    auto_mount_usb_docs BOOLEAN NOT NULL DEFAULT 1,\n"
        "    theme_name TEXT NOT NULL DEFAULT 'crt_amber'\n"
        ");\n"
        "\n"
        "-- 2. Media Pool\n"
        "CREATE TABLE IF NOT EXISTS media_pool (\n"
        "    id TEXT PRIMARY KEY,\n"
        "    file_path TEXT NOT NULL UNIQUE,\n"
        "    file_sha1 TEXT,\n"
        "    media_type TEXT NOT NULL,\n"
        "    physical_style TEXT NOT NULL DEFAULT 'BEIGE_35',\n"
        "    volume_label TEXT,\n"
        "    label_title TEXT NOT NULL,\n"
        "    label_subtitle TEXT,\n"
        "    label_font_style TEXT NOT NULL DEFAULT 'DOT_MATRIX',\n"
        "    has_redbook_audio BOOLEAN NOT NULL DEFAULT 0,\n"
        "    is_write_protected BOOLEAN NOT NULL DEFAULT 0,\n"
        "    is_removable_usb BOOLEAN NOT NULL DEFAULT 0,\n"
        "    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP\n"
        ");\n"
        "\n"
        "-- 3. Operating System Distributions\n"
        "CREATE TABLE IF NOT EXISTS os_distributions (\n"
        "    id TEXT PRIMARY KEY,\n"
        "    display_name TEXT NOT NULL,\n"
        "    os_family TEXT NOT NULL CHECK (os_family IN ('MSDOS', 'FREEDOS', 'WIN3X', 'WIN9X')),\n"
        "    min_archetype_tier INTEGER NOT NULL,\n"
        "    recommended_hdd_mb INTEGER NOT NULL,\n"
        "    filesystem_type TEXT NOT NULL CHECK (filesystem_type IN ('FAT12', 'FAT16', 'FAT32')),\n"
        "    install_strategy TEXT NOT NULL,\n"
        "    boot_floppy_media_id TEXT REFERENCES media_pool(id),\n"
        "    install_cd_media_id TEXT REFERENCES media_pool(id),\n"
        "    unattended_answer_file TEXT\n"
        ");\n"
        "\n"
        "-- 4. Virtual Machines\n"
        "CREATE TABLE IF NOT EXISTS machines (\n"
        "    id TEXT PRIMARY KEY,\n"
        "    display_name TEXT NOT NULL,\n"
        "    archetype_id TEXT NOT NULL,\n"
        "    archetype_tier INTEGER NOT NULL,\n"
        "    os_id TEXT REFERENCES os_distributions(id),\n"
        "    os_installed BOOLEAN NOT NULL DEFAULT 0,\n"
        "    boot_mode TEXT NOT NULL DEFAULT 'INTEGRATED_DOS'\n"
        "        CHECK (boot_mode IN ('INTEGRATED_DOS', 'REAL_HDD_BOOT')),\n"
        "    primary_hdd_path TEXT,\n"
        "    primary_hdd_chs TEXT,\n"
        "    custom_conf_overrides TEXT,\n"
        "    last_booted_at DATETIME,\n"
        "    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP\n"
        ");\n"
        "\n"
        "-- 5. Carousel Catalog Items\n"
        "CREATE TABLE IF NOT EXISTS catalog_items (\n"
        "    id TEXT PRIMARY KEY,\n"
        "    machine_id TEXT NOT NULL REFERENCES machines(id) ON DELETE CASCADE,\n"
        "    item_type TEXT NOT NULL CHECK (item_type IN ('GAME', 'APPLICATION')),\n"
        "    title TEXT NOT NULL,\n"
        "    developer TEXT,\n"
        "    release_year INTEGER,\n"
        "    source_origin TEXT NOT NULL DEFAULT 'SCANNED'\n"
        "        CHECK (source_origin IN ('SCANNED', 'GOG_IMPORT', 'STEAM_IMPORT', 'MANUAL')),\n"
        "    launch_mechanism TEXT NOT NULL DEFAULT 'INTEGRATED_AUTOEXEC',\n"
        "    host_container_dir TEXT,\n"
        "    guest_working_dir TEXT NOT NULL,\n"
        "    guest_executable TEXT NOT NULL,\n"
        "    guest_arguments TEXT,\n"
        "    return_on_exit BOOLEAN NOT NULL DEFAULT 1,\n"
        "    icon_cache_path TEXT,\n"
        "    screenshot_cache_path TEXT,\n"
        "    executable_crc32 TEXT,\n"
        "    play_count INTEGER NOT NULL DEFAULT 0,\n"
        "    last_launched_at DATETIME,\n"
        "    is_favorite BOOLEAN NOT NULL DEFAULT 0\n"
        ");\n"
        "\n"
        "-- 6. Multi-Disk Session Caddy Bindings\n"
        "CREATE TABLE IF NOT EXISTS caddy_bindings (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    machine_id TEXT REFERENCES machines(id) ON DELETE CASCADE,\n"
        "    catalog_item_id TEXT REFERENCES catalog_items(id) ON DELETE CASCADE,\n"
        "    media_id TEXT NOT NULL REFERENCES media_pool(id) ON DELETE CASCADE,\n"
        "    target_drive TEXT NOT NULL CHECK (target_drive IN ('A', 'B', 'D', 'E')),\n"
        "    slot_order INTEGER NOT NULL DEFAULT 1,\n"
        "    CHECK (\n"
        "        (machine_id IS NOT NULL AND catalog_item_id IS NULL) OR\n"
        "        (machine_id IS NULL AND catalog_item_id IS NOT NULL)\n"
        "    )\n"
        ");\n"
        "\n"
        "-- 7. Software Signatures\n"
        "CREATE TABLE IF NOT EXISTS software_signatures (\n"
        "    exec_filename TEXT NOT NULL,\n"
        "    file_size_bytes INTEGER,\n"
        "    header_crc32 TEXT,\n"
        "    detected_title TEXT NOT NULL,\n"
        "    item_type TEXT NOT NULL CHECK (item_type IN ('GAME', 'APPLICATION', 'IGNORE_SYSTEM')),\n"
        "    recommended_archetype TEXT,\n"
        "    PRIMARY KEY (exec_filename, header_crc32)\n"
        ");\n"
        "\n"
        "-- Performance Indexes\n"
        "CREATE INDEX IF NOT EXISTS idx_catalog_type_fav ON catalog_items(item_type, is_favorite DESC, last_launched_at DESC);\n"
        "CREATE INDEX IF NOT EXISTS idx_catalog_machine ON catalog_items(machine_id);\n"
        "CREATE INDEX IF NOT EXISTS idx_caddy_item ON caddy_bindings(catalog_item_id, target_drive, slot_order);\n"
        "CREATE INDEX IF NOT EXISTS idx_caddy_machine ON caddy_bindings(machine_id, target_drive, slot_order);\n"
        "CREATE INDEX IF NOT EXISTS idx_media_usb ON media_pool(is_removable_usb);\n";

    return ExecuteSqlInternal(schema_sql);
}

bool KioskDatabase::SeedDefaultData() {
    if (!m_db) return false;

    // 1. Seed kiosk_config if not present
    sqlite3_stmt* check_stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM kiosk_config WHERE id = 1;", -1, &check_stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(check_stmt) == SQLITE_ROW && sqlite3_column_int(check_stmt, 0) == 0) {
            ExecuteSqlInternal(
                "INSERT OR IGNORE INTO kiosk_config (id, boot_timeout_sec, default_boot_action, overlay_hotkey, default_caddy_view, auto_mount_usb_docs, theme_name) "
                "VALUES (1, 30, 'DEFAULT_DOSBOX', 'F12+D', 'SKEUOMORPHIC', 1, 'crt_amber');"
            );
        }
        sqlite3_finalize(check_stmt);
    }

    // 2. Seed default os_distributions if empty
    check_stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM os_distributions;", -1, &check_stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(check_stmt) == SQLITE_ROW && sqlite3_column_int(check_stmt, 0) == 0) {
            const char* seed_os_sql =
                "INSERT INTO os_distributions (id, display_name, os_family, min_archetype_tier, recommended_hdd_mb, filesystem_type, install_strategy) VALUES\n"
                "('PC_DOS_110', 'IBM Personal Computer DOS 1.10', 'MSDOS', 1, 10, 'FAT12', 'FLOPPY_SEQUENCE'),\n"
                "('MSDOS_330',  'Microsoft MS-DOS 3.30',           'MSDOS', 1, 20, 'FAT12', 'FLOPPY_SEQUENCE'),\n"
                "('MSDOS_500',  'Microsoft MS-DOS 5.00',           'MSDOS', 2, 40, 'FAT16', 'FLOPPY_SEQUENCE'),\n"
                "('MSDOS_622',  'Microsoft MS-DOS 6.22',           'MSDOS', 3, 250, 'FAT16', 'FLOPPY_SEQUENCE'),\n"
                "('FREEDOS_13',  'FreeDOS 1.3',                     'FREEDOS', 3, 500, 'FAT16', 'BOOTABLE_CD'),\n"
                "('WIN311_FWG', 'Windows for Workgroups 3.11',     'WIN3X', 3, 250, 'FAT16', 'FLOPPY_SEQUENCE'),\n"
                "('WIN95_OSR2', 'Microsoft Windows 95 OSR 2.5',    'WIN9X', 4, 1024, 'FAT32', 'WIN9X_CAB_PRECOPY'),\n"
                "('WIN98_SE',   'Microsoft Windows 98 Second Edition', 'WIN9X', 5, 2048, 'FAT32', 'WIN9X_CAB_PRECOPY');\n";
            ExecuteSqlInternal(seed_os_sql);
        }
        sqlite3_finalize(check_stmt);
    }

    return true;
}

// ----------------------------------------------------------------------------
// 1. Kiosk Configuration
// ----------------------------------------------------------------------------

bool KioskDatabase::GetConfig(KioskConfigRecord& out_config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return false;

    const char* sql = "SELECT id, boot_timeout_sec, default_boot_action, default_target_uuid, "
                      "overlay_hotkey, default_caddy_view, auto_mount_usb_docs, theme_name "
                      "FROM kiosk_config WHERE id = 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_config.id = sqlite3_column_int(stmt, 0);
        out_config.boot_timeout_sec = sqlite3_column_int(stmt, 1);
        out_config.default_boot_action = ColumnTextSafe(stmt, 2);
        out_config.default_target_uuid = ColumnTextSafe(stmt, 3);
        out_config.overlay_hotkey = ColumnTextSafe(stmt, 4);
        out_config.default_caddy_view = ColumnTextSafe(stmt, 5);
        out_config.auto_mount_usb_docs = sqlite3_column_int(stmt, 6) != 0;
        out_config.theme_name = ColumnTextSafe(stmt, 7);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool KioskDatabase::UpdateConfig(const KioskConfigRecord& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return false;

    const char* sql = "INSERT OR REPLACE INTO kiosk_config (id, boot_timeout_sec, default_boot_action, "
                      "default_target_uuid, overlay_hotkey, default_caddy_view, auto_mount_usb_docs, theme_name) "
                      "VALUES (1, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, config.boot_timeout_sec);
    sqlite3_bind_text(stmt, 2, config.default_boot_action.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 3, config.default_target_uuid);
    sqlite3_bind_text(stmt, 4, config.overlay_hotkey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, config.default_caddy_view.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, config.auto_mount_usb_docs ? 1 : 0);
    sqlite3_bind_text(stmt, 7, config.theme_name.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ----------------------------------------------------------------------------
// 2. Virtual Machines
// ----------------------------------------------------------------------------

std::vector<MachineRecord> KioskDatabase::GetAllMachines() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<MachineRecord> list;
    if (!m_db) return list;

    const char* sql = "SELECT id, display_name, archetype_id, archetype_tier, os_id, "
                      "os_installed, boot_mode, primary_hdd_path, primary_hdd_chs, "
                      "custom_conf_overrides, last_booted_at, created_at FROM machines ORDER BY display_name ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MachineRecord m;
        m.id = ColumnTextSafe(stmt, 0);
        m.display_name = ColumnTextSafe(stmt, 1);
        m.archetype_id = ColumnTextSafe(stmt, 2);
        m.archetype_tier = sqlite3_column_int(stmt, 3);
        m.os_id = ColumnTextSafe(stmt, 4);
        m.os_installed = sqlite3_column_int(stmt, 5) != 0;
        m.boot_mode = ColumnTextSafe(stmt, 6);
        m.primary_hdd_path = ColumnTextSafe(stmt, 7);
        m.primary_hdd_chs = ColumnTextSafe(stmt, 8);
        m.custom_conf_overrides = ColumnTextSafe(stmt, 9);
        m.last_booted_at = ColumnTextSafe(stmt, 10);
        m.created_at = ColumnTextSafe(stmt, 11);
        list.push_back(m);
    }
    sqlite3_finalize(stmt);
    return list;
}

bool KioskDatabase::GetMachine(const std::string& id, MachineRecord& out_machine) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || id.empty()) return false;

    const char* sql = "SELECT id, display_name, archetype_id, archetype_tier, os_id, "
                      "os_installed, boot_mode, primary_hdd_path, primary_hdd_chs, "
                      "custom_conf_overrides, last_booted_at, created_at FROM machines WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_machine.id = ColumnTextSafe(stmt, 0);
        out_machine.display_name = ColumnTextSafe(stmt, 1);
        out_machine.archetype_id = ColumnTextSafe(stmt, 2);
        out_machine.archetype_tier = sqlite3_column_int(stmt, 3);
        out_machine.os_id = ColumnTextSafe(stmt, 4);
        out_machine.os_installed = sqlite3_column_int(stmt, 5) != 0;
        out_machine.boot_mode = ColumnTextSafe(stmt, 6);
        out_machine.primary_hdd_path = ColumnTextSafe(stmt, 7);
        out_machine.primary_hdd_chs = ColumnTextSafe(stmt, 8);
        out_machine.custom_conf_overrides = ColumnTextSafe(stmt, 9);
        out_machine.last_booted_at = ColumnTextSafe(stmt, 10);
        out_machine.created_at = ColumnTextSafe(stmt, 11);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool KioskDatabase::InsertMachine(const MachineRecord& machine) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || machine.id.empty()) return false;

    const char* sql = "INSERT INTO machines (id, display_name, archetype_id, archetype_tier, os_id, "
                      "os_installed, boot_mode, primary_hdd_path, primary_hdd_chs, custom_conf_overrides) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, machine.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, machine.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, machine.archetype_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, machine.archetype_tier);
    BindTextOrNull(stmt, 5, machine.os_id);
    sqlite3_bind_int(stmt, 6, machine.os_installed ? 1 : 0);
    sqlite3_bind_text(stmt, 7, machine.boot_mode.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 8, machine.primary_hdd_path);
    BindTextOrNull(stmt, 9, machine.primary_hdd_chs);
    BindTextOrNull(stmt, 10, machine.custom_conf_overrides);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool KioskDatabase::UpdateMachine(const MachineRecord& machine) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || machine.id.empty()) return false;

    const char* sql = "UPDATE machines SET display_name = ?, archetype_id = ?, archetype_tier = ?, "
                      "os_id = ?, os_installed = ?, boot_mode = ?, primary_hdd_path = ?, primary_hdd_chs = ?, "
                      "custom_conf_overrides = ?, last_booted_at = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, machine.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, machine.archetype_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, machine.archetype_tier);
    BindTextOrNull(stmt, 4, machine.os_id);
    sqlite3_bind_int(stmt, 5, machine.os_installed ? 1 : 0);
    sqlite3_bind_text(stmt, 6, machine.boot_mode.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 7, machine.primary_hdd_path);
    BindTextOrNull(stmt, 8, machine.primary_hdd_chs);
    BindTextOrNull(stmt, 9, machine.custom_conf_overrides);
    BindTextOrNull(stmt, 10, machine.last_booted_at);
    sqlite3_bind_text(stmt, 11, machine.id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool KioskDatabase::DeleteMachine(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || id.empty()) return false;

    const char* sql = "DELETE FROM machines WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ----------------------------------------------------------------------------
// 3. Carousel Catalog Items (Games & Applications)
// ----------------------------------------------------------------------------

std::vector<CatalogItemRecord> KioskDatabase::GetAllCatalogItems() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CatalogItemRecord> list;
    if (!m_db) return list;

    const char* sql = "SELECT id, machine_id, item_type, title, developer, release_year, "
                      "source_origin, launch_mechanism, host_container_dir, guest_working_dir, "
                      "guest_executable, guest_arguments, return_on_exit, icon_cache_path, "
                      "screenshot_cache_path, executable_crc32, play_count, last_launched_at, "
                      "is_favorite FROM catalog_items ORDER BY is_favorite DESC, title ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CatalogItemRecord it;
        it.id = ColumnTextSafe(stmt, 0);
        it.machine_id = ColumnTextSafe(stmt, 1);
        it.item_type = ColumnTextSafe(stmt, 2);
        it.title = ColumnTextSafe(stmt, 3);
        it.developer = ColumnTextSafe(stmt, 4);
        it.release_year = sqlite3_column_int(stmt, 5);
        it.source_origin = ColumnTextSafe(stmt, 6);
        it.launch_mechanism = ColumnTextSafe(stmt, 7);
        it.host_container_dir = ColumnTextSafe(stmt, 8);
        it.guest_working_dir = ColumnTextSafe(stmt, 9);
        it.guest_executable = ColumnTextSafe(stmt, 10);
        it.guest_arguments = ColumnTextSafe(stmt, 11);
        it.return_on_exit = sqlite3_column_int(stmt, 12) != 0;
        it.icon_cache_path = ColumnTextSafe(stmt, 13);
        it.screenshot_cache_path = ColumnTextSafe(stmt, 14);
        it.executable_crc32 = ColumnTextSafe(stmt, 15);
        it.play_count = sqlite3_column_int(stmt, 16);
        it.last_launched_at = ColumnTextSafe(stmt, 17);
        it.is_favorite = sqlite3_column_int(stmt, 18) != 0;
        list.push_back(it);
    }
    sqlite3_finalize(stmt);
    return list;
}

std::vector<CatalogItemRecord> KioskDatabase::GetItemsByType(const std::string& item_type) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CatalogItemRecord> list;
    if (!m_db) return list;

    const char* sql = "SELECT id, machine_id, item_type, title, developer, release_year, "
                      "source_origin, launch_mechanism, host_container_dir, guest_working_dir, "
                      "guest_executable, guest_arguments, return_on_exit, icon_cache_path, "
                      "screenshot_cache_path, executable_crc32, play_count, last_launched_at, "
                      "is_favorite FROM catalog_items WHERE item_type = ? "
                      "ORDER BY is_favorite DESC, last_launched_at DESC, title ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    sqlite3_bind_text(stmt, 1, item_type.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CatalogItemRecord it;
        it.id = ColumnTextSafe(stmt, 0);
        it.machine_id = ColumnTextSafe(stmt, 1);
        it.item_type = ColumnTextSafe(stmt, 2);
        it.title = ColumnTextSafe(stmt, 3);
        it.developer = ColumnTextSafe(stmt, 4);
        it.release_year = sqlite3_column_int(stmt, 5);
        it.source_origin = ColumnTextSafe(stmt, 6);
        it.launch_mechanism = ColumnTextSafe(stmt, 7);
        it.host_container_dir = ColumnTextSafe(stmt, 8);
        it.guest_working_dir = ColumnTextSafe(stmt, 9);
        it.guest_executable = ColumnTextSafe(stmt, 10);
        it.guest_arguments = ColumnTextSafe(stmt, 11);
        it.return_on_exit = sqlite3_column_int(stmt, 12) != 0;
        it.icon_cache_path = ColumnTextSafe(stmt, 13);
        it.screenshot_cache_path = ColumnTextSafe(stmt, 14);
        it.executable_crc32 = ColumnTextSafe(stmt, 15);
        it.play_count = sqlite3_column_int(stmt, 16);
        it.last_launched_at = ColumnTextSafe(stmt, 17);
        it.is_favorite = sqlite3_column_int(stmt, 18) != 0;
        list.push_back(it);
    }
    sqlite3_finalize(stmt);
    return list;
}

bool KioskDatabase::GetItem(const std::string& id, CatalogItemRecord& out_item) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || id.empty()) return false;

    const char* sql = "SELECT id, machine_id, item_type, title, developer, release_year, "
                      "source_origin, launch_mechanism, host_container_dir, guest_working_dir, "
                      "guest_executable, guest_arguments, return_on_exit, icon_cache_path, "
                      "screenshot_cache_path, executable_crc32, play_count, last_launched_at, "
                      "is_favorite FROM catalog_items WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_item.id = ColumnTextSafe(stmt, 0);
        out_item.machine_id = ColumnTextSafe(stmt, 1);
        out_item.item_type = ColumnTextSafe(stmt, 2);
        out_item.title = ColumnTextSafe(stmt, 3);
        out_item.developer = ColumnTextSafe(stmt, 4);
        out_item.release_year = sqlite3_column_int(stmt, 5);
        out_item.source_origin = ColumnTextSafe(stmt, 6);
        out_item.launch_mechanism = ColumnTextSafe(stmt, 7);
        out_item.host_container_dir = ColumnTextSafe(stmt, 8);
        out_item.guest_working_dir = ColumnTextSafe(stmt, 9);
        out_item.guest_executable = ColumnTextSafe(stmt, 10);
        out_item.guest_arguments = ColumnTextSafe(stmt, 11);
        out_item.return_on_exit = sqlite3_column_int(stmt, 12) != 0;
        out_item.icon_cache_path = ColumnTextSafe(stmt, 13);
        out_item.screenshot_cache_path = ColumnTextSafe(stmt, 14);
        out_item.executable_crc32 = ColumnTextSafe(stmt, 15);
        out_item.play_count = sqlite3_column_int(stmt, 16);
        out_item.last_launched_at = ColumnTextSafe(stmt, 17);
        out_item.is_favorite = sqlite3_column_int(stmt, 18) != 0;
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

std::vector<CatalogItemRecord> KioskDatabase::GetItemsForMachine(const std::string& machine_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CatalogItemRecord> list;
    if (!m_db || machine_id.empty()) return list;

    const char* sql = "SELECT id, machine_id, item_type, title, developer, release_year, "
                      "source_origin, launch_mechanism, host_container_dir, guest_working_dir, "
                      "guest_executable, guest_arguments, return_on_exit, icon_cache_path, "
                      "screenshot_cache_path, executable_crc32, play_count, last_launched_at, "
                      "is_favorite FROM catalog_items WHERE machine_id = ? ORDER BY title ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    sqlite3_bind_text(stmt, 1, machine_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CatalogItemRecord it;
        it.id = ColumnTextSafe(stmt, 0);
        it.machine_id = ColumnTextSafe(stmt, 1);
        it.item_type = ColumnTextSafe(stmt, 2);
        it.title = ColumnTextSafe(stmt, 3);
        it.developer = ColumnTextSafe(stmt, 4);
        it.release_year = sqlite3_column_int(stmt, 5);
        it.source_origin = ColumnTextSafe(stmt, 6);
        it.launch_mechanism = ColumnTextSafe(stmt, 7);
        it.host_container_dir = ColumnTextSafe(stmt, 8);
        it.guest_working_dir = ColumnTextSafe(stmt, 9);
        it.guest_executable = ColumnTextSafe(stmt, 10);
        it.guest_arguments = ColumnTextSafe(stmt, 11);
        it.return_on_exit = sqlite3_column_int(stmt, 12) != 0;
        it.icon_cache_path = ColumnTextSafe(stmt, 13);
        it.screenshot_cache_path = ColumnTextSafe(stmt, 14);
        it.executable_crc32 = ColumnTextSafe(stmt, 15);
        it.play_count = sqlite3_column_int(stmt, 16);
        it.last_launched_at = ColumnTextSafe(stmt, 17);
        it.is_favorite = sqlite3_column_int(stmt, 18) != 0;
        list.push_back(it);
    }
    sqlite3_finalize(stmt);
    return list;
}

bool KioskDatabase::InsertCatalogItem(const CatalogItemRecord& item) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || item.id.empty() || item.machine_id.empty()) return false;

    const char* sql = "INSERT INTO catalog_items (id, machine_id, item_type, title, developer, release_year, "
                      "source_origin, launch_mechanism, host_container_dir, guest_working_dir, guest_executable, "
                      "guest_arguments, return_on_exit, icon_cache_path, screenshot_cache_path, executable_crc32, "
                      "play_count, is_favorite) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, item.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, item.machine_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, item.item_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, item.title.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 5, item.developer);
    sqlite3_bind_int(stmt, 6, item.release_year);
    sqlite3_bind_text(stmt, 7, item.source_origin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, item.launch_mechanism.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 9, item.host_container_dir);
    sqlite3_bind_text(stmt, 10, item.guest_working_dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, item.guest_executable.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 12, item.guest_arguments);
    sqlite3_bind_int(stmt, 13, item.return_on_exit ? 1 : 0);
    BindTextOrNull(stmt, 14, item.icon_cache_path);
    BindTextOrNull(stmt, 15, item.screenshot_cache_path);
    BindTextOrNull(stmt, 16, item.executable_crc32);
    sqlite3_bind_int(stmt, 17, item.play_count);
    sqlite3_bind_int(stmt, 18, item.is_favorite ? 1 : 0);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool KioskDatabase::UpdateCatalogItem(const CatalogItemRecord& item) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || item.id.empty()) return false;

    const char* sql = "UPDATE catalog_items SET machine_id = ?, item_type = ?, title = ?, developer = ?, "
                      "release_year = ?, source_origin = ?, launch_mechanism = ?, host_container_dir = ?, "
                      "guest_working_dir = ?, guest_executable = ?, guest_arguments = ?, return_on_exit = ?, "
                      "icon_cache_path = ?, screenshot_cache_path = ?, executable_crc32 = ?, play_count = ?, "
                      "last_launched_at = ?, is_favorite = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, item.machine_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, item.item_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, item.title.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 4, item.developer);
    sqlite3_bind_int(stmt, 5, item.release_year);
    sqlite3_bind_text(stmt, 6, item.source_origin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, item.launch_mechanism.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 8, item.host_container_dir);
    sqlite3_bind_text(stmt, 9, item.guest_working_dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, item.guest_executable.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 11, item.guest_arguments);
    sqlite3_bind_int(stmt, 12, item.return_on_exit ? 1 : 0);
    BindTextOrNull(stmt, 13, item.icon_cache_path);
    BindTextOrNull(stmt, 14, item.screenshot_cache_path);
    BindTextOrNull(stmt, 15, item.executable_crc32);
    sqlite3_bind_int(stmt, 16, item.play_count);
    BindTextOrNull(stmt, 17, item.last_launched_at);
    sqlite3_bind_int(stmt, 18, item.is_favorite ? 1 : 0);
    sqlite3_bind_text(stmt, 19, item.id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool KioskDatabase::DeleteCatalogItem(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || id.empty()) return false;

    const char* sql = "DELETE FROM catalog_items WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool KioskDatabase::IncrementPlayCount(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || id.empty()) return false;

    const char* sql = "UPDATE catalog_items SET play_count = play_count + 1, last_launched_at = CURRENT_TIMESTAMP WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ----------------------------------------------------------------------------
// 4. Global Media Pool
// ----------------------------------------------------------------------------

std::vector<MediaPoolRecord> KioskDatabase::GetAllMedia() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<MediaPoolRecord> list;
    if (!m_db) return list;

    const char* sql = "SELECT id, file_path, file_sha1, media_type, physical_style, "
                      "volume_label, label_title, label_subtitle, label_font_style, "
                      "has_redbook_audio, is_write_protected, is_removable_usb, created_at "
                      "FROM media_pool ORDER BY label_title ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MediaPoolRecord m;
        m.id = ColumnTextSafe(stmt, 0);
        m.file_path = ColumnTextSafe(stmt, 1);
        m.file_sha1 = ColumnTextSafe(stmt, 2);
        m.media_type = ColumnTextSafe(stmt, 3);
        m.physical_style = ColumnTextSafe(stmt, 4);
        m.volume_label = ColumnTextSafe(stmt, 5);
        m.label_title = ColumnTextSafe(stmt, 6);
        m.label_subtitle = ColumnTextSafe(stmt, 7);
        m.label_font_style = ColumnTextSafe(stmt, 8);
        m.has_redbook_audio = sqlite3_column_int(stmt, 9) != 0;
        m.is_write_protected = sqlite3_column_int(stmt, 10) != 0;
        m.is_removable_usb = sqlite3_column_int(stmt, 11) != 0;
        m.created_at = ColumnTextSafe(stmt, 12);
        list.push_back(m);
    }
    sqlite3_finalize(stmt);
    return list;
}

bool KioskDatabase::GetMedia(const std::string& id, MediaPoolRecord& out_media) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || id.empty()) return false;

    const char* sql = "SELECT id, file_path, file_sha1, media_type, physical_style, "
                      "volume_label, label_title, label_subtitle, label_font_style, "
                      "has_redbook_audio, is_write_protected, is_removable_usb, created_at "
                      "FROM media_pool WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_media.id = ColumnTextSafe(stmt, 0);
        out_media.file_path = ColumnTextSafe(stmt, 1);
        out_media.file_sha1 = ColumnTextSafe(stmt, 2);
        out_media.media_type = ColumnTextSafe(stmt, 3);
        out_media.physical_style = ColumnTextSafe(stmt, 4);
        out_media.volume_label = ColumnTextSafe(stmt, 5);
        out_media.label_title = ColumnTextSafe(stmt, 6);
        out_media.label_subtitle = ColumnTextSafe(stmt, 7);
        out_media.label_font_style = ColumnTextSafe(stmt, 8);
        out_media.has_redbook_audio = sqlite3_column_int(stmt, 9) != 0;
        out_media.is_write_protected = sqlite3_column_int(stmt, 10) != 0;
        out_media.is_removable_usb = sqlite3_column_int(stmt, 11) != 0;
        out_media.created_at = ColumnTextSafe(stmt, 12);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool KioskDatabase::GetMediaByPath(const std::string& file_path, MediaPoolRecord& out_media) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || file_path.empty()) return false;

    const char* sql = "SELECT id, file_path, file_sha1, media_type, physical_style, "
                      "volume_label, label_title, label_subtitle, label_font_style, "
                      "has_redbook_audio, is_write_protected, is_removable_usb, created_at "
                      "FROM media_pool WHERE file_path = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_media.id = ColumnTextSafe(stmt, 0);
        out_media.file_path = ColumnTextSafe(stmt, 1);
        out_media.file_sha1 = ColumnTextSafe(stmt, 2);
        out_media.media_type = ColumnTextSafe(stmt, 3);
        out_media.physical_style = ColumnTextSafe(stmt, 4);
        out_media.volume_label = ColumnTextSafe(stmt, 5);
        out_media.label_title = ColumnTextSafe(stmt, 6);
        out_media.label_subtitle = ColumnTextSafe(stmt, 7);
        out_media.label_font_style = ColumnTextSafe(stmt, 8);
        out_media.has_redbook_audio = sqlite3_column_int(stmt, 9) != 0;
        out_media.is_write_protected = sqlite3_column_int(stmt, 10) != 0;
        out_media.is_removable_usb = sqlite3_column_int(stmt, 11) != 0;
        out_media.created_at = ColumnTextSafe(stmt, 12);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool KioskDatabase::InsertMedia(const MediaPoolRecord& media) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || media.id.empty() || media.file_path.empty()) return false;

    const char* sql = "INSERT INTO media_pool (id, file_path, file_sha1, media_type, physical_style, "
                      "volume_label, label_title, label_subtitle, label_font_style, has_redbook_audio, "
                      "is_write_protected, is_removable_usb) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, media.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, media.file_path.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 3, media.file_sha1);
    sqlite3_bind_text(stmt, 4, media.media_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, media.physical_style.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 6, media.volume_label);
    sqlite3_bind_text(stmt, 7, media.label_title.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 8, media.label_subtitle);
    sqlite3_bind_text(stmt, 9, media.label_font_style.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 10, media.has_redbook_audio ? 1 : 0);
    sqlite3_bind_int(stmt, 11, media.is_write_protected ? 1 : 0);
    sqlite3_bind_int(stmt, 12, media.is_removable_usb ? 1 : 0);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool KioskDatabase::UpdateMedia(const MediaPoolRecord& media) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || media.id.empty()) return false;

    const char* sql = "UPDATE media_pool SET file_path = ?, file_sha1 = ?, media_type = ?, "
                      "physical_style = ?, volume_label = ?, label_title = ?, label_subtitle = ?, "
                      "label_font_style = ?, has_redbook_audio = ?, is_write_protected = ?, "
                      "is_removable_usb = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, media.file_path.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 2, media.file_sha1);
    sqlite3_bind_text(stmt, 3, media.media_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, media.physical_style.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 5, media.volume_label);
    sqlite3_bind_text(stmt, 6, media.label_title.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 7, media.label_subtitle);
    sqlite3_bind_text(stmt, 8, media.label_font_style.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, media.has_redbook_audio ? 1 : 0);
    sqlite3_bind_int(stmt, 10, media.is_write_protected ? 1 : 0);
    sqlite3_bind_int(stmt, 11, media.is_removable_usb ? 1 : 0);
    sqlite3_bind_text(stmt, 12, media.id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool KioskDatabase::DeleteMedia(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || id.empty()) return false;

    const char* sql = "DELETE FROM media_pool WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<MediaPoolRecord> KioskDatabase::GetRemovableUsbMedia() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<MediaPoolRecord> list;
    if (!m_db) return list;

    const char* sql = "SELECT id, file_path, file_sha1, media_type, physical_style, "
                      "volume_label, label_title, label_subtitle, label_font_style, "
                      "has_redbook_audio, is_write_protected, is_removable_usb, created_at "
                      "FROM media_pool WHERE is_removable_usb = 1 ORDER BY label_title ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MediaPoolRecord m;
        m.id = ColumnTextSafe(stmt, 0);
        m.file_path = ColumnTextSafe(stmt, 1);
        m.file_sha1 = ColumnTextSafe(stmt, 2);
        m.media_type = ColumnTextSafe(stmt, 3);
        m.physical_style = ColumnTextSafe(stmt, 4);
        m.volume_label = ColumnTextSafe(stmt, 5);
        m.label_title = ColumnTextSafe(stmt, 6);
        m.label_subtitle = ColumnTextSafe(stmt, 7);
        m.label_font_style = ColumnTextSafe(stmt, 8);
        m.has_redbook_audio = sqlite3_column_int(stmt, 9) != 0;
        m.is_write_protected = sqlite3_column_int(stmt, 10) != 0;
        m.is_removable_usb = sqlite3_column_int(stmt, 11) != 0;
        m.created_at = ColumnTextSafe(stmt, 12);
        list.push_back(m);
    }
    sqlite3_finalize(stmt);
    return list;
}

// ----------------------------------------------------------------------------
// 5. Session Caddy Bindings
// ----------------------------------------------------------------------------

std::vector<CaddyBindingRecord> KioskDatabase::GetCaddyBindingsForMachine(const std::string& machine_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CaddyBindingRecord> list;
    if (!m_db || machine_id.empty()) return list;

    const char* sql = "SELECT c.id, c.machine_id, c.catalog_item_id, c.media_id, c.target_drive, c.slot_order, "
                      "m.file_path, m.media_type, m.label_title FROM caddy_bindings c "
                      "JOIN media_pool m ON c.media_id = m.id WHERE c.machine_id = ? "
                      "ORDER BY c.target_drive ASC, c.slot_order ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    sqlite3_bind_text(stmt, 1, machine_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CaddyBindingRecord b;
        b.id = sqlite3_column_int(stmt, 0);
        b.machine_id = ColumnTextSafe(stmt, 1);
        b.catalog_item_id = ColumnTextSafe(stmt, 2);
        b.media_id = ColumnTextSafe(stmt, 3);
        b.target_drive = ColumnTextSafe(stmt, 4);
        b.slot_order = sqlite3_column_int(stmt, 5);
        b.resolved_file_path = ColumnTextSafe(stmt, 6);
        b.resolved_media_type = ColumnTextSafe(stmt, 7);
        b.resolved_label = ColumnTextSafe(stmt, 8);
        list.push_back(b);
    }
    sqlite3_finalize(stmt);
    return list;
}

std::vector<CaddyBindingRecord> KioskDatabase::GetCaddyBindingsForItem(const std::string& item_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CaddyBindingRecord> list;
    if (!m_db || item_id.empty()) return list;

    const char* sql = "SELECT c.id, c.machine_id, c.catalog_item_id, c.media_id, c.target_drive, c.slot_order, "
                      "m.file_path, m.media_type, m.label_title FROM caddy_bindings c "
                      "JOIN media_pool m ON c.media_id = m.id WHERE c.catalog_item_id = ? "
                      "ORDER BY c.target_drive ASC, c.slot_order ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    sqlite3_bind_text(stmt, 1, item_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CaddyBindingRecord b;
        b.id = sqlite3_column_int(stmt, 0);
        b.machine_id = ColumnTextSafe(stmt, 1);
        b.catalog_item_id = ColumnTextSafe(stmt, 2);
        b.media_id = ColumnTextSafe(stmt, 3);
        b.target_drive = ColumnTextSafe(stmt, 4);
        b.slot_order = sqlite3_column_int(stmt, 5);
        b.resolved_file_path = ColumnTextSafe(stmt, 6);
        b.resolved_media_type = ColumnTextSafe(stmt, 7);
        b.resolved_label = ColumnTextSafe(stmt, 8);
        list.push_back(b);
    }
    sqlite3_finalize(stmt);
    return list;
}

bool KioskDatabase::AddCaddyBinding(const CaddyBindingRecord& binding) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || binding.media_id.empty()) return false;
    if (binding.machine_id.empty() && binding.catalog_item_id.empty()) return false;

    const char* sql = "INSERT INTO caddy_bindings (machine_id, catalog_item_id, media_id, target_drive, slot_order) "
                      "VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    BindTextOrNull(stmt, 1, binding.machine_id);
    BindTextOrNull(stmt, 2, binding.catalog_item_id);
    sqlite3_bind_text(stmt, 3, binding.media_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, binding.target_drive.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, binding.slot_order);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool KioskDatabase::DeleteCaddyBinding(int id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || id <= 0) return false;

    const char* sql = "DELETE FROM caddy_bindings WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool KioskDatabase::ClearCaddyBindingsForMachine(const std::string& machine_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || machine_id.empty()) return false;

    const char* sql = "DELETE FROM caddy_bindings WHERE machine_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, machine_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool KioskDatabase::ClearCaddyBindingsForItem(const std::string& item_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || item_id.empty()) return false;

    const char* sql = "DELETE FROM caddy_bindings WHERE catalog_item_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, item_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ----------------------------------------------------------------------------
// 6. Pre-Staged Operating System Distributions
// ----------------------------------------------------------------------------

std::vector<OSDistributionRecord> KioskDatabase::GetAllOSDistributions() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<OSDistributionRecord> list;
    if (!m_db) return list;

    const char* sql = "SELECT id, display_name, os_family, min_archetype_tier, recommended_hdd_mb, "
                      "filesystem_type, install_strategy, boot_floppy_media_id, install_cd_media_id, "
                      "unattended_answer_file FROM os_distributions ORDER BY min_archetype_tier ASC, display_name ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OSDistributionRecord os;
        os.id = ColumnTextSafe(stmt, 0);
        os.display_name = ColumnTextSafe(stmt, 1);
        os.os_family = ColumnTextSafe(stmt, 2);
        os.min_archetype_tier = sqlite3_column_int(stmt, 3);
        os.recommended_hdd_mb = sqlite3_column_int(stmt, 4);
        os.filesystem_type = ColumnTextSafe(stmt, 5);
        os.install_strategy = ColumnTextSafe(stmt, 6);
        os.boot_floppy_media_id = ColumnTextSafe(stmt, 7);
        os.install_cd_media_id = ColumnTextSafe(stmt, 8);
        os.unattended_answer_file = ColumnTextSafe(stmt, 9);
        list.push_back(os);
    }
    sqlite3_finalize(stmt);
    return list;
}

bool KioskDatabase::GetOSDistribution(const std::string& id, OSDistributionRecord& out_os) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || id.empty()) return false;

    const char* sql = "SELECT id, display_name, os_family, min_archetype_tier, recommended_hdd_mb, "
                      "filesystem_type, install_strategy, boot_floppy_media_id, install_cd_media_id, "
                      "unattended_answer_file FROM os_distributions WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_os.id = ColumnTextSafe(stmt, 0);
        out_os.display_name = ColumnTextSafe(stmt, 1);
        out_os.os_family = ColumnTextSafe(stmt, 2);
        out_os.min_archetype_tier = sqlite3_column_int(stmt, 3);
        out_os.recommended_hdd_mb = sqlite3_column_int(stmt, 4);
        out_os.filesystem_type = ColumnTextSafe(stmt, 5);
        out_os.install_strategy = ColumnTextSafe(stmt, 6);
        out_os.boot_floppy_media_id = ColumnTextSafe(stmt, 7);
        out_os.install_cd_media_id = ColumnTextSafe(stmt, 8);
        out_os.unattended_answer_file = ColumnTextSafe(stmt, 9);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool KioskDatabase::InsertOSDistribution(const OSDistributionRecord& os) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || os.id.empty()) return false;

    const char* sql = "INSERT INTO os_distributions (id, display_name, os_family, min_archetype_tier, "
                      "recommended_hdd_mb, filesystem_type, install_strategy, boot_floppy_media_id, "
                      "install_cd_media_id, unattended_answer_file) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, os.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, os.display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, os.os_family.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, os.min_archetype_tier);
    sqlite3_bind_int(stmt, 5, os.recommended_hdd_mb);
    sqlite3_bind_text(stmt, 6, os.filesystem_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, os.install_strategy.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 8, os.boot_floppy_media_id);
    BindTextOrNull(stmt, 9, os.install_cd_media_id);
    BindTextOrNull(stmt, 10, os.unattended_answer_file);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

// ----------------------------------------------------------------------------
// 7. Software Signatures (Fingerprints)
// ----------------------------------------------------------------------------

bool KioskDatabase::FindSoftwareSignature(const std::string& exec_filename, const std::string& header_crc32, SoftwareSignatureRecord& out_sig) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || exec_filename.empty() || header_crc32.empty()) return false;

    const char* sql = "SELECT exec_filename, file_size_bytes, header_crc32, detected_title, "
                      "item_type, recommended_archetype FROM software_signatures "
                      "WHERE exec_filename = ? AND header_crc32 = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, exec_filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, header_crc32.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_sig.exec_filename = ColumnTextSafe(stmt, 0);
        out_sig.file_size_bytes = sqlite3_column_int64(stmt, 1);
        out_sig.header_crc32 = ColumnTextSafe(stmt, 2);
        out_sig.detected_title = ColumnTextSafe(stmt, 3);
        out_sig.item_type = ColumnTextSafe(stmt, 4);
        out_sig.recommended_archetype = ColumnTextSafe(stmt, 5);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool KioskDatabase::InsertSoftwareSignature(const SoftwareSignatureRecord& sig) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || sig.exec_filename.empty() || sig.header_crc32.empty()) return false;

    const char* sql = "INSERT OR REPLACE INTO software_signatures (exec_filename, file_size_bytes, "
                      "header_crc32, detected_title, item_type, recommended_archetype) "
                      "VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, sig.exec_filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, sig.file_size_bytes);
    sqlite3_bind_text(stmt, 3, sig.header_crc32.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, sig.detected_title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, sig.item_type.c_str(), -1, SQLITE_TRANSIENT);
    BindTextOrNull(stmt, 6, sig.recommended_archetype);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

#endif // defined(C_KIOSK)
