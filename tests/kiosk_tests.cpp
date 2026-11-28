#include "config.h"

#if defined(C_DEBUG) && defined(C_KIOSK)

#include "kiosk_types.h"
#include "kiosk_db.h"
#include "kiosk_host.h"
#include "ingest/usb_bridge.h"
#include "machine/archetypes.h"
#include "machine/config_synthesizer.h"
#include "machine/kioskext_bin.h"
#include "machine/boot_injector.h"
#include "ingest/signature_matcher.h"
#include "ingest/image_crawler.h"
#include "ingest/conf_translator.h"
#include "ingest/archive_importer.h"
#include <zlib.h>
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
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>
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
    EXPECT_NE(std::string::npos, conf.find("\"C:\\CUSTOM\\MY GAME.EXE\" -v"));
    EXPECT_NE(std::string::npos, conf.find("EXIT"));
}

TEST(KioskSynthesizerTest, LaunchUsesAbsoluteExecutablePath) {
    MachineRecord m;
    m.id = "mach-dos-abs";
    m.archetype_id = KioskArchetypes::DX_386;
    m.boot_mode = "INTEGRATED_DOS";

    CatalogItemRecord item;
    item.id = "game-abs";
    item.host_container_dir = "C:\\games\\abs";
    item.guest_working_dir = "D:\\";
    item.guest_executable = "SETUP.EXE";

    const std::string conf = ConfigSynthesizer::GetInstance().SynthesizeAutoexec(m, &item, {});
    EXPECT_NE(std::string::npos, conf.find("D:\nD:\\SETUP.EXE\n"));
    EXPECT_EQ(std::string::npos, conf.find("CD "));

    // Bare names from imported confs ("tie") stay launchable, pinned to their directory,
    // and legitimate switches and quoted arguments are emitted verbatim.
    CatalogItemRecord bare = item;
    bare.guest_working_dir = "C:\\TIE\\";
    bare.guest_executable = "TIE";
    bare.guest_arguments = "/S:5 -skill 4 \"a b\"";
    bare.return_on_exit = true;
    const std::string bare_conf = ConfigSynthesizer::GetInstance().SynthesizeAutoexec(m, &bare, {});
    EXPECT_NE(std::string::npos, bare_conf.find("C:\nCD \\TIE\nC:\\TIE\\TIE /S:5 -skill 4 \"a b\"\nEXIT\n"));
    EXPECT_EQ(std::string::npos, bare_conf.find("rejected"));
}

TEST(KioskGuestShutdownTest, PendingRequestIsRaisedOnceAndClearable) {
    Kiosk_ClearPendingGuestShutdown();
    EXPECT_NO_THROW(Kiosk_RaisePendingGuestShutdown());

    // Overlay "Quit to Kiosk" posts instead of throwing from inside GFX_Events()
    Kiosk_PostGuestShutdown(KioskShutdownReason::USER_OVERLAY_ABORT);
    bool raised = false;
    try {
        Kiosk_RaisePendingGuestShutdown();
    } catch (const KioskGuestShutdownException& ex) {
        raised = true;
        EXPECT_EQ(KioskShutdownReason::USER_OVERLAY_ABORT, ex.reason);
    }
    EXPECT_TRUE(raised);
    EXPECT_NO_THROW(Kiosk_RaisePendingGuestShutdown());

    // A request left over from a torn-down session is discarded, not raised later
    Kiosk_PostGuestShutdown(KioskShutdownReason::PORT_8900_SHUTDOWN);
    Kiosk_ClearPendingGuestShutdown();
    EXPECT_EQ(KioskShutdownReason::NONE, Kiosk_GetPendingGuestShutdown());
    EXPECT_NO_THROW(Kiosk_RaisePendingGuestShutdown());
}

TEST(KioskSynthesizerTest, HostileImportedFieldsAreNotEmitted) {
    MachineRecord m;
    m.id = "mach-dos-hostile";
    m.archetype_id = KioskArchetypes::DX_386;
    m.boot_mode = "INTEGRATED_DOS";

    CatalogItemRecord base;
    base.id = "game-hostile";
    base.host_container_dir = "C:\\games\\hostile";
    base.guest_working_dir = "C:\\GAME";
    base.guest_executable = "GAME.EXE";
    base.return_on_exit = true;

    struct Case { const char* name; CatalogItemRecord item; };
    std::vector<Case> cases;
    { CatalogItemRecord it = base; it.guest_arguments = "-x | MOUNT H C:\\"; cases.push_back({"pipe in args", it}); }
    { CatalogItemRecord it = base; it.guest_arguments = "%PATH%"; cases.push_back({"batch expansion", it}); }
    { CatalogItemRecord it = base; it.guest_arguments = "> C:\\OUT.TXT"; cases.push_back({"redirection", it}); }
    { CatalogItemRecord it = base; it.guest_arguments = "\"unbalanced"; cases.push_back({"unbalanced quote", it}); }
    { CatalogItemRecord it = base; it.guest_arguments = "a\nMOUNT H C:\\"; cases.push_back({"newline in args", it}); }
    { CatalogItemRecord it = base; it.guest_working_dir = "C:\\GAME\nMOUNT H C:\\"; cases.push_back({"newline in dir", it}); }
    { CatalogItemRecord it = base; it.guest_working_dir = "C:\\GAME&MOUNT"; cases.push_back({"ampersand in dir", it}); }
    { CatalogItemRecord it = base; it.guest_executable = "Z:\\MOUNT.COM"; cases.push_back({"drive-qualified exe", it}); }
    { CatalogItemRecord it = base; it.guest_executable = "CONFIG -set"; cases.push_back({"extensionless exe with options", it}); }
    { CatalogItemRecord it = base; it.guest_working_dir = "Z:\\"; it.guest_executable = "MOUNT.COM";
      it.guest_arguments = "H /"; cases.push_back({"Z: working drive", it}); }
    { CatalogItemRecord it = base; it.guest_working_dir = "Y:\\"; cases.push_back({"unmounted working drive", it}); }

    for (size_t i = 0; i < cases.size(); ++i) {
        const std::string conf = ConfigSynthesizer::GetInstance().SynthesizeAutoexec(m, &cases[i].item, {});
        EXPECT_EQ(std::string::npos, conf.find("MOUNT H")) << cases[i].name;
        EXPECT_EQ(std::string::npos, conf.find("MOUNT.COM")) << cases[i].name;
        EXPECT_EQ(std::string::npos, conf.find("%PATH%")) << cases[i].name;
        EXPECT_EQ(std::string::npos, conf.find("OUT.TXT")) << cases[i].name;
        EXPECT_EQ(std::string::npos, conf.find("GAME.EXE")) << cases[i].name;
        // A rejected launch returns to the kiosk shell instead of leaving a DOS prompt
        EXPECT_NE(std::string::npos, conf.find("launch command rejected (unsafe characters).\nEXIT\n")) << cases[i].name;
    }

    // Host paths that would break out of the quoted MOUNT/IMGMOUNT argument are dropped.
    CatalogItemRecord quoted = base;
    quoted.host_container_dir = "C:\\games\\x\" & MOUNT H \"C:\\";
    CaddyBindingRecord bad;
    bad.target_drive = "A";
    bad.slot_order = 1;
    bad.resolved_file_path = "C:\\disk%TEMP%.img";
    const std::string conf = ConfigSynthesizer::GetInstance().SynthesizeAutoexec(m, &quoted, {bad});
    EXPECT_EQ(std::string::npos, conf.find("MOUNT C"));
    EXPECT_EQ(std::string::npos, conf.find("IMGMOUNT A"));
    EXPECT_EQ(std::string::npos, conf.find("MOUNT H"));

    // Non-numeric CHS overrides are ignored instead of being spliced into IMGMOUNT.
    MachineRecord hdd;
    hdd.id = "mach-hdd-chs";
    hdd.archetype_id = KioskArchetypes::DX_386;
    hdd.boot_mode = "REAL_HDD_BOOT";
    hdd.primary_hdd_path = "C:\\kiosk\\hdd_c.img";
    hdd.primary_hdd_chs = "1024,16,63 -ro";
    const std::string hdd_conf = ConfigSynthesizer::GetInstance().SynthesizeAutoexec(hdd, nullptr, {});
    EXPECT_NE(std::string::npos, hdd_conf.find("IMGMOUNT 2 \"C:\\kiosk\\hdd_c.img\" -t hdd -fs none -ide 1m"));
    EXPECT_EQ(std::string::npos, hdd_conf.find("-ro"));
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
    Kiosk_ClearPendingGuestShutdown();

    // Partial or mismatched write should not throw
    EXPECT_NO_THROW({
        bochs_port_8900_write(0x8900, 'S', 1);
        bochs_port_8900_write(0x8900, 'h', 1);
        bochs_port_8900_write(0x8900, 'X', 1);
    });

    EXPECT_EQ(KioskShutdownReason::NONE, Kiosk_GetPendingGuestShutdown());

    // Complete "Shutdown" sequence on port 0x8900 must NOT throw (the handler can be
    // called from dynamic-core generated code); it posts a deferred request instead.
    const char* seq = "Shutdown";
    EXPECT_NO_THROW({
        for (size_t i = 0; seq[i] != '\0'; ++i) {
            bochs_port_8900_write(0x8900, static_cast<Bitu>(seq[i]), 1);
        }
    });
    EXPECT_EQ(KioskShutdownReason::PORT_8900_SHUTDOWN, Kiosk_GetPendingGuestShutdown());

    // normal_loop() raises the posted request from a safe frame, clearing it.
    bool caught_shutdown = false;
    try {
        Kiosk_RaisePendingGuestShutdown();
    } catch (const KioskGuestShutdownException& ex) {
        caught_shutdown = true;
        EXPECT_EQ(KioskShutdownReason::PORT_8900_SHUTDOWN, ex.reason);
        mgr.OnGuestShutdown(ex.reason);
    }

    EXPECT_TRUE(caught_shutdown);
    EXPECT_EQ(KioskShutdownReason::NONE, Kiosk_GetPendingGuestShutdown());
    EXPECT_NO_THROW(Kiosk_RaisePendingGuestShutdown());
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
    Kiosk_ClearPendingGuestShutdown();
    bool trapped = false;
    try {
        for (size_t i = 0; i < 8; ++i) {
            bochs_port_8900_write(0x8900, KIOSKEXT_COM_BYTES[49 + i], 1);
        }
        Kiosk_RaisePendingGuestShutdown();
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

    mgr.OnGuestShutdown(KioskShutdownReason::SHELL_EXIT); // End the previous session first
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

    mgr.OnGuestShutdown(KioskShutdownReason::SHELL_EXIT); // End the item session before booting the machine
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

    // Launch the item through the real lifecycle, then enter STATE_RUNTIME_OVERLAY
    KioskManager& km = KioskManager::GetInstance();
    ASSERT_TRUE(km.Initialize("", "")); // Keeps the fixture database open
    ASSERT_TRUE(km.RequestLaunchItem(item.id));
    std::string session_conf;
    ASSERT_TRUE(km.SynthesizeAndPrepareLaunch(session_conf));
    ASSERT_EQ(KioskState::STATE_EMULATION_RUNNING, km.GetState());
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
    KioskManager::GetInstance().CancelWatchdog(); // COUNTDOWN_IDLE -> SHELL_INTERACTIVE

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

// ============================================================================
// Phase 6: Non-Destructive FAT Disk Crawler, Shortcut & Icon Extractors
// ============================================================================

namespace {

void TestWriteLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void TestWriteLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint8_t TestFat83Checksum(const uint8_t name11[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) {
        sum = static_cast<uint8_t>(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + name11[i]);
    }
    return sum;
}

void TestWriteDirEntry83(
    uint8_t* slot,
    const char name11[11],
    uint8_t attr,
    uint32_t first_cluster,
    uint32_t file_size
) {
    std::memset(slot, 0, 32);
    std::memcpy(slot, name11, 11);
    slot[11] = attr;
    TestWriteLe16(slot + 20, static_cast<uint16_t>((first_cluster >> 16) & 0xFFFFu));
    TestWriteLe16(slot + 26, static_cast<uint16_t>(first_cluster & 0xFFFFu));
    TestWriteLe32(slot + 28, file_size);
}

void TestWriteSingleLfnEntry(
    uint8_t* slot,
    const std::string& ascii_lfn,
    uint8_t short_checksum
) {
    std::memset(slot, 0xFF, 32);
    slot[0] = 0x41; // Last & first LFN entry (ord = 1 | 0x40)
    slot[11] = 0x0F;
    slot[12] = 0x00;
    slot[13] = short_checksum;
    slot[26] = 0x00;
    slot[27] = 0x00;

    uint16_t chars[13];
    for (int i = 0; i < 13; ++i) {
        if (static_cast<size_t>(i) < ascii_lfn.size()) {
            chars[i] = static_cast<uint8_t>(ascii_lfn[static_cast<size_t>(i)]);
        } else if (static_cast<size_t>(i) == ascii_lfn.size()) {
            chars[i] = 0x0000;
        } else {
            chars[i] = 0xFFFF;
        }
    }
    for (int k = 0; k < 5; ++k)  TestWriteLe16(slot + 1 + k * 2, chars[k]);
    for (int k = 0; k < 6; ++k)  TestWriteLe16(slot + 14 + k * 2, chars[5 + k]);
    for (int k = 0; k < 2; ++k)  TestWriteLe16(slot + 28 + k * 2, chars[11 + k]);
}

std::vector<uint8_t> BuildSynthetic4bppIconDib(int w, int h) {
    // 40-byte BITMAPINFOHEADER + 16*4 palette + XOR (4bpp) + AND (1bpp)
    const size_t xor_stride = ((static_cast<size_t>(w) * 4U + 31U) / 32U) * 4U;
    const size_t and_stride = ((static_cast<size_t>(w) + 31U) / 32U) * 4U;
    const size_t total = 40U + 64U + xor_stride * h + and_stride * h;
    std::vector<uint8_t> dib(total, 0);

    TestWriteLe32(dib.data() + 0, 40);
    TestWriteLe32(dib.data() + 4, static_cast<uint32_t>(w));
    TestWriteLe32(dib.data() + 8, static_cast<uint32_t>(h * 2));
    TestWriteLe16(dib.data() + 12, 1);
    TestWriteLe16(dib.data() + 14, 4);
    TestWriteLe32(dib.data() + 32, 16);

    // Palette entry 1 = bright red (B=0, G=0, R=255)
    dib[40 + 4 + 0] = 0x00;
    dib[40 + 4 + 1] = 0x00;
    dib[40 + 4 + 2] = 0xFF;

    // Fill XOR pixels with palette index 1
    std::memset(dib.data() + 104, 0x11, xor_stride * h);
    // Leave AND mask 0 (opaque), except top-left pixel transparent (bottom-up row h-1, bit 7)
    dib[104 + xor_stride * h + (h - 1) * and_stride] = 0x80;
    return dib;
}

std::vector<uint8_t> BuildSynthetic8bppIconDib(int w, int h) {
    const size_t xor_stride = ((static_cast<size_t>(w) * 8U + 31U) / 32U) * 4U;
    const size_t and_stride = ((static_cast<size_t>(w) + 31U) / 32U) * 4U;
    const size_t total = 40U + 256U * 4U + xor_stride * h + and_stride * h;
    std::vector<uint8_t> dib(total, 0);

    TestWriteLe32(dib.data() + 0, 40);
    TestWriteLe32(dib.data() + 4, static_cast<uint32_t>(w));
    TestWriteLe32(dib.data() + 8, static_cast<uint32_t>(h * 2));
    TestWriteLe16(dib.data() + 12, 1);
    TestWriteLe16(dib.data() + 14, 8);
    TestWriteLe32(dib.data() + 32, 256);

    // Palette entry 2 = bright green (B=0, G=255, R=0)
    dib[40 + 8 + 1] = 0xFF;
    std::memset(dib.data() + 40 + 1024, 0x02, xor_stride * h);
    return dib;
}

bool VerifyPngHeaderAndDimensions(const std::string& png_path, uint32_t expected_w, uint32_t expected_h) {
    std::FILE* f = std::fopen(png_path.c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[24];
    size_t n = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    if (n < 24) return false;
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(hdr, kSig, 8) != 0) return false;
    uint32_t w = (static_cast<uint32_t>(hdr[16]) << 24) | (static_cast<uint32_t>(hdr[17]) << 16) |
                 (static_cast<uint32_t>(hdr[18]) << 8) | hdr[19];
    uint32_t h = (static_cast<uint32_t>(hdr[20]) << 24) | (static_cast<uint32_t>(hdr[21]) << 16) |
                 (static_cast<uint32_t>(hdr[22]) << 8) | hdr[23];
    return (w == expected_w && h == expected_h);
}

// Helper to build a minimal MBR + FAT16 image on disk with custom directory layout
bool WriteSyntheticFat16HddImage(const std::string& out_path) {
    // 64 sectors MBR gap + 10000 sectors FAT16 partition (~5.1MB, > 4085 clusters)
    const uint32_t part_lba = 63;
    const uint32_t part_secs = 9000;
    const uint32_t total_secs = part_lba + part_secs;
    std::vector<uint8_t> img(static_cast<size_t>(total_secs) * 512U, 0);

    // 1. MBR at LBA 0
    img[510] = 0x55;
    img[511] = 0xAA;
    uint8_t* p0 = img.data() + 0x1BE;
    p0[0] = 0x80; // Active
    p0[4] = 0x06; // FAT16
    TestWriteLe32(p0 + 8, part_lba);
    TestWriteLe32(p0 + 12, part_secs);

    // 2. FAT16 VBR at part_lba: 1 sec/cluster, 1 reserved, 2 FATs x 36 secs, 256 root entries (16 secs)
    // Data sectors = 9000 - (1 + 72 + 16) = 8911 clusters (> 4085, < 65525 -> FAT16)
    uint8_t* vbr = img.data() + static_cast<size_t>(part_lba) * 512U;
    vbr[0] = 0xEB; vbr[1] = 0x3C; vbr[2] = 0x90;
    TestWriteLe16(vbr + 11, 512);
    vbr[13] = 1;
    TestWriteLe16(vbr + 14, 1);
    vbr[16] = 2;
    TestWriteLe16(vbr + 17, 256);
    TestWriteLe16(vbr + 19, static_cast<uint16_t>(part_secs));
    vbr[21] = 0xF8;
    TestWriteLe16(vbr + 22, 36);
    vbr[510] = 0x55; vbr[511] = 0xAA;

    const uint32_t fat_lba = part_lba + 1;
    const uint32_t root_lba = fat_lba + 72;
    const uint32_t data_lba = root_lba + 16;

    auto set_fat16 = [&](uint16_t clus, uint16_t val) {
        TestWriteLe16(img.data() + static_cast<size_t>(fat_lba) * 512U + clus * 2U, val);
    };
    auto clus_ptr = [&](uint16_t clus) -> uint8_t* {
        return img.data() + static_cast<size_t>(data_lba + (clus - 2)) * 512U;
    };

    set_fat16(0, 0xFFF8);
    set_fat16(1, 0xFFFF);

    // Root entries:
    //  - \DOS (cluster 2) -> pruned system dir
    //  - \DOOM2 (cluster 3) -> game dir with DOOM2.EXE (clus 4), SETUP.EXE (clus 5), DOS4GW.EXE (clus 6)
    //  - \MYAPP (cluster 7, with VFAT LFN "My Retro App") -> MYAPP.BAT (clus 8) calling CORE.EXE (clus 9)
    uint8_t* root = img.data() + static_cast<size_t>(root_lba) * 512U;
    TestWriteDirEntry83(root + 0,  "DOS        ", 0x10, 2, 0);
    TestWriteDirEntry83(root + 32, "DOOM2      ", 0x10, 3, 0);

    uint8_t lfn_short[11];
    std::memcpy(lfn_short, "MYAPP      ", 11);
    TestWriteSingleLfnEntry(root + 64, "My Retro App", TestFat83Checksum(lfn_short));
    TestWriteDirEntry83(root + 96, "MYAPP      ", 0x10, 7, 0);

    // Cluster 2 (\DOS): EDIT.COM (cluster 10) - should be pruned
    set_fat16(2, 0xFFFF);
    TestWriteDirEntry83(clus_ptr(2), "EDIT    COM", 0x20, 10, 16);
    set_fat16(10, 0xFFFF);
    std::memcpy(clus_ptr(10), "EDIT_DOS_UTILITY", 16);

    // Cluster 3 (\DOOM2): DOOM2.EXE (clus 4), SETUP.EXE (clus 5), DOS4GW.EXE (clus 6)
    set_fat16(3, 0xFFFF);
    TestWriteDirEntry83(clus_ptr(3) + 0,  "DOOM2   EXE", 0x20, 4, 256);
    TestWriteDirEntry83(clus_ptr(3) + 32, "SETUP   EXE", 0x20, 5, 128);
    TestWriteDirEntry83(clus_ptr(3) + 64, "DOS4GW  EXE", 0x20, 6, 128);

    set_fat16(4, 0xFFFF);
    uint8_t* doom = clus_ptr(4);
    doom[0] = 'M'; doom[1] = 'Z';
    TestWriteLe32(doom + 0x3C, 0x80);
    doom[0x80] = 'L'; doom[0x81] = 'E';
    std::memcpy(doom + 0x90, "Copyright (C) 1994 id Software.", 31);

    set_fat16(5, 0xFFFF);
    clus_ptr(5)[0] = 'M'; clus_ptr(5)[1] = 'Z';
    set_fat16(6, 0xFFFF);
    clus_ptr(6)[0] = 'M'; clus_ptr(6)[1] = 'Z';

    // Cluster 7 (\My Retro App): PLAY.BAT (clus 8) calling CORE.EXE (clus 9)
    set_fat16(7, 0xFFFF);
    const char* bat_txt = "@ECHO OFF\r\nCORE.EXE /VGA\r\n";
    TestWriteDirEntry83(clus_ptr(7) + 0,  "PLAY    BAT", 0x20, 8, static_cast<uint32_t>(std::strlen(bat_txt)));
    TestWriteDirEntry83(clus_ptr(7) + 32, "CORE    EXE", 0x20, 9, 128);
    set_fat16(8, 0xFFFF);
    std::memcpy(clus_ptr(8), bat_txt, std::strlen(bat_txt));
    set_fat16(9, 0xFFFF);
    clus_ptr(9)[0] = 'M'; clus_ptr(9)[1] = 'Z';

    std::FILE* f = std::fopen(out_path.c_str(), "wb");
    if (!f) return false;
    size_t written = std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    return written == img.size();
}

} // anonymous namespace

TEST(KioskCrawlerTest, Fat12SuperfloppyAndReadOnlyVfs) {
    // Synthesize a valid 1.44MB FAT12 dispatch floppy via BootInjector
    std::string img_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fat12.img";
    std::string bat = "@ECHO OFF\r\nECHO HELLO FAT12\r\n";
    DispatchFileEntry dfe;
    dfe.filename_8_3 = "KIOSKGO.BAT";
    dfe.data.assign(bat.begin(), bat.end());
    std::vector<uint8_t> fat12_bytes;
    ASSERT_TRUE(BootInjector::GetInstance().BuildFat12ImageInMemory({dfe}, fat12_bytes));
    std::FILE* f = std::fopen(img_path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(fat12_bytes.data(), 1, fat12_bytes.size(), f), fat12_bytes.size());
    std::fclose(f);

    FatReadOnlyVfs vfs;
    ASSERT_TRUE(vfs.OpenImageFile(img_path));
    EXPECT_TRUE(vfs.IsWriteProtected());
    EXPECT_EQ(vfs.GetFilesystemType(), FilesystemType::FAT12);
    EXPECT_EQ(vfs.GetPartitionLbaStart(), 0u);

    FatDirEntry entry;
    ASSERT_TRUE(vfs.FindPath("C:\\KIOSKGO.BAT", entry));
    EXPECT_FALSE(entry.is_directory);
    std::vector<uint8_t> content;
    ASSERT_TRUE(vfs.ReadFile(entry, content));
    std::string text(reinterpret_cast<const char*>(content.data()), content.size());
    EXPECT_NE(text.find("HELLO FAT12"), std::string::npos);

    vfs.Close();
    std::remove(img_path.c_str());
}

TEST(KioskCrawlerTest, Fat16MbrVfatLfnPruningAndScoring) {
    KioskDatabase::GetInstance().Open(":memory:");
    const std::string hdd_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fat16.img";
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd_path));

    FatReadOnlyVfs vfs;
    ASSERT_TRUE(vfs.OpenImageFile(hdd_path));
    EXPECT_EQ(vfs.GetFilesystemType(), FilesystemType::FAT16);
    EXPECT_EQ(vfs.GetPartitionLbaStart(), 63u);

    // Verify VFAT LFN reconstruction
    FatDirEntry lfn_dir;
    ASSERT_TRUE(vfs.FindPath("C:\\My Retro App", lfn_dir));
    EXPECT_TRUE(lfn_dir.is_directory);
    EXPECT_EQ(lfn_dir.name, "My Retro App");
    EXPECT_EQ(lfn_dir.short_name, "MYAPP");

    // Run full crawler on the synthetic image
    std::vector<ImageCrawler::CrawlResult> found = ImageCrawler::ScanMountedVfs(vfs);
    ASSERT_EQ(found.size(), 2U);

    bool saw_doom = false;
    bool saw_bat = false;
    for (size_t i = 0; i < found.size(); ++i) {
        EXPECT_NE(found[i].executable, "EDIT.COM");
        EXPECT_NE(found[i].executable, "SETUP.EXE");
        EXPECT_NE(found[i].executable, "DOS4GW.EXE");
        if (found[i].executable == "DOOM2.EXE") {
            saw_doom = true;
            EXPECT_EQ(found[i].release_year, 1994);
            EXPECT_EQ(found[i].developer, "id Software");
        }
        if (found[i].executable == "PLAY.BAT") {
            saw_bat = true;
            EXPECT_EQ(found[i].working_dir, "C:\\My Retro App");
        }
    }
    EXPECT_TRUE(saw_doom);
    EXPECT_TRUE(saw_bat);

    vfs.Close();
    std::remove(hdd_path.c_str());
    KioskDatabase::GetInstance().Close();
}

