#ifndef DOSBOX_KIOSK_FAT_VOLUME_H
#define DOSBOX_KIOSK_FAT_VOLUME_H

// Shared FAT12/FAT16/FAT32 layer for every kiosk consumer that reads or mutates a guest
// disk image (boot injector AUTOEXEC hook, Mode B archive injection, image crawler).
//
// Crash safety: a writable volume never overwrites live metadata in place.
//  * New file/directory data is written straight into clusters that were free when the
//    volume was mounted; nothing on disk references them until the commit lands.
//  * Every other sector (FAT copies, directory sectors, FSInfo, rewritten file data) is
//    staged in memory and applied by KioskSectorDevice::CommitSectorsAtomically(). File
//    devices first write the original sectors to an fsync'd "<image>.kiosk-journal" undo
//    log, apply the new sectors, then delete the log. Opening an image with a leftover
//    log rolls it back, so the image is always either fully old or fully new.

#include "../kiosk_types.h"
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#if defined(C_KIOSK)

class imageDisk;

// Replaces a host file via "<path>.kiosk-tmp" + fsync + rename, so a crash leaves either
// the old or the new contents, never a truncated file.
bool KioskWriteHostFileAtomically(const std::string& path, const std::string& contents);

// Directory entry as reported by KioskFatVolume (and FatReadOnlyVfs)
struct FatDirEntry {
    std::string name;         // Reconstructed VFAT LFN (UTF-8) or 8.3 short name
    std::string short_name;   // Canonical 8.3 short name (uppercase)
    bool is_directory = false;
    uint8_t attributes = 0;
    uint32_t first_cluster = 0;
    uint32_t file_size = 0;
    uint64_t entry_lba = 0;      // Sector holding the 32-byte short-name entry
    uint32_t entry_offset = 0;   // Byte offset of that entry inside the sector
};

// ----------------------------------------------------------------------------
// Sector devices (512-byte sectors only)
// ----------------------------------------------------------------------------
class KioskSectorDevice {
public:
    typedef std::map<uint64_t, std::vector<uint8_t>> SectorMap;

    virtual ~KioskSectorDevice() {}
    virtual uint64_t GetSectorCount() const = 0;
    virtual bool IsWritable() const = 0;
    virtual bool ReadSectors(uint64_t lba, uint32_t count, uint8_t* out) = 0;
    // Unjournaled write. Only for sectors that no committed metadata references yet.
    virtual bool WriteSectorsDirect(uint64_t lba, uint32_t count, const uint8_t* in) = 0;
    // Durably replaces every sector in the map, all-or-nothing across crashes.
    virtual bool CommitSectorsAtomically(const SectorMap& sectors) = 0;
    // Called after a failed commit; the device must not be written through again.
    virtual void AbandonAfterFailedCommit() {}
};

class KioskFileSectorDevice : public KioskSectorDevice {
public:
    KioskFileSectorDevice() = default;
    ~KioskFileSectorDevice() override;
    KioskFileSectorDevice(const KioskFileSectorDevice&) = delete;
    KioskFileSectorDevice& operator=(const KioskFileSectorDevice&) = delete;

    // Holds a per-image lock until Close(): shared for read-only, exclusive for writable.
    // Writable opens roll back an interrupted commit first; read-only opens never write
    // and fail while a journal is pending.
    bool Open(const std::string& path, bool writable, std::string* out_error = nullptr);
    void Close();

    uint64_t GetSectorCount() const override { return m_sector_count; }
    bool IsWritable() const override { return m_writable && !m_failed; }
    bool ReadSectors(uint64_t lba, uint32_t count, uint8_t* out) override;
    bool WriteSectorsDirect(uint64_t lba, uint32_t count, const uint8_t* in) override;
    bool CommitSectorsAtomically(const SectorMap& sectors) override;
    void AbandonAfterFailedCommit() override;

