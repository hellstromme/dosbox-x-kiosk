#include "config.h"

#if defined(C_DEBUG) && defined(C_KIOSK)

#include "kiosk_types.h"
#include "kiosk_db.h"
#include "machine/archetypes.h"
#include "machine/config_synthesizer.h"
#include "machine/kioskext_bin.h"
#include "machine/boot_injector.h"
#include "ui/kiosk_ui.h"
#include "ui/input_router.h"
#include "ui/view_watchdog.h"
#include "ui/view_carousel.h"
#include "ui/view_wizard.h"
#include "ui/view_disk_caddy.h"
#include "ui/runtime_overlay.h"
#include "kiosk_main.h"
#include "../src/dos/drives.h"
#include "bios_disk.h"
#include "ide.h"
#include "pic.h"
#include "timer.h"
#include <cstdio>
#include <fstream>
#include "dosbox.h"
#include "SDL.h"
#include "sqlite3.h"

extern void Kiosk_ResetPort8900State(void);
extern void bochs_port_8900_write(Bitu port, Bitu val, Bitu iolen);

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
    m.primary_hdd_chs = "520,16,63";

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
    EXPECT_NE(std::string::npos, conf.find("IMGMOUNT 0 \"C:\\kiosk\\floppy1.img\" -t floppy -fs none"));
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

    EXPECT_NE(std::string::npos, conf.find("IMGMOUNT A \"C:\\games\\disk1.img\" -t floppy"));
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

// ----------------------------------------------------------------------------
// Phase 2: Kiosk Lifecycle State Machine & Emulation Trapping Tests
// ----------------------------------------------------------------------------

class KioskLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        KioskManager::GetInstance().SetKioskModeActive(true);
        ASSERT_TRUE(KioskManager::GetInstance().Initialize(":memory:", ""));
    }

    void TearDown() override {
        KioskManager::GetInstance().Shutdown();
    }
};

TEST_F(KioskLifecycleTest, BootToCountdownAndWatchdogExpiration) {
    auto& mgr = KioskManager::GetInstance();
    EXPECT_EQ(KioskState::STATE_COUNTDOWN_IDLE, mgr.GetState());
    EXPECT_FLOAT_EQ(30.0f, mgr.GetWatchdogRemainingSec());

    // Advance 10 seconds -> still in countdown
    mgr.UpdateWatchdog(10.0f);
    EXPECT_EQ(KioskState::STATE_COUNTDOWN_IDLE, mgr.GetState());
    EXPECT_FLOAT_EQ(20.0f, mgr.GetWatchdogRemainingSec());

    // Advance remaining 20 seconds -> triggers DEFAULT_DOSBOX -> STATE_SYNTHESIZE_LAUNCH
    mgr.UpdateWatchdog(20.0f);
    EXPECT_FLOAT_EQ(0.0f, mgr.GetWatchdogRemainingSec());
    EXPECT_EQ(KioskState::STATE_SYNTHESIZE_LAUNCH, mgr.GetState());
}

TEST_F(KioskLifecycleTest, WatchdogCancellationOnIntentionalInput) {
    auto& mgr = KioskManager::GetInstance();
    ASSERT_EQ(KioskState::STATE_COUNTDOWN_IDLE, mgr.GetState());

    // Small mouse jitter (< 5px) should be ignored
    SDL_Event jitter;
    memset(&jitter, 0, sizeof(jitter));
    jitter.type = SDL_MOUSEMOTION;
    jitter.motion.xrel = 2;
    jitter.motion.yrel = -3;
    EXPECT_FALSE(KioskManager::IsIntentionalUserInput(jitter));
    EXPECT_FALSE(mgr.HandlePreBootInputEvent(jitter));
    EXPECT_EQ(KioskState::STATE_COUNTDOWN_IDLE, mgr.GetState());

    // Small controller axis deadzone (<= 8000) should be ignored
    SDL_Event stick_noise;
    memset(&stick_noise, 0, sizeof(stick_noise));
    stick_noise.type = SDL_CONTROLLERAXISMOTION;
    stick_noise.caxis.value = 4000;
    EXPECT_FALSE(KioskManager::IsIntentionalUserInput(stick_noise));
    EXPECT_FALSE(mgr.HandlePreBootInputEvent(stick_noise));
    EXPECT_EQ(KioskState::STATE_COUNTDOWN_IDLE, mgr.GetState());

    // Intentional keydown cancels watchdog and transitions to STATE_SHELL_INTERACTIVE
    SDL_Event keydown;
    memset(&keydown, 0, sizeof(keydown));
    keydown.type = SDL_KEYDOWN;
    keydown.key.keysym.sym = SDLK_SPACE;
    EXPECT_TRUE(KioskManager::IsIntentionalUserInput(keydown));
    EXPECT_TRUE(mgr.HandlePreBootInputEvent(keydown));
    EXPECT_EQ(KioskState::STATE_SHELL_INTERACTIVE, mgr.GetState());
}

TEST_F(KioskLifecycleTest, ResolveDefaultBootActions) {
    auto& db = KioskDatabase::GetInstance();
    auto& mgr = KioskManager::GetInstance();

    MachineRecord m1;
    m1.id = "mach-boot-1";
    m1.display_name = "486 Gaming PC";
    m1.archetype_id = KioskArchetypes::DX2_486;
    m1.archetype_tier = 4;
    m1.boot_mode = "REAL_HDD_BOOT";
    m1.primary_hdd_path = "C:\\kiosk\\hdd.img";
    m1.primary_hdd_chs = "520,16,63";
    m1.last_booted_at = "2026-01-01 12:00:00";
    ASSERT_TRUE(db.InsertMachine(m1));

    CatalogItemRecord item1;
    item1.id = "item-boot-1";
    item1.machine_id = "mach-boot-1";
    item1.item_type = "GAME";
    item1.title = "Heretic";
    item1.host_container_dir = "C:\\games\\heretic";
    item1.guest_working_dir = "\\HERETIC";
    item1.guest_executable = "HERETIC.EXE";
    item1.last_launched_at = "2026-01-02 15:30:00";
    ASSERT_TRUE(db.InsertCatalogItem(item1));

    // 1. SPECIFIC_MACHINE
    KioskConfigRecord cfg;
    ASSERT_TRUE(db.GetConfig(cfg));
    cfg.default_boot_action = "SPECIFIC_MACHINE";
    cfg.default_target_uuid = "mach-boot-1";
    ASSERT_TRUE(db.UpdateConfig(cfg));
    LaunchContext ctx1 = mgr.ResolveDefaultBootContext();
    EXPECT_STREQ("mach-boot-1", ctx1.machine_id.c_str());
    EXPECT_TRUE(ctx1.catalog_item_id.empty());
    EXPECT_TRUE(ctx1.is_real_hdd_boot);

    // 2. SPECIFIC_ITEM
    cfg.default_boot_action = "SPECIFIC_ITEM";
    cfg.default_target_uuid = "item-boot-1";
    ASSERT_TRUE(db.UpdateConfig(cfg));
    LaunchContext ctx2 = mgr.ResolveDefaultBootContext();
    EXPECT_STREQ("mach-boot-1", ctx2.machine_id.c_str());
    EXPECT_STREQ("item-boot-1", ctx2.catalog_item_id.c_str());
    EXPECT_TRUE(ctx2.is_real_hdd_boot);

    // 3. LAST_PLAYED
    cfg.default_boot_action = "LAST_PLAYED";
    cfg.default_target_uuid.clear();
    ASSERT_TRUE(db.UpdateConfig(cfg));
    LaunchContext ctx3 = mgr.ResolveDefaultBootContext();
    EXPECT_STREQ("item-boot-1", ctx3.catalog_item_id.c_str());
    EXPECT_STREQ("mach-boot-1", ctx3.machine_id.c_str());

    // 4. Missing target falls back to DEFAULT_DOSBOX
    cfg.default_boot_action = "SPECIFIC_MACHINE";
    cfg.default_target_uuid = "nonexistent-machine-uuid";
    ASSERT_TRUE(db.UpdateConfig(cfg));
    LaunchContext ctx4 = mgr.ResolveDefaultBootContext();
    EXPECT_TRUE(ctx4.machine_id.empty());
    EXPECT_TRUE(ctx4.catalog_item_id.empty());
    EXPECT_FALSE(ctx4.is_real_hdd_boot);
}

TEST_F(KioskLifecycleTest, SynthesizeLaunchAndTelemetryUpdate) {
    auto& db = KioskDatabase::GetInstance();
    auto& mgr = KioskManager::GetInstance();

    MachineRecord m;
    m.id = "mach-synth-1";
    m.display_name = "386 Test Rig";
    m.archetype_id = KioskArchetypes::DX_386;
    m.archetype_tier = 3;
    m.boot_mode = "INTEGRATED_DOS";
    ASSERT_TRUE(db.InsertMachine(m));

    CatalogItemRecord item;
    item.id = "item-synth-1";
    item.machine_id = "mach-synth-1";
    item.item_type = "GAME";
    item.title = "Wolfenstein 3D";
    item.host_container_dir = "C:\\games\\wolf3d";
    item.guest_working_dir = "\\WOLF3D";
    item.guest_executable = "WOLF3D.EXE";
    item.return_on_exit = true;
    ASSERT_TRUE(db.InsertCatalogItem(item));

    mgr.CancelWatchdog();
    EXPECT_EQ(KioskState::STATE_SHELL_INTERACTIVE, mgr.GetState());

    ASSERT_TRUE(mgr.RequestLaunchItem("item-synth-1"));
    EXPECT_EQ(KioskState::STATE_SYNTHESIZE_LAUNCH, mgr.GetState());

    std::string synthesized_conf;
    ASSERT_TRUE(mgr.SynthesizeAndPrepareLaunch(synthesized_conf));
    EXPECT_EQ(KioskState::STATE_EMULATION_RUNNING, mgr.GetState());
    EXPECT_NE(std::string::npos, synthesized_conf.find("WOLF3D.EXE"));
    EXPECT_NE(std::string::npos, synthesized_conf.find("EXIT"));

    // Verify play_count was incremented
    CatalogItemRecord updated;
    ASSERT_TRUE(db.GetItem("item-synth-1", updated));
    EXPECT_EQ(1, updated.play_count);
}

TEST_F(KioskLifecycleTest, RuntimeOverlayStateTransitions) {
    auto& mgr = KioskManager::GetInstance();

    ASSERT_TRUE(mgr.RequestDefaultDosboxLaunch());
    std::string conf;
    ASSERT_TRUE(mgr.SynthesizeAndPrepareLaunch(conf));
    EXPECT_EQ(KioskState::STATE_EMULATION_RUNNING, mgr.GetState());

    // Enter runtime overlay (F12+D / Guide button)
    mgr.EnterRuntimeOverlay();
    EXPECT_EQ(KioskState::STATE_RUNTIME_OVERLAY, mgr.GetState());

    // Resume emulation
    mgr.ResumeEmulation();
    EXPECT_EQ(KioskState::STATE_EMULATION_RUNNING, mgr.GetState());

    // Re-enter overlay and abort session back to Kiosk Shell
    mgr.EnterRuntimeOverlay();
    EXPECT_EQ(KioskState::STATE_RUNTIME_OVERLAY, mgr.GetState());
    mgr.AbortSessionToShell();
    EXPECT_EQ(KioskState::STATE_SHELL_INTERACTIVE, mgr.GetState());
    EXPECT_EQ(KioskShutdownReason::USER_OVERLAY_ABORT, mgr.GetLastShutdownReason());
    EXPECT_EQ(1, mgr.GetCompletedSessionCount());

    // Subsequent OnGuestShutdown(USER_OVERLAY_ABORT) from the SDL exception catch handler is a no-op
    mgr.OnGuestShutdown(KioskShutdownReason::USER_OVERLAY_ABORT);
    EXPECT_EQ(1, mgr.GetCompletedSessionCount());
}

TEST_F(KioskLifecycleTest, Port8900ShutdownSequenceAndGuestTrap) {
    auto& mgr = KioskManager::GetInstance();
    ASSERT_TRUE(mgr.RequestDefaultDosboxLaunch());
    std::string conf;
    ASSERT_TRUE(mgr.SynthesizeAndPrepareLaunch(conf));
    EXPECT_EQ(KioskState::STATE_EMULATION_RUNNING, mgr.GetState());

    Kiosk_ResetPort8900State();

    // Partial or mismatched write should not throw
    EXPECT_NO_THROW({
        bochs_port_8900_write(0x8900, 'S', 1);
        bochs_port_8900_write(0x8900, 'h', 1);
        bochs_port_8900_write(0x8900, 'X', 1);
    });

    // Complete "Shutdown" sequence on port 0x8900 throws KioskGuestShutdownException
    const char* seq = "Shutdown";
    bool caught_shutdown = false;
    try {
        for (size_t i = 0; seq[i] != '\0'; ++i) {
            bochs_port_8900_write(0x8900, static_cast<Bitu>(seq[i]), 1);
        }
    } catch (const KioskGuestShutdownException& ex) {
        caught_shutdown = true;
        EXPECT_EQ(KioskShutdownReason::PORT_8900_SHUTDOWN, ex.reason);
        mgr.OnGuestShutdown(ex.reason);
    }

    EXPECT_TRUE(caught_shutdown);
    EXPECT_EQ(KioskState::STATE_SHELL_INTERACTIVE, mgr.GetState());
    EXPECT_EQ(KioskShutdownReason::PORT_8900_SHUTDOWN, mgr.GetLastShutdownReason());
    EXPECT_EQ(1, mgr.GetCompletedSessionCount());
}

TEST_F(KioskDatabaseFixture, SchemaCheckConstraintsRejectInvalidEnums) {
    auto& db = KioskDatabase::GetInstance();

    // 1. Invalid machine archetype_id should fail CHECK constraint
    MachineRecord bad_mach;
    bad_mach.id = "mach-bad-arch";
    bad_mach.display_name = "Bad Archetype Machine";
    bad_mach.archetype_id = "INVALID_ARCHETYPE_XYZ";
    bad_mach.archetype_tier = 3;
    EXPECT_FALSE(db.InsertMachine(bad_mach));

    // Insert a valid machine for FK tests
    MachineRecord valid_mach;
    valid_mach.id = "mach-valid-arch";
    valid_mach.display_name = "Valid 486";
    valid_mach.archetype_id = KioskArchetypes::DX2_486;
    valid_mach.archetype_tier = 4;
    ASSERT_TRUE(db.InsertMachine(valid_mach));

    // 2. Invalid catalog_items.launch_mechanism should fail CHECK constraint
    CatalogItemRecord bad_item;
    bad_item.id = "item-bad-launch";
    bad_item.machine_id = "mach-valid-arch";
    bad_item.item_type = "GAME";
    bad_item.title = "Bad Launch Game";
    bad_item.guest_working_dir = "\\GAME";
    bad_item.guest_executable = "GAME.EXE";
    bad_item.launch_mechanism = "INVALID_MECHANISM";
    EXPECT_FALSE(db.InsertCatalogItem(bad_item));

    // 3. Invalid media_pool.media_type / physical_style / label_font_style should fail CHECK constraint
    MediaPoolRecord bad_media = {};
    bad_media.id = "media-bad-type";
    bad_media.file_path = "/storage/bad.img";
    bad_media.media_type = "LASERDISC_12INCH";
    bad_media.physical_style = "BEIGE_35";
    bad_media.label_font_style = "DOT_MATRIX";
    bad_media.label_title = "Bad Media";
    EXPECT_FALSE(db.InsertMedia(bad_media));

    bad_media.media_type = "FLOPPY_35_1440K";
    bad_media.physical_style = "NEON_PINK";
    EXPECT_FALSE(db.InsertMedia(bad_media));

    bad_media.physical_style = "BEIGE_35";
    bad_media.label_font_style = "COMIC_SANS";
    EXPECT_FALSE(db.InsertMedia(bad_media));
}

