#include "config.h"

#if defined(C_DEBUG) && defined(C_KIOSK)

#include "kiosk_types.h"
#include "kiosk_db.h"
#include "machine/archetypes.h"
#include "machine/config_synthesizer.h"
#include "sqlite3.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

class KioskDatabaseFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Open a fresh isolated in-memory database for each test
        KioskDatabase::GetInstance().Open(":memory:");
    }

    void TearDown() override {
        KioskDatabase::GetInstance().Close();
    }
};

} // anonymous namespace

// ----------------------------------------------------------------------------
// Database Tests
// ----------------------------------------------------------------------------

TEST_F(KioskDatabaseFixture, SchemaInitializationAndMemoryFallback) {
    ASSERT_TRUE(KioskDatabase::GetInstance().IsOpen());
    EXPECT_FALSE(KioskDatabase::GetInstance().GetSQLiteVersion().empty());

    // Verify default config seeded
    KioskConfigRecord config;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetConfig(config));
    EXPECT_EQ(1, config.id);
    EXPECT_EQ(30, config.boot_timeout_sec);
    EXPECT_STREQ("DEFAULT_DOSBOX", config.default_boot_action.c_str());
    EXPECT_STREQ("F12+D", config.overlay_hotkey.c_str());
    EXPECT_STREQ("SKEUOMORPHIC", config.default_caddy_view.c_str());
    EXPECT_TRUE(config.auto_mount_usb_docs);
}

TEST_F(KioskDatabaseFixture, InaccessiblePathFallsBackToMemory) {
    // Attempt to open a path in a nonexistent directory
    bool opened = KioskDatabase::GetInstance().Open("/invalid_nonexistent_root_dir_9999/catalog.db");
    EXPECT_TRUE(opened);
    EXPECT_TRUE(KioskDatabase::GetInstance().IsOpen());

    // Verify fallback to :memory: created the tables and default config
    KioskConfigRecord config;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetConfig(config));
    EXPECT_EQ(1, config.id);
}

TEST_F(KioskDatabaseFixture, SeedDistributions) {
    auto distros = KioskDatabase::GetInstance().GetAllOSDistributions();
    EXPECT_GE(distros.size(), 8U);

    bool found_dos622 = false;
    bool found_win98 = false;
    for (const auto& d : distros) {
        if (d.id == "MSDOS_622") found_dos622 = true;
        if (d.id == "WIN98_SE") found_win98 = true;
    }
    EXPECT_TRUE(found_dos622);
    EXPECT_TRUE(found_win98);

    OSDistributionRecord dos622;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetOSDistribution("MSDOS_622", dos622));
    EXPECT_STREQ("Microsoft MS-DOS 6.22", dos622.display_name.c_str());
    EXPECT_STREQ("MSDOS", dos622.os_family.c_str());
    EXPECT_EQ(3, dos622.min_archetype_tier);
    EXPECT_STREQ("FAT16", dos622.filesystem_type.c_str());
}

TEST_F(KioskDatabaseFixture, MachineCrudOperations) {
    MachineRecord m;
    m.id = "mach-doom-486";
    m.display_name = "1994 Doom Rig";
    m.archetype_id = KioskArchetypes::DX2_486;
    m.archetype_tier = 4;
    m.os_id = "MSDOS_622";
    m.os_installed = true;
    m.boot_mode = "REAL_HDD_BOOT";
    m.primary_hdd_path = "/storage/machines/doom/hdd_c.img";
    m.primary_hdd_chs = "520,16,63";
    m.custom_conf_overrides = "[render]\naspect=true";

    EXPECT_TRUE(KioskDatabase::GetInstance().InsertMachine(m));

    MachineRecord retrieved;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMachine("mach-doom-486", retrieved));
    EXPECT_STREQ("1994 Doom Rig", retrieved.display_name.c_str());
    EXPECT_STREQ(KioskArchetypes::DX2_486, retrieved.archetype_id.c_str());
    EXPECT_EQ(4, retrieved.archetype_tier);
    EXPECT_STREQ("REAL_HDD_BOOT", retrieved.boot_mode.c_str());

    // Update
    m.display_name = "1994 Ultimate Doom Rig";
    EXPECT_TRUE(KioskDatabase::GetInstance().UpdateMachine(m));
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMachine("mach-doom-486", retrieved));
    EXPECT_STREQ("1994 Ultimate Doom Rig", retrieved.display_name.c_str());

    // Delete
    EXPECT_TRUE(KioskDatabase::GetInstance().DeleteMachine("mach-doom-486"));
    EXPECT_FALSE(KioskDatabase::GetInstance().GetMachine("mach-doom-486", retrieved));
}