TEST(KioskCrawlerTest, Fat32MbrClusterChainAndCycleProtection) {
    // Build a minimal in-memory FAT32 disk image (via imageDiskMemory or temp file)
    // FAT32 requires >= 65525 clusters. We create a sparse/small raw file of 300 sectors
    // with BPB reporting 70000 clusters so FatReadOnlyVfs classifies FAT32 without allocating 35MB.
    const std::string img_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fat32.img";
    const uint32_t part_lba = 32;
    const uint32_t rsvd_secs = 32;
    const uint32_t fat_secs = 16;
    const uint32_t data_lba = part_lba + rsvd_secs + fat_secs; // 80
    std::vector<uint8_t> img(200U * 512U, 0);

    // MBR
    img[510] = 0x55; img[511] = 0xAA;
    img[0x1BE + 0] = 0x80;
    img[0x1BE + 4] = 0x0C; // FAT32 LBA
    TestWriteLe32(img.data() + 0x1BE + 8, part_lba);
    TestWriteLe32(img.data() + 0x1BE + 12, 70100);

    // FAT32 VBR
    uint8_t* vbr = img.data() + part_lba * 512U;
    vbr[0] = 0xEB; vbr[1] = 0x58; vbr[2] = 0x90;
    TestWriteLe16(vbr + 11, 512);
    vbr[13] = 1;
    TestWriteLe16(vbr + 14, rsvd_secs);
    vbr[16] = 1; // 1 FAT
    TestWriteLe16(vbr + 17, 0); // FAT32 root entries = 0
    TestWriteLe32(vbr + 32, 70100);
    TestWriteLe32(vbr + 36, fat_secs);
    TestWriteLe32(vbr + 44, 2); // Root cluster = 2
    vbr[510] = 0x55; vbr[511] = 0xAA;

    // FAT32 table at part_lba + rsvd_secs
    uint8_t* fat = img.data() + (part_lba + rsvd_secs) * 512U;
    TestWriteLe32(fat + 0 * 4, 0x0FFFFFF8u);
    TestWriteLe32(fat + 1 * 4, 0x0FFFFFFFu);
    TestWriteLe32(fat + 2 * 4, 0x0FFFFFFFu); // Root dir ends after cluster 2
    TestWriteLe32(fat + 3 * 4, 4);           // GOOD.TXT: cluster 3 -> cluster 4 -> EOC
    TestWriteLe32(fat + 4 * 4, 0x0FFFFFFFu);
    TestWriteLe32(fat + 5 * 4, 6);           // LOOP.BIN: cluster 5 -> 6 -> 5 (intentional cycle!)
    TestWriteLe32(fat + 6 * 4, 5);

    // Root dir at cluster 2 (data_lba + 0)
    uint8_t* root = img.data() + data_lba * 512U;
    TestWriteDirEntry83(root + 0,  "GOOD    TXT", 0x20, 3, 600);
    TestWriteDirEntry83(root + 32, "LOOP    BIN", 0x20, 5, 4096);

    // Cluster 3 + Cluster 4 payload
    std::memset(img.data() + (data_lba + 1) * 512U, 'A', 512);
    std::memset(img.data() + (data_lba + 2) * 512U, 'B', 512);

    std::FILE* f = std::fopen(img_path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(img.data(), 1, img.size(), f), img.size());
    std::fclose(f);

    FatReadOnlyVfs vfs;
    ASSERT_TRUE(vfs.OpenImageFile(img_path));
    EXPECT_EQ(vfs.GetFilesystemType(), FilesystemType::FAT32);

    FatDirEntry good_entry;
    ASSERT_TRUE(vfs.FindPath("\\GOOD.TXT", good_entry));
    std::vector<uint8_t> good_data;
    ASSERT_TRUE(vfs.ReadFile(good_entry, good_data));
    ASSERT_EQ(good_data.size(), 600U);
    EXPECT_EQ(good_data[0], 'A');
    EXPECT_EQ(good_data[512], 'B');

    // Cycle-protected read terminates cleanly after 2 unique clusters (1024 bytes)
    // and reports false because fewer than file_size (4096) bytes were readable.
    FatDirEntry loop_entry;
    ASSERT_TRUE(vfs.FindPath("\\LOOP.BIN", loop_entry));
    std::vector<uint8_t> loop_data;
    EXPECT_FALSE(vfs.ReadFile(loop_entry, loop_data));
    EXPECT_EQ(loop_data.size(), 1024U);

    vfs.Close();
    std::remove(img_path.c_str());
}

TEST(KioskCrawlerTest, Dos4gwStubStrippingFingerprintAndSignatureMatch) {
    KioskDatabase::GetInstance().Open(":memory:");

    // Build two synthetic 32KB EXEs with an IDENTICAL 26KB DOS/4GW stub,
    // differing only in their embedded LE payload at offset 26624 (0x6800).
    const size_t stub_size = 26624;
    std::vector<uint8_t> exe_a(stub_size + 4096, 0x90);
    exe_a[0] = 'M'; exe_a[1] = 'Z';
    TestWriteLe16(exe_a.data() + 0x02, 0);
    TestWriteLe16(exe_a.data() + 0x04, static_cast<uint16_t>(stub_size / 512));
    TestWriteLe32(exe_a.data() + 0x3C, static_cast<uint32_t>(stub_size));
    std::memcpy(exe_a.data() + 0x100, "DOS/4GW Protected Mode Run-time", 31);

    std::vector<uint8_t> exe_b = exe_a;

    // Distinct LE payloads
    exe_a[stub_size] = 'L'; exe_a[stub_size + 1] = 'E';
    std::memcpy(exe_a.data() + stub_size + 16, "DOOM_II_ENGINE_CODE", 19);

    exe_b[stub_size] = 'L'; exe_b[stub_size + 1] = 'E';
    std::memcpy(exe_b.data() + stub_size + 16, "DUKE3D_BUILD_ENGINE", 19);

    ExecutableFingerprint fp_a, fp_b;
    ASSERT_TRUE(SignatureMatcher::ComputeExecutableFingerprint(exe_a.data(), exe_a.size(), "GAME.EXE", fp_a));
    ASSERT_TRUE(SignatureMatcher::ComputeExecutableFingerprint(exe_b.data(), exe_b.size(), "GAME.EXE", fp_b));

    EXPECT_EQ(fp_a.format, KioskExecFormat::DOS_LE);
    EXPECT_TRUE(fp_a.has_dos_extender_stub);
    EXPECT_EQ(fp_a.payload_offset, stub_size);
    EXPECT_NE(fp_a.header_crc32, fp_b.header_crc32);

    // Register fp_a in software_signatures and confirm Tier 1 match
    SoftwareSignatureRecord sig;
    sig.exec_filename = "GAME.EXE";
    sig.file_size_bytes = static_cast<int64_t>(exe_a.size());
    sig.header_crc32 = fp_a.header_crc32;
    sig.detected_title = "DOOM II: Hell on Earth";
    sig.item_type = "GAME";
    sig.recommended_archetype = KioskArchetypes::DX2_486;
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertSoftwareSignature(sig));

    SoftwareSignatureRecord matched;
    EXPECT_TRUE(SignatureMatcher::MatchSignature("C:\\DOOM2\\game.exe", fp_a, matched));
    EXPECT_EQ(matched.detected_title, "DOOM II: Hell on Earth");
    EXPECT_FALSE(SignatureMatcher::MatchSignature("C:\\DUKE3D\\game.exe", fp_b, matched));

    KioskDatabase::GetInstance().Close();
}

TEST(KioskCrawlerTest, Win31GrpParserAndEmbeddedIcon) {
    // Build a minimal valid native Windows 3.1 "PMCC" .GRP buffer (34-byte GROUPHEADER + rgiItems[])
    // with 1 item + 16x16 embedded 4bpp icon
    std::vector<uint8_t> grp(512, 0);
    std::memcpy(grp.data(), "PMCC", 4);
    TestWriteLe16(grp.data() + 6, static_cast<uint16_t>(grp.size()));
    TestWriteLe16(grp.data() + 22, 40); // pName = offset 40 ("Games")
    TestWriteLe16(grp.data() + 30, 0);  // wReserved = 0
    TestWriteLe16(grp.data() + 32, 1);  // cItems = 1
    TestWriteLe16(grp.data() + 34, 64); // rgiItems[0] = offset 64

    std::memcpy(grp.data() + 40, "Games", 6);

    // Win31ItemData at offset 64 (24 bytes)
    uint8_t* it = grp.data() + 64;
    TestWriteLe16(it + 4, 0);    // iIcon
    TestWriteLe16(it + 6, 12);   // cbHeader
    TestWriteLe16(it + 8, 32);   // cbANDPlane (16x16 @ 1bpp = 32 bytes)
    TestWriteLe16(it + 10, 128); // cbXORPlane (16x16 @ 4bpp = 128 bytes)
    TestWriteLe16(it + 12, 160); // pHeader
    TestWriteLe16(it + 14, 172); // pANDPlane
    TestWriteLe16(it + 16, 204); // pXORPlane
    TestWriteLe16(it + 18, 96);  // pName
    TestWriteLe16(it + 20, 112); // pCommand
    TestWriteLe16(it + 22, 140); // pIconPath

    std::memcpy(grp.data() + 96, "Civilization", 13);
    std::memcpy(grp.data() + 112, "C:\\CIV\\CIV.EXE /VGA", 20);
    std::memcpy(grp.data() + 140, "C:\\CIV\\CIV.EXE", 15);

    // CURSORSHAPE header at 160: w=16, h=16, widthBytes=8, planes=1, bpp=4
    TestWriteLe16(grp.data() + 164, 16);
    TestWriteLe16(grp.data() + 166, 16);
    TestWriteLe16(grp.data() + 168, 8);
    grp[170] = 1;
    grp[171] = 4;
    std::memset(grp.data() + 204, 0x99, 128); // Bright red pixels

    std::vector<Win31GrpItem> items;
    ASSERT_TRUE(ImageCrawler::ParseWin31GrpBuffer(grp.data(), grp.size(), items));
    ASSERT_EQ(items.size(), 1U);
    EXPECT_EQ(items[0].group_title, "Games");
    EXPECT_EQ(items[0].title, "Civilization");
    EXPECT_EQ(items[0].executable, "C:\\CIV\\CIV.EXE");
    EXPECT_EQ(items[0].arguments, "/VGA");
    EXPECT_EQ(items[0].embedded_icon_w, 16);
    EXPECT_EQ(items[0].embedded_icon_h, 16);
    EXPECT_EQ(items[0].embedded_rgba.size(), 16U * 16U * 4U);

    const std::string out_png = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".grp.png";
    ASSERT_TRUE(ImageCrawler::WriteRgbaPng(out_png, 16, 16, items[0].embedded_rgba));
    EXPECT_TRUE(VerifyPngHeaderAndDimensions(out_png, 16, 16));
    std::remove(out_png.c_str());
}