TEST(KioskSkipCountdownTest, SkipCountdownTransitionsDirectlyToShellInteractive) {
    auto& mgr = KioskManager::GetInstance();
    mgr.SetKioskModeActive(true);
    mgr.SetSkipCountdown(true);
    ASSERT_TRUE(mgr.Initialize(":memory:", ""));

    EXPECT_EQ(KioskState::STATE_SHELL_INTERACTIVE, mgr.GetState());
    EXPECT_FLOAT_EQ(0.0f, mgr.GetWatchdogRemainingSec());

    mgr.Shutdown();
}

// ----------------------------------------------------------------------------
// Phase 3: Ephemeral Boot Injector & Guest Return Mechanism Tests
// ----------------------------------------------------------------------------

TEST(KioskBootInjectorTest, KioskExtComBinaryLayoutAndPort8900Execution) {
    EXPECT_EQ(57U, KIOSKEXT_COM_SIZE);

    // 1. Stage 1: DOS Disk Reset (Flush File Buffers): MOV AH, 0Dh; INT 21h
    EXPECT_EQ(0xB4, KIOSKEXT_COM_BYTES[0]);
    EXPECT_EQ(0x0D, KIOSKEXT_COM_BYTES[1]);
    EXPECT_EQ(0xCD, KIOSKEXT_COM_BYTES[2]);
    EXPECT_EQ(0x21, KIOSKEXT_COM_BYTES[3]);

    // 2. Stage 2: APM Real-Mode Connect (0x0104) & JC +0x15 (0x010B -> 0x0122 / index 34)
    EXPECT_EQ(0xB8, KIOSKEXT_COM_BYTES[4]);
    EXPECT_EQ(0x01, KIOSKEXT_COM_BYTES[5]);
    EXPECT_EQ(0x53, KIOSKEXT_COM_BYTES[6]);
    EXPECT_EQ(0x72, KIOSKEXT_COM_BYTES[11]);
    EXPECT_EQ(0x15, KIOSKEXT_COM_BYTES[12]); // Next IP 0x010D + 0x15 = 0x0122 (index 34)

    // 3. Stage 3: Fallback at offset 0x22 (index 34): MOV DX, 8900h; MOV SI, 0131h ("Shutdown"); MOV CX, 8
    EXPECT_EQ(0xBA, KIOSKEXT_COM_BYTES[34]);
    EXPECT_EQ(0x00, KIOSKEXT_COM_BYTES[35]);
    EXPECT_EQ(0x89, KIOSKEXT_COM_BYTES[36]);
    EXPECT_EQ(0xBE, KIOSKEXT_COM_BYTES[37]);
    EXPECT_EQ(0x31, KIOSKEXT_COM_BYTES[38]);
    EXPECT_EQ(0x01, KIOSKEXT_COM_BYTES[39]);
    EXPECT_EQ(0xB9, KIOSKEXT_COM_BYTES[40]);
    EXPECT_EQ(0x08, KIOSKEXT_COM_BYTES[41]);
    EXPECT_EQ(0x00, KIOSKEXT_COM_BYTES[42]);

    // 4. RET at offset 48 (0x0130) and "Shutdown" (8 bytes) at offset 49..56 (0x0131..0x0138)
    EXPECT_EQ(0xC3, KIOSKEXT_COM_BYTES[48]);
    EXPECT_EQ(0, memcmp(&KIOSKEXT_COM_BYTES[49], "Shutdown", 8));

    // 4. Simulate the fallback OUT DX, AL loop feeding bytes from offset 49..56 to port 0x8900
    auto& mgr = KioskManager::GetInstance();
    mgr.SetKioskModeActive(true);
    ASSERT_TRUE(mgr.Initialize(":memory:", ""));
    ASSERT_TRUE(mgr.RequestDefaultDosboxLaunch());
    std::string conf;
    ASSERT_TRUE(mgr.SynthesizeAndPrepareLaunch(conf));

    Kiosk_ResetPort8900State();
    bool trapped = false;
    try {
        for (size_t i = 0; i < 8; ++i) {
            bochs_port_8900_write(0x8900, KIOSKEXT_COM_BYTES[49 + i], 1);
        }
    } catch (const KioskGuestShutdownException& ex) {
        trapped = true;
        EXPECT_EQ(KioskShutdownReason::PORT_8900_SHUTDOWN, ex.reason);
    }
    EXPECT_TRUE(trapped);
    mgr.Shutdown();
}

