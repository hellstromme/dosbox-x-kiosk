#include "config.h"
#include "kiosk_db.h"
#include "sqlite3.h"
#include "logging.h"

KioskDatabase& KioskDatabase::GetInstance() {
    static KioskDatabase instance;
    return instance;
}

KioskDatabase::~KioskDatabase() {
    Close();
}

bool KioskDatabase::Open(const std::string& db_path) {
    if (m_db) {
        Close();
    }

    std::string path = db_path.empty() ? ":memory:" : db_path;
    int rc = sqlite3_open(path.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        LOG_MSG("KIOSK: Failed to open SQLite database '%s': %s", path.c_str(), sqlite3_errmsg(m_db));
        Close();
        return false;
    }

    LOG_MSG("KIOSK: Opened SQLite database '%s' (SQLite v%s)", path.c_str(), sqlite3_libversion());
    return true;
}

void KioskDatabase::Close() {
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

std::string KioskDatabase::GetSQLiteVersion() const {
    return sqlite3_libversion();
}
