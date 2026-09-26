// A second character compositor owned by the Gilnean Model frame, never by Glue's singleton.
#pragma once
#include <cstdint>
#include "wxl/AppearanceApi.h"
namespace wxl_modern_m2
{
    bool InstallGilneanPreview();
    bool GilneanInFront();
    bool GilneanCustomizationPending();
    uint32_t GilneanOptionCount(uint32_t bodySex);
    bool AlternateOptionAt(uint32_t retailRace, uint32_t bodySex, uint32_t index, WXL_ChrOption* out);
    uint32_t AlternateOptionCount(uint32_t retailRace, uint32_t bodySex);
    bool AlternateFormIdentity(const void* component, uint32_t& retailRace, uint32_t& sex);
    bool GilneanOptionAt(uint32_t bodySex, uint32_t index, WXL_ChrOption* out);
    bool SetLinkedWorgenChoice(uint32_t model, uint32_t index, uint32_t choice);
    bool GilneanPreviewIdentity(const void* component, uint32_t& retailRace, uint32_t& sex);
    int GilneanPreviewChoice(uint32_t model, uint32_t optionIndex);
    uint32_t GilneanPreviewChoices(const void* component, uint32_t model, uint32_t* out, uint32_t capacity);
    uint32_t GilneanChoices(uint32_t sex, uint32_t* out, uint32_t capacity);
    void ResetGilneanChoices();
    void ForgetCustomizationComponent(void* component, void* root);
    void SyncGilneanEquipment(void* primary, void* alternate);
    void ReleaseGilneanEquipment(void* alternate);
    bool GilneanGearVisible();
}