TEST(KioskBootInjectorTest, OneTimeAutoexecHookIdempotency) {
    // 1. In-memory string helper idempotency
    std::string original = "@ECHO OFF\r\nPATH C:\\DOS;C:\\UTILS\r\n";
    std::string first_pass = BootInjector::InjectAutoexecHookToString(original);
    EXPECT_NE(std::string::npos, first_pass.find("IF EXIST B:\\KIOSKGO.BAT CALL B:\\KIOSKGO.BAT\r\n"));

    std::string second_pass = BootInjector::InjectAutoexecHookToString(first_pass);
    EXPECT_EQ(first_pass, second_pass);

    // 2. Raw FAT12 disk image inspection & mutation via EnsureOneTimeAutoexecHook
    std::vector<DispatchFileEntry> seed_files;
    DispatchFileEntry autoexec_entry;
    autoexec_entry.filename_8_3 = "AUTOEXEC.BAT";
    const std::string seed_autoexec = "@ECHO OFF\r\nPROMPT $P$G\r\n";
    autoexec_entry.data.assign(seed_autoexec.begin(), seed_autoexec.end());
    seed_files.push_back(autoexec_entry);

    std::vector<uint8_t> disk_img;
    ASSERT_TRUE(BootInjector::GetInstance().BuildFat12ImageInMemory(seed_files, disk_img));

    const std::string temp_hdd_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".autoexec_test.img";
    {
        std::ofstream ofs(temp_hdd_path.c_str(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(ofs.is_open());
        ofs.write(reinterpret_cast<const char*>(disk_img.data()), static_cast<std::streamsize>(disk_img.size()));
    }

    // First call injects the hook into AUTOEXEC.BAT inside the raw FAT12 image
    EXPECT_TRUE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(temp_hdd_path));

    // Read image back and verify AUTOEXEC.BAT contains the hook
    std::vector<uint8_t> modified_img;
    {
        std::ifstream ifs(temp_hdd_path.c_str(), std::ios::binary);
        ASSERT_TRUE(ifs.is_open());
        modified_img.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    std::vector<uint8_t> autoexec_after_first;
    ASSERT_TRUE(BootInjector::GetInstance().ReadFileFromFat12Image(modified_img, "AUTOEXEC.BAT", autoexec_after_first));
    std::string content_first(autoexec_after_first.begin(), autoexec_after_first.end());
    EXPECT_NE(std::string::npos, content_first.find("IF EXIST B:\\KIOSKGO.BAT CALL B:\\KIOSKGO.BAT\r\n"));

    // Second call is a no-op and produces identical AUTOEXEC.BAT content
    EXPECT_TRUE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(temp_hdd_path));
    {
        std::ifstream ifs(temp_hdd_path.c_str(), std::ios::binary);
        ASSERT_TRUE(ifs.is_open());
        modified_img.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    std::vector<uint8_t> autoexec_after_second;
    ASSERT_TRUE(BootInjector::GetInstance().ReadFileFromFat12Image(modified_img, "AUTOEXEC.BAT", autoexec_after_second));
    std::string content_second(autoexec_after_second.begin(), autoexec_after_second.end());
    EXPECT_EQ(content_first, content_second);

    std::remove(temp_hdd_path.c_str());
}

TEST(KioskBootInjectorTest, SynthesizeKioskGoBatForDosAndWin9x) {
    MachineRecord m;
    m.id = "mach-go";
    m.archetype_id = KioskArchetypes::DX2_486;
    m.archetype_tier = 4;
    m.boot_mode = "REAL_HDD_BOOT";

    // 1. Standard DOS game on C: with return_on_exit = true
    CatalogItemRecord dos_game;
    dos_game.guest_working_dir = "\\DOOM2";
    dos_game.guest_executable = "DOOM2.EXE";
    dos_game.guest_arguments = "-skill 4";
    dos_game.launch_mechanism = "DOS_AUTOEXEC_INJECT";
    dos_game.return_on_exit = true;

    std::string bat_dos = BootInjector::GetInstance().SynthesizeKioskGoBat(m, dos_game);
    EXPECT_NE(std::string::npos, bat_dos.find("C:\r\n"));
    EXPECT_NE(std::string::npos, bat_dos.find("CD \\DOOM2\r\n"));
    EXPECT_NE(std::string::npos, bat_dos.find("DOOM2.EXE -skill 4\r\n"));
    EXPECT_NE(std::string::npos, bat_dos.find("B:\\KIOSKEXT.COM\r\n"));

    // 2. Game located on CD-ROM drive D: with return_on_exit = false
    CatalogItemRecord cd_game;
    cd_game.guest_working_dir = "D:\\MYST";
    cd_game.guest_executable = "MYST.EXE";
    cd_game.launch_mechanism = "DOS_AUTOEXEC_INJECT";
    cd_game.return_on_exit = false;

    std::string bat_cd = BootInjector::GetInstance().SynthesizeKioskGoBat(m, cd_game);
    EXPECT_NE(std::string::npos, bat_cd.find("D:\r\n"));
    EXPECT_NE(std::string::npos, bat_cd.find("CD \\MYST\r\n"));
    EXPECT_EQ(std::string::npos, bat_cd.find("KIOSKEXT.COM"));

    // 3. Windows 3.x / Windows 9x hook uses C:\WINDOWS\WIN.COM without mutating SYSTEM.INI
    CatalogItemRecord win_app;
    win_app.guest_working_dir = "C:\\WINAPPS\\OFFICE";
    win_app.guest_executable = "WINWORD.EXE";
    win_app.guest_arguments = "README.DOC";
    win_app.launch_mechanism = "WIN3X_PROGMAN_HOOK";
    win_app.return_on_exit = true;

    std::string bat_win = BootInjector::GetInstance().SynthesizeKioskGoBat(m, win_app);
    EXPECT_NE(std::string::npos, bat_win.find("C:\r\n"));
    EXPECT_NE(std::string::npos, bat_win.find("CD \\WINAPPS\\OFFICE\r\n"));
    EXPECT_NE(std::string::npos, bat_win.find("C:\\WINDOWS\\WIN.COM WINWORD.EXE README.DOC\r\n"));
    EXPECT_NE(std::string::npos, bat_win.find("B:\\KIOSKEXT.COM\r\n"));
}

TEST(KioskBootInjectorTest, Fat12DispatchImageSynthesisAndFileReadback) {
    MachineRecord m;
    m.id = "mach-fat12";
    m.archetype_id = KioskArchetypes::DX2_486;
    m.archetype_tier = 4;
    m.boot_mode = "REAL_HDD_BOOT";

    CatalogItemRecord item;
    item.id = "item-fat12";
    item.machine_id = "mach-fat12";
    item.title = "Descent";
    item.guest_working_dir = "\\DESCENT";
    item.guest_executable = "DESCENT.EXE";
    item.launch_mechanism = "DOS_AUTOEXEC_INJECT";
    item.return_on_exit = true;

    const std::string img_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fat12_test.img";
    ASSERT_TRUE(BootInjector::GetInstance().BuildDispatchVolume(img_path, m, item));

    std::vector<uint8_t> image_bytes;
    {
        std::ifstream ifs(img_path.c_str(), std::ios::binary);
        ASSERT_TRUE(ifs.is_open());
        image_bytes.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    // Verify 1.44 MB floppy size (2880 sectors * 512 bytes = 1,474,560 bytes) and boot signature 0x55 0xAA
    ASSERT_EQ(1474560U, image_bytes.size());
    EXPECT_EQ(0x55, image_bytes[510]);
    EXPECT_EQ(0xAA, image_bytes[511]);

    // Verify KIOSKEXT.COM readback matches KIOSKEXT_COM_BYTES byte-for-byte
    std::vector<uint8_t> ext_bytes;
    ASSERT_TRUE(BootInjector::GetInstance().ReadFileFromFat12Image(image_bytes, "KIOSKEXT.COM", ext_bytes));
    ASSERT_EQ(KIOSKEXT_COM_SIZE, ext_bytes.size());
    EXPECT_EQ(0, memcmp(ext_bytes.data(), KIOSKEXT_COM_BYTES, KIOSKEXT_COM_SIZE));

    // Verify KIOSKGO.BAT readback contains synthesized launch commands
    std::vector<uint8_t> bat_bytes;
    ASSERT_TRUE(BootInjector::GetInstance().ReadFileFromFat12Image(image_bytes, "KIOSKGO.BAT", bat_bytes));
    std::string bat_str(bat_bytes.begin(), bat_bytes.end());
    EXPECT_NE(std::string::npos, bat_str.find("CD \\DESCENT\r\n"));
    EXPECT_NE(std::string::npos, bat_str.find("DESCENT.EXE\r\n"));
    EXPECT_NE(std::string::npos, bat_str.find("B:\\KIOSKEXT.COM\r\n"));

    BootInjector::GetInstance().CleanupDispatchVolume(img_path);
}

TEST(KioskBootInjectorTest, RealHddCarouselLaunchMountsDispatchOnFloppyBAndPreservesCdRomD) {
    auto& db = KioskDatabase::GetInstance();
    auto& mgr = KioskManager::GetInstance();
    mgr.SetKioskModeActive(true);
    ASSERT_TRUE(mgr.Initialize(":memory:", ""));

    // Create a minimal valid FAT12 HDD image so EnsureOneTimeAutoexecHook succeeds
    std::vector<DispatchFileEntry> hdd_files;
    DispatchFileEntry autoexec_file;
    autoexec_file.filename_8_3 = "AUTOEXEC.BAT";
    const std::string autoexec_init = "@ECHO OFF\r\n";
    autoexec_file.data.assign(autoexec_init.begin(), autoexec_init.end());
    hdd_files.push_back(autoexec_file);

    std::vector<uint8_t> hdd_img;
    ASSERT_TRUE(BootInjector::GetInstance().BuildFat12ImageInMemory(hdd_files, hdd_img));
    const std::string temp_hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".mode_a_hdd.img";
    {
        std::ofstream ofs(temp_hdd.c_str(), std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(hdd_img.data()), static_cast<std::streamsize>(hdd_img.size()));
    }

    MachineRecord m;
    m.id = "mach-mode-a";
    m.display_name = "486 CD-ROM Rig";
    m.archetype_id = KioskArchetypes::DX2_486;
    m.archetype_tier = 4;
    m.boot_mode = "REAL_HDD_BOOT";
    m.primary_hdd_path = temp_hdd;
    m.primary_hdd_chs = "520,16,63";
    ASSERT_TRUE(db.InsertMachine(m));

    MediaPoolRecord iso;
    iso.id = "media-mode-a-iso";
    iso.file_path = "C:\\kiosk\\game_cd.iso";
    iso.media_type = "CDROM_ISO";
    iso.physical_style = "JEWEL_CD";
    iso.label_font_style = "PRINTED_OEM";
    iso.label_title = "Game CD";
    ASSERT_TRUE(db.InsertMedia(iso));

    CaddyBindingRecord binding;
    binding.machine_id = "mach-mode-a";
    binding.media_id = "media-mode-a-iso";
    binding.target_drive = "D";
    binding.slot_order = 1;
    ASSERT_TRUE(db.AddCaddyBinding(binding));

    CatalogItemRecord item;
    item.id = "item-mode-a";
    item.machine_id = "mach-mode-a";
    item.item_type = "GAME";
    item.title = "Command & Conquer";
    item.guest_working_dir = "\\C&C";
    item.guest_executable = "C&C.EXE";
    item.launch_mechanism = "DOS_AUTOEXEC_INJECT";
    item.return_on_exit = true;
    ASSERT_TRUE(db.InsertCatalogItem(item));

    ASSERT_TRUE(mgr.RequestLaunchItem("item-mode-a"));
    std::string conf;
    ASSERT_TRUE(mgr.SynthesizeAndPrepareLaunch(conf));

    // Verify Floppy B (drive index 1) mounts kiosk_dispatch.img and CD-ROM D mounts game_cd.iso before BOOT -l c
    size_t pos_b = conf.find("IMGMOUNT 1 \"");
    size_t pos_d = conf.find("IMGMOUNT D \"C:\\kiosk\\game_cd.iso\" -t iso -fs iso -ide 2m");
    size_t pos_boot = conf.find("BOOT -l c");
    EXPECT_NE(std::string::npos, pos_b);
    EXPECT_NE(std::string::npos, conf.find("kiosk_dispatch.img\" -t floppy -fs none"));
    EXPECT_NE(std::string::npos, pos_d);
    EXPECT_NE(std::string::npos, pos_boot);
    EXPECT_LT(pos_b, pos_boot);
    EXPECT_LT(pos_d, pos_boot);

    mgr.Shutdown();
    std::remove(temp_hdd.c_str());
}

TEST(KioskBootInjectorTest, RealHddMachineOnlyBootMountsIdleDispatchVolumeWithoutKioskGoBat) {
    auto& db = KioskDatabase::GetInstance();
    auto& mgr = KioskManager::GetInstance();
    mgr.SetKioskModeActive(true);
    ASSERT_TRUE(mgr.Initialize(":memory:", ""));

    // Create a valid temporary FAT12 HDD image for Machine-Only boot
    std::vector<DispatchFileEntry> hdd_files;
    std::vector<uint8_t> hdd_img;
    ASSERT_TRUE(BootInjector::GetInstance().BuildFat12ImageInMemory(hdd_files, hdd_img));
    const std::string temp_hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".mode_b_hdd.img";
    {
        std::ofstream ofs(temp_hdd.c_str(), std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(hdd_img.data()), static_cast<std::streamsize>(hdd_img.size()));
    }

    MachineRecord m;
    m.id = "mach-mode-b";
    m.display_name = "Pure DOS 6.22 Workstation";
    m.archetype_id = KioskArchetypes::DX_386;
    m.archetype_tier = 3;
    m.boot_mode = "REAL_HDD_BOOT";
    m.primary_hdd_path = temp_hdd;
    m.primary_hdd_chs = "520,16,63";
    ASSERT_TRUE(db.InsertMachine(m));

    ASSERT_TRUE(mgr.RequestLaunchMachine("mach-mode-b"));
    std::string conf;
    ASSERT_TRUE(mgr.SynthesizeAndPrepareLaunch(conf));

    // Verify Floppy B mounts kiosk_dispatch.img
    EXPECT_NE(std::string::npos, conf.find("IMGMOUNT 1 \""));
    EXPECT_NE(std::string::npos, conf.find("kiosk_dispatch.img\" -t floppy -fs none"));

    const std::string dispatch_path = mgr.GetActiveDispatchImagePath();
    ASSERT_FALSE(dispatch_path.empty());

    std::vector<uint8_t> image_bytes;
    {
        std::ifstream ifs(dispatch_path.c_str(), std::ios::binary);
        ASSERT_TRUE(ifs.is_open());
        image_bytes.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    // Idle dispatch volume contains KIOSKEXT.COM (for manual shutdown) but NO KIOSKGO.BAT
    std::vector<uint8_t> ext_bytes;
    EXPECT_TRUE(BootInjector::GetInstance().ReadFileFromFat12Image(image_bytes, "KIOSKEXT.COM", ext_bytes));
    EXPECT_EQ(KIOSKEXT_COM_SIZE, ext_bytes.size());

    std::vector<uint8_t> bat_bytes;
    EXPECT_FALSE(BootInjector::GetInstance().ReadFileFromFat12Image(image_bytes, "KIOSKGO.BAT", bat_bytes));

    // Verify missing primary_hdd_path fails SynthesizeAndPrepareLaunch and recovers to STATE_SHELL_INTERACTIVE
    MachineRecord m_missing;
    m_missing.id = "mach-missing-hdd";
    m_missing.display_name = "Missing HDD Rig";
    m_missing.archetype_id = KioskArchetypes::DX_386;
    m_missing.archetype_tier = 3;
    m_missing.boot_mode = "REAL_HDD_BOOT";
    m_missing.primary_hdd_path = "C:\\kiosk\\nonexistent_hdd_image_12345.img";
    ASSERT_TRUE(db.InsertMachine(m_missing));

    ASSERT_TRUE(mgr.RequestLaunchMachine("mach-missing-hdd"));
    std::string conf_failed;
    EXPECT_FALSE(mgr.SynthesizeAndPrepareLaunch(conf_failed));
    EXPECT_EQ(KioskState::STATE_SHELL_INTERACTIVE, mgr.GetState());

    mgr.Shutdown();
    std::remove(temp_hdd.c_str());
}

TEST(KioskBootInjectorTest, RealHddCarouselLaunchReservesDriveBForDispatchAndQueuesDriveBCaddyOnDriveA) {
    auto& db = KioskDatabase::GetInstance();
    auto& mgr = KioskManager::GetInstance();
    mgr.SetKioskModeActive(true);
    ASSERT_TRUE(mgr.Initialize(":memory:", ""));

    std::vector<DispatchFileEntry> hdd_files;
    std::vector<uint8_t> hdd_img;
    ASSERT_TRUE(BootInjector::GetInstance().BuildFat12ImageInMemory(hdd_files, hdd_img));
    const std::string temp_hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".conflict_hdd.img";
    {
        std::ofstream ofs(temp_hdd.c_str(), std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(hdd_img.data()), static_cast<std::streamsize>(hdd_img.size()));
    }

    MachineRecord m;
    m.id = "mach-b-conflict";
    m.display_name = "486 Dual Floppy Rig";
    m.archetype_id = KioskArchetypes::DX2_486;
    m.archetype_tier = 4;
    m.boot_mode = "REAL_HDD_BOOT";
    m.primary_hdd_path = temp_hdd;
    m.primary_hdd_chs = "520,16,63";
    ASSERT_TRUE(db.InsertMachine(m));

    MediaPoolRecord flop_a;
    flop_a.id = "media-flop-a";
    flop_a.file_path = "C:\\kiosk\\disk1.img";
    flop_a.media_type = "FLOPPY_35_1440K";
    flop_a.physical_style = "BEIGE_35";
    flop_a.label_font_style = "PRINTED_OEM";
    flop_a.label_title = "Disk 1";
    ASSERT_TRUE(db.InsertMedia(flop_a));

    MediaPoolRecord flop_b;
    flop_b.id = "media-flop-b";
    flop_b.file_path = "C:\\kiosk\\disk2_on_b.img";
    flop_b.media_type = "FLOPPY_35_1440K";
    flop_b.physical_style = "BLUE_35";
    flop_b.label_font_style = "PRINTED_OEM";
    flop_b.label_title = "Disk 2";
    ASSERT_TRUE(db.InsertMedia(flop_b));

    CatalogItemRecord item;
    item.id = "item-b-conflict";
    item.machine_id = "mach-b-conflict";
    item.item_type = "GAME";
    item.title = "Ultima VII";
    item.guest_working_dir = "\\ULTIMA7";
    item.guest_executable = "ULTIMA7.COM";
    item.launch_mechanism = "DOS_AUTOEXEC_INJECT";
    item.return_on_exit = true;
    ASSERT_TRUE(db.InsertCatalogItem(item));

    CaddyBindingRecord bind_a;
    bind_a.catalog_item_id = "item-b-conflict";
    bind_a.media_id = "media-flop-a";
    bind_a.target_drive = "A";
    bind_a.slot_order = 1;
    ASSERT_TRUE(db.AddCaddyBinding(bind_a));

    CaddyBindingRecord bind_b;
    bind_b.catalog_item_id = "item-b-conflict";
    bind_b.media_id = "media-flop-b";
    bind_b.target_drive = "B";
    bind_b.slot_order = 1;
    ASSERT_TRUE(db.AddCaddyBinding(bind_b));

    // Mode A (Carousel Item Launch): B: must mount kiosk_dispatch.img, while B: caddy floppy is queued on A: (IMGMOUNT 0)
    ASSERT_TRUE(mgr.RequestLaunchItem("item-b-conflict"));
    std::string conf_item;
    ASSERT_TRUE(mgr.SynthesizeAndPrepareLaunch(conf_item));

    EXPECT_NE(std::string::npos, conf_item.find("IMGMOUNT 0 \"C:\\kiosk\\disk1.img\" \"C:\\kiosk\\disk2_on_b.img\" -t floppy -fs none"));
    EXPECT_NE(std::string::npos, conf_item.find("IMGMOUNT 1 \""));
    EXPECT_NE(std::string::npos, conf_item.find("kiosk_dispatch.img\" -t floppy -fs none"));
    EXPECT_EQ(std::string::npos, conf_item.find("IMGMOUNT 1 \"C:\\kiosk\\disk2_on_b.img\""));

    // Mode B (Machine-Only Boot): An explicit machine B: caddy binding is honored on IMGMOUNT 1
    CaddyBindingRecord mach_bind_b;
    mach_bind_b.machine_id = "mach-b-conflict";
    mach_bind_b.media_id = "media-flop-b";
    mach_bind_b.target_drive = "B";
    mach_bind_b.slot_order = 1;
    ASSERT_TRUE(db.AddCaddyBinding(mach_bind_b));

    ASSERT_TRUE(mgr.RequestLaunchMachine("mach-b-conflict"));
    std::string conf_mach;
    ASSERT_TRUE(mgr.SynthesizeAndPrepareLaunch(conf_mach));

    EXPECT_NE(std::string::npos, conf_mach.find("IMGMOUNT 1 \"C:\\kiosk\\disk2_on_b.img\" -t floppy -fs none"));

    mgr.Shutdown();
    std::remove(temp_hdd.c_str());
}

TEST(KioskBootInjectorTest, HostAutoexecPreservesExistingCommandsAndRejectsCorruptImage) {
    // 1. Verify plain-text host AUTOEXEC.BAT preserves all existing lines and appends hook
    const std::string host_autoexec_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".host_autoexec.bat";
    const std::string initial_commands = "@ECHO OFF\r\nSET BLASTER=A220 I7 D1 H5 T6\r\nLH C:\\DOS\\SMARTDRV.EXE\r\n";
    {
        std::ofstream out(host_autoexec_path.c_str(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out.write(initial_commands.data(), static_cast<std::streamsize>(initial_commands.size()));
    }

    EXPECT_TRUE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(host_autoexec_path));

    std::string result_content;
    {
        std::ifstream in(host_autoexec_path.c_str(), std::ios::binary);
        ASSERT_TRUE(in.is_open());
        result_content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(0U, result_content.find(initial_commands));
    EXPECT_NE(std::string::npos, result_content.find("IF EXIST B:\\KIOSKGO.BAT CALL B:\\KIOSKGO.BAT\r\n"));
    EXPECT_EQ(std::string::npos, result_content.find('\0'));
    std::remove(host_autoexec_path.c_str());

    // 2. Verify hook is inserted BEFORE a trailing WIN / C:\WINDOWS\WIN.COM startup line
    const std::string win_autoexec = "@ECHO OFF\r\nPATH C:\\WINDOWS;C:\\DOS\r\nC:\\WINDOWS\\WIN.COM\r\n";
    const std::string injected_win = BootInjector::InjectAutoexecHookToString(win_autoexec);
    size_t hook_pos = injected_win.find("IF EXIST B:\\KIOSKGO.BAT CALL B:\\KIOSKGO.BAT");
    size_t win_pos = injected_win.find("C:\\WINDOWS\\WIN.COM");
    ASSERT_NE(std::string::npos, hook_pos);
    ASSERT_NE(std::string::npos, win_pos);
    EXPECT_LT(hook_pos, win_pos);

    // 3. Verify a small (< 64 KB) or large (>= 64 KB) .img file without a 0x55AA signature is rejected untouched
    const std::string bad_img_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".bad_raw.img";
    std::vector<char> raw_pattern(4096, 'X');
    {
        std::ofstream out(bad_img_path.c_str(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out.write(raw_pattern.data(), static_cast<std::streamsize>(raw_pattern.size()));
    }

    EXPECT_FALSE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(bad_img_path));

    std::vector<char> after_bytes;
    {
        std::ifstream in(bad_img_path.c_str(), std::ios::binary);
        ASSERT_TRUE(in.is_open());
        after_bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(raw_pattern, after_bytes);
    std::remove(bad_img_path.c_str());
}

TEST(KioskBootInjectorTest, MbrPartitionedFat16AndFat32AutoexecHookCreationAndClusterGrowth) {
    auto write_le16 = [](uint8_t* p, uint16_t val) {
        p[0] = static_cast<uint8_t>(val & 0xFFU);
        p[1] = static_cast<uint8_t>((val >> 8) & 0xFFU);
    };
    auto write_le32 = [](uint8_t* p, uint32_t val) {
        p[0] = static_cast<uint8_t>(val & 0xFFU);
        p[1] = static_cast<uint8_t>((val >> 8) & 0xFFU);
        p[2] = static_cast<uint8_t>((val >> 16) & 0xFFU);
        p[3] = static_cast<uint8_t>((val >> 24) & 0xFFU);
    };
    auto read_le16 = [](const uint8_t* p) -> uint16_t {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    };
    auto read_le32 = [](const uint8_t* p) -> uint32_t {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    };

    // ------------------------------------------------------------------------
    // Part 1: MBR-Partitioned FAT16 Image with Cluster Boundary Growth
    // ------------------------------------------------------------------------
    // Layout:
    // - LBA 0: MBR with active partition (status 0x80, type 0x06) at LBA 63, count 4200
    // - Partition VBR at LBA 63:
    //   BytsPerSec=512, SecPerClus=1 (512B cluster), RsvdSecCnt=1, NumFATs=2,
    //   RootEntCnt=32 (2 sectors), FATSz16=17, TotSec16=4200 -> 4163 clusters (>= 4085 -> FAT16)
    // - Existing AUTOEXEC.BAT has 480 bytes in Cluster 2 (so adding the 97-byte hook forces a 2nd cluster allocation)
    const uint32_t fat16_lba_start = 63;
    const uint32_t fat16_part_sectors = 4200;
    const uint32_t fat16_total_sectors = fat16_lba_start + fat16_part_sectors;
    std::vector<uint8_t> fat16_img(static_cast<size_t>(fat16_total_sectors) * 512U, 0);

    // MBR at LBA 0
    uint8_t* mbr16 = fat16_img.data();
    mbr16[0x1BE + 0] = 0x80; // Active bootable
    mbr16[0x1BE + 4] = 0x06; // FAT16B
    write_le32(mbr16 + 0x1BE + 8, fat16_lba_start);
    write_le32(mbr16 + 0x1BE + 12, fat16_part_sectors);
    mbr16[510] = 0x55;
    mbr16[511] = 0xAA;

    // FAT16 VBR at LBA 63
    const size_t part16_off = static_cast<size_t>(fat16_lba_start) * 512U;
    uint8_t* vbr16 = fat16_img.data() + part16_off;
    vbr16[0] = 0xEB;
    vbr16[1] = 0x3C;
    vbr16[2] = 0x90;
    std::memcpy(vbr16 + 3, "MSDOS5.0", 8);
    write_le16(vbr16 + 11, 512);  // BytsPerSec
    vbr16[13] = 1;                // SecPerClus (512 bytes)
    write_le16(vbr16 + 14, 1);    // RsvdSecCnt
    vbr16[16] = 2;                // NumFATs
    write_le16(vbr16 + 17, 32);   // RootEntCnt (2 sectors)
    write_le16(vbr16 + 19, static_cast<uint16_t>(fat16_part_sectors)); // TotSec16
    vbr16[21] = 0xF8;             // Fixed disk media
    write_le16(vbr16 + 22, 17);   // FATSz16
    vbr16[510] = 0x55;
    vbr16[511] = 0xAA;

    const size_t fat16_1_off = part16_off + 1U * 512U;
    const size_t fat16_2_off = part16_off + (1U + 17U) * 512U;
    const size_t root16_off = part16_off + (1U + 34U) * 512U;
    const size_t data16_off = part16_off + (1U + 34U + 2U) * 512U; // first_data_sec = 37

    // Initialize FAT1 and FAT2 media headers and Cluster 2 EOF (0xFFFF)
    write_le16(fat16_img.data() + fat16_1_off + 0, 0xFFF8U);
    write_le16(fat16_img.data() + fat16_1_off + 2, 0xFFFFU);
    write_le16(fat16_img.data() + fat16_1_off + 4, 0xFFFFU); // Cluster 2 = EOF
    std::memcpy(fat16_img.data() + fat16_2_off, fat16_img.data() + fat16_1_off, 17U * 512U);

    // Seed a 480-byte AUTOEXEC.BAT in Cluster 2 (root entry 0)
    std::string seed_fat16_autoexec = "@ECHO OFF\r\n";
    while (seed_fat16_autoexec.size() < 470U) {
        seed_fat16_autoexec += "REM PADDING LINE FOR CLUSTER BOUNDARY EXPANSION\r\n";
    }
    seed_fat16_autoexec.resize(478U, 'X');
    seed_fat16_autoexec += "\r\n"; // 480 bytes total

    uint8_t* dent16 = fat16_img.data() + root16_off;
    std::memcpy(dent16, "AUTOEXECBAT", 11);
    dent16[11] = 0x20;
    write_le16(dent16 + 26, 2); // FirstCluster = 2
    write_le32(dent16 + 28, static_cast<uint32_t>(seed_fat16_autoexec.size()));
    std::memcpy(fat16_img.data() + data16_off, seed_fat16_autoexec.data(), seed_fat16_autoexec.size());

    const std::string temp_fat16_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".mbr_fat16_test.img";
    {
        std::ofstream out(temp_fat16_path.c_str(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out.write(reinterpret_cast<const char*>(fat16_img.data()), static_cast<std::streamsize>(fat16_img.size()));
    }

    ASSERT_TRUE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(temp_fat16_path));

    // Read modified FAT16 image back and verify 2-cluster chain (Cluster 2 -> Cluster 3 -> 0xFFFF) in both FATs
    std::vector<uint8_t> mod_fat16;
    {
        std::ifstream in(temp_fat16_path.c_str(), std::ios::binary);
        ASSERT_TRUE(in.is_open());
        mod_fat16.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(3U, read_le16(mod_fat16.data() + fat16_1_off + 2 * 2));
    EXPECT_EQ(0xFFFFU, read_le16(mod_fat16.data() + fat16_1_off + 3 * 2));
    EXPECT_EQ(3U, read_le16(mod_fat16.data() + fat16_2_off + 2 * 2));
    EXPECT_EQ(0xFFFFU, read_le16(mod_fat16.data() + fat16_2_off + 3 * 2));

    uint32_t new_fat16_size = read_le32(mod_fat16.data() + root16_off + 28);
    EXPECT_GT(new_fat16_size, 512U);
    ASSERT_LE(new_fat16_size, 1024U);

    // Reconstruct file data across Cluster 2 and Cluster 3
    std::string reconstructed16;
    reconstructed16.append(reinterpret_cast<const char*>(mod_fat16.data() + data16_off), 512U);
    reconstructed16.append(reinterpret_cast<const char*>(mod_fat16.data() + data16_off + 512U), new_fat16_size - 512U);
    EXPECT_EQ(0U, reconstructed16.find(seed_fat16_autoexec));
    EXPECT_NE(std::string::npos, reconstructed16.find("IF EXIST B:\\KIOSKGO.BAT CALL B:\\KIOSKGO.BAT\r\n"));

    // Verify idempotency on second call
    EXPECT_TRUE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(temp_fat16_path));
    std::remove(temp_fat16_path.c_str());

    // ------------------------------------------------------------------------
    // Part 2: MBR-Partitioned FAT32 Image with Missing AUTOEXEC.BAT Creation
    // ------------------------------------------------------------------------
    // Layout:
    // - LBA 0: MBR with active partition (status 0x80, type 0x0C) at LBA 63, count 200
    // - Partition VBR at LBA 63:
    //   BytsPerSec=512, SecPerClus=1, RsvdSecCnt=32, NumFATs=2, RootEntCnt=0,
    //   FATSz16=0, TotSec32=200, FATSz32=2, RootClus=2
    const uint32_t fat32_lba_start = 63;
    const uint32_t fat32_part_sectors = 200;
    const uint32_t fat32_total_sectors = fat32_lba_start + fat32_part_sectors;
    std::vector<uint8_t> fat32_img(static_cast<size_t>(fat32_total_sectors) * 512U, 0);

    uint8_t* mbr32 = fat32_img.data();
    mbr32[0x1BE + 0] = 0x80; // Active
    mbr32[0x1BE + 4] = 0x0C; // FAT32 LBA
    write_le32(mbr32 + 0x1BE + 8, fat32_lba_start);
    write_le32(mbr32 + 0x1BE + 12, fat32_part_sectors);
    mbr32[510] = 0x55;
    mbr32[511] = 0xAA;

    const size_t part32_off = static_cast<size_t>(fat32_lba_start) * 512U;
    uint8_t* vbr32 = fat32_img.data() + part32_off;
    vbr32[0] = 0xEB;
    vbr32[1] = 0x58;
    vbr32[2] = 0x90;
    std::memcpy(vbr32 + 3, "MSWIN4.1", 8);
    write_le16(vbr32 + 11, 512);  // BytsPerSec
    vbr32[13] = 1;                // SecPerClus
    write_le16(vbr32 + 14, 32);   // RsvdSecCnt
    vbr32[16] = 2;                // NumFATs
    write_le16(vbr32 + 17, 0);    // RootEntCnt (0 for FAT32)
    write_le16(vbr32 + 19, 0);    // TotSec16 (0 for FAT32)
    vbr32[21] = 0xF8;
    write_le16(vbr32 + 22, 0);    // FATSz16 (0 for FAT32)
    write_le32(vbr32 + 32, fat32_part_sectors); // TotSec32
    write_le32(vbr32 + 36, 2);    // FATSz32 (2 sectors per FAT)
    write_le32(vbr32 + 44, 2);    // RootClus = 2
    vbr32[510] = 0x55;
    vbr32[511] = 0xAA;

    const size_t fat32_1_off = part32_off + 32U * 512U;
    const size_t fat32_2_off = part32_off + 34U * 512U;
    const size_t data32_off = part32_off + 36U * 512U; // Cluster 2 = Root directory

    write_le32(fat32_img.data() + fat32_1_off + 0, 0x0FFFFFF8U);
    write_le32(fat32_img.data() + fat32_1_off + 4, 0x0FFFFFFFU);
    write_le32(fat32_img.data() + fat32_1_off + 8, 0x0FFFFFFFU); // Cluster 2 (Root dir) = EOF
    std::memcpy(fat32_img.data() + fat32_2_off, fat32_img.data() + fat32_1_off, 2U * 512U);

    const std::string temp_fat32_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".mbr_fat32_test.img";
    {
        std::ofstream out(temp_fat32_path.c_str(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out.write(reinterpret_cast<const char*>(fat32_img.data()), static_cast<std::streamsize>(fat32_img.size()));
    }

    // First call creates AUTOEXEC.BAT in the FAT32 root directory cluster (Cluster 2) and allocates Cluster 3 for its content
    ASSERT_TRUE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(temp_fat32_path));

    std::vector<uint8_t> mod_fat32;
    {
        std::ifstream in(temp_fat32_path.c_str(), std::ios::binary);
        ASSERT_TRUE(in.is_open());
        mod_fat32.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    const uint8_t* root32_ent = mod_fat32.data() + data32_off;
    EXPECT_EQ(0, std::memcmp(root32_ent, "AUTOEXECBAT", 11));
    uint32_t fst_clus32 = (static_cast<uint32_t>(read_le16(root32_ent + 20)) << 16) | read_le16(root32_ent + 26);
    uint32_t file_size32 = read_le32(root32_ent + 28);
    EXPECT_EQ(3U, fst_clus32);
    EXPECT_EQ(BootInjector::GetAutoexecHookBlock().size(), file_size32);

    std::string created_fat32_content(
        reinterpret_cast<const char*>(mod_fat32.data() + data32_off + (fst_clus32 - 2U) * 512U),
        file_size32
    );
    EXPECT_EQ(BootInjector::GetAutoexecHookBlock(), created_fat32_content);

    // Second call is idempotent
    EXPECT_TRUE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(temp_fat32_path));
    std::remove(temp_fat32_path.c_str());
}

TEST(KioskBootInjectorTest, Windows9xRunOnceAndOsIdFallbackSynthesis) {
    MachineRecord m;
    m.id = "mach-win98";
    m.archetype_id = KioskArchetypes::PENTIUM_3DFX;
    m.archetype_tier = 5;
    m.os_id = "WIN98_SE";
    m.boot_mode = "REAL_HDD_BOOT";

    // 1. Explicit WIN9X_RUNONCE_HOOK with spaced working directory and spaced executable
    CatalogItemRecord win9x_game;
    win9x_game.guest_working_dir = "C:\\Program Files\\Valve/Half-Life";
    win9x_game.guest_executable = "HL.EXE";
    win9x_game.guest_arguments = "-console -game cstrike";
    win9x_game.launch_mechanism = "WIN9X_RUNONCE_HOOK";
    win9x_game.return_on_exit = true;

    std::string bat_win9x = BootInjector::GetInstance().SynthesizeKioskGoBat(m, win9x_game);
    EXPECT_NE(std::string::npos, bat_win9x.find("C:\r\n"));
    EXPECT_NE(std::string::npos, bat_win9x.find("CD \"\\Program Files\\Valve\\Half-Life\"\r\n"));
    EXPECT_NE(std::string::npos, bat_win9x.find("C:\\WINDOWS\\WIN.COM HL.EXE -console -game cstrike\r\n"));
    EXPECT_NE(std::string::npos, bat_win9x.find("B:\\KIOSKEXT.COM\r\n"));

    // 2. Fallback to machine.os_id ("WIN*") when launch_mechanism is unset and executable has spaces
    CatalogItemRecord win_fallback;
    win_fallback.guest_working_dir = "C:\\GAMES\\DIABLO";
    win_fallback.guest_executable = "DiabloLauncher.exe";
    win_fallback.launch_mechanism = "";
    win_fallback.return_on_exit = false;

    std::string bat_fallback = BootInjector::GetInstance().SynthesizeKioskGoBat(m, win_fallback);
    EXPECT_NE(std::string::npos, bat_fallback.find("C:\\WINDOWS\\WIN.COM DiabloLauncher.exe\r\n"));
    EXPECT_EQ(std::string::npos, bat_fallback.find("KIOSKEXT.COM"));
}

// ----------------------------------------------------------------------------
// Phase 4: Skeuomorphic Media Caddy & Runtime Disk-Swap Overlay Tests
// ----------------------------------------------------------------------------

namespace {

void AdvanceEmulatedPicTimeMs(int milliseconds) {
    if (CPU_CycleMax <= 0) {
        CPU_CycleMax = 3000;
    }
    for (int ms = 0; ms < milliseconds; ++ms) {
        CPU_CycleLeft = CPU_CycleMax;
        CPU_Cycles = 0;
        TIMER_AddTick();
        CPU_CycleLeft = CPU_CycleMax;
        CPU_Cycles = 0;
        PIC_RunQueue();
    }
}

bool WriteRawFloppyFileWithVolumeLabel(
    const std::string& path,
    size_t total_bytes,
    const std::string& bpb_label,
    const std::string& root_vol_label = ""
) {
    std::vector<uint8_t> img(total_bytes, 0);
    if (total_bytes < 4096) return false;

    // Minimal valid FAT12 BPB
    img[0x00] = 0xEB; img[0x01] = 0x3C; img[0x02] = 0x90;
    std::memcpy(&img[0x03], "MSDOS5.0", 8);
    img[0x0B] = 0x00; img[0x0C] = 0x02; // 512 bytes/sec
    img[0x0D] = 0x01;                   // 1 sec/clus
    img[0x0E] = 0x01; img[0x0F] = 0x00; // 1 reserved sector
    img[0x10] = 0x02;                   // 2 FATs
    img[0x11] = 0xE0; img[0x12] = 0x00; // 224 root entries
    const uint16_t tot_sec = static_cast<uint16_t>(total_bytes / 512u);
    img[0x13] = static_cast<uint8_t>(tot_sec & 0xFFu);
    img[0x14] = static_cast<uint8_t>((tot_sec >> 8) & 0xFFu);
    img[0x15] = 0xF0;
    img[0x16] = 0x03; img[0x17] = 0x00; // 3 sectors/FAT
    img[0x18] = 18;   img[0x19] = 0;
    img[0x1A] = 2;    img[0x1B] = 0;
    img[0x26] = 0x29;                   // Extended boot signature
    std::memset(&img[0x2B], ' ', 11);
    for (size_t i = 0; i < bpb_label.size() && i < 11; ++i) {
        img[0x2B + i] = static_cast<uint8_t>(bpb_label[i]);
    }
    std::memcpy(&img[0x36], "FAT12   ", 8);
    img[0x1FE] = 0x55; img[0x1FF] = 0xAA;

    // Root directory starts at sector (1 + 2 * 3) = sector 7 (offset 7 * 512 = 3584)
    if (!root_vol_label.empty()) {
        const size_t root_ofs = 7u * 512u;
        std::memset(&img[root_ofs], ' ', 11);
        for (size_t i = 0; i < root_vol_label.size() && i < 11; ++i) {
            img[root_ofs + i] = static_cast<uint8_t>(root_vol_label[i]);
        }
        img[root_ofs + 11] = 0x08; // ATTR_VOLUME_ID
    }

    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    const size_t written = std::fwrite(img.data(), 1, img.size(), fp);
    std::fclose(fp);
    return (written == img.size());
}

} // anonymous namespace

TEST(KioskDiskCaddyTest, MediaInspectorDetectsFloppyGeometriesAndFatVolumeLabel) {
    const std::string base = BootInjector::GetInstance().GetDefaultDispatchImagePath();
    const std::string p360  = base + "_360k.img";
    const std::string p720  = base + "_720k.img";
    const std::string p1200 = base + "_1200k.img";
    const std::string p1440 = base + "_1440k.img";

    ASSERT_TRUE(WriteRawFloppyFileWithVolumeLabel(p360,  360u * 1024u,  "ULTIMA_IV", ""));
    ASSERT_TRUE(WriteRawFloppyFileWithVolumeLabel(p720,  720u * 1024u,  "NO NAME",   "MONKEY_DISK"));
    ASSERT_TRUE(WriteRawFloppyFileWithVolumeLabel(p1200, 1200u * 1024u, "WING_CMD1", ""));
    ASSERT_TRUE(WriteRawFloppyFileWithVolumeLabel(p1440, 1440u * 1024u, "DOOM2_DISK1", "MONKEY_DISK"));

    InspectedMediaInfo info360, info720, info1200, info1440;
    ASSERT_TRUE(DiskCaddyView::InspectMediaFile(p360, info360));
    EXPECT_STREQ("FLOPPY_525_360K", info360.media_type.c_str());
    EXPECT_STREQ("CHARCOAL_525", info360.physical_style.c_str());
    EXPECT_STREQ("ULTIMA_IV", info360.volume_label.c_str());

    ASSERT_TRUE(DiskCaddyView::InspectMediaFile(p720, info720));
    EXPECT_STREQ("FLOPPY_35_720K", info720.media_type.c_str());
    EXPECT_STREQ("MONKEY_DISK", info720.volume_label.c_str());

    ASSERT_TRUE(DiskCaddyView::InspectMediaFile(p1200, info1200));
    EXPECT_STREQ("FLOPPY_525_1200K", info1200.media_type.c_str());
    EXPECT_STREQ("CHARCOAL_525", info1200.physical_style.c_str());
    EXPECT_STREQ("WING_CMD1", info1200.volume_label.c_str());

    ASSERT_TRUE(DiskCaddyView::InspectMediaFile(p1440, info1440));
    EXPECT_STREQ("FLOPPY_35_1440K", info1440.media_type.c_str());
    EXPECT_STREQ("MONKEY_DISK", info1440.volume_label.c_str());

    std::remove(p360.c_str());
    std::remove(p720.c_str());
    std::remove(p1200.c_str());
    std::remove(p1440.c_str());
}

TEST(KioskDiskCaddyTest, MediaInspectorExtractsIso9660PvdAndCueRedbookAudio) {
    const std::string base = BootInjector::GetInstance().GetDefaultDispatchImagePath();
    const std::string iso_path = base + "_sc2000.iso";
    const std::string cue_path = base + "_sc2000.cue";

    // Build a minimal 18-sector ISO9660 image with Primary Volume Descriptor at sector 16 (0x8000)
    std::vector<uint8_t> iso_bytes(18u * 2048u, 0);
    iso_bytes[0x8000] = 0x01; // Primary Volume Descriptor type
    std::memcpy(&iso_bytes[0x8001], "CD001", 5);
    iso_bytes[0x8006] = 0x01; // PVD version
    std::memset(&iso_bytes[0x8028], ' ', 32);
    const char* vol_id = "SC2000_SE_CD";
    std::memcpy(&iso_bytes[0x8028], vol_id, std::strlen(vol_id));

    // Terminator Volume Descriptor at sector 17
    iso_bytes[17u * 2048u] = 0xFF;
    std::memcpy(&iso_bytes[17u * 2048u + 1], "CD001", 5);

    {
        FILE* fp = std::fopen(iso_path.c_str(), "wb");
        ASSERT_NE(nullptr, fp);
        ASSERT_EQ(iso_bytes.size(), std::fwrite(iso_bytes.data(), 1, iso_bytes.size(), fp));
        std::fclose(fp);
    }

    {
        std::ofstream cue_ofs(cue_path.c_str(), std::ios::binary);
        ASSERT_TRUE(cue_ofs.is_open());
        cue_ofs << "FILE \"" << iso_path << "\" BINARY\r\n"
                << "  TRACK 01 MODE1/2048\r\n"
                << "    INDEX 01 00:00:00\r\n"
                << "  TRACK 02 AUDIO\r\n"
                << "    INDEX 01 02:00:00\r\n";
    }

    InspectedMediaInfo iso_info;
    ASSERT_TRUE(DiskCaddyView::InspectMediaFile(iso_path, iso_info));
    EXPECT_STREQ("CDROM_ISO", iso_info.media_type.c_str());
    EXPECT_STREQ("JEWEL_CD", iso_info.physical_style.c_str());
    EXPECT_STREQ("SC2000_SE_CD", iso_info.volume_label.c_str());
    EXPECT_FALSE(iso_info.has_redbook_audio);

    InspectedMediaInfo cue_info;
    ASSERT_TRUE(DiskCaddyView::InspectMediaFile(cue_path, cue_info));
    EXPECT_STREQ("CDROM_CUE", cue_info.media_type.c_str());
    EXPECT_STREQ("JEWEL_CD", cue_info.physical_style.c_str());
    EXPECT_STREQ("SC2000_SE_CD", cue_info.volume_label.c_str());
    EXPECT_TRUE(cue_info.has_redbook_audio);

    std::remove(cue_path.c_str());
    std::remove(iso_path.c_str());
}

TEST(KioskDiskCaddyTest, DeterministicPhysicalStyleVarietyAndAngleJitter) {
    bool saw_beige = false;
    bool saw_blue = false;
    bool saw_black = false;

    for (int i = 0; i < 32; ++i) {
        const std::string seed = "floppy_seed_" + std::to_string(i);
        const std::string style1 = DiskCaddyView::ResolvePhysicalStyle("FLOPPY_35_1440K", "", seed);
        const std::string style2 = DiskCaddyView::ResolvePhysicalStyle("FLOPPY_35_1440K", "", seed);
        EXPECT_EQ(style1, style2); // Deterministic across runs

        const uint32_t crc_mod = DiskCaddyView::ComputeStringCrc32(seed) % 3u;
        if (crc_mod == 0u) {
            EXPECT_STREQ("BEIGE_35", style1.c_str());
            saw_beige = true;
        } else if (crc_mod == 1u) {
            EXPECT_STREQ("BLUE_35", style1.c_str());
            saw_blue = true;
        } else {
            EXPECT_STREQ("BLACK_35", style1.c_str());
            saw_black = true;
        }

        const float jitter1 = DiskCaddyView::ComputeStickerAngleJitterDeg(seed);
        const float jitter2 = DiskCaddyView::ComputeStickerAngleJitterDeg(seed);
        EXPECT_FLOAT_EQ(jitter1, jitter2);
        EXPECT_GE(jitter1, -0.8f);
        EXPECT_LE(jitter1, 0.8f);
    }

    EXPECT_TRUE(saw_beige);
    EXPECT_TRUE(saw_blue);
    EXPECT_TRUE(saw_black);

    // 5.25" always resolves to CHARCOAL_525; CD-ROM respects GOLD_CD
    EXPECT_STREQ("CHARCOAL_525",
                 DiskCaddyView::ResolvePhysicalStyle("FLOPPY_525_360K", "", "any").c_str());
    EXPECT_STREQ("GOLD_CD",
                 DiskCaddyView::ResolvePhysicalStyle("CDROM_ISO", "GOLD_CD", "any").c_str());
}

TEST(KioskRuntimeSwapTest, FloppyHotSwapTogglesFdc0x3F7DskChgAndHoldsDoorOpen100ms) {
    const std::string base = BootInjector::GetInstance().GetDefaultDispatchImagePath();
    const std::string disk1_path = base + "_swap_disk1.img";
    const std::string disk2_path = base + "_swap_disk2.img";
    const std::string diskb_path = base + "_swap_diskb.img";

    ASSERT_TRUE(WriteRawFloppyFileWithVolumeLabel(disk1_path, 1440u * 1024u, "DISK_ONE"));
    ASSERT_TRUE(WriteRawFloppyFileWithVolumeLabel(disk2_path, 1440u * 1024u, "DISK_TWO"));
    ASSERT_TRUE(WriteRawFloppyFileWithVolumeLabel(diskb_path, 1440u * 1024u, "DISK_BEE"));

    // Mount initial floppies in Drive A: (0) and Drive B: (1)
    ASSERT_TRUE(DriveManager::AppendAndSwapDisk(0, disk1_path, false, false));
    ASSERT_TRUE(DriveManager::AppendAndSwapDisk(1, diskb_path, false, false));

    // Close door after initial mount by advancing 110ms
    AdvanceEmulatedPicTimeMs(110);
    EXPECT_FALSE(FDC_IsDoorOpen(0));
    EXPECT_FALSE(FDC_IsDoorOpen(1));

    // Simulate guest clearing initial DSKCHG on A: and B:
    imageDiskChange[0] = false;
    imageDiskChange[1] = false;
    EXPECT_EQ(0u, FDC_DebugReadDIR(0) & 0x80u);
    EXPECT_EQ(0u, FDC_DebugReadDIR(1) & 0x80u);

    // Hot-swap Disk 2 into Drive A: with write_protected = true
    ASSERT_TRUE(DriveManager::AppendAndSwapDisk(0, disk2_path, false, true));
    ASSERT_NE(nullptr, imageDiskList[0]);
    EXPECT_STREQ(disk2_path.c_str(), DriveManager::GetKioskActiveDiskPath(0).c_str());

    // Verify 100ms virtual door-open state & 0x3F7 bit 7 (0x80 DSKCHG) high on Drive A:,
    // while Drive B: remains untouched!
    EXPECT_TRUE(FDC_IsDoorOpen(0));
    EXPECT_NE(0u, FDC_DebugReadDIR(0) & 0x80u);
    EXPECT_FALSE(FDC_IsDoorOpen(1));
    EXPECT_EQ(0u, FDC_DebugReadDIR(1) & 0x80u);

    // Verify write-protection blocks sector writes with Int13Status::WriteProtected (0x03)
    uint8_t dummy_sector[512] = { 0 };
    EXPECT_EQ(Int13Status::WriteProtected, imageDiskList[0]->Write_AbsoluteSector(0, dummy_sector));

    // At 50ms of emulated CPU time, the virtual floppy door must STILL be held open
    AdvanceEmulatedPicTimeMs(50);
    EXPECT_TRUE(FDC_IsDoorOpen(0));
    EXPECT_NE(0u, FDC_DebugReadDIR(0) & 0x80u);

    // Past 100ms (e.g. +60ms -> 110ms total), the virtual door closes and DSKCHG remains latched
    AdvanceEmulatedPicTimeMs(60);
    EXPECT_FALSE(FDC_IsDoorOpen(0));
    EXPECT_TRUE(imageDiskChange[0]);
    EXPECT_NE(0u, FDC_DebugReadDIR(0) & 0x80u);

    // Clean up test floppy mounts
    DriveManager::EjectDriveMedia(0, false);
    DriveManager::EjectDriveMedia(1, false);
    for (int i = 0; i < MAX_SWAPPABLE_DISKS; ++i) {
        if (diskSwap[i] != NULL) {
            diskSwap[i]->Release();
            diskSwap[i] = NULL;
        }
    }
    std::remove(disk1_path.c_str());
    std::remove(disk2_path.c_str());
    std::remove(diskb_path.c_str());
}

TEST(KioskRuntimeSwapTest, AtapiCdRomHotSwapTriggersUnitAttentionAsc28) {
    // 1. Immediate transition (0.0ms) -> LOAD_DISC_READIED with SK=0x06 (UNIT ATTENTION), ASC=0x28, ASCQ=0x00
    ASSERT_TRUE(Kiosk_IDE_NotifyATAPIMediaChange(3 /* Drive D: */, 0.0));
    uint8_t sk = 0, asc = 0, ascq = 0;
    int loading_mode = -1;
    ASSERT_TRUE(Kiosk_IDE_GetATAPISense(3, sk, asc, ascq, loading_mode));
    EXPECT_EQ(0x06u, sk);
    EXPECT_EQ(0x28u, asc);
    EXPECT_EQ(0x00u, ascq);

    // 2. Timed 250ms virtual tray eject/insert cycle:
    //    t=0ms:    LOAD_INSERT_CD (SK=0x02 NOT READY, ASC=0x3A MEDIUM NOT PRESENT)
    //    t=130ms:  LOAD_DISC_LOADING (SK=0x02 NOT READY, ASC=0x04, ASCQ=0x01 BECOMING READY)
    //    t=260ms:  LOAD_DISC_READIED (SK=0x06 UNIT ATTENTION, ASC=0x28, ASCQ=0x00 MEDIUM MAY HAVE CHANGED)
    ASSERT_TRUE(Kiosk_IDE_NotifyATAPIMediaChange(3 /* Drive D: */, 250.0));
    ASSERT_TRUE(Kiosk_IDE_GetATAPISense(3, sk, asc, ascq, loading_mode));
    EXPECT_EQ(0x02u, sk);
    EXPECT_EQ(0x3Au, asc);

    AdvanceEmulatedPicTimeMs(135);
    ASSERT_TRUE(Kiosk_IDE_GetATAPISense(3, sk, asc, ascq, loading_mode));
    EXPECT_EQ(0x02u, sk);
    EXPECT_EQ(0x04u, asc);
    EXPECT_EQ(0x01u, ascq);

    AdvanceEmulatedPicTimeMs(135);
    ASSERT_TRUE(Kiosk_IDE_GetATAPISense(3, sk, asc, ascq, loading_mode));
    EXPECT_EQ(0x06u, sk);
    EXPECT_EQ(0x28u, asc);
    EXPECT_EQ(0x00u, ascq);
}

TEST_F(KioskDatabaseFixture, OverlayCachesSessionCaddyAndTogglesViewMode) {
    const std::string base = BootInjector::GetInstance().GetDefaultDispatchImagePath();
    const std::string disk1_path = base + "_ovl_disk1.img";
    const std::string disk2_path = base + "_ovl_disk2.img";
    ASSERT_TRUE(WriteRawFloppyFileWithVolumeLabel(disk1_path, 1440u * 1024u, "KQ6_DISK1"));
    ASSERT_TRUE(WriteRawFloppyFileWithVolumeLabel(disk2_path, 1440u * 1024u, "KQ6_DISK2"));

    KioskDatabase& db = KioskDatabase::GetInstance();

    MachineRecord mach;
    mach.id = "mach-486";
    mach.display_name = "486DX2 Sound Blaster 16";
    mach.archetype_id = KioskArchetypes::DX2_486;
    mach.archetype_tier = 4;
    mach.boot_mode = "INTEGRATED_DOS";
    ASSERT_TRUE(db.InsertMachine(mach));

    CatalogItemRecord item;
    item.id = "item-kq6";
    item.machine_id = mach.id;
    item.item_type = "GAME";
    item.title = "King's Quest VI";
    item.launch_mechanism = "INTEGRATED_AUTOEXEC";
    item.guest_executable = "SIERRA.EXE";
    ASSERT_TRUE(db.InsertCatalogItem(item));

    MediaPoolRecord m1;
    m1.id = "med-kq6-1";
    m1.file_path = disk1_path;
    m1.media_type = "FLOPPY_35_1440K";
    m1.label_title = "King's Quest VI - Disk 1";
    ASSERT_TRUE(db.InsertMedia(m1));

    MediaPoolRecord m2;
    m2.id = "med-kq6-2";
    m2.file_path = disk2_path;
    m2.media_type = "FLOPPY_35_1440K";
    m2.label_title = "King's Quest VI - Disk 2";
    m2.is_write_protected = true;
    ASSERT_TRUE(db.InsertMedia(m2));

    CaddyBindingRecord b1;
    b1.catalog_item_id = item.id;
    b1.media_id = m1.id;
    b1.target_drive = "A";
    b1.slot_order = 1;
    ASSERT_TRUE(db.AddCaddyBinding(b1));

    CaddyBindingRecord b2;
    b2.catalog_item_id = item.id;
    b2.media_id = m2.id;
    b2.target_drive = "A";
    b2.slot_order = 2;
    ASSERT_TRUE(db.AddCaddyBinding(b2));

    // Set active launch context and enter STATE_RUNTIME_OVERLAY
    KioskManager& km = KioskManager::GetInstance();
    LaunchContext ctx;
    ctx.machine_id = mach.id;
    ctx.catalog_item_id = item.id;
    ctx.is_real_hdd_boot = false;
    km.SetLaunchContext(ctx);
    km.SetState(KioskState::STATE_EMULATION_RUNNING);
    km.EnterRuntimeOverlay();
    EXPECT_EQ(KioskState::STATE_RUNTIME_OVERLAY, km.GetState());

    RuntimeOverlay& ovl = RuntimeOverlay::GetInstance();
    ASSERT_GE(ovl.GetSessionCaddy().size(), 2u);
    EXPECT_STREQ("KQ6_DISK1", ovl.GetSessionCaddy()[0].media.volume_label.c_str());
    EXPECT_STREQ("KQ6_DISK2", ovl.GetSessionCaddy()[1].media.volume_label.c_str());
    EXPECT_EQ(CaddyViewMode::SKEUOMORPHIC, ovl.GetViewMode());

    // Toggle view mode via 'V' key event
    SDL_Event key_v = {};
    key_v.type = SDL_KEYDOWN;
    key_v.key.repeat = 0;
    key_v.key.keysym.sym = SDLK_v;
    EXPECT_TRUE(ovl.HandleInputEvent(key_v));
    EXPECT_EQ(CaddyViewMode::LIST, ovl.GetViewMode());

    // Toggle view mode back via Gamepad Y button event
    SDL_Event pad_y = {};
    pad_y.type = SDL_CONTROLLERBUTTONDOWN;
    pad_y.cbutton.button = SDL_CONTROLLER_BUTTON_Y;
    EXPECT_TRUE(ovl.HandleInputEvent(pad_y));
    EXPECT_EQ(CaddyViewMode::SKEUOMORPHIC, ovl.GetViewMode());

    // Perform hot-swap of Caddy Slot 1 (Disk 2, write-protected)
    ASSERT_TRUE(ovl.PerformHotSwapCaddySlot(1));
    EXPECT_STREQ(disk2_path.c_str(), DriveManager::GetKioskActiveDiskPath(0).c_str());
    EXPECT_TRUE(FDC_IsWriteProtected(0));

    // Resume emulation
    km.ResumeEmulation();
    EXPECT_EQ(KioskState::STATE_EMULATION_RUNNING, km.GetState());

    DriveManager::EjectDriveMedia(0, false);
    for (int i = 0; i < MAX_SWAPPABLE_DISKS; ++i) {
        if (diskSwap[i] != NULL) {
            diskSwap[i]->Release();
            diskSwap[i] = NULL;
        }
    }
    std::remove(disk1_path.c_str());
    std::remove(disk2_path.c_str());
}

TEST(KioskRuntimeSwapTest, OverlayHotkeyChordLatchesUntilKeyRelease) {
    KioskManager& km = KioskManager::GetInstance();
    uint8_t kb_state[SDL_NUM_SCANCODES] = {};

    // Ensure clean unlatched state
    SDL_Event reset_evt = {};
    reset_evt.type = SDL_KEYUP;
    reset_evt.key.keysym.sym = SDLK_d;
    EXPECT_FALSE(km.IsOverlayHotkeyEvent(reset_evt, kb_state));

    // Simulate both F12 and D held in the same SDL event pump
    kb_state[SDL_SCANCODE_F12] = 1;
    kb_state[SDL_SCANCODE_D] = 1;

    SDL_Event f12_down = {};
    f12_down.type = SDL_KEYDOWN;
    f12_down.key.repeat = 0;
    f12_down.key.keysym.sym = SDLK_F12;
    f12_down.key.keysym.scancode = SDL_SCANCODE_F12;

    SDL_Event d_down = {};
    d_down.type = SDL_KEYDOWN;
    d_down.key.repeat = 0;
    d_down.key.keysym.sym = SDLK_d;
    d_down.key.keysym.scancode = SDL_SCANCODE_D;

    // First queued keydown triggers once; second queued keydown is suppressed by latch
    EXPECT_TRUE(km.IsOverlayHotkeyEvent(f12_down, kb_state));
    EXPECT_FALSE(km.IsOverlayHotkeyEvent(d_down, kb_state));

    // Releasing D clears the latch
    kb_state[SDL_SCANCODE_D] = 0;
    SDL_Event d_up = {};
    d_up.type = SDL_KEYUP;
    d_up.key.keysym.sym = SDLK_d;
    d_up.key.keysym.scancode = SDL_SCANCODE_D;
    EXPECT_FALSE(km.IsOverlayHotkeyEvent(d_up, kb_state));

    // Pressing D again while F12 is held triggers once more
    kb_state[SDL_SCANCODE_D] = 1;
    EXPECT_TRUE(km.IsOverlayHotkeyEvent(d_down, kb_state));

    // Clean up latch for subsequent tests
    kb_state[SDL_SCANCODE_F12] = 0;
    kb_state[SDL_SCANCODE_D] = 0;
    EXPECT_FALSE(km.IsOverlayHotkeyEvent(d_up, kb_state));
}

TEST(KioskRuntimeSwapTest, LogicalWriteProtectDecoupledFromHostReadOnly) {
    const std::string disk_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + "_wp_toggle_test.img";
    ASSERT_TRUE(WriteRawFloppyFileWithVolumeLabel(disk_path, 1440u * 1024u, "WP_TEST"));

    // Mount writable host image initially with logical write protection enabled
    ASSERT_TRUE(DriveManager::AppendAndSwapDisk(0, disk_path, false, /*write_protected=*/true));
    ASSERT_NE(nullptr, imageDiskList[0]);
    EXPECT_TRUE(FDC_IsWriteProtected(0));
    EXPECT_TRUE(imageDiskList[0]->kiosk_write_protected);
    EXPECT_FALSE(imageDiskList[0]->kiosk_host_readonly);

    // Logical WP can be disabled because host stream is writable
    EXPECT_TRUE(DriveManager::SetKioskFloppyWriteProtect(0, false));
    EXPECT_FALSE(FDC_IsWriteProtected(0));
    EXPECT_FALSE(imageDiskList[0]->kiosk_write_protected);

    // Simulate a physically read-only host file: unlocking WP must fail and retain protection
    imageDiskList[0]->kiosk_host_readonly = true;
    EXPECT_TRUE(DriveManager::SetKioskFloppyWriteProtect(0, true));
    EXPECT_FALSE(DriveManager::SetKioskFloppyWriteProtect(0, false));
    EXPECT_TRUE(FDC_IsWriteProtected(0));
    EXPECT_TRUE(imageDiskList[0]->kiosk_write_protected);

    // Eject and verify active path clears while ejected
    EXPECT_TRUE(DriveManager::EjectDriveMedia(0, false));
    EXPECT_TRUE(DriveManager::GetKioskActiveDiskPath(0).empty());

    for (int i = 0; i < MAX_SWAPPABLE_DISKS; ++i) {
        if (diskSwap[i] != NULL) {
            diskSwap[i]->Release();
            diskSwap[i] = NULL;
        }
    }
    std::remove(disk_path.c_str());
}

TEST(KioskRuntimeSwapTest, AtapiReadOnlyLookupDoesNotMutateAssignedDriveIndex) {
    ASSERT_TRUE(Kiosk_IDE_NotifyATAPIMediaChange(3 /* Drive D: */, 0.0));

    uint8_t sk = 0, asc = 0, ascq = 0;
    int loading_mode = -1;
    ASSERT_TRUE(Kiosk_IDE_GetATAPISense(3, sk, asc, ascq, loading_mode));

    // Read-only sense lookup for unmounted Drive E: must return false without hijacking D:
    EXPECT_FALSE(Kiosk_IDE_GetATAPISense(4 /* Drive E: */, sk, asc, ascq, loading_mode));

    // Drive D: must remain intact and queryable
    EXPECT_TRUE(Kiosk_IDE_GetATAPISense(3, sk, asc, ascq, loading_mode));
}

// ============================================================================
// Phase 5: Pre-Boot Shell & Big-Picture UI Unit Tests
// ============================================================================

TEST(KioskInputRouterTest, MapsKeyboardGamepadAndJoystickActions) {
    KioskInputRouter& router = KioskInputRouter::GetInstance();
    router.ClearPendingActions();

    // 1. Keyboard arrows + action keys
    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.repeat = 0;

    ev.key.keysym.sym = SDLK_RIGHT;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 1000));
    ev.key.keysym.sym = SDLK_RETURN;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 1010));
    ev.key.keysym.sym = SDLK_c;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 1020));
    ev.key.keysym.sym = SDLK_v;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 1030));
    ev.key.keysym.sym = SDLK_f;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 1040));

    KioskInputAction act = KioskInputAction::NONE;
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::NAV_RIGHT);
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::ACTION_CONFIRM);
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::ACTION_CADDY_QUEUE);
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::ACTION_VIEW_TOGGLE);
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::ACTION_FAVORITE);
    EXPECT_FALSE(router.PollAction(act));

    // 2. Gamepad D-Pad, buttons, and analog stick deadzone/repeat
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_CONTROLLERBUTTONDOWN;
    ev.cbutton.button = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 2000));
    ev.cbutton.button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 2010));

    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::NAV_LEFT);
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::NAV_TAB_NEXT);

    // Analog stick inside deadzone -> ignored
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_CONTROLLERAXISMOTION;
    ev.caxis.axis = SDL_CONTROLLER_AXIS_LEFTX;
    ev.caxis.value = 5000;
    router.ProcessSDLEvent(ev, false, 3000);
    EXPECT_EQ(router.GetPendingActionCount(), 0u);

    // Analog stick past deadzone -> emits NAV_RIGHT immediately, then repeats after delay
    ev.caxis.value = 24000;
    router.ProcessSDLEvent(ev, false, 3000);
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::NAV_RIGHT);

    router.UpdateRepeat(3400);
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::NAV_RIGHT);

    // Release stick
    ev.caxis.value = 0;
    router.ProcessSDLEvent(ev, false, 3500);
    router.ClearPendingActions();

    // 3. Joystick hat
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_JOYHATMOTION;
    ev.jhat.value = SDL_HAT_UP;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 4000));
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::NAV_UP);
    ev.jhat.value = SDL_HAT_CENTERED;
    router.ProcessSDLEvent(ev, false, 4010);
}

