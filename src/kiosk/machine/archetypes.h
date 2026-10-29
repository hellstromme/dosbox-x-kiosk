#ifndef DOSBOX_KIOSK_ARCHETYPES_H
#define DOSBOX_KIOSK_ARCHETYPES_H

#include "kiosk_types.h"
#include <string>
#include <vector>

class ArchetypeManager {
public:
    static ArchetypeManager& GetInstance();

    const ArchetypeProfile* GetArchetype(const std::string& archetype_id) const;
    std::vector<ArchetypeProfile> GetAllArchetypes() const;

    bool ValidateMachineCompatibility(const MachineRecord& machine, const OSDistributionRecord& os, std::string& out_reason) const;

    std::string GenerateArchetypeConfig(const std::string& archetype_id) const;

private:
    ArchetypeManager();
    void InitProfiles();

    std::vector<ArchetypeProfile> m_profiles;
};

#endif // DOSBOX_KIOSK_ARCHETYPES_H
