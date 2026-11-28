#ifndef DOSBOX_KIOSK_DB_H
#define DOSBOX_KIOSK_DB_H

#include "kiosk_types.h"
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <mutex>

struct sqlite3;

class KioskDatabase {
public:
    static KioskDatabase& GetInstance();

    bool Open(const std::string& db_path);
    void Close();
    bool IsOpen() const;
    std::string GetActiveDatabasePath() const;
    static std::string GetDefaultDatabasePath();

    std::string GetSQLiteVersion() const;

    // Change notification without upward dependencies: every successful mutation (and every
    // Open/Close) bumps a monotonic generation. Consumers (UI caches, previews) poll it and
    // reload when it moves; the data layer never calls into them.
    uint64_t GetChangeGeneration() const { return m_change_generation.load(std::memory_order_acquire); }
    uint64_t GetConfigGeneration() const { return m_config_generation.load(std::memory_order_acquire); }
    bool InitializeSchema();
    bool SeedDefaultData();

    // 1. Kiosk Configuration
    bool GetConfig(KioskConfigRecord& out_config);
    bool UpdateConfig(const KioskConfigRecord& config);

    // 2. Virtual Machines
    std::vector<MachineRecord> GetAllMachines();
    bool GetMachine(const std::string& id, MachineRecord& out_machine);
    bool InsertMachine(const MachineRecord& machine);
    bool InsertMachineWithBindings(const MachineRecord& machine, const std::vector<CaddyBindingRecord>& bindings);
    bool UpdateMachine(const MachineRecord& machine);
    bool DeleteMachine(const std::string& id);

    // 3. Carousel Catalog Items (Games & Applications)
    std::vector<CatalogItemRecord> GetAllCatalogItems();
    std::vector<CatalogItemRecord> GetItemsByType(const std::string& item_type);
    bool GetItem(const std::string& id, CatalogItemRecord& out_item);
    std::vector<CatalogItemRecord> GetItemsForMachine(const std::string& machine_id);
    bool GetItemsForMachine(const std::string& machine_id, std::vector<CatalogItemRecord>& out_items);
    bool InsertCatalogItem(const CatalogItemRecord& item);
    bool UpdateCatalogItem(const CatalogItemRecord& item);
    bool DeleteCatalogItem(const std::string& id);
    bool IncrementPlayCount(const std::string& id);

    // 4. Global Media Pool
    std::vector<MediaPoolRecord> GetAllMedia();
    bool GetMedia(const std::string& id, MediaPoolRecord& out_media);
    bool GetMediaByPath(const std::string& file_path, MediaPoolRecord& out_media);
    bool InsertMedia(const MediaPoolRecord& media);
    bool UpdateMedia(const MediaPoolRecord& media);
    bool DeleteMedia(const std::string& id);
    bool RenameMediaGroup(const std::string& old_group, const std::string& new_group);
    bool AssignMediaGroupBatch(const std::vector<std::string>& media_ids, const std::string& new_group);
    std::vector<MediaPoolRecord> GetRemovableUsbMedia();
    int DeleteRemovableUsbMediaByPrefix(const std::string& mount_prefix);
    int PurgeStaleRemovableUsbMedia();

    // 5. Session Caddy Bindings
    std::vector<CaddyBindingRecord> GetCaddyBindingsForMachine(const std::string& machine_id);
    std::vector<CaddyBindingRecord> GetCaddyBindingsForItem(const std::string& item_id);
    bool AddCaddyBinding(const CaddyBindingRecord& binding);
    bool DeleteCaddyBinding(int id);
    bool ClearCaddyBindingsForMachine(const std::string& machine_id);
    bool ClearCaddyBindingsForItem(const std::string& item_id);

    // 6. Pre-Staged Operating System Distributions
    std::vector<OSDistributionRecord> GetAllOSDistributions();
    bool GetOSDistribution(const std::string& id, OSDistributionRecord& out_os);
    bool InsertOSDistribution(const OSDistributionRecord& os);

    // 7. Software Signatures (Fingerprints)
    bool FindSoftwareSignature(const std::string& exec_filename, const std::string& header_crc32, SoftwareSignatureRecord& out_sig);
    bool InsertSoftwareSignature(const SoftwareSignatureRecord& sig);

private:
    KioskDatabase() = default;
    ~KioskDatabase();
    KioskDatabase(const KioskDatabase&) = delete;
    KioskDatabase& operator=(const KioskDatabase&) = delete;

    bool OpenInternal(const std::string& path);
    bool ExecuteSqlInternal(const char* sql);
    void CloseInternal();
    void NotifyChanged(bool config_changed = false) {
        m_change_generation.fetch_add(1, std::memory_order_acq_rel);
        if (config_changed) m_config_generation.fetch_add(1, std::memory_order_acq_rel);
    }

    std::atomic<uint64_t> m_change_generation{1};
    std::atomic<uint64_t> m_config_generation{1};
    mutable std::mutex m_mutex;
    sqlite3* m_db = nullptr;
    std::string m_active_path;
};

#endif // DOSBOX_KIOSK_DB_H