TEST(KioskInputRouterTest, SuppressesLetterHotkeysDuringTextInput) {
    KioskInputRouter& router = KioskInputRouter::GetInstance();
    router.ClearPendingActions();

    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.repeat = 0;

    // Letter hotkeys must NOT fire when an ImGui text input is focused
    ev.key.keysym.sym = SDLK_a;
    EXPECT_FALSE(router.ProcessSDLEvent(ev, true, 1000));
    ev.key.keysym.sym = SDLK_c;
    EXPECT_FALSE(router.ProcessSDLEvent(ev, true, 1010));
    ev.key.keysym.sym = SDLK_SPACE;
    EXPECT_FALSE(router.ProcessSDLEvent(ev, true, 1020));
    EXPECT_EQ(router.GetPendingActionCount(), 0u);

    // Escape and gamepad B still map to ACTION_BACK
    ev.key.keysym.sym = SDLK_ESCAPE;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, true, 1030));
    KioskInputAction act = KioskInputAction::NONE;
    ASSERT_TRUE(router.PollAction(act));
    EXPECT_EQ(act, KioskInputAction::ACTION_BACK);
}

TEST(KioskCarouselTest, FiltersSortsAndNavigatesItems) {
    ASSERT_TRUE(KioskDatabase::GetInstance().Open(":memory:"));
    KioskManager::GetInstance().Initialize("", "");
    KioskManager::GetInstance().SetState(KioskState::STATE_SHELL_INTERACTIVE);

    MachineRecord mach;
    mach.id = "mach-carousel-1";
    mach.display_name = "Test 486 Rig";
    mach.archetype_id = KioskArchetypes::DX2_486;
    mach.archetype_tier = 4;
    mach.boot_mode = "INTEGRATED_DOS";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMachine(mach));

    CatalogItemRecord g1;
    g1.id = "game-doom";
    g1.machine_id = mach.id;
    g1.item_type = "GAME";
    g1.title = "Doom II";
    g1.developer = "id Software";
    g1.guest_working_dir = "C:\\DOOM2";
    g1.guest_executable = "DOOM2.EXE";
    g1.is_favorite = false;
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertCatalogItem(g1));

    CatalogItemRecord g2;
    g2.id = "game-keen";
    g2.machine_id = mach.id;
    g2.item_type = "GAME";
    g2.title = "Commander Keen 4";
    g2.developer = "id Software";
    g2.guest_working_dir = "C:\\KEEN4";
    g2.guest_executable = "KEEN4E.EXE";
    g2.is_favorite = true;
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertCatalogItem(g2));

    CatalogItemRecord app1;
    app1.id = "app-wp";
    app1.machine_id = mach.id;
    app1.item_type = "APPLICATION";
    app1.title = "WordPerfect 5.1";
    app1.developer = "WordPerfect Corp";
    app1.guest_working_dir = "C:\\WP51";
    app1.guest_executable = "WP.EXE";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertCatalogItem(app1));

    KioskUI::GetInstance().InvalidateCatalogCache();
    KioskUI::GetInstance().EnsureCatalogCacheLoaded();

    // Verify filtering and favorites-first sorting
    std::vector<CatalogItemRecord> games = ViewCarousel::FilterAndSortCatalogItems(
        KioskUI::GetInstance().GetCachedItems(), "GAME", "");
    ASSERT_EQ(games.size(), 2u);
    EXPECT_EQ(games[0].id, "game-keen"); // Favorite comes first
    EXPECT_EQ(games[1].id, "game-doom");

    std::vector<CatalogItemRecord> search_res = ViewCarousel::FilterAndSortCatalogItems(
        KioskUI::GetInstance().GetCachedItems(), "GAME", "doom");
    ASSERT_EQ(search_res.size(), 1u);
    EXPECT_EQ(search_res[0].id, "game-doom");

    // Navigate with NAV_RIGHT, verify ACTION_CONTEXT_MENU opens/closes modal cleanly, then launch
    ViewCarousel& carousel = ViewCarousel::GetInstance();
    carousel.SetSearchFilter("");
    EXPECT_EQ(carousel.GetSelectedGameIndex(), 0);
    EXPECT_TRUE(carousel.HandleAction(KioskHubTab::GAMES, KioskInputAction::NAV_RIGHT));
    EXPECT_EQ(carousel.GetSelectedGameIndex(), 1);
    EXPECT_TRUE(carousel.HandleAction(KioskHubTab::GAMES, KioskInputAction::ACTION_CONTEXT_MENU));
    EXPECT_TRUE(carousel.IsContextModalOpen());
    EXPECT_TRUE(carousel.HandleAction(KioskHubTab::GAMES, KioskInputAction::ACTION_BACK));
    EXPECT_FALSE(carousel.IsContextModalOpen());
    EXPECT_TRUE(carousel.HandleAction(KioskHubTab::GAMES, KioskInputAction::ACTION_CONFIRM));
    EXPECT_EQ(KioskManager::GetInstance().GetState(), KioskState::STATE_SYNTHESIZE_LAUNCH);
    EXPECT_EQ(KioskManager::GetInstance().GetCurrentLaunchContext().catalog_item_id, "game-doom");

    KioskManager::GetInstance().Shutdown();
    KioskDatabase::GetInstance().Close();
}

