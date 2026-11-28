#ifndef DOSBOX_KIOSK_CONFIG_SYNTHESIZER_H
#define DOSBOX_KIOSK_CONFIG_SYNTHESIZER_H

#include "kiosk_types.h"
#include <string>
#include <vector>

class ConfigSynthesizer {
public:
    static ConfigSynthesizer& GetInstance();

    // Builds the complete multi-layer DOSBox-X configuration string
    std::string SynthesizeConfig(
        const MachineRecord& machine,
        const CatalogItemRecord* item = nullptr,
        const std::vector<CaddyBindingRecord>& caddy = {},
        const std::string& base_conf_path = "",
        const std::string& dispatch_image_path = "",
        bool boot_installer_media = false
    ) const;

    // Builds the dynamic [autoexec] section block
    std::string SynthesizeAutoexec(
        const MachineRecord& machine,
        const CatalogItemRecord* item = nullptr,
        const std::vector<CaddyBindingRecord>& caddy = {},
        const std::string& dispatch_image_path = "",
        bool boot_installer_media = false
    ) const;

    // Writes synthesized config content to a target file
    bool WriteConfigFile(const std::string& config_content, const std::string& target_path) const;

    // Returns standard kiosk base configuration
    std::string GetDefaultBaseConfig() const;

    // Validates that a host path can be safely emitted as a single quoted [autoexec] MOUNT argument
    static bool IsSafeAutoexecMountPath(const std::string& path);

    // Validates a guest DOS directory (drive prefix already stripped) for an unquoted CD line
    static bool IsSafeGuestDosPath(const std::string& path);

    // Validates a guest executable: relative DOS path, bare name or .EXE/.COM/.BAT file
    static bool IsSafeGuestExecutable(const std::string& exe);

    // True for drive letters a kiosk launch may target (A-E; never Z:)
    static bool IsLaunchableGuestDrive(char drive_letter);

    // Validates guest arguments: no batch expansion, redirection, piping or control chars
    static bool IsSafeGuestArguments(const std::string& args);

private:
    ConfigSynthesizer() = default;
};

#endif // DOSBOX_KIOSK_CONFIG_SYNTHESIZER_H