    // Journal of the canonical image path (symlinks resolved / full path), shared with the lock
    static std::string JournalPathFor(const std::string& image_path);
    static bool HasPendingJournal(const std::string& image_path);
    // Rolls back an interrupted commit and deletes the journal. A torn journal is discarded
    // (the image is only touched after the journal is durable). A journal that does not
    // match the image (size changed, or a sector holds neither the original nor the
    // journaled new contents) is moved to ".bad" and never applied.
    // Returns false when the image is busy (lock not acquired) or rollback failed.
    static bool RecoverPendingJournal(const std::string& image_path, bool* out_rolled_back = nullptr,
                                      bool wait_for_lock = true);
    // Call after creating or replacing an image file: a leftover journal belongs to the old one.
    static void QuarantinePendingJournal(const std::string& image_path);

    // Holds a shared lock on the image while DOSBox-X has it mounted for a guest session, so
    // no kiosk writer (e.g. a background Mode B import) can modify it underneath the guest.
    // Non-blocking: returns false if a writer currently holds the image. Replaces any
    // previous pin; released by ReleaseGuestSessionPin() or process exit/re-exec.
    static bool PinForGuestSession(const std::string& image_path);
    static void ReleaseGuestSessionPin();

    // Test hook: when >= 0, the next commit stops after the journal is durable and this
    // many sectors were applied. simulate_power_cut leaves the journal as a dead process
    // would; otherwise it behaves like an I/O error (immediate rollback). Fires once.
    static void SetCommitFaultInjection(int sectors_applied_before_abort, bool simulate_power_cut = true);
    // Test hook: the next commit fails to invalidate its journal (the durable commit point).
    static void SetCommitInvalidateFault(bool fail_next_invalidation);

private:
    std::FILE* m_file = nullptr;
    std::string m_path;
    uint64_t m_sector_count = 0;
    uint64_t m_file_bytes = 0;
    bool m_writable = false;
    bool m_failed = false;
    bool m_lock_exclusive = false;
    std::shared_ptr<std::shared_timed_mutex> m_image_mutex;
};

// Read-only (or writable) view over an in-memory raw image
class KioskMemorySectorDevice : public KioskSectorDevice {
public:
    // Read-only unless writable access is requested explicitly (never inferred from constness)
    explicit KioskMemorySectorDevice(const std::vector<uint8_t>& image) : m_ro(&image) {}
    enum WritableTag { kWritable };
    KioskMemorySectorDevice(std::vector<uint8_t>& image, WritableTag) : m_ro(&image), m_rw(&image) {}

    uint64_t GetSectorCount() const override { return m_ro->size() / 512U; }
    bool IsWritable() const override { return m_rw != nullptr; }
    bool ReadSectors(uint64_t lba, uint32_t count, uint8_t* out) override;
    bool WriteSectorsDirect(uint64_t lba, uint32_t count, const uint8_t* in) override;
    bool CommitSectorsAtomically(const SectorMap& sectors) override;

private:
    const std::vector<uint8_t>* m_ro = nullptr;
    std::vector<uint8_t>* m_rw = nullptr;
};

// Read-only adapter over a DOSBox-X imageDisk (VHD and other container formats)
class KioskImageDiskSectorDevice : public KioskSectorDevice {
public:
    KioskImageDiskSectorDevice(imageDisk* disk, bool owns_disk);
    ~KioskImageDiskSectorDevice() override;
    KioskImageDiskSectorDevice(const KioskImageDiskSectorDevice&) = delete;
    KioskImageDiskSectorDevice& operator=(const KioskImageDiskSectorDevice&) = delete;

    bool IsUsable() const;
    uint64_t GetSectorCount() const override;
    bool IsWritable() const override { return false; }
    bool ReadSectors(uint64_t lba, uint32_t count, uint8_t* out) override;
    bool WriteSectorsDirect(uint64_t, uint32_t, const uint8_t*) override { return false; }
    bool CommitSectorsAtomically(const SectorMap&) override { return false; }

private:
    imageDisk* m_disk = nullptr;
    bool m_owns = false;
};

// ----------------------------------------------------------------------------
// FAT volume
// ----------------------------------------------------------------------------
class KioskFatVolume {
public:
    // Directory handle for write APIs: 0 is always the root directory (all FAT types).
    static const uint32_t kRootDir = 0;