TEST_F(KioskDatabaseFixture, CatalogItemCrudOperations) {
    // Need a parent machine first due to FK constraints
    MachineRecord m;
    m.id = "mach-1";
    m.display_name = "Test Machine";
    m.archetype_id = KioskArchetypes::DX_386;
    m.archetype_tier = 3;
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMachine(m));

    CatalogItemRecord game;
    game.id = "game-doom2";
    game.machine_id = "mach-1";
    game.item_type = "GAME";
    game.title = "Doom II";
    game.developer = "id Software";
    game.release_year = 1994;
    game.guest_working_dir = "\\DOOM2";
    game.guest_executable = "DOOM2.EXE";
    game.guest_arguments = "-warp 01";
    game.return_on_exit = true;
    game.is_favorite = true;

    EXPECT_TRUE(KioskDatabase::GetInstance().InsertCatalogItem(game));

    CatalogItemRecord app;
    app.id = "app-wp51";
    app.machine_id = "mach-1";
    app.item_type = "APPLICATION";
    app.title = "WordPerfect 5.1";
    app.developer = "WordPerfect Corp";
    app.release_year = 1989;
    app.guest_working_dir = "\\WP51";
    app.guest_executable = "WP.EXE";
    EXPECT_TRUE(KioskDatabase::GetInstance().InsertCatalogItem(app));

    // Query by type
    auto games = KioskDatabase::GetInstance().GetItemsByType("GAME");
    ASSERT_EQ(1U, games.size());
    EXPECT_STREQ("Doom II", games[0].title.c_str());

    auto apps = KioskDatabase::GetInstance().GetItemsByType("APPLICATION");
    ASSERT_EQ(1U, apps.size());
    EXPECT_STREQ("WordPerfect 5.1", apps[0].title.c_str());

    // Play count increment
    EXPECT_TRUE(KioskDatabase::GetInstance().IncrementPlayCount("game-doom2"));
    CatalogItemRecord updated_game;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetItem("game-doom2", updated_game));
    EXPECT_EQ(1, updated_game.play_count);

    // Delete
    EXPECT_TRUE(KioskDatabase::GetInstance().DeleteCatalogItem("game-doom2"));
    EXPECT_FALSE(KioskDatabase::GetInstance().GetItem("game-doom2", updated_game));
}

TEST_F(KioskDatabaseFixture, MediaPoolAndCaddyBindings) {
    MachineRecord m;
    m.id = "mach-monkey";
    m.display_name = "Adventure PC";
    m.archetype_id = KioskArchetypes::AT_286;
    m.archetype_tier = 2;
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMachine(m));

    MediaPoolRecord disk1;
    disk1.id = "media-mi-1";
    disk1.file_path = "/storage/media/monkey_disk1.img";
    disk1.media_type = "FLOPPY_35_1440K";
    disk1.physical_style = "BEIGE_35";
    disk1.label_title = "Monkey Island Disk 1";
    EXPECT_TRUE(KioskDatabase::GetInstance().InsertMedia(disk1));

    MediaPoolRecord disk2;
    disk2.id = "media-mi-2";
    disk2.file_path = "/storage/media/monkey_disk2.img";
    disk2.media_type = "FLOPPY_35_1440K";
    disk2.physical_style = "BEIGE_35";
    disk2.label_title = "Monkey Island Disk 2";
    EXPECT_TRUE(KioskDatabase::GetInstance().InsertMedia(disk2));

    // Bind to machine caddy
    CaddyBindingRecord b1;
    b1.machine_id = "mach-monkey";
    b1.media_id = "media-mi-1";
    b1.target_drive = "A";
    b1.slot_order = 1;
    EXPECT_TRUE(KioskDatabase::GetInstance().AddCaddyBinding(b1));

    CaddyBindingRecord b2;
    b2.machine_id = "mach-monkey";
    b2.media_id = "media-mi-2";
    b2.target_drive = "A";
    b2.slot_order = 2;
    EXPECT_TRUE(KioskDatabase::GetInstance().AddCaddyBinding(b2));

    auto bindings = KioskDatabase::GetInstance().GetCaddyBindingsForMachine("mach-monkey");
    ASSERT_EQ(2U, bindings.size());
    EXPECT_EQ(1, bindings[0].slot_order);
    EXPECT_STREQ("/storage/media/monkey_disk1.img", bindings[0].resolved_file_path.c_str());
    EXPECT_EQ(2, bindings[1].slot_order);
    EXPECT_STREQ("/storage/media/monkey_disk2.img", bindings[1].resolved_file_path.c_str());

    // Clear
    EXPECT_TRUE(KioskDatabase::GetInstance().ClearCaddyBindingsForMachine("mach-monkey"));
    bindings = KioskDatabase::GetInstance().GetCaddyBindingsForMachine("mach-monkey");
    EXPECT_TRUE(bindings.empty());
}