TEST(KioskCrawlerTest, Win9xLnkParserAnsiAndUnicode) {
    // Build a valid [MS-SHLLINK] .LNK buffer with IDList + LinkInfo + Unicode StringData
    std::vector<uint8_t> lnk(0x4C, 0);
    TestWriteLe32(lnk.data() + 0, 0x4C);
    static const uint8_t kClsid[16] = {
        0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    std::memcpy(lnk.data() + 4, kClsid, 16);
    // Flags: HasIDList(0x01) | HasLinkInfo(0x02) | HasWorkingDir(0x10) | HasArgs(0x20) | HasIcon(0x40) | IsUnicode(0x80)
    TestWriteLe32(lnk.data() + 0x14, 0x01u | 0x02u | 0x10u | 0x20u | 0x40u | 0x80u);
    TestWriteLe32(lnk.data() + 0x38, 2); // IconIndex = 2

    // Dummy 4-byte IDList
    lnk.push_back(0x04); lnk.push_back(0x00);
    lnk.insert(lnk.end(), 4, 0x00);

    // LinkInfo block with LocalBasePath = "C:\\GRIM\\GRIM.EXE"
    const std::string base_path = "C:\\GRIM\\GRIM.EXE";
    const uint32_t li_hdr_sz = 0x1C;
    const uint32_t li_sz = li_hdr_sz + static_cast<uint32_t>(base_path.size()) + 2;
    size_t li_pos = lnk.size();
    lnk.resize(li_pos + li_sz, 0);
    TestWriteLe32(lnk.data() + li_pos + 0, li_sz);
    TestWriteLe32(lnk.data() + li_pos + 4, li_hdr_sz);
    TestWriteLe32(lnk.data() + li_pos + 8, 0x01u); // VolumeIDAndLocalBasePath
    TestWriteLe32(lnk.data() + li_pos + 16, li_hdr_sz);
    TestWriteLe32(lnk.data() + li_pos + 24, li_hdr_sz + static_cast<uint32_t>(base_path.size()) + 1);
    std::memcpy(lnk.data() + li_pos + li_hdr_sz, base_path.c_str(), base_path.size() + 1);

    auto append_u16_str = [&](const std::string& s) {
        size_t p = lnk.size();
        lnk.resize(p + 2 + s.size() * 2, 0);
        TestWriteLe16(lnk.data() + p, static_cast<uint16_t>(s.size()));
        for (size_t i = 0; i < s.size(); ++i) {
            lnk[p + 2 + i * 2] = static_cast<uint8_t>(s[i]);
        }
    };
    append_u16_str("C:\\GRIM");
    append_u16_str("-fullscreen");
    append_u16_str("C:\\GRIM\\GRIM.ICO");

    Win9xLnkInfo info;
    ASSERT_TRUE(ImageCrawler::ParseWin9xLnkBuffer(lnk.data(), lnk.size(), info));
    EXPECT_EQ(info.target_path, "C:\\GRIM\\GRIM.EXE");
    EXPECT_EQ(info.working_dir, "C:\\GRIM");
    EXPECT_EQ(info.arguments, "-fullscreen");
    EXPECT_EQ(info.icon_location, "C:\\GRIM\\GRIM.ICO");
    EXPECT_EQ(info.icon_index, 2);
}

TEST(KioskCrawlerTest, NeAndPeIconExtractionSelectsBestVariant) {
    const std::vector<uint8_t> dib_16x16 = BuildSynthetic4bppIconDib(16, 16);
    const std::vector<uint8_t> dib_32x32 = BuildSynthetic8bppIconDib(32, 32);

    // --- 1. Synthetic 16-bit NE executable with RT_GROUP_ICON (16x16 4bpp + 32x32 8bpp) ---
    std::vector<uint8_t> ne(8192, 0);
    ne[0] = 'M'; ne[1] = 'Z';
    TestWriteLe32(ne.data() + 0x3C, 0x80);
    ne[0x80] = 'N'; ne[0x81] = 'E';
    TestWriteLe16(ne.data() + 0x80 + 0x24, 0x40); // ne_rsrctab = 0xC0

    // Resource table at 0xC0, align_shift = 4 (16-byte alignment)
    size_t rt = 0xC0;
    TestWriteLe16(ne.data() + rt, 4);
    size_t p = rt + 2;
    // Type 0x800E (RT_GROUP_ICON), count = 1, offset = 0x200 >> 4 = 0x20, len = 0x40 >> 4 = 4
    TestWriteLe16(ne.data() + p + 0, 0x800Eu);
    TestWriteLe16(ne.data() + p + 2, 1);
    p += 8;
    TestWriteLe16(ne.data() + p + 0, 0x20);
    TestWriteLe16(ne.data() + p + 2, 4);
    TestWriteLe16(ne.data() + p + 6, 0x8001u);
    p += 12;

    // Type 0x8003 (RT_ICON), count = 2 (ID 1 = 16x16 at 0x400, ID 2 = 32x32 at 0x800)
    TestWriteLe16(ne.data() + p + 0, 0x8003u);
    TestWriteLe16(ne.data() + p + 2, 2);
    p += 8;
    TestWriteLe16(ne.data() + p + 0, 0x40);
    TestWriteLe16(ne.data() + p + 2, static_cast<uint16_t>((dib_16x16.size() + 15) >> 4));
    TestWriteLe16(ne.data() + p + 6, 0x8001u);
    p += 12;
    TestWriteLe16(ne.data() + p + 0, 0x80);
    TestWriteLe16(ne.data() + p + 2, static_cast<uint16_t>((dib_32x32.size() + 15) >> 4));
    TestWriteLe16(ne.data() + p + 6, 0x8002u);

    // GRPICONDIR at 0x200 listing both variants (should pick 32x32 8bpp ID 2)
    TestWriteLe16(ne.data() + 0x200 + 0, 0);
    TestWriteLe16(ne.data() + 0x200 + 2, 1);
    TestWriteLe16(ne.data() + 0x200 + 4, 2);
    // Entry 0: 16x16 4bpp -> nID = 1
    ne[0x206 + 0] = 16; ne[0x206 + 1] = 16; ne[0x206 + 2] = 16;
    TestWriteLe16(ne.data() + 0x206 + 6, 4);
    TestWriteLe16(ne.data() + 0x206 + 12, 1);
    // Entry 1: 32x32 8bpp -> nID = 2
    ne[0x206 + 14 + 0] = 32; ne[0x206 + 14 + 1] = 32;
    TestWriteLe16(ne.data() + 0x206 + 14 + 6, 8);
    TestWriteLe16(ne.data() + 0x206 + 14 + 12, 2);

    std::memcpy(ne.data() + 0x400, dib_16x16.data(), dib_16x16.size());
    std::memcpy(ne.data() + 0x800, dib_32x32.data(), dib_32x32.size());

    const std::string ne_png = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".ne.png";
    ASSERT_TRUE(ImageCrawler::ExtractPeNeIcon(ne.data(), ne.size(), ne_png));
    EXPECT_TRUE(VerifyPngHeaderAndDimensions(ne_png, 32, 32));
    std::remove(ne_png.c_str());

    // --- 2. Synthetic 32-bit PE executable with .rsrc section containing RT_ICON (32x32 8bpp) ---
    std::vector<uint8_t> pe(8192, 0);
    pe[0] = 'M'; pe[1] = 'Z';
    TestWriteLe32(pe.data() + 0x3C, 0x80);
    pe[0x80] = 'P'; pe[0x81] = 'E';
    TestWriteLe16(pe.data() + 0x80 + 6, 1);    // NumberOfSections = 1
    TestWriteLe16(pe.data() + 0x80 + 20, 224); // SizeOfOptionalHeader (PE32)
    size_t opt = 0x80 + 24;
    TestWriteLe16(pe.data() + opt, 0x010Bu);   // PE32 magic
    // Data Directory 2 (Resource): RVA = 0x2000, Size = 0x1000
    TestWriteLe32(pe.data() + opt + 96 + 16, 0x2000);
    TestWriteLe32(pe.data() + opt + 96 + 20, 0x1000);

    // Section 0 (.rsrc): VirtualSize = 0x1000, VirtualAddress = 0x2000, RawSize = 0x1000, RawPtr = 0x400
    size_t sec = opt + 224;
    std::memcpy(pe.data() + sec, ".rsrc", 5);
    TestWriteLe32(pe.data() + sec + 8, 0x1000);
    TestWriteLe32(pe.data() + sec + 12, 0x2000);
    TestWriteLe32(pe.data() + sec + 16, 0x1000);
    TestWriteLe32(pe.data() + sec + 20, 0x400);

    // Resource tree at file offset 0x400 (RVA 0x2000):
    // Root dir (0x00): 1 ID entry -> Type 3 (RT_ICON) -> subdir at 0x20
    size_t rr = 0x400;
    TestWriteLe16(pe.data() + rr + 14, 1);
    TestWriteLe32(pe.data() + rr + 16, 3);
    TestWriteLe32(pe.data() + rr + 20, 0x80000020u);
    // Name dir (0x20): 1 ID entry -> Name 1 -> subdir at 0x40
    TestWriteLe16(pe.data() + rr + 0x20 + 14, 1);
    TestWriteLe32(pe.data() + rr + 0x20 + 16, 1);
    TestWriteLe32(pe.data() + rr + 0x20 + 20, 0x80000040u);
    // Lang dir (0x40): 1 ID entry -> Lang 1033 -> data entry at 0x60
    TestWriteLe16(pe.data() + rr + 0x40 + 14, 1);
    TestWriteLe32(pe.data() + rr + 0x40 + 16, 1033);
    TestWriteLe32(pe.data() + rr + 0x40 + 20, 0x60);
    // Data entry (0x60): DataRVA = 0x2100 (file off 0x500), Size = dib_32x32.size()
    TestWriteLe32(pe.data() + rr + 0x60 + 0, 0x2100);
    TestWriteLe32(pe.data() + rr + 0x60 + 4, static_cast<uint32_t>(dib_32x32.size()));
    std::memcpy(pe.data() + 0x500, dib_32x32.data(), dib_32x32.size());

    const std::string pe_png = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".pe.png";
    ASSERT_TRUE(ImageCrawler::ExtractPeNeIcon(pe.data(), pe.size(), pe_png));
    EXPECT_TRUE(VerifyPngHeaderAndDimensions(pe_png, 32, 32));
    std::remove(pe_png.c_str());
}

TEST(KioskCrawlerTest, ScanAndCatalogMachineDeduplicatesOnRescan) {
    KioskDatabase::GetInstance().Open(":memory:");
    const std::string hdd_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".scan_mach.img";
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd_path));

    MachineRecord mach;
    mach.id = "mach-phase6-scan";
    mach.display_name = "486DX2 Test Rig";
    mach.archetype_id = KioskArchetypes::DX2_486;
    mach.archetype_tier = 4;
    mach.boot_mode = "REAL_HDD_BOOT";
    mach.primary_hdd_path = hdd_path;
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMachine(mach));

    ImageCrawler::CrawlSummary sum1;
    ASSERT_TRUE(ImageCrawler::ScanAndCatalogMachine(mach.id, "", sum1));
    EXPECT_EQ(sum1.total_discovered, 2);
    EXPECT_EQ(sum1.newly_inserted, 2);
    EXPECT_EQ(sum1.skipped_duplicates, 0);

    // Second scan must be idempotent (0 new inserts, 2 skipped duplicates)
    ImageCrawler::CrawlSummary sum2;
    ASSERT_TRUE(ImageCrawler::ScanAndCatalogMachine(mach.id, "", sum2));
    EXPECT_EQ(sum2.total_discovered, 2);
    EXPECT_EQ(sum2.newly_inserted, 0);
    EXPECT_EQ(sum2.skipped_duplicates, 2);

    std::vector<CatalogItemRecord> items = KioskDatabase::GetInstance().GetItemsForMachine(mach.id);
    EXPECT_EQ(items.size(), 2U);

    std::remove(hdd_path.c_str());
    KioskDatabase::GetInstance().Close();
}

TEST(KioskDatabasePersistenceTest, DefaultPathAndMachinePersistsAcrossReopen) {
    const std::string def_path = KioskDatabase::GetDefaultDatabasePath();
    EXPECT_FALSE(def_path.empty());
    EXPECT_NE(def_path, ":memory:");
    EXPECT_NE(def_path.find("catalog.db"), std::string::npos);

    const std::string temp_db = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".persist_test.db";
    std::remove(temp_db.c_str());
    std::remove((temp_db + "-wal").c_str());
    std::remove((temp_db + "-shm").c_str());

    ASSERT_TRUE(KioskDatabase::GetInstance().Open(temp_db));
    EXPECT_EQ(KioskDatabase::GetInstance().GetActiveDatabasePath(), temp_db);

    MachineRecord mach;
    mach.id = "mach-persist-01";
    mach.display_name = "Persistent 386DX";
    mach.archetype_id = KioskArchetypes::DX_386;
    mach.archetype_tier = 3;
    mach.boot_mode = "INTEGRATED_DOS";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMachine(mach));
    KioskDatabase::GetInstance().Close();

    // Reopen from disk and confirm the machine survived restart
    ASSERT_TRUE(KioskDatabase::GetInstance().Open(temp_db));
    MachineRecord loaded;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMachine("mach-persist-01", loaded));
    EXPECT_EQ(loaded.display_name, "Persistent 386DX");
    EXPECT_EQ(loaded.archetype_id, KioskArchetypes::DX_386);
    KioskDatabase::GetInstance().Close();

    std::remove(temp_db.c_str());
    std::remove((temp_db + "-wal").c_str());
    std::remove((temp_db + "-shm").c_str());
}

TEST(KioskCrawlerTest, Fat32ActiveFatAndSectorSizeGuard) {
    // 1. Non-512B sector size disk must be rejected before reading into 512B buffer
    imageDiskMemory non512_disk(10, 2, 8, 1024);
    FatReadOnlyVfs bad_vfs;
    EXPECT_FALSE(bad_vfs.OpenFromImageDisk(&non512_disk));

    // 2. FAT32 with BPB_ExtFlags = 0x0081 (mirroring disabled, FAT 1 active):
    // FAT 0 has stale EOC at cluster 3, while FAT 1 chains cluster 3 -> cluster 4.
    const std::string img_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fat32_active.img";
    const uint32_t part_lba = 32;
    const uint32_t rsvd_secs = 32;
    const uint32_t fat_secs = 8;
    const uint32_t data_lba = part_lba + rsvd_secs + (2 * fat_secs); // 80
    std::vector<uint8_t> img(160U * 512U, 0);

    img[510] = 0x55; img[511] = 0xAA;
    img[0x1BE + 0] = 0x80;
    img[0x1BE + 4] = 0x0C;
    TestWriteLe32(img.data() + 0x1BE + 8, part_lba);
    TestWriteLe32(img.data() + 0x1BE + 12, 70100);

    uint8_t* vbr = img.data() + part_lba * 512U;
    vbr[0] = 0xEB; vbr[1] = 0x58; vbr[2] = 0x90;
    TestWriteLe16(vbr + 11, 512);
    vbr[13] = 1;
    TestWriteLe16(vbr + 14, rsvd_secs);
    vbr[16] = 2; // 2 FATs
    TestWriteLe32(vbr + 32, 70100);
    TestWriteLe32(vbr + 36, fat_secs);
    TestWriteLe16(vbr + 40, 0x0081u); // Mirroring off, active FAT = 1
    TestWriteLe32(vbr + 44, 2);       // Root cluster = 2
    vbr[510] = 0x55; vbr[511] = 0xAA;

    // FAT 0: stale EOC on cluster 3
    uint8_t* fat0 = img.data() + (part_lba + rsvd_secs) * 512U;
    TestWriteLe32(fat0 + 2 * 4, 0x0FFFFFFFu);
    TestWriteLe32(fat0 + 3 * 4, 0x0FFFFFFFu);

    // FAT 1 (authoritative): cluster 3 -> cluster 4 -> EOC
    uint8_t* fat1 = img.data() + (part_lba + rsvd_secs + fat_secs) * 512U;
    TestWriteLe32(fat1 + 2 * 4, 0x0FFFFFFFu);
    TestWriteLe32(fat1 + 3 * 4, 4);
    TestWriteLe32(fat1 + 4 * 4, 0x0FFFFFFFu);

    uint8_t* root = img.data() + data_lba * 512U;
    TestWriteDirEntry83(root, "ACTIVE  TXT", 0x20, 3, 600);
    std::memset(img.data() + (data_lba + 1) * 512U, 'X', 512);
    std::memset(img.data() + (data_lba + 2) * 512U, 'Y', 512);

    std::FILE* f = std::fopen(img_path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(img.data(), 1, img.size(), f), img.size());
    std::fclose(f);

    FatReadOnlyVfs vfs;
    ASSERT_TRUE(vfs.OpenImageFile(img_path));
    FatDirEntry active_entry;
    ASSERT_TRUE(vfs.FindPath("\\ACTIVE.TXT", active_entry));
    std::vector<uint8_t> data;
    ASSERT_TRUE(vfs.ReadFile(active_entry, data));
    ASSERT_EQ(data.size(), 600U);
    EXPECT_EQ(data[0], 'X');
    EXPECT_EQ(data[512], 'Y');

    vfs.Close();
    std::remove(img_path.c_str());
}

TEST(KioskCrawlerTest, Round2DriveGuardMbrBoundsAndTruncatedLeWindow) {
    // 1. FindPath must reject non-C: drive letters (e.g. D:\) against mounted C: volume
    const std::string hdd_path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".r2_drive.img";
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd_path));
    FatReadOnlyVfs vfs;
    ASSERT_TRUE(vfs.OpenImageFile(hdd_path));
    FatDirEntry fe;
    EXPECT_TRUE(vfs.FindPath("C:\\DOOM2\\DOOM2.EXE", fe));
    EXPECT_FALSE(vfs.FindPath("D:\\DOOM2\\DOOM2.EXE", fe));
    vfs.Close();
    std::remove(hdd_path.c_str());

    // 2. Truncated LE payload window (size < true_file_size and < 4096 bytes available after LE) must return false
    std::vector<uint8_t> trunc_le(4096, 0x90);
    trunc_le[0] = 'M'; trunc_le[1] = 'Z';
    TestWriteLe32(trunc_le.data() + 0x3C, 3000);
    trunc_le[3000] = 'L'; trunc_le[3001] = 'E';
    ExecutableFingerprint fp;
    EXPECT_FALSE(SignatureMatcher::ComputeExecutableFingerprint(trunc_le.data(), trunc_le.size(), "BIG.EXE", fp, 10000));
    // Same buffer when true_file_size == buffer size (genuinely short LE) must succeed
    EXPECT_TRUE(SignatureMatcher::ComputeExecutableFingerprint(trunc_le.data(), trunc_le.size(), "SMALL.EXE", fp, 4096));
}

// ----------------------------------------------------------------------------
// Phase 7: GOG & Steam Ingestion Engine Tests
// ----------------------------------------------------------------------------

namespace {

struct TestZipFileSpec {
    std::string path;
    std::vector<uint8_t> data;
    bool deflate = false;
};

// Minimal in-memory PKZIP builder (Stored + Deflate via zlib) for Phase 7 tests
bool WriteSyntheticZipArchive(const std::string& zip_path, const std::vector<TestZipFileSpec>& files) {
    std::vector<uint8_t> out;
    struct CdRec {
        std::string path;
        uint16_t method;
        uint32_t crc;
        uint32_t csz;
        uint32_t usz;
        uint32_t lhoff;
    };
    std::vector<CdRec> cds;

    auto push16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto push32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };

    for (size_t i = 0; i < files.size(); ++i) {
        const TestZipFileSpec& f = files[i];
        uint32_t crc = SignatureMatcher::ComputeCrc32(f.data.data(), f.data.size());
        std::vector<uint8_t> payload = f.data;
        uint16_t method = 0;

        if (f.deflate && !f.data.empty()) {
            z_stream zs;
            std::memset(&zs, 0, sizeof(zs));
            if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                return false;
            }
            payload.resize(deflateBound(&zs, static_cast<uLong>(f.data.size())));
            zs.next_in = const_cast<Bytef*>(f.data.data());
            zs.avail_in = static_cast<uInt>(f.data.size());
            zs.next_out = payload.data();
            zs.avail_out = static_cast<uInt>(payload.size());
            int zrc = deflate(&zs, Z_FINISH);
            deflateEnd(&zs);
            if (zrc != Z_STREAM_END) return false;
            payload.resize(zs.total_out);
            method = 8;
        }

        uint32_t lhoff = static_cast<uint32_t>(out.size());
        push32(0x04034b50U);
        push16(20);
        push16(0);
        push16(method);
        push16(0);
        push16(0);
        push32(crc);
        push32(static_cast<uint32_t>(payload.size()));
        push32(static_cast<uint32_t>(f.data.size()));
        push16(static_cast<uint16_t>(f.path.size()));
        push16(0);
        out.insert(out.end(), f.path.begin(), f.path.end());
        out.insert(out.end(), payload.begin(), payload.end());