    KioskFatVolume() = default;
    ~KioskFatVolume();  // Discards uncommitted changes
    KioskFatVolume(const KioskFatVolume&) = delete;
    KioskFatVolume& operator=(const KioskFatVolume&) = delete;

    // The single partition-selection rule shared by every kiosk FAT consumer:
    //  1. LBA 0 holds a valid FAT boot sector -> superfloppy volume at LBA 0.
    //  2. Otherwise MBR entries with a FAT partition type (status 0x00/0x80), active
    //     entries first, then table order; the first whose boot sector validates wins.
    // A FAT-typed entry whose boot sector looks like FAT but fails validation rejects
    // the disk instead of falling through to a different volume.
    static bool SelectPartition(KioskSectorDevice& device, bool require_within_device,
                                uint32_t& out_lba, uint32_t& out_sector_count);

    // Mounts the selected partition of a caller-owned device. Writable mounts require
    // a writable device and a partition that lies entirely inside it.
    bool Mount(KioskSectorDevice* device, std::string* out_error = nullptr);
    // Opens an image file and owns the device. ".VHD" opens read-only via imageDisk.
    bool OpenImageFile(const std::string& path, bool writable, std::string* out_error = nullptr);
    // Mounts a caller-owned imageDisk read-only.
    bool OpenImageDisk(imageDisk* non_owning_disk, std::string* out_error = nullptr);
    void Close();  // Discards uncommitted changes

    bool IsMounted() const { return m_mounted; }
    bool IsWritable() const { return m_mounted && m_writable; }
    FilesystemType GetFilesystemType() const { return m_fs_type; }
    uint32_t GetPartitionLbaStart() const { return m_part_lba; }
    uint32_t GetBytesPerSector() const { return 512U; }
    uint32_t GetSectorsPerCluster() const { return m_sectors_per_cluster; }
    uint32_t GetClusterCount() const { return m_cluster_count; }

    // ---- Reading (sees staged changes) ----
    bool ListDirectory(const FatDirEntry* dir, std::vector<FatDirEntry>& out_entries) const;
    bool ListDirectoryCluster(uint32_t dir_cluster, std::vector<FatDirEntry>& out_entries) const;
    // Case-insensitive lookup by LFN or 8.3 name; accepts an optional "C:" prefix only.
    bool FindPath(const std::string& guest_path, FatDirEntry& out_entry) const;
    bool PathExists(const std::string& guest_path) const;
    bool ReadFile(const FatDirEntry& entry, std::vector<uint8_t>& out_data,
                  size_t max_bytes = 16U * 1024U * 1024U) const;

    // ---- Writing (staged until Commit) ----
    // Contract: if any write call fails, the caller must discard (DiscardChanges() or let the
    // volume go out of scope) rather than Commit(): a failed call may leave partial staging.
    // A failed Commit() unmounts the volume; reopen the image to continue.
    uint64_t ComputeFreeBytes() const;
    bool EnsureDirectoryPath(const std::string& guest_path, bool allow_lfn,
                             uint32_t& out_dir_cluster, std::string& out_error);
    bool EnsureChildDirectory(uint32_t parent_dir, const std::string& name, bool allow_lfn,
                              uint32_t& out_child_dir, std::string& out_error);
    // Creates a new file; fails if a file or directory of that name already exists.
    // (Not named CreateFile: that is a Win32 macro and would rename the symbol per-TU.)
    bool AddFile(uint32_t parent_dir, const std::string& name, const std::vector<uint8_t>& data,
                 bool allow_lfn, std::string& out_error);
    // Rewrites an existing file's contents in place (reusing its cluster chain) or creates it.
    bool WriteFileReplacing(uint32_t parent_dir, const std::string& name, const std::vector<uint8_t>& data,
                            std::string& out_error);