TEST_F(KioskDatabaseFixture, SoftwareSignatureLookup) {
    SoftwareSignatureRecord sig;
    sig.exec_filename = "DOOM.EXE";
    sig.file_size_bytes = 705500;
    sig.header_crc32 = "A1B2C3D4";
    sig.detected_title = "DOOM v1.9";
    sig.item_type = "GAME";
    sig.recommended_archetype = KioskArchetypes::DX2_486;

    EXPECT_TRUE(KioskDatabase::GetInstance().InsertSoftwareSignature(sig));

    SoftwareSignatureRecord found;
    ASSERT_TRUE(KioskDatabase::GetInstance().FindSoftwareSignature("DOOM.EXE", "A1B2C3D4", found));
    EXPECT_STREQ("DOOM v1.9", found.detected_title.c_str());
    EXPECT_STREQ(KioskArchetypes::DX2_486, found.recommended_archetype.c_str());
}

// ----------------------------------------------------------------------------
// Archetype Engine Tests
// ----------------------------------------------------------------------------

TEST(KioskArchetypeTest, ProfileDefinitions) {
    auto archetypes = ArchetypeManager::GetInstance().GetAllArchetypes();
    ASSERT_EQ(5U, archetypes.size());

    const auto* xt = ArchetypeManager::GetInstance().GetArchetype(KioskArchetypes::XT_8088);
    ASSERT_NE(nullptr, xt);
    EXPECT_EQ(1, xt->tier);
    EXPECT_STREQ("8088", xt->cpu_type.c_str());
    EXPECT_STREQ("cga", xt->machine_type.c_str());
    EXPECT_FALSE(xt->apm_bios);

    const auto* pentium = ArchetypeManager::GetInstance().GetArchetype(KioskArchetypes::PENTIUM_3DFX);
    ASSERT_NE(nullptr, pentium);
    EXPECT_EQ(5, pentium->tier);
    EXPECT_STREQ("pentium_mmx", pentium->cpu_type.c_str());
    EXPECT_TRUE(pentium->voodoo_enabled);
    EXPECT_TRUE(pentium->apm_bios);
    EXPECT_STREQ("1.2", pentium->apm_version.c_str());
}

TEST(KioskArchetypeTest, OSCompatibilityValidation) {
    MachineRecord xt_machine;
    xt_machine.archetype_id = KioskArchetypes::XT_8088;
    xt_machine.archetype_tier = 1;

    MachineRecord pentium_machine;
    pentium_machine.archetype_id = KioskArchetypes::PENTIUM_3DFX;
    pentium_machine.archetype_tier = 5;

    OSDistributionRecord dos1;
    dos1.id = "PC_DOS_110";
    dos1.display_name = "DOS 1.10";
    dos1.os_family = "MSDOS";
    dos1.min_archetype_tier = 1;
    dos1.filesystem_type = "FAT12";

    OSDistributionRecord win98;
    win98.id = "WIN98_SE";
    win98.display_name = "Windows 98 SE";
    win98.os_family = "WIN9X";
    win98.min_archetype_tier = 5;
    win98.filesystem_type = "FAT32";

    std::string reason;
    // DOS 1 on XT -> Valid
    EXPECT_TRUE(ArchetypeManager::GetInstance().ValidateMachineCompatibility(xt_machine, dos1, reason));

    // Win98 on XT -> Rejected
    reason.clear();
    EXPECT_FALSE(ArchetypeManager::GetInstance().ValidateMachineCompatibility(xt_machine, win98, reason));
    EXPECT_FALSE(reason.empty());

    // Win98 on Pentium -> Valid
    reason.clear();
    EXPECT_TRUE(ArchetypeManager::GetInstance().ValidateMachineCompatibility(pentium_machine, win98, reason));
}

// ----------------------------------------------------------------------------
// Configuration Synthesizer Tests
// ----------------------------------------------------------------------------

