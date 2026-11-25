#ifndef DOSBOX_KIOSK_VIEW_WIZARD_H
#define DOSBOX_KIOSK_VIEW_WIZARD_H

#include "../kiosk_types.h"
#include "input_router.h"
#include <string>
#include <vector>

enum class WizardStep {
    STEP_ARCHETYPE = 0,
    STEP_IDENTITY_BOOT = 1,
    STEP_OS_STORAGE = 2,
    STEP_REVIEW_CREATE = 3
};

class ViewWizard {
public:
    static ViewWizard& GetInstance();

    void Reset();
    void Render();
    bool HandleAction(KioskInputAction action);

    // Programmatic state controls (also used by Google Tests)
    WizardStep GetCurrentStep() const { return m_step; }
    bool SetStep(WizardStep step);

    bool SelectArchetype(const std::string& archetype_id);
    void SetDisplayName(const std::string& name);
    void SetBootMode(const std::string& boot_mode);
    bool SelectOSDistribution(const std::string& os_id, std::string& out_error);
    void SetHddSizeMb(int size_mb);
    void SetPrimaryHddPath(const std::string& path);
    void SetCustomHddChsOverride(const std::string& chs);
    void SetCreateBlankHddOnDisk(bool enable);
    bool GetCreateBlankHddOnDisk() const { return m_create_blank_hdd_file; }
    void SetInstallerFloppyMediaId(const std::string& media_id) { m_installer_floppy_media_id = media_id; }
    void SetInstallerCdMediaId(const std::string& media_id) { m_installer_cd_media_id = media_id; }

    // Persists the configured machine + optional installer caddy bindings to KioskDatabase.
    // Returns true on success and populates out_machine_id.
    bool CommitMachine(std::string& out_machine_id, std::string& out_error);

    // Pure helpers for deterministic CHS geometry and sparse-friendly blank HDD image creation
    static std::string ComputeHddChsGeometry(int size_mb);
    static bool CreateBlankHddImage(const std::string& file_path, int size_mb, std::string& out_chs);

    const MachineRecord& GetDraftMachine() const { return m_draft; }
    const std::string& GetLastError() const { return m_last_error; }

private:
    ViewWizard();

    void RenderStepHeader();
    void RenderStep1Archetype();
    void RenderStep2IdentityBoot();
    void RenderStep3OsStorage();
    void RenderStep4ReviewCreate();

    std::string GenerateDefaultHddPath(const std::string& machine_name, const std::string& machine_id) const;
    static std::string GenerateMachineUuid(const std::string& seed);

    WizardStep m_step = WizardStep::STEP_ARCHETYPE;
    MachineRecord m_draft;
    int m_hdd_size_mb = 250;
    bool m_name_edited_by_user = false;
    bool m_create_blank_hdd_file = true;
    std::string m_installer_floppy_media_id;
    std::string m_installer_cd_media_id;
    std::string m_last_error;

    char m_name_buf[128] = {0};
    char m_hdd_path_buf[512] = {0};
    char m_custom_chs_buf[48] = {0};
};

#endif // DOSBOX_KIOSK_VIEW_WIZARD_H