TEST(KioskWizardTest, EnforcesArchetypeOsTierCompatibilityAndCreatesMachine) {
    ASSERT_TRUE(KioskDatabase::GetInstance().Open(":memory:"));

    // Add a floppy media record so we can verify installer caddy binding
    MediaPoolRecord floppy;
    floppy.id = "med-dos622-d1";
    floppy.file_path = "dos622_disk1.img";
    floppy.media_type = "FLOPPY_35_1440K";
    floppy.label_title = "MS-DOS 6.22 Disk 1";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMedia(floppy));

    ViewWizard& wiz = ViewWizard::GetInstance();
    wiz.Reset();

    // 1. Select Tier 2 (286) and verify Windows 98 SE (Tier 5) is rejected
    ASSERT_TRUE(wiz.SelectArchetype(KioskArchetypes::AT_286));
    std::string err;
    EXPECT_FALSE(wiz.SelectOSDistribution("WIN98_SE", err));
    EXPECT_FALSE(err.empty());

    // 2. Upgrade to Tier 5 (Pentium 3dfx) and verify Windows 98 SE is accepted
    ASSERT_TRUE(wiz.SelectArchetype(KioskArchetypes::PENTIUM_3DFX));
    EXPECT_TRUE(wiz.SelectOSDistribution("WIN98_SE", err));
    EXPECT_TRUE(err.empty());

    // 3. Configure identity, REAL_HDD_BOOT, exclusive blank HDD creation, CHS geometry, and installer media
    std::string temp_dir = "/tmp/";