        CdRec cd;
        cd.path = f.path;
        cd.method = method;
        cd.crc = crc;
        cd.csz = static_cast<uint32_t>(payload.size());
        cd.usz = static_cast<uint32_t>(f.data.size());
        cd.lhoff = lhoff;
        cds.push_back(cd);
    }

    uint32_t cd_off = static_cast<uint32_t>(out.size());
    for (size_t i = 0; i < cds.size(); ++i) {
        const CdRec& cd = cds[i];
        push32(0x02014b50U);
        push16(20);
        push16(20);
        push16(0);
        push16(cd.method);
        push16(0);
        push16(0);
        push32(cd.crc);
        push32(cd.csz);
        push32(cd.usz);
        push16(static_cast<uint16_t>(cd.path.size()));
        push16(0);
        push16(0);
        push16(0);
        push16(0);
        push32(0);
        push32(cd.lhoff);
        out.insert(out.end(), cd.path.begin(), cd.path.end());
    }
    uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_off;

    push32(0x06054b50U);
    push16(0);
    push16(0);
    push16(static_cast<uint16_t>(cds.size()));
    push16(static_cast<uint16_t>(cds.size()));
    push32(cd_size);
    push32(cd_off);
    push16(0);

    std::ofstream ofs(zip_path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return ofs.good();
}

std::vector<uint8_t> StrToBytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> BuildSyntheticIso9660Bytes(const std::string& volume_label) {
    std::vector<uint8_t> iso(0x8800, 0);
    iso[0x8000] = 0x01;
    std::memcpy(iso.data() + 0x8001, "CD001", 5);
    std::memset(iso.data() + 0x8028, ' ', 32);
    size_t n = (std::min)(volume_label.size(), static_cast<size_t>(32));
    std::memcpy(iso.data() + 0x8028, volume_label.data(), n);
    return iso;
}

} // anonymous namespace

TEST(KioskPhase7Test, ConfTranslatorArchetypeScoringAndSecondaryFilter) {
    EXPECT_TRUE(ConfTranslator::IsSecondaryProfileConf("dosbox_doom_settings.conf"));
    EXPECT_TRUE(ConfTranslator::IsSecondaryProfileConf("dosbox_doom_server.conf"));
    EXPECT_FALSE(ConfTranslator::IsSecondaryProfileConf("dosbox_doom.conf"));
    EXPECT_FALSE(ConfTranslator::IsSecondaryProfileConf("dosbox_doom_single.conf"));

    std::string delta;

    // 1. XT 8088 (CGA / cycles < 600)
    ConfTranslationResult r_xt;
    ConfTranslator::ParseConfString("[dosbox]\nmachine=cga\n[cpu]\ncycles=fixed 300\n", ".", r_xt);
    EXPECT_EQ(ConfTranslator::ScoreArchetype(r_xt, false, false, delta), KioskArchetypes::XT_8088);

    // 2. AT 286 (EGA / cycles 1500)
    ConfTranslationResult r_286;
    ConfTranslator::ParseConfString("[dosbox]\nmachine=ega\n[cpu]\ncycles=fixed 1500\n", ".", r_286);
    EXPECT_EQ(ConfTranslator::ScoreArchetype(r_286, false, false, delta), KioskArchetypes::AT_286);

    // 3. 386DX (SVGA / cycles 8000)
    ConfTranslationResult r_386;
    ConfTranslator::ParseConfString("[dosbox]\nmachine=svga_s3\n[cpu]\ncycles=fixed 8000\n[sblaster]\nsbtype=sbpro2\n", ".", r_386);
    EXPECT_EQ(ConfTranslator::ScoreArchetype(r_386, false, false, delta), KioskArchetypes::DX_386);

    // 4. 486DX2 (cycles > 40000 -> emits cycles=max override)
    ConfTranslationResult r_486;
    ConfTranslator::ParseConfString("[cpu]\ncycles=fixed 50000\n", ".", r_486);
    EXPECT_EQ(ConfTranslator::ScoreArchetype(r_486, false, false, delta), KioskArchetypes::DX2_486);
    EXPECT_NE(delta.find("cycles = max"), std::string::npos);

    // 5. Pentium 3dfx (glide=true or Win9x PE)
    ConfTranslationResult r_p5;
    ConfTranslator::ParseConfString("[glide]\nglide=true\n", ".", r_p5);
    EXPECT_EQ(ConfTranslator::ScoreArchetype(r_p5, false, false, delta), KioskArchetypes::PENTIUM_3DFX);
    EXPECT_EQ(ConfTranslator::ScoreArchetype(r_386, true, false, delta), KioskArchetypes::PENTIUM_3DFX);
}

TEST(KioskPhase7Test, ConfTranslatorAutoexecAst) {
    const std::string base_conf =
        "[dosbox]\nmemsize=16\n"
        "[cpu]\ncycles=max\n";
    const std::string single_conf =
        "[autoexec]\n"
        "# GOG launch script\n"
        "@echo off\n"
        "mount C \"..\"\n"
        "mount C \"..\\cloud_saves\" -t overlay\n"
        "imgmount d \"..\\cd1.inst\" \"..\\cd2.inst\" -t iso -fs iso\n"
        "c:\n"
        "cd tomb\n"
        "dos4gw.exe tomb.exe -nojoy\n"
        "exit\n";

    ConfTranslationResult res;
    ASSERT_TRUE(ConfTranslator::ParseConfString(base_conf, "/sandbox/__support/app", res));
    ASSERT_TRUE(ConfTranslator::ParseConfString(single_conf, "/sandbox/__support/app", res));

    EXPECT_EQ(res.resolved_game_root, "/sandbox/__support");
    ASSERT_EQ(res.overlay_dirs.size(), 1U);
    EXPECT_EQ(res.overlay_dirs[0], "/sandbox/__support/cloud_saves");
    ASSERT_EQ(res.media_mounts.size(), 1U);
    EXPECT_EQ(res.media_mounts[0].drive_letter, 'D');
    EXPECT_TRUE(res.media_mounts[0].is_imgmount);
    ASSERT_EQ(res.media_mounts[0].raw_paths.size(), 2U);
    EXPECT_EQ(res.media_mounts[0].raw_paths[0], "..\\cd1.inst");
    EXPECT_EQ(res.media_mounts[0].raw_paths[1], "..\\cd2.inst");

    EXPECT_EQ(res.guest_working_dir, "C:\\TOMB");
    EXPECT_EQ(res.guest_executable, "DOS4GW.EXE");
    EXPECT_EQ(res.guest_arguments, "tomb.exe -nojoy");
}

TEST(KioskPhase7Test, CueSanitizerAndIsoNormalizer) {
    const std::string base_tmp = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".p7_cue";
    const std::string src_dir = base_tmp + "_src";
    const std::string pool_dir = base_tmp + "_pool";
    std::remove((pool_dir + "/homm2_cd1.cue").c_str());
    std::remove((pool_dir + "/homm2_cd1.bin").c_str());
    std::remove((pool_dir + "/homm2_cd2.iso").c_str());

    // Create mock .inst + .gog + audio track
    std::vector<TestZipFileSpec> specs;
    specs.push_back({"GAME.GOG", BuildSyntheticIso9660Bytes("HOMM2_CD1"), false});
    specs.push_back({"GAME.INST", StrToBytes(
        "FILE \"GAME.GOG\" BINARY\n"
        "  TRACK 01 MODE1/2048\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"MUSIC\\TRACK02.OGG\" MP3\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:00:00\n"), false});
    specs.push_back({"MUSIC/TRACK02.OGG", StrToBytes("OggS_mock_audio_data"), false});
    specs.push_back({"STANDALONE.GOG", BuildSyntheticIso9660Bytes("STANDALONE_DISC"), false});

    const std::string zip_path = base_tmp + ".zip";
    ASSERT_TRUE(WriteSyntheticZipArchive(zip_path, specs));
    std::string err;
    ASSERT_TRUE(ArchiveImporter::ExtractZipArchive(zip_path, src_dir, err)) << err;

    std::vector<NormalizedMediaEntry> entries;
    ASSERT_TRUE(ArchiveImporter::NormalizeMediaInSandbox(src_dir, "homm2", pool_dir, entries, err)) << err;
    ASSERT_EQ(entries.size(), 2U);

    // Entry 0: CUE+BIN with Redbook audio
    EXPECT_EQ(entries[0].media_type, "CDROM_CUE");
    EXPECT_TRUE(entries[0].has_redbook_audio);
    EXPECT_EQ(entries[0].volume_label, "HOMM2_CD1");

    // Verify rewritten CUE sheet uses lowercase relative POSIX paths
    std::ifstream cf(entries[0].normalized_pool_path.c_str());
    ASSERT_TRUE(cf.is_open());
    std::stringstream cbuf;
    cbuf << cf.rdbuf();
    std::string cue_out = cbuf.str();
    EXPECT_NE(cue_out.find("FILE \"homm2_cd1.bin\" BINARY"), std::string::npos);
    EXPECT_NE(cue_out.find("FILE \"homm2_cd1_audio/track02.ogg\" MP3"), std::string::npos);

    // Entry 1: Standalone .gog -> .iso
    EXPECT_EQ(entries[1].media_type, "CDROM_ISO");
    EXPECT_FALSE(entries[1].has_redbook_audio);
    EXPECT_EQ(entries[1].volume_label, "STANDALONE_DISC");

    std::remove(zip_path.c_str());
}

TEST(KioskPhase7Test, ZipExtractorDeflateZipSlipAndSteamBackupGuard) {
    const std::string base_tmp = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".p7_zip";

    // 1. Valid Deflate + Stored Steam zip with redundant SteamApps/common/MyGame/ wrapper
    const std::string good_zip = base_tmp + "_good.zip";
    const std::string expected_exe = "MZ_FAKE_DOS_BINARY_PAYLOAD_1234567890";
    const std::string expected_appid = "2310\n";
    std::vector<TestZipFileSpec> good_specs;
    good_specs.push_back({"SteamApps\\common\\MyGame\\GAME.EXE", StrToBytes(expected_exe), true});
    good_specs.push_back({"SteamApps\\common\\MyGame\\steam_appid.txt", StrToBytes(expected_appid), false});
    ASSERT_TRUE(WriteSyntheticZipArchive(good_zip, good_specs));

    std::string err;
    const std::string out_dir = base_tmp + "_out_v2";
    std::remove((out_dir + "/GAME.EXE").c_str());
    std::remove((out_dir + "/steam_appid.txt").c_str());
    ASSERT_TRUE(ArchiveImporter::ExtractZipArchive(good_zip, out_dir, err)) << err;
    {
        std::ifstream f((out_dir + "/GAME.EXE").c_str(), std::ios::binary);
        ASSERT_TRUE(f.is_open());
        std::string actual((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        EXPECT_EQ(actual, expected_exe);
    }
    {
        std::ifstream f((out_dir + "/steam_appid.txt").c_str(), std::ios::binary);
        ASSERT_TRUE(f.is_open());
        std::string actual((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        EXPECT_EQ(actual, expected_appid);
    }
    std::remove((out_dir + "/GAME.EXE").c_str());
    std::remove((out_dir + "/steam_appid.txt").c_str());

    // 2. Zip-Slip rejection (both standard ".." and Win32-normalized ".. " trailing space)
    const std::string slip_zip = base_tmp + "_slip.zip";
    std::vector<TestZipFileSpec> slip_specs;
    slip_specs.push_back({"../../evil.txt", StrToBytes("pwned"), false});
    ASSERT_TRUE(WriteSyntheticZipArchive(slip_zip, slip_specs));
    EXPECT_FALSE(ArchiveImporter::ExtractZipArchive(slip_zip, out_dir + "_slip", err));
    EXPECT_NE(err.find("Zip-Slip"), std::string::npos);

    const std::string win_slip_zip = base_tmp + "_winslip.zip";
    std::vector<TestZipFileSpec> win_slip_specs;
    win_slip_specs.push_back({".. /evil.txt", StrToBytes("pwned"), false});
    ASSERT_TRUE(WriteSyntheticZipArchive(win_slip_zip, win_slip_specs));
    EXPECT_FALSE(ArchiveImporter::ExtractZipArchive(win_slip_zip, out_dir + "_winslip", err));
    EXPECT_NE(err.find("Zip-Slip"), std::string::npos);
    std::remove(win_slip_zip.c_str());

    // 3. Steam encrypted backup (.csd + sku.sis) detection
    const std::string csd_zip = base_tmp + "_csd.zip";
    std::vector<TestZipFileSpec> csd_specs;
    csd_specs.push_back({"Indy/sku.sis", StrToBytes("\"sku\" {}"), false});
    csd_specs.push_back({"Indy/6011_depotcache_1.csd", StrToBytes("encrypted"), false});
    ASSERT_TRUE(WriteSyntheticZipArchive(csd_zip, csd_specs));
    EXPECT_FALSE(ArchiveImporter::ExtractZipArchive(csd_zip, out_dir + "_csd", err));
    EXPECT_NE(err.find("encrypted backup"), std::string::npos);

    std::remove(good_zip.c_str());
    std::remove(slip_zip.c_str());
    std::remove(csd_zip.c_str());
}

TEST(KioskPhase7Test, GogMetadataJsonIcoAndCruftSanitizer) {
    const std::string gog_json =
        "{\n"
        "  \"gameId\": \"1432575012\",\n"
        "  \"name\": \"Eye of the Beholder\",\n"
        "  \"developer\": \"Westwood Associates\",\n"
        "  \"releaseDate\": \"1991-01-01\",\n"
        "  \"playTasks\": [\n"
        "    { \"category\": \"tool\", \"path\": \"DOSBOX\\\\GOGDOSConfig.exe\" },\n"
        "    { \"category\": \"game\", \"isPrimary\": true, \"path\": \"DOSBOX\\\\DOSBox.exe\", \"arguments\": \"-conf dosbox_eob1.conf\" }\n"
        "  ]\n"
        "}";
    IngestMetadata meta;
    ASSERT_TRUE(ArchiveImporter::ParseGogInfoJson(gog_json, meta));
    EXPECT_EQ(meta.title, "Eye of the Beholder");
    EXPECT_EQ(meta.gog_game_id, "1432575012");
    EXPECT_EQ(meta.developer, "Westwood Associates");
    EXPECT_EQ(meta.release_year, 1991);
    EXPECT_EQ(meta.source_origin, "GOG_IMPORT");
    EXPECT_EQ(meta.primary_play_task_path, "DOSBOX\\DOSBox.exe");

    // Verify Inno Setup PE marker detection
    const std::string fake_inno = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".p7_inno.exe";
    {
        std::ofstream f(fake_inno.c_str(), std::ios::binary | std::ios::trunc);
        f << "MZ" << std::string(128, '\0') << "Inno Setup Setup Data (5.6.2)";
    }
    EXPECT_EQ(ArchiveImporter::DetectSourceFormat(fake_inno), IngestSourceFormat::GOG_INNOSETUP);
    std::remove(fake_inno.c_str());
}

TEST_F(KioskDatabaseFixture, Phase7ModeAApplianceContainerEndToEnd) {
    const std::string base_tmp = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".p7_modea";
    const std::string zip_path = base_tmp + ".zip";
    const std::string data_root = base_tmp + "_root";

    std::vector<uint8_t> fake_exe(128, 0x90);
    fake_exe[0] = 'M'; fake_exe[1] = 'Z';

    std::vector<TestZipFileSpec> specs;
    specs.push_back({"EOB/EOB.EXE", fake_exe, true});
    specs.push_back({"EOB/DOSBOX/DOSBox.exe", StrToBytes("MZ_CRUFT"), false});
    specs.push_back({"EOB/SDL.dll", StrToBytes("MZ_CRUFT"), false});
    specs.push_back({"EOB/__support/app/dosbox_eob1.conf", StrToBytes("[dosbox]\nmachine=svga_s3\n[cpu]\ncycles=fixed 6000\n"), true});
    specs.push_back({"EOB/__support/app/dosbox_eob1_single.conf", StrToBytes("[autoexec]\nmount C \"..\\..\"\nc:\neob.exe\nexit\n"), true});
    specs.push_back({"EOB/goggame-1432575012.info", StrToBytes("{\"name\":\"Eye of the Beholder\",\"gameId\":\"1432575012\",\"releaseDate\":\"1991-01-01\"}"), false});
    specs.push_back({"EOB/DISC.GOG", BuildSyntheticIso9660Bytes("EOB_CD1"), false});
    ASSERT_TRUE(WriteSyntheticZipArchive(zip_path, specs));

    IngestOptions opts;
    opts.source_path = zip_path;
    opts.deploy_mode = IngestDeployMode::APPLIANCE_CONTAINER;
    opts.kiosk_data_root = data_root;

    IngestResult res = ArchiveImporter::ImportArchive(opts);
    ASSERT_TRUE(res.success) << res.error_message;
    EXPECT_EQ(res.title, "Eye of the Beholder");
    EXPECT_EQ(res.archetype_id, KioskArchetypes::DX_386);
    EXPECT_EQ(res.media_registered_count, 1);
    EXPECT_EQ(res.caddy_bindings_count, 1);

    CatalogItemRecord item;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetItem(res.catalog_item_id, item));
    EXPECT_EQ(item.launch_mechanism, "INTEGRATED_AUTOEXEC");
    EXPECT_EQ(item.guest_executable, "EOB.EXE");
    EXPECT_EQ(item.source_origin, "GOG_IMPORT");
    EXPECT_FALSE(item.host_container_dir.empty());

    // Verify wrapper cruft was stripped from deployed container
    std::ifstream cruft_check((item.host_container_dir + "/SDL.dll").c_str());
    EXPECT_FALSE(cruft_check.is_open());
    std::ifstream game_check((item.host_container_dir + "/EOB.EXE").c_str());
    EXPECT_TRUE(game_check.is_open());

    std::remove(zip_path.c_str());
}

TEST_F(KioskDatabaseFixture, Phase7ModeBFat16HddInjectionAndPathPatching) {
    const std::string base_tmp = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".p7_modeb";
    const std::string hdd_path = base_tmp + "_hdd.img";
    const std::string zip_path = base_tmp + ".zip";
    const std::string data_root = base_tmp + "_root";

    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd_path));

    MachineRecord mach;
    mach.id = "mach-p7-modeb";
    mach.display_name = "My 1993 486DX2";
    mach.archetype_id = KioskArchetypes::DX2_486;
    mach.archetype_tier = 4;
    mach.boot_mode = "REAL_HDD_BOOT";
    mach.os_installed = true;
    mach.primary_hdd_path = hdd_path;
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMachine(mach));

    std::vector<uint8_t> fake_exe(128, 0x90);
    fake_exe[0] = 'M'; fake_exe[1] = 'Z';

    std::vector<TestZipFileSpec> specs;
    specs.push_back({"KQ6/SIERRA.EXE", fake_exe, true});
    specs.push_back({"KQ6/RESOURCE.CFG", StrToBytes("patchDir=C:\\\ncdrom=D:\\\n"), false});
    specs.push_back({"KQ6/dosbox_kq6.conf", StrToBytes("[autoexec]\nmount C \".\"\nc:\nsierra.exe\nexit\n"), false});
    ASSERT_TRUE(WriteSyntheticZipArchive(zip_path, specs));

    IngestOptions opts;
    opts.source_path = zip_path;
    opts.deploy_mode = IngestDeployMode::HDD_INJECT;
    opts.target_machine_id = mach.id;
    opts.kiosk_data_root = data_root;

    IngestResult res = ArchiveImporter::ImportArchive(opts);
    ASSERT_TRUE(res.success) << res.error_message;
    EXPECT_EQ(res.config_files_patched, 1);

    // Verify injected files & patched RESOURCE.CFG can be read back via Phase 6 FatReadOnlyVfs
    FatReadOnlyVfs vfs;
    ASSERT_TRUE(vfs.OpenImageFile(hdd_path));
    FatDirEntry exe_ent;
    ASSERT_TRUE(vfs.FindPath(res.deployed_path + "\\SIERRA.EXE", exe_ent));
    FatDirEntry cfg_ent;
    ASSERT_TRUE(vfs.FindPath(res.deployed_path + "\\RESOURCE.CFG", cfg_ent));

    std::vector<uint8_t> cfg_bytes;
    ASSERT_TRUE(vfs.ReadFile(cfg_ent, cfg_bytes));
    std::string cfg_text(reinterpret_cast<const char*>(cfg_bytes.data()), cfg_bytes.size());
    EXPECT_NE(cfg_text.find("patchDir=" + res.deployed_path + "\\"), std::string::npos);
    EXPECT_NE(cfg_text.find("cdrom=D:\\"), std::string::npos);
    vfs.Close();

    std::remove(hdd_path.c_str());
    std::remove(zip_path.c_str());
}

