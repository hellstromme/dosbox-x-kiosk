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
        const std::string& base_conf_path = ""
    ) const;

    // Builds the dynamic [autoexec] section block
    std::string SynthesizeAutoexec(
        const MachineRecord& machine,
        const CatalogItemRecord* item = nullptr,
        const std::vector<CaddyBindingRecord>& caddy = {}
    ) const;

    // Writes synthesized config content to a target file
    bool WriteConfigFile(const std::string& config_content, const std::string& target_path) const;

    // Returns standard kiosk base configuration
    std::string GetDefaultBaseConfig() const;

private:
    ConfigSynthesizer() = default;
};

#endif // DOSBOX_KIOSK_CONFIG_SYNTHESIZER_H