#if defined(_WIN32)
    if (const char* td = std::getenv("TEMP")) {
        temp_dir = td;
        if (!temp_dir.empty() && temp_dir.back() != '\\' && temp_dir.back() != '/') temp_dir += '\\';
    }
#endif
    const std::string temp_hdd = temp_dir + "kiosk_test_wizard_win98.img";
    std::remove(temp_hdd.c_str());

    wiz.SetDisplayName("My Win98 Voodoo Rig");
    wiz.SetBootMode("REAL_HDD_BOOT");
    wiz.SetHddSizeMb(20);
    wiz.SetPrimaryHddPath(temp_hdd);
    wiz.SetCreateBlankHddOnDisk(true);
    wiz.SetInstallerFloppyMediaId(floppy.id);

    const std::string chs = ViewWizard::ComputeHddChsGeometry(20);
    EXPECT_FALSE(chs.empty());
    EXPECT_NE(chs.find(",16,63"), std::string::npos);

    ASSERT_TRUE(wiz.SetStep(WizardStep::STEP_REVIEW_CREATE));
    std::string created_id;
    ASSERT_TRUE(wiz.CommitMachine(created_id, err)) << err;
    EXPECT_FALSE(created_id.empty());

    // Attempting to create a blank HDD over an existing file must be refused (no truncation)
    std::string dup_chs;
    EXPECT_FALSE(ViewWizard::CreateBlankHddImage(temp_hdd, 20, dup_chs));

    // Verify persistence in SQLite
    MachineRecord persisted;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMachine(created_id, persisted));
    EXPECT_EQ(persisted.display_name, "My Win98 Voodoo Rig");
    EXPECT_EQ(persisted.archetype_id, KioskArchetypes::PENTIUM_3DFX);
    EXPECT_EQ(persisted.os_id, "WIN98_SE");
    EXPECT_EQ(persisted.boot_mode, "REAL_HDD_BOOT");
    EXPECT_EQ(persisted.primary_hdd_chs, chs);
    EXPECT_FALSE(persisted.os_installed);

    auto bindings = KioskDatabase::GetInstance().GetCaddyBindingsForMachine(created_id);
    ASSERT_EQ(bindings.size(), 1u);
    EXPECT_EQ(bindings[0].media_id, floppy.id);
    EXPECT_EQ(bindings[0].target_drive, "A");

    // Launching this newly created blank REAL_HDD_BOOT machine with bound installer media
    // must skip EnsureOneTimeAutoexecHook and boot Floppy Drive A: (BOOT -l a)
    KioskManager::GetInstance().Initialize("", "");
    ASSERT_TRUE(KioskManager::GetInstance().RequestLaunchMachine(created_id));
    std::string synth_conf;
    ASSERT_TRUE(KioskManager::GetInstance().SynthesizeAndPrepareLaunch(synth_conf));
    EXPECT_NE(std::string::npos, synth_conf.find("BOOT -l a"));
    EXPECT_EQ(std::string::npos, synth_conf.find("BOOT -l c"));

    KioskManager::GetInstance().Shutdown();
    KioskDatabase::GetInstance().Close();
    std::remove(temp_hdd.c_str());
}