TEST_F(KioskDatabaseFixture, Phase7SteamSubdirDosExecutableSkipsWin32WrapperAndHonorsArchetypeOverride) {
    const std::string base_tmp = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".p7_indy";
    const std::string zip_path = base_tmp + ".zip";
    const std::string data_root = base_tmp + "_root";

    // Build a realistic Win32 PE wrapper (like Steam's root launcher) + real DOS MZ inside ATLANTIS/
    std::vector<uint8_t> win32_pe(256, 0);
    win32_pe[0] = 'M'; win32_pe[1] = 'Z';
    TestWriteLe32(win32_pe.data() + 0x3C, 0x80);
    win32_pe[0x80] = 'P'; win32_pe[0x81] = 'E'; win32_pe[0x82] = 0; win32_pe[0x83] = 0;

    std::vector<uint8_t> dos_mz(40000, 0x90);
    dos_mz[0] = 'M'; dos_mz[1] = 'Z';

    std::vector<TestZipFileSpec> specs;
    specs.push_back({"Indiana Jones and the Fate of Atlantis/Indiana Jones and the Fate of Atlantis.exe", win32_pe, false});
    specs.push_back({"Indiana Jones and the Fate of Atlantis/testapp.exe", win32_pe, false});
    specs.push_back({"Indiana Jones and the Fate of Atlantis/ATLANTIS/ATLANTIS.EXE", dos_mz, true});
    specs.push_back({"Indiana Jones and the Fate of Atlantis/ATLANTIS/HELPME.EXE", std::vector<uint8_t>{'M','Z',0x90,0x90}, false});
    ASSERT_TRUE(WriteSyntheticZipArchive(zip_path, specs));

    IngestOptions opts;
    opts.source_path = zip_path;
    opts.deploy_mode = IngestDeployMode::APPLIANCE_CONTAINER;
    opts.archetype_override = KioskArchetypes::AT_286;
    opts.kiosk_data_root = data_root;

    IngestResult res = ArchiveImporter::ImportArchive(opts);
    ASSERT_TRUE(res.success) << res.error_message;
    EXPECT_EQ(res.archetype_id, KioskArchetypes::AT_286);

    CatalogItemRecord item;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetItem(res.catalog_item_id, item));
    EXPECT_EQ(item.guest_working_dir, "C:\\ATLANTIS");
    EXPECT_EQ(item.guest_executable, "ATLANTIS.EXE");

    // Verify testapp.exe was stripped
    std::ifstream testapp_check((item.host_container_dir + "/testapp.exe").c_str());
    EXPECT_FALSE(testapp_check.is_open());

    std::remove(zip_path.c_str());
}

TEST(KioskFilePickerTest, MatchesImportAndMediaPoolExtensions) {
    // Import archive filter (.exe / .zip)
    EXPECT_TRUE(ViewCarousel::MatchesFilePickerFilter("setup_doom.exe", ViewCarousel::FilePickerTarget::IMPORT_ARCHIVE));
    EXPECT_TRUE(ViewCarousel::MatchesFilePickerFilter("GAME.ZIP", ViewCarousel::FilePickerTarget::IMPORT_ARCHIVE));
    EXPECT_FALSE(ViewCarousel::MatchesFilePickerFilter("disk1.img", ViewCarousel::FilePickerTarget::IMPORT_ARCHIVE));
    EXPECT_FALSE(ViewCarousel::MatchesFilePickerFilter("readme.txt", ViewCarousel::FilePickerTarget::IMPORT_ARCHIVE));

    // Media pool filter (.img, .ima, .iso, .cue, .bin, .vfd, .flp)
    EXPECT_TRUE(ViewCarousel::MatchesFilePickerFilter("dos622_disk1.img", ViewCarousel::FilePickerTarget::MEDIA_POOL));
    EXPECT_TRUE(ViewCarousel::MatchesFilePickerFilter("BOOTDISK.IMA", ViewCarousel::FilePickerTarget::MEDIA_POOL));
    EXPECT_TRUE(ViewCarousel::MatchesFilePickerFilter("myst.iso", ViewCarousel::FilePickerTarget::MEDIA_POOL));
    EXPECT_TRUE(ViewCarousel::MatchesFilePickerFilter("quake.CUE", ViewCarousel::FilePickerTarget::MEDIA_POOL));
    EXPECT_TRUE(ViewCarousel::MatchesFilePickerFilter("quake.bin", ViewCarousel::FilePickerTarget::MEDIA_POOL));
    EXPECT_TRUE(ViewCarousel::MatchesFilePickerFilter("floppy.vfd", ViewCarousel::FilePickerTarget::MEDIA_POOL));
    EXPECT_TRUE(ViewCarousel::MatchesFilePickerFilter("floppy.flp", ViewCarousel::FilePickerTarget::MEDIA_POOL));
    EXPECT_FALSE(ViewCarousel::MatchesFilePickerFilter("setup_doom.exe", ViewCarousel::FilePickerTarget::MEDIA_POOL));
    EXPECT_FALSE(ViewCarousel::MatchesFilePickerFilter("archive.zip", ViewCarousel::FilePickerTarget::MEDIA_POOL));
    EXPECT_FALSE(ViewCarousel::MatchesFilePickerFilter("notes.txt", ViewCarousel::FilePickerTarget::MEDIA_POOL));
}

TEST(KioskFilePickerTest, MediaPoolBatchAddAndLastDirectoryPersistence) {
    ASSERT_TRUE(KioskDatabase::GetInstance().Open(":memory:"));

    std::string temp_dir = "/tmp";
#if defined(_WIN32)
    if (const char* td = std::getenv("TEMP")) {
        temp_dir = td;
        while (temp_dir.size() > 3 && (temp_dir.back() == '\\' || temp_dir.back() == '/')) {
            temp_dir.pop_back();
        }
    }
    const std::string sep = "\\";
#else
    const std::string sep = "/";
#endif

    // Create two synthetic 1.44MB floppy images in temp_dir
    const std::string img1 = temp_dir + sep + "kiosk_batch_disk1.img";
    const std::string img2 = temp_dir + sep + "kiosk_batch_disk2.img";
    const std::string missing = temp_dir + sep + "kiosk_batch_missing_999.img";
    {
        std::vector<uint8_t> zeros(1474560u, 0);
        std::ofstream f1(img1.c_str(), std::ios::binary | std::ios::trunc);
        f1.write(reinterpret_cast<const char*>(zeros.data()), static_cast<std::streamsize>(zeros.size()));
        std::ofstream f2(img2.c_str(), std::ios::binary | std::ios::trunc);
        f2.write(reinterpret_cast<const char*>(zeros.data()), static_cast<std::streamsize>(zeros.size()));
    }

    // Batch add both images + one missing file in one call
    std::vector<std::string> batch;
    batch.push_back(img1);
    batch.push_back(img2);
    batch.push_back(missing);
    int already = 0;
    int failed = 0;
    const int added = ViewCarousel::AddMultipleMediaFilesToPool(batch, already, failed);
    EXPECT_EQ(added, 2);
    EXPECT_EQ(already, 0);
    EXPECT_EQ(failed, 1);
    EXPECT_EQ(KioskDatabase::GetInstance().GetAllMedia().size(), 2u);

    // Re-adding the same two images should report them as already existing (zero duplicates)
    int already2 = 0;
    int failed2 = 0;
    std::vector<std::string> batch2;
    batch2.push_back(img1);
    batch2.push_back(img2);
    EXPECT_EQ(ViewCarousel::AddMultipleMediaFilesToPool(batch2, already2, failed2), 0);
    EXPECT_EQ(already2, 2);
    EXPECT_EQ(failed2, 0);

    // Verify picker remembers last browsed directory when reopened with empty initial_dir
    ViewCarousel& vc = ViewCarousel::GetInstance();
    vc.OpenFilePickerForTest(temp_dir, ViewCarousel::FilePickerTarget::MEDIA_POOL);
    EXPECT_EQ(vc.GetLastMediaPickerDir(), temp_dir);

    vc.OpenFilePickerForTest("", ViewCarousel::FilePickerTarget::MEDIA_POOL);
    EXPECT_EQ(vc.GetCurrentPickerDir(), temp_dir);
    vc.ResetTransientModals();

    std::remove(img1.c_str());
    std::remove(img2.c_str());
    KioskDatabase::GetInstance().Close();
}

TEST(KioskWizardTest, SelectingGuestOSAutoSwitchesToRealHddBoot) {
    ASSERT_TRUE(KioskDatabase::GetInstance().Open(":memory:"));

    ViewWizard& wiz = ViewWizard::GetInstance();
    wiz.Reset();
    ASSERT_TRUE(wiz.SelectArchetype(KioskArchetypes::DX2_486));
    ASSERT_EQ(wiz.GetDraftMachine().boot_mode, "INTEGRATED_DOS");

    std::string err;
    ASSERT_TRUE(wiz.SelectOSDistribution("MSDOS_622", err)) << err;
    EXPECT_EQ(wiz.GetDraftMachine().boot_mode, "REAL_HDD_BOOT");
    EXPECT_FALSE(wiz.GetDraftMachine().primary_hdd_chs.empty());
    EXPECT_TRUE(wiz.GetCreateBlankHddOnDisk());

    // Verify Windows 3.1 (WIN310) is also seeded and compatible with 486DX2
    ASSERT_TRUE(wiz.SelectOSDistribution("WIN310", err)) << err;
    EXPECT_EQ(wiz.GetDraftMachine().os_id, "WIN310");
    EXPECT_EQ(wiz.GetDraftMachine().boot_mode, "REAL_HDD_BOOT");

    KioskDatabase::GetInstance().Close();
}

TEST(KioskRebootTest, InSessionRebootRespectsEjectedInstallerFloppyAndMarksOsInstalled) {
    ASSERT_TRUE(KioskDatabase::GetInstance().Open(":memory:"));

    std::string temp_dir = "/tmp/";
#if defined(_WIN32)
    if (const char* td = std::getenv("TEMP")) {
        temp_dir = td;
        if (!temp_dir.empty() && temp_dir.back() != '\\' && temp_dir.back() != '/') temp_dir += '\\';
    }
#endif
    const std::string hdd_path = temp_dir + "kiosk_test_reboot_hdd.img";
    {
        std::vector<uint8_t> mbr(512, 0);
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
        std::ofstream ofs(hdd_path.c_str(), std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(mbr.data()), 512);
    }

    MediaPoolRecord disk1;
    disk1.id = "med-reboot-d1";
    disk1.file_path = "dos622_disk1.img";
    disk1.media_type = "FLOPPY_35_1440K";
    disk1.label_title = "MS-DOS 6.22 Disk 1";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMedia(disk1));

    MachineRecord mach;
    mach.id = "mach-reboot-test";
    mach.display_name = "DOS Install Reboot Rig";
    mach.archetype_id = KioskArchetypes::DX2_486;
    mach.archetype_tier = 4;
    mach.os_id = "MSDOS_622";
    mach.os_installed = false;
    mach.boot_mode = "REAL_HDD_BOOT";
    mach.primary_hdd_path = hdd_path;
    mach.primary_hdd_chs = "507,16,63";
    std::vector<CaddyBindingRecord> bindings;
    CaddyBindingRecord b;
    b.media_id = disk1.id;
    b.target_drive = "A";
    b.slot_order = 1;
    bindings.push_back(b);
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMachineWithBindings(mach, bindings));

    KioskManager& mgr = KioskManager::GetInstance();
    mgr.Initialize("", "");
    ASSERT_TRUE(mgr.RequestLaunchMachine(mach.id));
    std::string init_conf;
    ASSERT_TRUE(mgr.SynthesizeAndPrepareLaunch(init_conf));
    EXPECT_NE(init_conf.find("BOOT -l a"), std::string::npos);

    // 1. Mid-install reboot with Disk 1 still in Drive A: -> keeps booting A:
    std::string mid_reboot;
    ASSERT_TRUE(mgr.BuildInSessionRebootAutoexec(mid_reboot, disk1.file_path, "", "", ""));
    EXPECT_NE(mid_reboot.find("IMGMOUNT 0"), std::string::npos);
    EXPECT_NE(mid_reboot.find("BOOT -l a"), std::string::npos);
    MachineRecord mid_state;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMachine(mach.id, mid_state));
    EXPECT_FALSE(mid_state.os_installed);

    // 2. Post-install reboot after ejecting Drive A: -> omits IMGMOUNT 0, boots C:, and marks os_installed=true
    std::string post_reboot;
    ASSERT_TRUE(mgr.BuildInSessionRebootAutoexec(post_reboot, "", "", "", ""));
    EXPECT_EQ(post_reboot.find("IMGMOUNT 0"), std::string::npos);
    EXPECT_EQ(post_reboot.find("BOOT -l a"), std::string::npos);
    EXPECT_NE(post_reboot.find("BOOT -l c"), std::string::npos);

    MachineRecord post_state;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMachine(mach.id, post_state));
    EXPECT_TRUE(post_state.os_installed);

    mgr.Shutdown();
    KioskDatabase::GetInstance().Close();
    std::remove(hdd_path.c_str());
}

TEST(KioskMediaGroupTest, DerivesGroupPersistsAndFormatsDisplayName) {
    EXPECT_EQ(DiskCaddyView::DeriveDefaultMediaGroup("C:\\Retro\\MS-DOS 6.22\\Disk1.img"), "MS-DOS 6.22");
    EXPECT_EQ(DiskCaddyView::DeriveDefaultMediaGroup("/roms/Windows 3.1/Disk2.ima"), "Windows 3.1");
    EXPECT_EQ(DiskCaddyView::DeriveDefaultMediaGroup("C:\\Users\\test\\Downloads\\Disk1.img"), "");
    EXPECT_EQ(DiskCaddyView::DeriveDefaultMediaGroup("Disk1.img"), "");

    ASSERT_TRUE(KioskDatabase::GetInstance().Open(":memory:"));

    MediaPoolRecord m;
    m.id = "med-grp-dos1";
    m.file_path = "C:\\Retro\\MS-DOS 6.22\\Disk1.img";
    m.media_type = "FLOPPY_35_1440K";
    m.label_title = "Disk1";
    m.label_subtitle = "Bootable Setup Disk 1 of 3";
    m.media_group = "MS-DOS 6.22";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMedia(m));

    MediaPoolRecord loaded;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMedia(m.id, loaded));
    EXPECT_EQ(loaded.media_group, "MS-DOS 6.22");
    EXPECT_EQ(loaded.label_subtitle, "Bootable Setup Disk 1 of 3");
    EXPECT_EQ(DiskCaddyView::FormatMediaDisplayName(loaded), "[MS-DOS 6.22] Disk1");

    loaded.media_group = "MS-DOS 6.22 OEM";
    loaded.label_title = "Disk 1 - Setup";
    ASSERT_TRUE(KioskDatabase::GetInstance().UpdateMedia(loaded));

    MediaPoolRecord updated;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMedia(m.id, updated));
    EXPECT_EQ(updated.media_group, "MS-DOS 6.22 OEM");
    EXPECT_EQ(DiskCaddyView::FormatMediaDisplayName(updated), "[MS-DOS 6.22 OEM] Disk 1 - Setup");

    // Add second disk in same group + third ungrouped disk, then test RenameMediaGroup & AssignMediaGroupBatch
    MediaPoolRecord m2 = m;
    m2.id = "med-grp-dos2";
    m2.file_path = "C:\\Retro\\MS-DOS 6.22\\Disk2.img";
    m2.label_title = "Disk2";
    m2.media_group = "MS-DOS 6.22 OEM";
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMedia(m2));

    MediaPoolRecord m3 = m;
    m3.id = "med-grp-wp1";
    m3.file_path = "C:\\Retro\\WordPerfect51\\Disk1.img";
    m3.label_title = "Disk1";
    m3.media_group.clear();
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMedia(m3));

    ASSERT_TRUE(KioskDatabase::GetInstance().RenameMediaGroup("MS-DOS 6.22 OEM", "MS-DOS 6.22"));
    MediaPoolRecord r1, r2, r3;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMedia(m.id, r1));
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMedia(m2.id, r2));
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMedia(m3.id, r3));
    EXPECT_EQ(r1.media_group, "MS-DOS 6.22");
    EXPECT_EQ(r2.media_group, "MS-DOS 6.22");
    EXPECT_EQ(r3.media_group, "");

    std::vector<std::string> batch_ids;
    batch_ids.push_back(m3.id);
    ASSERT_TRUE(KioskDatabase::GetInstance().AssignMediaGroupBatch(batch_ids, "WordPerfect51"));
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMedia(m3.id, r3));
    EXPECT_EQ(r3.media_group, "WordPerfect51");

    KioskDatabase::GetInstance().Close();
}

// ----------------------------------------------------------------------------
// Phase 8: Dual-Target Host Layer & Cross-Platform Removable Storage Bridge
// ----------------------------------------------------------------------------

TEST(KioskPhase8Test, HostDataRootAndSubdirectoryCreation) {
#if defined(_WIN32)
    const char* tmp_env = std::getenv("TEMP");
    const std::string base_tmp = (tmp_env && tmp_env[0] != '\0') ? std::string(tmp_env) : ".";
#else
    const std::string base_tmp = "/tmp";
#endif
    const std::string root = KioskHost::JoinPath(base_tmp, "dosbox_kiosk_p8_host_test");
    ASSERT_TRUE(KioskHost::EnsureDataDirectories(root));

    EXPECT_EQ(KioskHost::GetDefaultDatabasePath(root), KioskHost::JoinPath(root, "catalog.db"));
    EXPECT_EQ(KioskHost::GetDefaultMapperPath(root),
              KioskHost::JoinPath(KioskHost::JoinPath(root, "config"), "kiosk_mapper.map"));
    EXPECT_EQ(KioskHost::GetDefaultBaseConfPath(root),
              KioskHost::JoinPath(KioskHost::JoinPath(root, "config"), "kiosk_base.conf"));

    // Verify subdirectories actually exist by creating a probe file inside config/ and cache/icons/
    const std::string probe1 = KioskHost::JoinPath(KioskHost::GetConfigDir(root), "probe.tmp");
    const std::string probe2 = KioskHost::JoinPath(KioskHost::GetIconCacheDir(root), "probe.tmp");
    {
        std::ofstream f1(probe1.c_str(), std::ios::binary);
        ASSERT_TRUE(f1.is_open());
        f1 << "ok";
    }
    {
        std::ofstream f2(probe2.c_str(), std::ios::binary);
        ASSERT_TRUE(f2.is_open());
        f2 << "ok";
    }
    std::remove(probe1.c_str());
    std::remove(probe2.c_str());
}

