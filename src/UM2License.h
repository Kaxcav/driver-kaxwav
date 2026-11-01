#pragma once
#include <string>
#include <cstdint>

namespace UM2License {

struct LicenseInfo {
    bool isLicensed{ false };
    std::string tier{ "DEMO" };
    uint32_t daysLeft{ 0 };
    std::string hwid;          // Formato amigavel: "UM2-XXXX-XXXX-XXXX-XXXX"
    std::string rawHwid;       // 16 chars hexadecimais
    std::string licenseKey;
};

// Identificador unico do hardware desta maquina
std::string GetMachineHWID();
std::string GetRawHWID();
std::string GetRawHWID_v1();
std::string GetRawHWID_v2();

// Valida uma chave de licenca contra um HWID especifico
bool ValidateKey(const std::string& key, const std::string& rawHwid, std::string& outTier);

// Valida uma chave contra o hardware da maquina atual (compativel com HWID v1 e v2 SMBIOS)
bool ValidateKeyForCurrentMachine(const std::string& key, std::string& outTier);

// Verifica o status da licenca armazenada no Registro / Arquivo local
LicenseInfo CheckLicense();

// Salva e ativa uma nova chave no Registro / Arquivo local
bool ActivateLicense(const std::string& key, LicenseInfo& outInfo);

} // namespace UM2License