TEST(KioskWizardTest, ImportedHddPreservesNativeGeometryAndRejectsInvalidBindings) {
    ASSERT_TRUE(KioskDatabase::GetInstance().Open(":memory:"));

    std::string temp_dir = "/tmp/";
#if defined(_WIN32)
    if (const char* td = std::getenv("TEMP")) {
        temp_dir = td;
        if (!temp_dir.empty() && temp_dir.back() != '\\' && temp_dir.back() != '/') temp_dir += '\\';
    }
#endif
    const std::string existing_hdd = temp_dir + "kiosk_test_imported_hdd.img";
    {
        std::ofstream ofs(existing_hdd.c_str(), std::ios::binary | std::ios::trunc);
        std::vector<uint8_t> stub(512, 0);
        stub[510] = 0x55;
        stub[511] = 0xAA;
        ofs.write(reinterpret_cast<const char*>(stub.data()), static_cast<std::streamsize>(stub.size()));
    }

    ViewWizard& wiz = ViewWizard::GetInstance();
    wiz.Reset();
    ASSERT_TRUE(wiz.SelectArchetype(KioskArchetypes::DX2_486));
    wiz.SetDisplayName("Imported 486 Rig");
    wiz.SetBootMode("REAL_HDD_BOOT");
    wiz.SetHddSizeMb(250);
    wiz.SetPrimaryHddPath(existing_hdd);
    wiz.SetCreateBlankHddOnDisk(false);

    // Nonexistent installer media ID must fail atomically without persisting an orphan machine
    wiz.SetInstallerFloppyMediaId("nonexistent-media-uuid");
    std::string id_fail, err;
    EXPECT_FALSE(wiz.CommitMachine(id_fail, err));
    EXPECT_TRUE(KioskDatabase::GetInstance().GetAllMachines().empty());

    // Valid import must succeed with empty primary_hdd_chs (preserving native geometry) and os_installed=true
    wiz.SetInstallerFloppyMediaId("");
    std::string id_ok;
    ASSERT_TRUE(wiz.CommitMachine(id_ok, err)) << err;
    MachineRecord persisted;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMachine(id_ok, persisted));
    EXPECT_TRUE(persisted.primary_hdd_chs.empty());
    EXPECT_TRUE(persisted.os_installed);

    // Optional explicit CHS override for imported HDD must be preserved without slider overwrite
    wiz.Reset();
    ASSERT_TRUE(wiz.SelectArchetype(KioskArchetypes::DX2_486));
    wiz.SetDisplayName("Imported Custom CHS Rig");
    wiz.SetBootMode("REAL_HDD_BOOT");
    wiz.SetCreateBlankHddOnDisk(false);
    wiz.SetPrimaryHddPath(existing_hdd);
    wiz.SetCustomHddChsOverride("1015,16,63");
    wiz.SetHddSizeMb(250);
    std::string id_chs;
    ASSERT_TRUE(wiz.CommitMachine(id_chs, err)) << err;
    MachineRecord persisted_chs;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMachine(id_chs, persisted_chs));
    EXPECT_EQ(persisted_chs.primary_hdd_chs, "1015,16,63");

    // Atomic transaction test: 1st binding valid, 2nd binding invalid -> rolls back machine & 1st binding
    MediaPoolRecord valid_flp;
    valid_flp.id = "med-atomic-flp";
    valid_flp.file_path = "atomic_flp.img";
    valid_flp.media_type = "FLOPPY_35_1440K";
    valid_flp.label_title = "Atomic Floppy";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMedia(valid_flp));

    MachineRecord atomic_mach;
    atomic_mach.id = "mach-atomic-rollback";
    atomic_mach.display_name = "Atomic Test";
    atomic_mach.archetype_id = KioskArchetypes::DX_386;
    atomic_mach.archetype_tier = 3;
    atomic_mach.boot_mode = "INTEGRATED_DOS";
    std::vector<CaddyBindingRecord> bad_bindings;
    CaddyBindingRecord good_b; good_b.media_id = valid_flp.id; good_b.target_drive = "A"; good_b.slot_order = 1;
    CaddyBindingRecord bad_b;  bad_b.media_id = "missing-cd-id"; bad_b.target_drive = "D"; bad_b.slot_order = 1;
    bad_bindings.push_back(good_b);
    bad_bindings.push_back(bad_b);
    EXPECT_FALSE(KioskDatabase::GetInstance().InsertMachineWithBindings(atomic_mach, bad_bindings));
    MachineRecord rollback_probe;
    EXPECT_FALSE(KioskDatabase::GetInstance().GetMachine(atomic_mach.id, rollback_probe));
    EXPECT_TRUE(KioskDatabase::GetInstance().GetCaddyBindingsForMachine(atomic_mach.id).empty());

    // Partitioned-but-unformatted HDD (0x55AA signature, no valid FAT) + CD-ROM on D: -> BOOT -el-torito D:
    MediaPoolRecord cd_rec;
    cd_rec.id = "med-eltorito-cd";
    cd_rec.file_path = "freedos_install.iso";
    cd_rec.media_type = "CDROM_ISO";
    cd_rec.label_title = "FreeDOS 1.3 LiveCD";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMedia(cd_rec));

    MachineRecord unfmt_mach;
    unfmt_mach.id = "mach-unformatted-cd";
    unfmt_mach.display_name = "Unformatted CD Install Rig";
    unfmt_mach.archetype_id = KioskArchetypes::DX_386;
    unfmt_mach.archetype_tier = 3;
    unfmt_mach.boot_mode = "REAL_HDD_BOOT";
    unfmt_mach.os_installed = false;
    unfmt_mach.primary_hdd_path = existing_hdd; // has 0x55AA but no valid FAT BPB
    std::vector<CaddyBindingRecord> cd_bindings;
    CaddyBindingRecord cd_b; cd_b.media_id = cd_rec.id; cd_b.target_drive = "D"; cd_b.slot_order = 1;
    cd_bindings.push_back(cd_b);
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMachineWithBindings(unfmt_mach, cd_bindings));

    KioskManager::GetInstance().Initialize("", "");
    ASSERT_TRUE(KioskManager::GetInstance().RequestLaunchMachine(unfmt_mach.id));
    std::string cd_conf;
    ASSERT_TRUE(KioskManager::GetInstance().SynthesizeAndPrepareLaunch(cd_conf));
    EXPECT_NE(std::string::npos, cd_conf.find("BOOT -el-torito D:"));
    EXPECT_EQ(std::string::npos, cd_conf.find("BOOT -l c"));
    KioskManager::GetInstance().Shutdown();

    KioskDatabase::GetInstance().Close();
    std::remove(existing_hdd.c_str());
}