TEST(KioskPhase8Test, HostProfileDetectionAndSystemdNoOp) {
    const KioskHostProfile profile = KioskHost::DetectHostProfile();
#if defined(_WIN32)
    EXPECT_EQ(profile, KioskHostProfile::DESKTOP_BIG_PICTURE);
    EXPECT_FALSE(KioskHost::IsSystemdSupportCompiled());
#endif
    EXPECT_NE(KioskHost::GetHostProfileDisplayName(profile), nullptr);
    // Must be safe no-ops without libsystemd
    KioskHost::NotifySystemdReady();
    KioskHost::PingSystemdWatchdog(0);
    KioskHost::NotifySystemdStopping();
}

TEST(KioskPhase8Test, UsbBridgeExtensionAndArchiveClassification) {
    EXPECT_TRUE(KioskUsbBridge::IsSupportedDiskImageExtension("disk1.img"));
    EXPECT_TRUE(KioskUsbBridge::IsSupportedDiskImageExtension("DISK2.IMA"));
    EXPECT_TRUE(KioskUsbBridge::IsSupportedDiskImageExtension("boot.vfd"));
    EXPECT_TRUE(KioskUsbBridge::IsSupportedDiskImageExtension("game.ISO"));
    EXPECT_TRUE(KioskUsbBridge::IsSupportedDiskImageExtension("audio.cue"));
    EXPECT_FALSE(KioskUsbBridge::IsSupportedDiskImageExtension("readme.txt"));
    EXPECT_FALSE(KioskUsbBridge::IsSupportedDiskImageExtension("doom.exe"));

    bool is_gog = false;
    EXPECT_TRUE(KioskUsbBridge::IsImportableArchiveFilename("setup_doom_2.0.exe", &is_gog));
    EXPECT_TRUE(is_gog);

    is_gog = true;
    EXPECT_TRUE(KioskUsbBridge::IsImportableArchiveFilename("Indiana_Jones.zip", &is_gog));
    EXPECT_FALSE(is_gog);

    EXPECT_FALSE(KioskUsbBridge::IsImportableArchiveFilename("dosbox.exe", &is_gog));
    EXPECT_FALSE(KioskUsbBridge::IsImportableArchiveFilename("notes.txt", &is_gog));
}

TEST(KioskPhase8Test, UsbBridgeMountScanThreeLevelsAndRemovePurge) {
    ASSERT_TRUE(KioskDatabase::GetInstance().Open(":memory:"));
    KioskUsbBridge& bridge = KioskUsbBridge::GetInstance();
    ASSERT_TRUE(bridge.Initialize("", false));

    // Seed a permanent non-USB media entry to verify removal never touches it
    MediaPoolRecord perm;
    perm.id = "perm-floppy-1";
    perm.file_path = "C:\\Permanent\\disk1.img";
    perm.media_type = "FLOPPY_35_1440K";
    perm.physical_style = "BEIGE_35";
    perm.label_title = "Permanent Disk";
    perm.is_removable_usb = false;
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMedia(perm));

    // Build a temporary simulated USB mount tree with depths 0..4
#if defined(_WIN32)
    const char* tmp_env = std::getenv("TEMP");
    const std::string base_tmp = (tmp_env && tmp_env[0] != '\0') ? std::string(tmp_env) : ".";
#else
    const std::string base_tmp = "/tmp";
#endif
    const std::string usb_root = KioskHost::JoinPath(base_tmp, "dosbox_kiosk_p8_usb_sim");
    const std::string d1 = KioskHost::JoinPath(usb_root, "L1");
    const std::string d2 = KioskHost::JoinPath(d1, "L2");
    const std::string d3 = KioskHost::JoinPath(d2, "L3");
    const std::string d4 = KioskHost::JoinPath(d3, "L4");
    const std::string docs = KioskHost::JoinPath(usb_root, "DOCUMENTS");
    ASSERT_TRUE(KioskHost::EnsureDirectoryRecursive(d4));
    ASSERT_TRUE(KioskHost::EnsureDirectoryRecursive(docs));

    // Create a valid 1.44MB floppy image at depth 0 and depth 3, plus a depth 4 image that must be skipped
    std::vector<uint8_t> floppy_144(1474560, 0);
    const std::string img_d0 = KioskHost::JoinPath(usb_root, "boot_d0.img");
    const std::string img_d3 = KioskHost::JoinPath(d3, "deep_d3.img");
    const std::string img_d4 = KioskHost::JoinPath(d4, "too_deep_d4.img");
    const std::string zip_d1 = KioskHost::JoinPath(d1, "steam_game.zip");
    const std::string gog_d2 = KioskHost::JoinPath(d2, "setup_eob_1.0.exe");

    auto write_bytes = [](const std::string& p, const uint8_t* data, size_t sz) {
        std::ofstream f(p.c_str(), std::ios::binary | std::ios::trunc);
        if (f.is_open() && sz > 0) {
            f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(sz));
        }
    };
    write_bytes(img_d0, floppy_144.data(), floppy_144.size());
    write_bytes(img_d3, floppy_144.data(), floppy_144.size());
    write_bytes(img_d4, floppy_144.data(), floppy_144.size());
    const uint8_t dummy_zip[4] = { 'P', 'K', 3, 4 };
    write_bytes(zip_d1, dummy_zip, sizeof(dummy_zip));
    const uint8_t dummy_mz[2] = { 'M', 'Z' };
    write_bytes(gog_d2, dummy_mz, sizeof(dummy_mz));

    // 1. Mount simulated USB volume
    ASSERT_TRUE(bridge.OnVolumeMounted(usb_root, "RETRO_USB", false));

    const std::vector<UsbVolumeInfo> vols = bridge.GetActiveVolumes();
    ASSERT_EQ(vols.size(), 1u);
    EXPECT_EQ(vols[0].volume_label, "RETRO_USB");
    EXPECT_EQ(vols[0].media_count, 2);   // depth 0 + depth 3 (depth 4 skipped)
    EXPECT_EQ(vols[0].archive_count, 2); // zip + setup_*.exe
    EXPECT_EQ(bridge.GetPreferredUsbDocumentsDir(), docs);

    const std::vector<MediaPoolRecord> usb_media = KioskDatabase::GetInstance().GetRemovableUsbMedia();
    ASSERT_EQ(usb_media.size(), 2u);
    EXPECT_EQ(usb_media[0].media_group, "USB: RETRO_USB");
    EXPECT_TRUE(usb_media[0].is_removable_usb);

    const std::vector<UsbArchiveCandidate> archives = bridge.GetDiscoveredArchives();
    ASSERT_EQ(archives.size(), 2u);

    // 2. Verify ConfigSynthesizer emits portable mapperfile & USB DOCUMENTS auto-mount on Drive E:
    MachineRecord m;
    m.id = "mach-usb-docs";
    m.display_name = "USB Docs Test";
    m.archetype_id = KioskArchetypes::DX_386;
    m.archetype_tier = 3;
    m.boot_mode = "INTEGRATED_DOS";
    m.primary_hdd_path = "C:\\Games\\Test";

    KioskConfigRecord cfg;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetConfig(cfg));
    cfg.auto_mount_usb_docs = true;
    ASSERT_TRUE(KioskDatabase::GetInstance().UpdateConfig(cfg));
    KioskManager::GetInstance().ReloadConfig();

    const std::string synth_with_usb = ConfigSynthesizer::GetInstance().SynthesizeConfig(m);
    EXPECT_NE(synth_with_usb.find(KioskHost::GetDefaultMapperPath()), std::string::npos);
    EXPECT_NE(synth_with_usb.find("MOUNT E \"" + docs + "\""), std::string::npos);

    // When an optical E: binding already exists, USB DOCUMENTS must NOT shadow E:
    CaddyBindingRecord cd_e;
    cd_e.target_drive = "E";
    cd_e.slot_order = 1;
    cd_e.resolved_file_path = "C:\\Media\\disc2.iso";
    std::vector<CaddyBindingRecord> caddy_e;
    caddy_e.push_back(cd_e);
    const std::string synth_with_cd_e = ConfigSynthesizer::GetInstance().SynthesizeConfig(m, nullptr, caddy_e);
    EXPECT_NE(synth_with_cd_e.find("IMGMOUNT E \"C:\\Media\\disc2.iso\""), std::string::npos);
    EXPECT_EQ(synth_with_cd_e.find("MOUNT E \"" + docs + "\""), std::string::npos);

    // Verify AUTOEXEC USB path safety rejects control chars, quotes, and '%' batch expansion
    EXPECT_TRUE(ConfigSynthesizer::IsSafeAutoexecMountPath("/run/media/kiosk/My USB Drive/DOCUMENTS"));
    EXPECT_TRUE(ConfigSynthesizer::IsSafeAutoexecMountPath("E:\\My Files\\DOCUMENTS"));
    EXPECT_FALSE(ConfigSynthesizer::IsSafeAutoexecMountPath(""));
    EXPECT_FALSE(ConfigSynthesizer::IsSafeAutoexecMountPath("/run/media/kiosk/USB\nDIR/DOCUMENTS"));
    EXPECT_FALSE(ConfigSynthesizer::IsSafeAutoexecMountPath("/run/media/kiosk/USB\rDIR/DOCUMENTS"));
    EXPECT_FALSE(ConfigSynthesizer::IsSafeAutoexecMountPath("/run/media/kiosk/USB\"DIR/DOCUMENTS"));
    EXPECT_FALSE(ConfigSynthesizer::IsSafeAutoexecMountPath("/run/media/kiosk/USB%1/DOCUMENTS"));
    EXPECT_FALSE(ConfigSynthesizer::IsSafeAutoexecMountPath(std::string("/run/media/kiosk/USB") + static_cast<char>(0x1A) + "/DOCUMENTS"));

    // 3. Remove simulated USB volume and verify DB purge preserves permanent media
    ASSERT_TRUE(bridge.OnVolumeRemoved(usb_root));
    EXPECT_TRUE(bridge.GetActiveVolumes().empty());
    EXPECT_TRUE(bridge.GetDiscoveredArchives().empty());
    EXPECT_TRUE(KioskDatabase::GetInstance().GetRemovableUsbMedia().empty());

    MediaPoolRecord perm_check;
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMedia(perm.id, perm_check));
    EXPECT_FALSE(perm_check.is_removable_usb);

    // 4. Test PurgeStaleRemovableUsbMedia() removes orphaned rows whose files no longer exist
    MediaPoolRecord stale_usb;
    stale_usb.id = "usb-stale-999";
    stale_usb.file_path = KioskHost::JoinPath(usb_root, "nonexistent_ghost.img");
    stale_usb.media_type = "FLOPPY_35_1440K";
    stale_usb.physical_style = "BLUE_35";
    stale_usb.label_title = "Ghost USB Disk";
    stale_usb.is_removable_usb = true;
    ASSERT_TRUE(KioskDatabase::GetInstance().InsertMedia(stale_usb));
    EXPECT_EQ(KioskDatabase::GetInstance().PurgeStaleRemovableUsbMedia(), 1);
    EXPECT_TRUE(KioskDatabase::GetInstance().GetRemovableUsbMedia().empty());
    ASSERT_TRUE(KioskDatabase::GetInstance().GetMedia(perm.id, perm_check));

    // Clean up temporary files
    std::remove(img_d0.c_str());
    std::remove(img_d3.c_str());
    std::remove(img_d4.c_str());
    std::remove(zip_d1.c_str());
    std::remove(gog_d2.c_str());
    bridge.Shutdown();
    KioskDatabase::GetInstance().Close();
}

// ============================================================================
// Phase 9: Shared journaled FAT layer (src/kiosk/fs/fat_volume.*)
// ============================================================================

namespace {

std::vector<uint8_t> TestReadWholeFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool TestFileExists(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return in.is_open();
}

std::vector<uint8_t> TestPatternBytes(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 31U + seed) & 0xFFU);
    return v;
}

// FAT16 region of WriteSyntheticFat16HddImage(): LBA 64 .. 64 + 2 * 36 sectors
const size_t kSynthFat16FatOffset = 64U * 512U;
const size_t kSynthFat16FatBytes = 72U * 512U;
const size_t kSynthFat16RootOffset = (64U + 72U) * 512U;
const size_t kSynthFat16RootBytes = 16U * 512U;

// Disarms the commit fault hook even when an assertion aborts the test early
struct FatFaultInjectionGuard {
    ~FatFaultInjectionGuard() {
        KioskFileSectorDevice::SetCommitFaultInjection(-1);
        KioskFileSectorDevice::SetCommitInvalidateFault(false);
    }
};

uint16_t TestReadFat12Raw(const std::vector<uint8_t>& img, uint32_t cluster) {
    const size_t off = 512U + cluster + cluster / 2U; // FAT 1 starts at sector 1 on the 1.44MB floppy
    const uint16_t raw = static_cast<uint16_t>(img[off] | (static_cast<uint16_t>(img[off + 1]) << 8));
    return (cluster & 1U) ? static_cast<uint16_t>(raw >> 4) : static_cast<uint16_t>(raw & 0x0FFFU);
}

} // anonymous namespace

TEST(KioskFatVolumeTest, InterruptedCommitRollsBackToExactOriginalImage) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatjournal.img";
    const std::string journal = KioskFileSectorDevice::JournalPathFor(hdd);
    std::remove(journal.c_str());
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    const std::vector<uint8_t> original = TestReadWholeFile(hdd);
    FatFaultInjectionGuard guard;

    // Power cut after the journal is durable and 2 metadata sectors were applied
    KioskFileSectorDevice::SetCommitFaultInjection(2);
    EXPECT_FALSE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(hdd));
    EXPECT_TRUE(TestFileExists(journal));
    EXPECT_NE(original, TestReadWholeFile(hdd)); // Torn state on disk

    // Recovery restores every byte of metadata; only previously free clusters may differ
    bool rolled_back = false;
    ASSERT_TRUE(KioskFileSectorDevice::RecoverPendingJournal(hdd, &rolled_back));
    EXPECT_TRUE(rolled_back);
    EXPECT_FALSE(TestFileExists(journal));
    const std::vector<uint8_t> recovered = TestReadWholeFile(hdd);
    ASSERT_EQ(original.size(), recovered.size());
    EXPECT_TRUE(std::equal(original.begin(), original.begin() + kSynthFat16RootOffset + kSynthFat16RootBytes,
                           recovered.begin()));

    FatReadOnlyVfs vfs;
    ASSERT_TRUE(vfs.OpenImageFile(hdd));
    FatDirEntry e;
    EXPECT_FALSE(vfs.FindPath("C:\\AUTOEXEC.BAT", e));
    vfs.Close();

    // A clean retry succeeds and leaves no journal behind
    EXPECT_TRUE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(hdd));
    EXPECT_FALSE(TestFileExists(journal));
    ASSERT_TRUE(vfs.OpenImageFile(hdd));
    ASSERT_TRUE(vfs.FindPath("C:\\AUTOEXEC.BAT", e));
    std::vector<uint8_t> content;
    ASSERT_TRUE(vfs.ReadFile(e, content));
    EXPECT_EQ(BootInjector::GetAutoexecHookBlock(), std::string(content.begin(), content.end()));
    vfs.Close();
    std::remove(hdd.c_str());
}

TEST(KioskFatVolumeTest, ReadOnlyOpenRefusesPendingJournalAndTornJournalIsDiscarded) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatjournal2.img";
    const std::string journal = KioskFileSectorDevice::JournalPathFor(hdd);
    std::remove(journal.c_str());
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    const std::vector<uint8_t> original = TestReadWholeFile(hdd);

    FatFaultInjectionGuard guard;
    // Read-only opens never write: they refuse an image with a pending rollback
    KioskFileSectorDevice::SetCommitFaultInjection(0);
    EXPECT_FALSE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(hdd));
    ASSERT_TRUE(TestFileExists(journal));
    {
        FatReadOnlyVfs vfs;
        EXPECT_FALSE(vfs.OpenImageFile(hdd));
    }
    EXPECT_TRUE(TestFileExists(journal));
    // A writable open (as the launch path and writers do) rolls back first
    {
        KioskFatVolume vol;
        std::string err;
        ASSERT_TRUE(vol.OpenImageFile(hdd, true, &err)) << err;
        EXPECT_FALSE(vol.PathExists("C:\\AUTOEXEC.BAT"));
    }
    EXPECT_FALSE(TestFileExists(journal));

    // A truncated journal (crash while writing it) never touches the image
    const std::vector<uint8_t> before_torn = TestReadWholeFile(hdd);
    {
        std::ofstream torn(journal.c_str(), std::ios::binary | std::ios::trunc);
        torn << "KSKJRNL1 truncated";
    }
    bool rolled_back = true;
    ASSERT_TRUE(KioskFileSectorDevice::RecoverPendingJournal(hdd, &rolled_back));
    EXPECT_FALSE(rolled_back);
    EXPECT_FALSE(TestFileExists(journal));
    EXPECT_EQ(before_torn, TestReadWholeFile(hdd));
    EXPECT_TRUE(std::equal(original.begin(), original.begin() + kSynthFat16RootOffset + kSynthFat16RootBytes,
                           before_torn.begin()));
    std::remove(hdd.c_str());
}

TEST(KioskFatVolumeTest, DiscardedTransactionLeavesMetadataUntouched) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatdiscard.img";
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    const std::vector<uint8_t> original = TestReadWholeFile(hdd);
    {
        KioskFatVolume vol;
        std::string err;
        ASSERT_TRUE(vol.OpenImageFile(hdd, /*writable=*/true, &err)) << err;
        uint32_t dir = 0;
        ASSERT_TRUE(vol.EnsureDirectoryPath("C:\\GAMES\\Long Named Game", true, dir, err)) << err;
        ASSERT_TRUE(vol.AddFile(dir, "PLAY.EXE", TestPatternBytes(5000, 7), true, err)) << err;
        EXPECT_TRUE(vol.HasPendingChanges());
        // Staged changes are visible through the same volume before commit
        EXPECT_TRUE(vol.PathExists("C:\\GAMES\\Long Named Game\\PLAY.EXE"));
        // Destroyed without Commit()
    }
    const std::vector<uint8_t> after = TestReadWholeFile(hdd);
    EXPECT_TRUE(std::equal(original.begin() + kSynthFat16FatOffset,
                           original.begin() + kSynthFat16RootOffset + kSynthFat16RootBytes,
                           after.begin() + kSynthFat16FatOffset));
    FatReadOnlyVfs vfs;
    ASSERT_TRUE(vfs.OpenImageFile(hdd));
    FatDirEntry e;
    EXPECT_FALSE(vfs.FindPath("C:\\GAMES", e));
    vfs.Close();
    std::remove(hdd.c_str());
}