    bool HasPendingChanges() const { return !m_dirty.empty(); }
    bool Commit(std::string* out_error = nullptr);
    void DiscardChanges();

private:
    bool ParseBootSector(uint32_t part_lba, uint32_t max_part_sectors, bool require_within_device);
    bool MountInternal(KioskSectorDevice* device, bool writable, std::string* out_error);

    bool ReadSector(uint64_t lba, uint8_t out[512]) const;
    bool StageSector(uint64_t lba, const uint8_t in[512]);
    bool IsFatSector(uint64_t lba) const;
    // Zero-copy view of a FAT sector (staged or cached); valid until the next peek.
    const uint8_t* PeekFatSector(uint64_t lba) const;

    uint64_t ClusterToLba(uint32_t cluster) const;
    bool IsValidCluster(uint32_t cluster) const;
    bool IsEndOfChain(uint32_t value) const;
    uint32_t EndOfChainValue() const;
    uint32_t ResolveDirCluster(uint32_t dir_handle) const;
    bool GetFatEntry(uint32_t cluster, uint32_t& out_value) const;
    bool SetFatEntry(uint32_t cluster, uint32_t value);
    bool AllocateChain(uint32_t count, std::vector<uint32_t>& out_chain);
    bool FreeChain(const std::vector<uint32_t>& chain);
    bool CollectChain(uint32_t first_cluster, std::vector<uint32_t>& out_chain) const;
    bool WriteClusterData(uint32_t cluster, const uint8_t* data);

    bool GetDirectorySectors(uint32_t dir_handle, std::vector<uint64_t>& out_lbas,
                             uint32_t& out_last_cluster) const;
    bool ParseDirectorySectors(const std::vector<uint64_t>& lbas, std::vector<FatDirEntry>& out_entries) const;
    bool FindInDirectory(uint32_t dir_handle, const std::string& name, FatDirEntry& out_entry) const;
    bool ShortNameExists(const std::vector<uint64_t>& lbas, const uint8_t sfn[11]) const;
    bool AddDirectoryEntry(uint32_t dir_handle, const std::string& name, uint8_t attr,
                           uint32_t first_cluster, uint32_t file_size, bool allow_lfn, std::string& out_error);
    bool CreateSubdirectory(uint32_t parent_dir, const std::string& name, bool allow_lfn,
                            uint32_t& out_cluster, std::string& out_error);
    bool StageFsInfoUpdate();

    std::unique_ptr<KioskSectorDevice> m_owned_device;
    KioskSectorDevice* m_device = nullptr;
    bool m_mounted = false;
    bool m_writable = false;

    FilesystemType m_fs_type = FilesystemType::FAT16;
    uint32_t m_part_lba = 0;
    uint32_t m_sectors_per_cluster = 1;
    uint32_t m_reserved_sectors = 1;
    uint32_t m_num_fats = 2;
    uint32_t m_total_sectors = 0;
    uint32_t m_fat_size_sectors = 0;
    uint32_t m_root_dir_sectors = 0;
    uint32_t m_first_data_sector = 0;
    uint32_t m_cluster_count = 0;      // Usable clusters (limited by FAT capacity)
    uint32_t m_alloc_limit = 0;        // Allocatable clusters (limited to the image file size)
    uint32_t m_root_cluster = 0;       // FAT32 only
    uint32_t m_active_fat = 0;
    bool m_fat_mirroring = true;
    uint32_t m_fsinfo_sector = 0;      // FAT32 only, relative to partition (0 = none)

    // Transaction state
    std::map<uint64_t, std::vector<uint8_t>> m_dirty;            // Staged sectors
    mutable std::map<uint64_t, std::vector<uint8_t>> m_fat_cache; // Clean FAT sectors
    std::set<uint32_t> m_fresh_clusters;   // Free at mount, allocated since: written directly
    std::set<uint32_t> m_freed_clusters;   // Freed since mount: never reused before commit
    int64_t m_free_cluster_delta = 0;
    bool m_poisoned = false;            // A FAT update half-staged: only DiscardChanges() is safe
    uint32_t m_alloc_hint = 2;
};

#endif // defined(C_KIOSK)

#endif // DOSBOX_KIOSK_FAT_VOLUME_H