TEST(KioskInputRouterTest, ClearsHeldStickOnDisconnectAndSuppressesConfirmWhenFocused) {
    KioskInputRouter& router = KioskInputRouter::GetInstance();
    router.ClearPendingActions();

    // Deflect stick on controller instance 7
    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_CONTROLLERAXISMOTION;
    ev.caxis.which = 7;
    ev.caxis.axis = SDL_CONTROLLER_AXIS_LEFTX;
    ev.caxis.value = 24000;
    router.ProcessSDLEvent(ev, false, 1000, false, false);
    KioskInputAction act = KioskInputAction::NONE;
    ASSERT_TRUE(router.PollAction(act));
    EXPECT_EQ(act, KioskInputAction::NAV_RIGHT);

    // Physical release while actions are suppressed must clear held state so UpdateRepeat stops
    ev.caxis.value = 0;
    router.ProcessSDLEvent(ev, false, 1100, true /* suppress_actions */, false);
    router.UpdateRepeat(2000, false);
    EXPECT_EQ(router.GetPendingActionCount(), 0u);

    // Multi-controller same-direction test: controllers 7 and 8 both hold left;
    // disconnecting 7 must preserve 8's repeat!
    ev.caxis.which = 7;
    ev.caxis.value = -24000;
    router.ProcessSDLEvent(ev, false, 2100, false, false);
    ASSERT_TRUE(router.PollAction(act));
    EXPECT_EQ(act, KioskInputAction::NAV_LEFT);

    ev.caxis.which = 8;
    ev.caxis.value = -24000;
    router.ProcessSDLEvent(ev, false, 2150, false, false);
    ASSERT_TRUE(router.PollAction(act));
    EXPECT_EQ(act, KioskInputAction::NAV_LEFT);

    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_CONTROLLERDEVICEREMOVED;
    ev.cdevice.which = 7;
    router.ProcessSDLEvent(ev, false, 2200, false, false);

    // Controller 8 must still emit NAV_LEFT on UpdateRepeat
    router.UpdateRepeat(2600, false);
    ASSERT_TRUE(router.PollAction(act));
    EXPECT_EQ(act, KioskInputAction::NAV_LEFT);

    // Disconnect controller 8 -> repeats stop
    ev.cdevice.which = 8;
    router.ProcessSDLEvent(ev, false, 2650, false, false);
    router.ClearPendingActions();
    router.UpdateRepeat(3000, false);
    EXPECT_EQ(router.GetPendingActionCount(), 0u);

    // Enter/A with suppress_confirm=true must NOT queue ACTION_CONFIRM
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_RETURN;
    EXPECT_FALSE(router.ProcessSDLEvent(ev, false, 3100, false, true /* suppress_confirm */));
    EXPECT_EQ(router.GetPendingActionCount(), 0u);

    // Raw unmanaged joystick button 0 with suppress_confirm=true MUST still queue ACTION_CONFIRM
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_JOYBUTTONDOWN;
    ev.jbutton.which = 99;
    ev.jbutton.button = 0;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 3200, false, true /* suppress_confirm */));
    ASSERT_TRUE(router.PollAction(act));
    EXPECT_EQ(act, KioskInputAction::ACTION_CONFIRM);

    // Raw unmanaged joystick button 1 (ACTION_BACK) must pass through suppress_actions so
    // users can dismiss modals, while button 0/2 remain suppressed while a modal is open.
    ev.jbutton.button = 2;
    EXPECT_FALSE(router.ProcessSDLEvent(ev, false, 3210, true /* suppress_actions */, false));
    ev.jbutton.button = 1;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 3220, true /* suppress_actions */, false));
    ASSERT_TRUE(router.PollAction(act));
    EXPECT_EQ(act, KioskInputAction::ACTION_BACK);

    // Multi-joystick independent hat tracking: joystick 7 and joystick 8 both pressing RIGHT
    // must each produce NAV_RIGHT independently.
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_JOYHATMOTION;
    ev.jhat.which = 7;
    ev.jhat.hat = 0;
    ev.jhat.value = SDL_HAT_RIGHT;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 3300, false, false));
    ev.jhat.which = 8;
    EXPECT_TRUE(router.ProcessSDLEvent(ev, false, 3310, false, false));
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::NAV_RIGHT);
    ASSERT_TRUE(router.PollAction(act)); EXPECT_EQ(act, KioskInputAction::NAV_RIGHT);

    // Disconnect both joysticks to clear hat state
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_JOYDEVICEREMOVED;
    ev.jdevice.which = 7;
    router.ProcessSDLEvent(ev, false, 3320, false, false);
    ev.jdevice.which = 8;
    router.ProcessSDLEvent(ev, false, 3330, false, false);
}

TEST(KioskWizardTest, ValidatesChsOverrideCreatesParentDirsAndHandlesFormattedPendingInstall) {
    // 1. Strict CHS override validation (#23)
    std::string norm;
    EXPECT_TRUE(ViewWizard::ParseAndNormalizeChs("", norm));
    EXPECT_TRUE(norm.empty());
    EXPECT_TRUE(ViewWizard::ParseAndNormalizeChs("  1015 , 16 , 63  ", norm));
    EXPECT_EQ(norm, "1015,16,63");
    EXPECT_FALSE(ViewWizard::ParseAndNormalizeChs("abc", norm));
    EXPECT_FALSE(ViewWizard::ParseAndNormalizeChs("1,2,3 -fs fat", norm));
    EXPECT_FALSE(ViewWizard::ParseAndNormalizeChs("0,16,63", norm));
    EXPECT_FALSE(ViewWizard::ParseAndNormalizeChs("100,256,63", norm));
    EXPECT_FALSE(ViewWizard::ParseAndNormalizeChs("100,16,64", norm));

    // 2. Parent directory creation + distinct ALREADY_EXISTS status (#22)
    std::string temp_dir = "/tmp/";
#if defined(_WIN32)
    if (const char* td = std::getenv("TEMP")) {
        temp_dir = td;
        if (!temp_dir.empty() && temp_dir.back() != '\\' && temp_dir.back() != '/') temp_dir += '\\';
    }
#endif
    const std::string nested_dir = temp_dir + "kiosk_test_nested_dir";
    const std::string nested_hdd = nested_dir + "/nested_blank.img";
    std::remove(nested_hdd.c_str());

    std::string created_chs;
    EXPECT_EQ(ViewWizard::CreateBlankHddImageEx(nested_hdd, 20, created_chs), HddCreateStatus::OK);
    EXPECT_EQ(ViewWizard::CreateBlankHddImageEx(nested_hdd, 20, created_chs), HddCreateStatus::ALREADY_EXISTS);
    std::remove(nested_hdd.c_str());

    // 3. Formatted FAT HDD with !os_installed + bound installer media must still boot installer (#17)
    ASSERT_TRUE(KioskDatabase::GetInstance().Open(":memory:"));
    std::vector<uint8_t> fat_img;
    ASSERT_TRUE(BootInjector::GetInstance().BuildFat12ImageInMemory({}, fat_img));
    const std::string fmt_hdd = temp_dir + "kiosk_test_formatted_pending.img";
    {
        std::ofstream ofs(fmt_hdd.c_str(), std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(fat_img.data()), static_cast<std::streamsize>(fat_img.size()));
    }

    MediaPoolRecord flp;
    flp.id = "med-pending-flp";
    flp.file_path = "dos_setup1.img";
    flp.media_type = "FLOPPY_35_1440K";
    flp.label_title = "DOS Setup 1";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMedia(flp));

    MachineRecord mach;
    mach.id = "mach-formatted-pending";
    mach.display_name = "Interrupted Install Rig";
    mach.archetype_id = KioskArchetypes::DX_386;
    mach.archetype_tier = 3;
    mach.boot_mode = "REAL_HDD_BOOT";
    mach.os_installed = false;
    mach.primary_hdd_path = fmt_hdd;
    std::vector<CaddyBindingRecord> b_list;
    CaddyBindingRecord b; b.media_id = flp.id; b.target_drive = "A"; b.slot_order = 1;
    b_list.push_back(b);
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMachineWithBindings(mach, b_list));

    KioskManager::GetInstance().Initialize("", "");
    ASSERT_TRUE(KioskManager::GetInstance().RequestLaunchMachine(mach.id));
    std::string conf;
    ASSERT_TRUE(KioskManager::GetInstance().SynthesizeAndPrepareLaunch(conf));
    EXPECT_NE(std::string::npos, conf.find("BOOT -l a"));
    EXPECT_EQ(std::string::npos, conf.find("BOOT -l c"));
    KioskManager::GetInstance().Shutdown();
    KioskDatabase::GetInstance().Close();
    std::remove(fmt_hdd.c_str());
}

TEST(KioskCarouselTest, ValidatesDriveMediaCompatibilityAndComputesMaxSlotOrder) {
    EXPECT_TRUE(ViewCarousel::IsMediaCompatibleWithDrive("FLOPPY_35_1440K", "A"));
    EXPECT_TRUE(ViewCarousel::IsMediaCompatibleWithDrive("FLOPPY_525_360K", "B"));
    EXPECT_FALSE(ViewCarousel::IsMediaCompatibleWithDrive("CDROM_ISO", "A"));
    EXPECT_FALSE(ViewCarousel::IsMediaCompatibleWithDrive("CDROM_CUE", "B"));
    EXPECT_TRUE(ViewCarousel::IsMediaCompatibleWithDrive("CDROM_ISO", "D"));
    EXPECT_TRUE(ViewCarousel::IsMediaCompatibleWithDrive("CDROM_CUE", "E"));
    EXPECT_FALSE(ViewCarousel::IsMediaCompatibleWithDrive("FLOPPY_35_1440K", "D"));

    // Deleting slots 1 & 2 from [1,2,3,4] leaves [3,4] -> next slot for Drive A must be 5, not 3
    std::vector<CaddyBindingRecord> bindings;
    CaddyBindingRecord b3; b3.target_drive = "A"; b3.slot_order = 3; bindings.push_back(b3);
    CaddyBindingRecord b4; b4.target_drive = "A"; b4.slot_order = 4; bindings.push_back(b4);
    CaddyBindingRecord d1; d1.target_drive = "D"; d1.slot_order = 1; bindings.push_back(d1);
    EXPECT_EQ(ViewCarousel::ComputeNextSlotOrder(bindings, "A"), 5);
    EXPECT_EQ(ViewCarousel::ComputeNextSlotOrder(bindings, "D"), 2);
    EXPECT_EQ(ViewCarousel::ComputeNextSlotOrder(bindings, "B"), 1);
}

TEST(KioskWatchdogTest, ResolvesPreviewAndRendersAllShellStatesHeadless) {
    ASSERT_TRUE(KioskDatabase::GetInstance().Open(":memory:"));
    KioskManager::GetInstance().Initialize("", "");

    MachineRecord mach;
    mach.id = "mach-watchdog-1";
    mach.display_name = "My 1992 386 Rig";
    mach.archetype_id = KioskArchetypes::DX_386;
    mach.archetype_tier = 3;
    mach.boot_mode = "REAL_HDD_BOOT";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMachine(mach));

    KioskConfigRecord cfg = KioskManager::GetInstance().GetConfig();
    cfg.default_boot_action = "SPECIFIC_MACHINE";
    cfg.default_target_uuid = mach.id;
    cfg.boot_timeout_sec = 25;
    cfg.theme_name = "crt_green";
    ASSERT_TRUE(KioskDatabase::GetInstance().UpdateConfig(cfg));
    ASSERT_TRUE(KioskManager::GetInstance().ReloadConfig());

    KioskUI& ui = KioskUI::GetInstance();
    ASSERT_TRUE(ui.InitializeContext());
    ui.InvalidateCatalogCache();
    ui.EnsureCatalogCacheLoaded();
    EXPECT_EQ(ui.GetThemePalette().name, "crt_green");

    ViewWatchdog& wd = ViewWatchdog::GetInstance();
    wd.RefreshPreview();
    EXPECT_EQ(wd.GetCachedPreview().title, "My 1992 386 Rig");
    EXPECT_EQ(wd.GetCachedPreview().boot_mode_badge, "REAL HDD BOOT");
    EXPECT_EQ(wd.GetCachedPreview().total_timeout_sec, 25);

    // Exercise full headless ImGui rendering across Watchdog, all 6 Hub tabs, and Runtime Overlay
    ui.NewFrame();
    ui.RenderCountdownBanner(12.5f);
    ui.RenderFrame();

    const KioskHubTab tabs[6] = {
        KioskHubTab::GAMES, KioskHubTab::APPLICATIONS, KioskHubTab::MACHINES,
        KioskHubTab::MEDIA_POOL, KioskHubTab::NEW_MACHINE, KioskHubTab::SETTINGS
    };
    for (int i = 0; i < 6; ++i) {
        ui.SetActiveTab(tabs[i]);
        ui.NewFrame();
        ui.RenderInteractiveShell();
        ui.RenderFrame();
    }

    // Verify ImGui keyboard+gamepad nav flags are cleared on carousel tabs and restored
    // when entering STATE_RUNTIME_OVERLAY or STATE_COUNTDOWN_IDLE (#25)
    ui.SetActiveTab(KioskHubTab::GAMES);
    ui.UpdateImGuiNavFlagsForState(KioskState::STATE_SHELL_INTERACTIVE);
    EXPECT_EQ(ImGui::GetIO().ConfigFlags & (ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad), 0);

    ui.UpdateImGuiNavFlagsForState(KioskState::STATE_RUNTIME_OVERLAY);
    EXPECT_NE(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard, 0);
    EXPECT_NE(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad, 0);

    ui.ShowToast("Phase 5 Verification Toast", 2.0f);
    EXPECT_EQ(ui.GetActiveToastMessage(), "Phase 5 Verification Toast");

    KioskManager::GetInstance().Shutdown();
    KioskDatabase::GetInstance().Close();
}

#endif // defined(C_DEBUG) && defined(C_KIOSK)