TEST(KioskFatVolumeTest, Fat16WriterRoundTripsLfnDirectoryGrowthAndMirroredFats) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatwrite.img";
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    std::string err;
    {
        KioskFatVolume vol;
        ASSERT_TRUE(vol.OpenImageFile(hdd, true, &err)) << err;
        EXPECT_EQ(FilesystemType::FAT16, vol.GetFilesystemType());
        const uint64_t free_before = vol.ComputeFreeBytes();
        uint32_t dir = 0;
        ASSERT_TRUE(vol.EnsureDirectoryPath("\\GAMES\\Ultima Underworld", true, dir, err)) << err;
        // 40 files with long names force the 1-sector-per-cluster directory to grow
        for (int i = 0; i < 40; ++i) {
            const std::string name = "Level Data File " + std::to_string(i) + ".dat";
            ASSERT_TRUE(vol.AddFile(dir, name, TestPatternBytes(700 + i, static_cast<uint8_t>(i)), true, err)) << err;
        }
        ASSERT_TRUE(vol.AddFile(dir, "UW.EXE", TestPatternBytes(3000, 99), true, err)) << err;
        EXPECT_FALSE(vol.AddFile(dir, "uw.exe", TestPatternBytes(10, 1), true, err)); // Duplicate
        EXPECT_LT(vol.ComputeFreeBytes(), free_before);
        ASSERT_TRUE(vol.Commit(&err)) << err;
        EXPECT_FALSE(vol.HasPendingChanges());
    }
    EXPECT_FALSE(TestFileExists(KioskFileSectorDevice::JournalPathFor(hdd)));

    const std::vector<uint8_t> img = TestReadWholeFile(hdd);
    // Both FAT copies were written identically (mirroring enabled)
    const size_t half = kSynthFat16FatBytes / 2U;
    const std::vector<uint8_t> fat0(img.begin() + kSynthFat16FatOffset, img.begin() + kSynthFat16FatOffset + half);
    const std::vector<uint8_t> fat1(img.begin() + kSynthFat16FatOffset + half,
                                    img.begin() + kSynthFat16FatOffset + 2U * half);
    // FAT 1 of the synthetic image started blank, so compare only the entries written now
    // Every entry written now must appear in both copies; require that plenty were checked.
    size_t mirrored = 0;
    for (size_t off = 2U * 2U; off < half; off += 2U) {
        if (fat1[off] != 0 || fat1[off + 1] != 0) {
            ASSERT_EQ(fat0[off], fat1[off]) << off;
            ASSERT_EQ(fat0[off + 1], fat1[off + 1]) << off;
            ++mirrored;
        }
    }
    EXPECT_GE(mirrored, 50U);

    FatReadOnlyVfs vfs;
    ASSERT_TRUE(vfs.OpenImageFile(hdd));
    FatDirEntry d;
    ASSERT_TRUE(vfs.FindPath("C:\\GAMES\\Ultima Underworld", d));
    EXPECT_TRUE(d.is_directory);
    EXPECT_EQ("Ultima Underworld", d.name);
    std::vector<FatDirEntry> listing;
    ASSERT_TRUE(vfs.ListDirectory(&d, listing));
    EXPECT_EQ(41U, listing.size());
    for (int i = 0; i < 40; i += 13) {
        FatDirEntry f;
        ASSERT_TRUE(vfs.FindPath("C:\\GAMES\\Ultima Underworld\\Level Data File " + std::to_string(i) + ".dat", f));
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(vfs.ReadFile(f, bytes));
        EXPECT_EQ(TestPatternBytes(700 + i, static_cast<uint8_t>(i)), bytes);
    }
    FatDirEntry exe;
    ASSERT_TRUE(vfs.FindPath("\\GAMES\\ULTIMAUN\\UW.EXE", exe));
    std::vector<uint8_t> exe_bytes;
    ASSERT_TRUE(vfs.ReadFile(exe, exe_bytes));
    EXPECT_EQ(TestPatternBytes(3000, 99), exe_bytes);
    vfs.Close();
    std::remove(hdd.c_str());
}

TEST(KioskFatVolumeTest, Fat12EntriesStraddlingSectorBoundaryRoundTrip) {
    // Cluster 341 is the first FAT12 entry whose 12 bits straddle FAT sectors 0 and 1
    std::vector<uint8_t> img;
    ASSERT_TRUE(BootInjector::GetInstance().BuildFat12ImageInMemory({}, img));
    const std::vector<uint8_t> payload = TestPatternBytes(800U * 512U + 123U, 5);
    {
        KioskMemorySectorDevice dev(img, KioskMemorySectorDevice::kWritable);
        KioskFatVolume vol;
        std::string err;
        ASSERT_TRUE(vol.Mount(&dev, &err)) << err;
        ASSERT_EQ(FilesystemType::FAT12, vol.GetFilesystemType());
        ASSERT_TRUE(vol.AddFile(KioskFatVolume::kRootDir, "BIG.DAT", payload, false, err)) << err;
        ASSERT_TRUE(vol.Commit(&err)) << err;
    }
    std::vector<uint8_t> back;
    ASSERT_TRUE(BootInjector::GetInstance().ReadFileFromFat12Image(img, "BIG.DAT", back));
    EXPECT_EQ(payload, back);
    // Independent raw decode of the straddling entries (odd 341 at byte 511, even 682 at byte 1023)
    EXPECT_EQ(342U, TestReadFat12Raw(img, 341));
    EXPECT_EQ(683U, TestReadFat12Raw(img, 682));
    EXPECT_EQ(0x0FFFU, TestReadFat12Raw(img, 802)); // Last of 801 clusters (2..802)

    // Rewriting in place shrinks the chain and frees the tail
    {
        KioskMemorySectorDevice dev(img, KioskMemorySectorDevice::kWritable);
        KioskFatVolume vol;
        std::string err;
        ASSERT_TRUE(vol.Mount(&dev, &err)) << err;
        const uint64_t free_before = vol.ComputeFreeBytes();
        ASSERT_TRUE(vol.WriteFileReplacing(KioskFatVolume::kRootDir, "BIG.DAT", TestPatternBytes(600, 9), err)) << err;
        ASSERT_TRUE(vol.Commit(&err)) << err;
        KioskFatVolume reread;
        ASSERT_TRUE(reread.Mount(&dev, &err));
        EXPECT_EQ(free_before + 799U * 512U, reread.ComputeFreeBytes());
    }
    ASSERT_TRUE(BootInjector::GetInstance().ReadFileFromFat12Image(img, "BIG.DAT", back));
    EXPECT_EQ(TestPatternBytes(600, 9), back);
}

TEST(KioskFatVolumeTest, Fat32FsInfoFreeCountTracksAllocations) {
    // Minimal FAT32 superfloppy: 32 reserved sectors (FSInfo at 1), 1 FAT of 8 sectors, root cluster 2
    const uint32_t total = 32U + 8U + 600U;
    std::vector<uint8_t> img(static_cast<size_t>(total) * 512U, 0);
    uint8_t* vbr = img.data();
    vbr[0] = 0xEB; vbr[1] = 0x58; vbr[2] = 0x90;
    TestWriteLe16(vbr + 11, 512);
    vbr[13] = 1;
    TestWriteLe16(vbr + 14, 32);
    vbr[16] = 1;
    TestWriteLe32(vbr + 32, total);
    TestWriteLe32(vbr + 36, 8);
    TestWriteLe32(vbr + 44, 2);
    TestWriteLe16(vbr + 48, 1);
    vbr[510] = 0x55; vbr[511] = 0xAA;
    uint8_t* fsi = img.data() + 512U;
    TestWriteLe32(fsi, 0x41615252U);
    TestWriteLe32(fsi + 484, 0x61417272U);
    TestWriteLe32(fsi + 488, 599U); // 600 clusters, root uses 1
    TestWriteLe32(fsi + 492, 3U);
    fsi[510] = 0x55; fsi[511] = 0xAA;
    uint8_t* fat = img.data() + 32U * 512U;
    TestWriteLe32(fat + 0, 0x0FFFFFF8U);
    TestWriteLe32(fat + 4, 0x0FFFFFFFU);
    TestWriteLe32(fat + 8, 0x0FFFFFFFU);

    {
        KioskMemorySectorDevice dev(img, KioskMemorySectorDevice::kWritable);
        KioskFatVolume vol;
        std::string err;
        ASSERT_TRUE(vol.Mount(&dev, &err)) << err;
        ASSERT_EQ(FilesystemType::FAT32, vol.GetFilesystemType());
        ASSERT_TRUE(vol.AddFile(KioskFatVolume::kRootDir, "FIVE.BIN", TestPatternBytes(5U * 512U, 1), false, err)) << err;
        ASSERT_TRUE(vol.Commit(&err)) << err;
    }
    uint32_t free_count = static_cast<uint32_t>(fsi[488]) | (static_cast<uint32_t>(fsi[489]) << 8) |
                          (static_cast<uint32_t>(fsi[490]) << 16) | (static_cast<uint32_t>(fsi[491]) << 24);
    EXPECT_EQ(594U, free_count);
}

TEST(KioskFatVolumeTest, PartitionSelectionPrefersActiveFatEntry) {
    // Two FAT16 partitions: entry 0 inactive, entry 1 active -> the active one wins
    const uint32_t part_secs = 9000;
    std::vector<uint8_t> single;
    const std::string tmp = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatsel.img";
    ASSERT_TRUE(WriteSyntheticFat16HddImage(tmp));
    single = TestReadWholeFile(tmp);
    std::remove(tmp.c_str());

    const uint32_t second_lba = 63U + part_secs;
    std::vector<uint8_t> img(static_cast<size_t>(second_lba + part_secs) * 512U, 0);
    std::copy(single.begin(), single.end(), img.begin());
    std::copy(single.begin() + 63U * 512U, single.end(), img.begin() + static_cast<size_t>(second_lba) * 512U);
    uint8_t* p0 = img.data() + 0x1BE;
    p0[0] = 0x00; // first entry no longer active
    uint8_t* p1 = img.data() + 0x1BE + 16;
    p1[0] = 0x80;
    p1[4] = 0x06;
    TestWriteLe32(p1 + 8, second_lba);
    TestWriteLe32(p1 + 12, part_secs);

    KioskMemorySectorDevice dev(img);
    uint32_t lba = 0;
    uint32_t count = 0;
    ASSERT_TRUE(KioskFatVolume::SelectPartition(dev, true, lba, count));
    EXPECT_EQ(second_lba, lba);

    // A FAT-typed partition whose boot sector is inconsistent rejects the disk
    TestWriteLe16(img.data() + static_cast<size_t>(second_lba) * 512U + 19, 0);
    TestWriteLe32(img.data() + static_cast<size_t>(second_lba) * 512U + 32, part_secs + 50U);
    EXPECT_FALSE(KioskFatVolume::SelectPartition(dev, true, lba, count));
}

TEST(KioskFatVolumeTest, HostAutoexecRewriteReplacesViaTempFile) {
    const std::string path = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".host_autoexec.bat";
    {
        std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
        out << "@ECHO OFF\r\nPATH C:\\DOS\r\n";
    }
    ASSERT_TRUE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(path));
    EXPECT_FALSE(TestFileExists(path + ".kiosk-tmp"));
    const std::vector<uint8_t> bytes = TestReadWholeFile(path);
    const std::string text(bytes.begin(), bytes.end());
    EXPECT_EQ(0U, text.find("@ECHO OFF\r\nPATH C:\\DOS\r\n"));
    EXPECT_NE(std::string::npos, text.find("IF EXIST B:\\KIOSKGO.BAT CALL B:\\KIOSKGO.BAT"));
    std::remove(path.c_str());
}

TEST(KioskFatVolumeTest, StaleJournalForReplacedImageIsQuarantinedNotApplied) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatstale.img";
    const std::string journal = KioskFileSectorDevice::JournalPathFor(hdd);
    std::remove(journal.c_str());
    std::remove((journal + ".bad").c_str());
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    FatFaultInjectionGuard guard;
    KioskFileSectorDevice::SetCommitFaultInjection(1);
    EXPECT_FALSE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(hdd));
    ASSERT_TRUE(TestFileExists(journal));

    // The machine is deleted and a new image created at the same path (same size, new contents)
    std::vector<uint8_t> replacement = TestReadWholeFile(hdd);
    std::fill(replacement.begin(), replacement.end(), static_cast<uint8_t>(0x5A));
    {
        std::ofstream out(hdd.c_str(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(replacement.data()), static_cast<std::streamsize>(replacement.size()));
    }
    bool rolled_back = true;
    ASSERT_TRUE(KioskFileSectorDevice::RecoverPendingJournal(hdd, &rolled_back));
    EXPECT_FALSE(rolled_back);
    EXPECT_FALSE(TestFileExists(journal));
    EXPECT_TRUE(TestFileExists(journal + ".bad"));
    EXPECT_EQ(replacement, TestReadWholeFile(hdd)); // Not a single sector overwritten

    // A size change is rejected the same way
    std::remove((journal + ".bad").c_str());
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    KioskFileSectorDevice::SetCommitFaultInjection(1);
    EXPECT_FALSE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(hdd));
    ASSERT_TRUE(TestFileExists(journal));
    {
        std::ofstream grow(hdd.c_str(), std::ios::binary | std::ios::app);
        const std::vector<uint8_t> pad(4096, 0);
        grow.write(reinterpret_cast<const char*>(pad.data()), static_cast<std::streamsize>(pad.size()));
    }
    ASSERT_TRUE(KioskFileSectorDevice::RecoverPendingJournal(hdd, &rolled_back));
    EXPECT_FALSE(rolled_back);
    EXPECT_TRUE(TestFileExists(journal + ".bad"));

    // Creating a fresh image quarantines any leftover journal up front
    std::remove((journal + ".bad").c_str());
    {
        std::ofstream j(journal.c_str(), std::ios::binary | std::ios::trunc);
        j << "leftover";
    }
    KioskFileSectorDevice::QuarantinePendingJournal(hdd);
    EXPECT_FALSE(TestFileExists(journal));
    std::remove((journal + ".bad").c_str());
    std::remove(hdd.c_str());
}

TEST(KioskFatVolumeTest, FailedCommitRollsBackImmediatelyAndRefusesRetry) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatretry.img";
    const std::string journal = KioskFileSectorDevice::JournalPathFor(hdd);
    std::remove(journal.c_str());
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    const std::vector<uint8_t> original = TestReadWholeFile(hdd);
    FatFaultInjectionGuard guard;
    {
        KioskFatVolume vol;
        std::string err;
        ASSERT_TRUE(vol.OpenImageFile(hdd, true, &err)) << err;
        ASSERT_TRUE(vol.AddFile(KioskFatVolume::kRootDir, "RETRY.BIN", TestPatternBytes(2000, 3), false, err)) << err;
        KioskFileSectorDevice::SetCommitFaultInjection(1, /*simulate_power_cut=*/false); // I/O error mid-apply
        EXPECT_FALSE(vol.Commit(&err));
        EXPECT_FALSE(vol.IsMounted());
        EXPECT_FALSE(vol.HasPendingChanges());
        EXPECT_FALSE(vol.Commit(&err)); // No second attempt against a half-written image
    }
    EXPECT_FALSE(TestFileExists(journal));
    const std::vector<uint8_t> after = TestReadWholeFile(hdd);
    EXPECT_TRUE(std::equal(original.begin(), original.begin() + kSynthFat16RootOffset + kSynthFat16RootBytes,
                           after.begin()));
    std::remove(hdd.c_str());
}

TEST(KioskFatVolumeTest, WritersHoldAnExclusivePerImageLock) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatlock.img";
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    // Contention is always probed from a second thread (re-locking on the owner is undefined)
    auto probe_from_other_thread = [&hdd](bool writable) -> bool {
        bool opened = false;
        std::thread t([&]() {
            if (writable) {
                opened = KioskFileSectorDevice::RecoverPendingJournal(hdd, nullptr, /*wait_for_lock=*/false);
            } else {
                FatReadOnlyVfs vfs;
                opened = vfs.OpenImageFile(hdd);
            }
        });
        t.join();
        return opened;
    };
    {
        KioskFatVolume writer;
        std::string err;
        ASSERT_TRUE(writer.OpenImageFile(hdd, true, &err)) << err;
        EXPECT_FALSE(probe_from_other_thread(true));  // Launch-path probe reports busy
        EXPECT_FALSE(probe_from_other_thread(false)); // Readers fail fast instead of stalling the UI
    }
    EXPECT_TRUE(probe_from_other_thread(true));
    {
        FatReadOnlyVfs reader;
        ASSERT_TRUE(reader.OpenImageFile(hdd));
        EXPECT_TRUE(probe_from_other_thread(false));  // Readers share the lock
        EXPECT_FALSE(probe_from_other_thread(true));  // Writers/recovery are excluded
    }
    // A guest-session pin keeps writers out until released
    ASSERT_TRUE(KioskFileSectorDevice::PinForGuestSession(hdd));
    EXPECT_FALSE(probe_from_other_thread(true));
    EXPECT_TRUE(probe_from_other_thread(false));
    KioskFileSectorDevice::ReleaseGuestSessionPin();
    EXPECT_TRUE(probe_from_other_thread(true));
    std::remove(hdd.c_str());
}

TEST(KioskFatVolumeTest, Fat32WithMirroringDisabledWritesOnlyTheActiveFat) {
    // FAT32 superfloppy with 2 FATs, BPB_ExtFlags = 0x0081 (mirroring off, FAT #1 active)
    const uint32_t total = 32U + 2U * 8U + 600U;
    std::vector<uint8_t> img(static_cast<size_t>(total) * 512U, 0);
    uint8_t* vbr = img.data();
    vbr[0] = 0xEB; vbr[1] = 0x58; vbr[2] = 0x90;
    TestWriteLe16(vbr + 11, 512);
    vbr[13] = 1;
    TestWriteLe16(vbr + 14, 32);
    vbr[16] = 2;
    TestWriteLe32(vbr + 32, total);
    TestWriteLe32(vbr + 36, 8);
    TestWriteLe16(vbr + 40, 0x0081);
    TestWriteLe32(vbr + 44, 2);
    vbr[510] = 0x55; vbr[511] = 0xAA;
    uint8_t* fat0 = img.data() + 32U * 512U;
    uint8_t* fat1 = img.data() + 40U * 512U;
    uint8_t* fats[2] = {fat0, fat1};
    for (int i = 0; i < 2; ++i) {
        TestWriteLe32(fats[i] + 0, 0x0FFFFFF8U);
        TestWriteLe32(fats[i] + 4, 0x0FFFFFFFU);
        TestWriteLe32(fats[i] + 8, 0x0FFFFFFFU);
    }
    const std::vector<uint8_t> fat0_before(fat0, fat0 + 8U * 512U);
    {
        KioskMemorySectorDevice dev(img, KioskMemorySectorDevice::kWritable);
        KioskFatVolume vol;
        std::string err;
        ASSERT_TRUE(vol.Mount(&dev, &err)) << err;
        ASSERT_TRUE(vol.AddFile(KioskFatVolume::kRootDir, "ACTIVE.BIN", TestPatternBytes(3U * 512U, 2), false, err)) << err;
        ASSERT_TRUE(vol.Commit(&err)) << err;
    }
    EXPECT_EQ(fat0_before, std::vector<uint8_t>(fat0, fat0 + 8U * 512U)); // Inactive FAT untouched
    const uint32_t c3 = static_cast<uint32_t>(fat1[12]) | (static_cast<uint32_t>(fat1[13]) << 8);
    EXPECT_EQ(4U, c3); // Active FAT carries the new chain 3 -> 4 -> 5
}

