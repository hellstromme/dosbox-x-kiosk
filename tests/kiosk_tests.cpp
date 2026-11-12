#include "config.h"

#if defined(C_DEBUG) && defined(C_KIOSK)

#include "kiosk_types.h"
#include "kiosk_db.h"
#include "machine/archetypes.h"
#include "machine/config_synthesizer.h"
#include "machine/kioskext_bin.h"
#include "machine/boot_injector.h"
#include "ui/kiosk_ui.h"
#include "kiosk_main.h"
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

#endif // defined(C_DEBUG) && defined(C_KIOSK)
