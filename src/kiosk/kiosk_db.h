#ifndef DOSBOX_KIOSK_DB_H
#define DOSBOX_KIOSK_DB_H

#include <string>
#include <vector>

struct sqlite3;

class KioskDatabase {
public:
    static KioskDatabase& GetInstance();

    bool Open(const std::string& db_path);
    void Close();
    bool IsOpen() const { return m_db != nullptr; }

    std::string GetSQLiteVersion() const;

private:
    KioskDatabase() = default;
    ~KioskDatabase();
    KioskDatabase(const KioskDatabase&) = delete;
    KioskDatabase& operator=(const KioskDatabase&) = delete;

    sqlite3* m_db = nullptr;
};

#endif // DOSBOX_KIOSK_DB_H