TEST(KioskFatVolumeTest, TrimmedImageMountsWritableButNeverAllocatesPastEof) {
    // The BPB describes a 1.44MB floppy, but the file holds only 100 sectors (data starts at 33)
    std::vector<uint8_t> img;
    ASSERT_TRUE(BootInjector::GetInstance().BuildFat12ImageInMemory({}, img));
    img.resize(100U * 512U);
    KioskMemorySectorDevice dev(img, KioskMemorySectorDevice::kWritable);
    KioskFatVolume vol;
    std::string err;
    ASSERT_TRUE(vol.Mount(&dev, &err)) << err;
    EXPECT_EQ((100U - 33U) * 512U, vol.ComputeFreeBytes());
    EXPECT_FALSE(vol.AddFile(KioskFatVolume::kRootDir, "TOOBIG.BIN", TestPatternBytes(80U * 512U, 1), false, err));
    vol.DiscardChanges();
    EXPECT_TRUE(vol.AddFile(KioskFatVolume::kRootDir, "FITS.BIN", TestPatternBytes(60U * 512U, 1), false, err)) << err;
    EXPECT_TRUE(vol.Commit(&err)) << err;
}

TEST(KioskFatVolumeTest, OddSizedImageStillRollsBack) {
    // Image size not a multiple of 512 (trailing partial sector) must not defeat recovery
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatodd.img";
    const std::string journal = KioskFileSectorDevice::JournalPathFor(hdd);
    std::remove(journal.c_str());
    std::remove((journal + ".bad").c_str());
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    {
        std::ofstream tail(hdd.c_str(), std::ios::binary | std::ios::app);
        tail << "odd-tail";
    }
    const std::vector<uint8_t> original = TestReadWholeFile(hdd);
    FatFaultInjectionGuard guard;
    KioskFileSectorDevice::SetCommitFaultInjection(1);
    EXPECT_FALSE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(hdd));
    ASSERT_TRUE(TestFileExists(journal));
    bool rolled_back = false;
    ASSERT_TRUE(KioskFileSectorDevice::RecoverPendingJournal(hdd, &rolled_back));
    EXPECT_TRUE(rolled_back);
    EXPECT_FALSE(TestFileExists(journal + ".bad"));
    const std::vector<uint8_t> after = TestReadWholeFile(hdd);
    ASSERT_EQ(original.size(), after.size());
    EXPECT_TRUE(std::equal(original.begin(), original.begin() + kSynthFat16RootOffset + kSynthFat16RootBytes,
                           after.begin()));
    std::remove(hdd.c_str());
}

TEST(KioskSynthesizerTest, GameLaunchLocksHostReachingZDrivePrograms) {
    // Blocklist covers every Z:\ program that reaches host files, settings, ports or network
    const char* blocked[] = {"MOUNT.COM", "imgmount.com", "IMGMAKE.COM", "VHDMAKE.COM", "BOOT.COM",
                             "CONFIG.COM", "CFGTOOL.COM", "START.COM", "SERIAL.COM", "PARALLEL.COM",
                             "ETHNET.COM", "IPXNET.COM", "LOADROM.COM"};
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i) {
        EXPECT_TRUE(Kiosk_IsHostReachingProgram(blocked[i])) << blocked[i];
    }
    const char* allowed[] = {"MIXER.COM", "MOUSE.COM", "KEYB.COM", "LOADFIX.COM", "KIOSKLCK.COM", "IMGSWAP.COM"};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        EXPECT_FALSE(Kiosk_IsHostReachingProgram(allowed[i])) << allowed[i];
    }

    // The lock is engaged after the kiosk's own mounts and immediately before the game
    MachineRecord m;
    m.id = "mach-lock";
    m.archetype_id = KioskArchetypes::DX_386;
    m.boot_mode = "INTEGRATED_DOS";
    CatalogItemRecord item;
    item.id = "game-lock";
    item.host_container_dir = "C:\\games\\lock";
    item.guest_working_dir = "C:\\GAME";
    item.guest_executable = "GAME.BAT";
    item.return_on_exit = true;
    CaddyBindingRecord cd;
    cd.target_drive = "D";
    cd.slot_order = 1;
    cd.resolved_file_path = "C:\\games\\lock\\cd.iso";
    const std::string conf = ConfigSynthesizer::GetInstance().SynthesizeAutoexec(m, &item, {cd});
    const size_t mount_pos = conf.find("MOUNT C");
    const size_t cd_pos = conf.find("IMGMOUNT D");
    const size_t lock_pos = conf.find("Z:\\KIOSKLCK.COM\n");
    const size_t launch_pos = conf.find("C:\\GAME\\GAME.BAT");
    ASSERT_NE(std::string::npos, lock_pos);
    EXPECT_LT(mount_pos, lock_pos);
    EXPECT_LT(cd_pos, lock_pos);
    EXPECT_LT(lock_pos, launch_pos);

    // A machine-only INTEGRATED_DOS boot (deliberate DOS prompt) is not locked
    const std::string prompt_conf = ConfigSynthesizer::GetInstance().SynthesizeAutoexec(m, nullptr, {});
    EXPECT_EQ(std::string::npos, prompt_conf.find("KIOSKLCK"));
}

TEST(KioskBootInjectorTest, KioskGoBatRejectsUnsafeImportedFields) {
    MachineRecord m;
    m.id = "mach-gobat";
    m.os_id = "MSDOS622";
    CatalogItemRecord base;
    base.id = "item-gobat";
    base.guest_working_dir = "C:\\GAMES\\DOOM2";
    base.guest_executable = "DOOM2.EXE";
    base.guest_arguments = "-skill 4";
    base.return_on_exit = true;

    const std::string ok_bat = BootInjector::GetInstance().SynthesizeKioskGoBat(m, base);
    EXPECT_NE(std::string::npos, ok_bat.find("DOOM2.EXE -skill 4\r\n"));

    CatalogItemRecord bad_args = base;
    bad_args.guest_arguments = "-skill 4\r\nDELTREE /Y C:\\";
    CatalogItemRecord bad_dir = base;
    bad_dir.guest_working_dir = "C:\\GAMES\\%COMSPEC%";
    CatalogItemRecord bad_exe = base;
    bad_exe.guest_executable = "DOOM2.EXE > NUL";
    const CatalogItemRecord* bad[] = {&bad_args, &bad_dir, &bad_exe};
    for (size_t i = 0; i < 3; ++i) {
        const std::string bat = BootInjector::GetInstance().SynthesizeKioskGoBat(m, *bad[i]);
        EXPECT_NE(std::string::npos, bat.find("launch command rejected")) << i;
        EXPECT_EQ(std::string::npos, bat.find("DELTREE")) << i;
        EXPECT_EQ(std::string::npos, bat.find("%COMSPEC%")) << i;
        EXPECT_EQ(std::string::npos, bat.find("> NUL")) << i;
        EXPECT_NE(std::string::npos, bat.find("B:\\KIOSKEXT.COM\r\n")) << i; // Still returns to the kiosk
    }
}

TEST(KioskFatVolumeTest, ImageLocksAreKeyedByCanonicalPath) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatcanon.img";
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    const size_t slash = hdd.find_last_of("\\/");
    ASSERT_NE(std::string::npos, slash);
    const std::string alias = hdd.substr(0, slash + 1) + "." + hdd.substr(slash, 1) + hdd.substr(slash + 1);
    ASSERT_NE(hdd, alias);
    {
        KioskFatVolume writer;
        std::string err;
        ASSERT_TRUE(writer.OpenImageFile(hdd, true, &err)) << err;
        bool busy_via_alias = false;
        std::thread t([&]() {
            busy_via_alias = !KioskFileSectorDevice::RecoverPendingJournal(alias, nullptr, /*wait_for_lock=*/false);
        });
        t.join();
        EXPECT_TRUE(busy_via_alias); // "dir/./img" contends with "dir/img"
    }
    std::remove(hdd.c_str());
}

TEST_F(KioskDatabaseFixture, MutationsBumpChangeGenerationAndUiReloadsWithoutCallbacks) {
    KioskDatabase& db = KioskDatabase::GetInstance();
    KioskUI& ui = KioskUI::GetInstance();
    ui.EnsureCatalogCacheLoaded();
    const size_t machines_before = ui.GetCachedMachines().size();

    const uint64_t gen0 = db.GetChangeGeneration();
    const uint64_t cfg0 = db.GetConfigGeneration();
    MachineRecord m;
    m.id = "mach-generation";
    m.display_name = "Generation Test";
    m.archetype_id = KioskArchetypes::DX_386;
    m.archetype_tier = 3;
    m.boot_mode = "INTEGRATED_DOS";
    ASSERT_TRUE(db.InsertMachine(m));
    const uint64_t gen1 = db.GetChangeGeneration();
    EXPECT_GT(gen1, gen0);
    EXPECT_EQ(cfg0, db.GetConfigGeneration()); // Not a settings change

    // A failed mutation (duplicate primary key) must not report a change
    EXPECT_FALSE(db.InsertMachine(m));
    EXPECT_EQ(gen1, db.GetChangeGeneration());

    // The UI notices on its own: the data layer no longer calls into KioskUI
    ui.EnsureCatalogCacheLoaded();
    EXPECT_EQ(machines_before + 1, ui.GetCachedMachines().size());

    KioskConfigRecord cfg;
    ASSERT_TRUE(db.GetConfig(cfg));
    cfg.boot_timeout_sec = cfg.boot_timeout_sec + 1;
    ASSERT_TRUE(db.UpdateConfig(cfg));
    EXPECT_GT(db.GetConfigGeneration(), cfg0);
    EXPECT_GT(db.GetChangeGeneration(), gen1);
}

TEST(KioskLifecycleTableTest, OnlyListedTransitionsAreLegal) {
    typedef KioskState S;
    // Happy path
    EXPECT_TRUE(KioskManager::IsLegalTransition(S::STATE_KIOSK_BOOT, S::STATE_COUNTDOWN_IDLE));
    EXPECT_TRUE(KioskManager::IsLegalTransition(S::STATE_COUNTDOWN_IDLE, S::STATE_SHELL_INTERACTIVE));
    EXPECT_TRUE(KioskManager::IsLegalTransition(S::STATE_SHELL_INTERACTIVE, S::STATE_SYNTHESIZE_LAUNCH));
    EXPECT_TRUE(KioskManager::IsLegalTransition(S::STATE_SYNTHESIZE_LAUNCH, S::STATE_EMULATION_RUNNING));
    EXPECT_TRUE(KioskManager::IsLegalTransition(S::STATE_EMULATION_RUNNING, S::STATE_RUNTIME_OVERLAY));
    EXPECT_TRUE(KioskManager::IsLegalTransition(S::STATE_RUNTIME_OVERLAY, S::STATE_EMULATION_RUNNING));
    EXPECT_TRUE(KioskManager::IsLegalTransition(S::STATE_RUNTIME_OVERLAY, S::STATE_SHELL_INTERACTIVE));
    EXPECT_TRUE(KioskManager::IsLegalTransition(S::STATE_EMULATION_RUNNING, S::DISABLED));
    // Rejected: launching mid-session, skipping synthesis, leaving DISABLED, self-loops
    EXPECT_FALSE(KioskManager::IsLegalTransition(S::STATE_EMULATION_RUNNING, S::STATE_SYNTHESIZE_LAUNCH));
    EXPECT_FALSE(KioskManager::IsLegalTransition(S::STATE_RUNTIME_OVERLAY, S::STATE_SYNTHESIZE_LAUNCH));
    EXPECT_FALSE(KioskManager::IsLegalTransition(S::STATE_SYNTHESIZE_LAUNCH, S::STATE_SYNTHESIZE_LAUNCH));
    EXPECT_FALSE(KioskManager::IsLegalTransition(S::STATE_SHELL_INTERACTIVE, S::STATE_EMULATION_RUNNING));
    EXPECT_FALSE(KioskManager::IsLegalTransition(S::STATE_SHELL_INTERACTIVE, S::STATE_SHELL_INTERACTIVE));
    EXPECT_FALSE(KioskManager::IsLegalTransition(S::DISABLED, S::STATE_SHELL_INTERACTIVE));
}

TEST_F(KioskDatabaseFixture, LifecycleRejectsOutOfOrderRequests) {
    KioskManager& km = KioskManager::GetInstance();
    ASSERT_TRUE(km.Initialize("", ""));
    km.CancelWatchdog();
    ASSERT_EQ(KioskState::STATE_SHELL_INTERACTIVE, km.GetState());

    // Synthesis without a request does nothing
    std::string conf;
    EXPECT_FALSE(km.SynthesizeAndPrepareLaunch(conf));
    EXPECT_EQ(KioskState::STATE_SHELL_INTERACTIVE, km.GetState());

    ASSERT_TRUE(km.RequestDefaultDosboxLaunch());
    // A second launch request while one is pending is refused
    EXPECT_FALSE(km.RequestDefaultDosboxLaunch());
    ASSERT_TRUE(km.SynthesizeAndPrepareLaunch(conf));
    ASSERT_EQ(KioskState::STATE_EMULATION_RUNNING, km.GetState());
    // ... and so is a launch while a session is running
    EXPECT_FALSE(km.RequestDefaultDosboxLaunch());
    EXPECT_EQ(KioskState::STATE_EMULATION_RUNNING, km.GetState());

    // Ending the session twice (overlay abort, then the unwinding handler) counts once
    const int sessions = km.GetCompletedSessionCount();
    km.EnterRuntimeOverlay();
    km.AbortSessionToShell();
    km.OnGuestShutdown(KioskShutdownReason::USER_OVERLAY_ABORT);
    km.OnGuestShutdown(KioskShutdownReason::SHELL_EXIT);
    EXPECT_EQ(sessions + 1, km.GetCompletedSessionCount());
    EXPECT_EQ(KioskShutdownReason::USER_OVERLAY_ABORT, km.GetLastShutdownReason());
    EXPECT_EQ(KioskState::STATE_SHELL_INTERACTIVE, km.GetState());

    // Resume / overlay calls outside a session are ignored
    km.ResumeEmulation();
    km.EnterRuntimeOverlay();
    EXPECT_EQ(KioskState::STATE_SHELL_INTERACTIVE, km.GetState());
    km.Shutdown();
}

TEST(KioskFatVolumeTest, PowerTornSectorIsStillRolledBack) {
    // A power cut can leave a journaled sector holding neither the original nor the new bytes.
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fattorn.img";
    const std::string journal = KioskFileSectorDevice::JournalPathFor(hdd);
    std::remove(journal.c_str());
    std::remove((journal + ".bad").c_str());
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    const std::vector<uint8_t> original = TestReadWholeFile(hdd);
    FatFaultInjectionGuard guard;
    KioskFileSectorDevice::SetCommitFaultInjection(1);
    EXPECT_FALSE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(hdd));
    ASSERT_TRUE(TestFileExists(journal));
    {
        // Tear the first FAT sector (a journaled metadata sector) with garbage
        std::fstream f(hdd.c_str(), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>(kSynthFat16FatOffset));
        const std::vector<uint8_t> garbage(512, 0xA7);
        f.write(reinterpret_cast<const char*>(garbage.data()), 512);
    }
    bool rolled_back = false;
    ASSERT_TRUE(KioskFileSectorDevice::RecoverPendingJournal(hdd, &rolled_back));
    EXPECT_TRUE(rolled_back);
    EXPECT_FALSE(TestFileExists(journal + ".bad"));
    const std::vector<uint8_t> after = TestReadWholeFile(hdd);
    EXPECT_TRUE(std::equal(original.begin(), original.begin() + kSynthFat16RootOffset + kSynthFat16RootBytes,
                           after.begin()));
    std::remove(hdd.c_str());
}

TEST(KioskFatVolumeTest, CommitIsOnlyAcknowledgedAfterDurableCommitPoint) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatcp.img";
    const std::string journal = KioskFileSectorDevice::JournalPathFor(hdd);
    std::remove(journal.c_str());
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    const std::vector<uint8_t> original = TestReadWholeFile(hdd);
    FatFaultInjectionGuard guard;
    KioskFileSectorDevice::SetCommitInvalidateFault(true);
    // Every sector was applied, but the journal could not be invalidated: report failure and
    // roll back so a later recovery can never revert an acknowledged write.
    EXPECT_FALSE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(hdd));
    KioskFileSectorDevice::SetCommitInvalidateFault(false);
    EXPECT_FALSE(TestFileExists(journal));
    const std::vector<uint8_t> after = TestReadWholeFile(hdd);
    EXPECT_TRUE(std::equal(original.begin(), original.begin() + kSynthFat16RootOffset + kSynthFat16RootBytes,
                           after.begin()));
    EXPECT_TRUE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(hdd)); // Clean retry works
    std::remove(hdd.c_str());
}

TEST(KioskFatVolumeTest, GuestPinIsRefusedWhileAJournalIsPending) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatpin.img";
    const std::string journal = KioskFileSectorDevice::JournalPathFor(hdd);
    std::remove(journal.c_str());
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    FatFaultInjectionGuard guard;
    KioskFileSectorDevice::SetCommitFaultInjection(0);
    EXPECT_FALSE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(hdd));
    ASSERT_TRUE(KioskFileSectorDevice::HasPendingJournal(hdd));
    EXPECT_FALSE(KioskFileSectorDevice::PinForGuestSession(hdd)); // Never mount a half-written image
    ASSERT_TRUE(KioskFileSectorDevice::RecoverPendingJournal(hdd));
    EXPECT_TRUE(KioskFileSectorDevice::PinForGuestSession(hdd));
    KioskFileSectorDevice::ReleaseGuestSessionPin();
    std::remove(hdd.c_str());
}

TEST(KioskFatVolumeTest, AliasPathsShareOneJournalIdentity) {
    const std::string hdd = BootInjector::GetInstance().GetDefaultDispatchImagePath() + ".fatalias.img";
    const size_t slash = hdd.find_last_of("\\/");
    ASSERT_NE(std::string::npos, slash);
    const std::string alias = hdd.substr(0, slash + 1) + "." + hdd.substr(slash, 1) + hdd.substr(slash + 1);
    EXPECT_EQ(KioskFileSectorDevice::JournalPathFor(hdd), KioskFileSectorDevice::JournalPathFor(alias));
    std::remove(KioskFileSectorDevice::JournalPathFor(hdd).c_str());
    ASSERT_TRUE(WriteSyntheticFat16HddImage(hdd));
    FatFaultInjectionGuard guard;
    KioskFileSectorDevice::SetCommitFaultInjection(1);
    EXPECT_FALSE(BootInjector::GetInstance().EnsureOneTimeAutoexecHook(alias)); // Interrupted via the alias
    EXPECT_TRUE(KioskFileSectorDevice::HasPendingJournal(hdd));               // ... visible via the real path
    FatReadOnlyVfs vfs;
    EXPECT_FALSE(vfs.OpenImageFile(hdd)); // A reader via the other spelling still refuses the torn image
    ASSERT_TRUE(KioskFileSectorDevice::RecoverPendingJournal(hdd));
    EXPECT_FALSE(KioskFileSectorDevice::HasPendingJournal(alias));
    std::remove(hdd.c_str());
}

#endif // defined(C_DEBUG) && defined(C_KIOSK)