TEST(KioskSynthesizerTest, SynthesizeRealHddBootConfig) {
    MachineRecord m;
    m.id = "mach-real";
    m.archetype_id = KioskArchetypes::DX2_486;
    m.archetype_tier = 4;
    m.boot_mode = "REAL_HDD_BOOT";
    m.primary_hdd_path = "C:\\kiosk\\hdd_c.img";
    m.primary_hdd_chs = "512,63,16,520";

    CaddyBindingRecord b1;
    b1.target_drive = "A";
    b1.slot_order = 1;
    b1.resolved_file_path = "C:\\kiosk\\floppy1.img";

    CaddyBindingRecord b2;
    b2.target_drive = "D";
    b2.slot_order = 1;
    b2.resolved_file_path = "C:\\kiosk\\cdrom.iso";

    std::vector<CaddyBindingRecord> caddy;
    caddy.push_back(b1);
    caddy.push_back(b2);

    std::string conf = ConfigSynthesizer::GetInstance().SynthesizeConfig(m, nullptr, caddy);

    // Verify sections
    EXPECT_NE(std::string::npos, conf.find("[sdl]"));
    EXPECT_NE(std::string::npos, conf.find("output              = openglpp"));
    EXPECT_NE(std::string::npos, conf.find("[cpu]"));
    EXPECT_NE(std::string::npos, conf.find("cputype             = 486"));
    EXPECT_NE(std::string::npos, conf.find("[autoexec]"));
    EXPECT_NE(std::string::npos, conf.find("IMGMOUNT 2 \"C:\\kiosk\\hdd_c.img\" -t hdd -fs none -size 512,63,16,520 -ide 1m"));
    EXPECT_NE(std::string::npos, conf.find("IMGMOUNT 0 \"C:\\kiosk\\floppy1.img\" -t fd -fs none"));
    EXPECT_NE(std::string::npos, conf.find("IMGMOUNT D \"C:\\kiosk\\cdrom.iso\" -t iso -fs iso -ide 2m"));
    EXPECT_NE(std::string::npos, conf.find("BOOT -l c"));
}

TEST(KioskSynthesizerTest, SynthesizeIntegratedDosConfig) {
    MachineRecord m;
    m.id = "mach-dos";
    m.archetype_id = KioskArchetypes::DX_386;
    m.archetype_tier = 3;
    m.boot_mode = "INTEGRATED_DOS";

    CatalogItemRecord item;
    item.id = "game-simcity";
    item.title = "SimCity 2000";
    item.host_container_dir = "C:\\games\\sc2000";
    item.guest_working_dir = "\\SC2000";
    item.guest_executable = "SC2000.EXE";
    item.guest_arguments = "";
    item.return_on_exit = true;

    std::string conf = ConfigSynthesizer::GetInstance().SynthesizeConfig(m, &item, {});

    EXPECT_NE(std::string::npos, conf.find("MOUNT C \"C:\\games\\sc2000\""));
    EXPECT_NE(std::string::npos, conf.find("CD \\SC2000"));
    EXPECT_NE(std::string::npos, conf.find("SC2000.EXE"));
    EXPECT_NE(std::string::npos, conf.find("EXIT"));
}

TEST(KioskSynthesizerTest, SynthesizeIntegratedDosWithCaddyAndSpacedExe) {
    MachineRecord m;
    m.id = "mach-dos-caddy";
    m.archetype_id = KioskArchetypes::AT_286;
    m.archetype_tier = 2;
    m.boot_mode = "INTEGRATED_DOS";

    CatalogItemRecord item;
    item.id = "game-spaced";
    item.title = "Custom Game";
    item.host_container_dir = "C:\\games\\custom";
    item.guest_working_dir = "\\CUSTOM";
    item.guest_executable = "MY GAME.EXE";
    item.guest_arguments = "-v";
    item.return_on_exit = true;

    CaddyBindingRecord b1;
    b1.target_drive = "A";
    b1.slot_order = 1;
    b1.resolved_file_path = "C:\\games\\disk1.img";

    CaddyBindingRecord b2;
    b2.target_drive = "D";
    b2.slot_order = 1;
    b2.resolved_file_path = "C:\\games\\music.iso";

    std::vector<CaddyBindingRecord> caddy;
    caddy.push_back(b1);
    caddy.push_back(b2);

    std::string conf = ConfigSynthesizer::GetInstance().SynthesizeConfig(m, &item, caddy);

    EXPECT_NE(std::string::npos, conf.find("IMGMOUNT 0 \"C:\\games\\disk1.img\" -t fd -fs none"));
    EXPECT_NE(std::string::npos, conf.find("IMGMOUNT D \"C:\\games\\music.iso\" -t iso"));
    EXPECT_NE(std::string::npos, conf.find("\"MY GAME.EXE\" -v"));
    EXPECT_NE(std::string::npos, conf.find("EXIT"));
}

TEST(KioskSynthesizerTest, CustomOverridesMerge) {
    MachineRecord m;
    m.id = "mach-override";
    m.archetype_id = KioskArchetypes::XT_8088;
    m.archetype_tier = 1;
    m.boot_mode = "INTEGRATED_DOS";
    m.custom_conf_overrides = "[custom_section]\nmy_key=my_value";

    std::string conf = ConfigSynthesizer::GetInstance().SynthesizeConfig(m, nullptr, {});
    EXPECT_NE(std::string::npos, conf.find("[custom_section]"));
    EXPECT_NE(std::string::npos, conf.find("my_key=my_value"));
}

#endif // defined(C_DEBUG) && defined(C_KIOSK)
