#include "UM2License.h"
#include "UM2LicenseSecret.h"
#include <windows.h>
#include <shlobj.h>
#include <vector>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <cstring>

namespace UM2License {

namespace {

// =========================================================================
// CRIPTOGRAFIA AUTOCONTIDA: SHA-256 & HMAC-SHA256 (Padrão FIPS 180-2 / RFC 2104)
// Totalmente independente de DLLs externas, resistente a hooks de APIs do Windows
// =========================================================================

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline uint32_t ROR32(uint32_t val, uint32_t shift) {
    return (val >> shift) | (val << (32 - shift));
}

struct SHA256Context {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
};

void SHA256_Init(SHA256Context* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void SHA256_Transform(uint32_t state[8], const uint8_t data[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    uint32_t w[64];

    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
               (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(data[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void SHA256_Update(SHA256Context* ctx, const uint8_t* data, size_t len) {
    size_t index = static_cast<size_t>((ctx->count >> 3) & 63);
    ctx->count += static_cast<uint64_t>(len) << 3;
    size_t partLen = 64 - index;
    size_t i = 0;

    if (len >= partLen) {
        std::memcpy(&ctx->buffer[index], data, partLen);
        SHA256_Transform(ctx->state, ctx->buffer);
        for (i = partLen; i + 63 < len; i += 64) {
            SHA256_Transform(ctx->state, &data[i]);
        }
        index = 0;
    }
    if (i < len) {
        std::memcpy(&ctx->buffer[index], &data[i], len - i);
    }
}

void SHA256_Final(SHA256Context* ctx, uint8_t digest[32]) {
    uint8_t bits[8];
    for (int i = 0; i < 8; ++i) {
        bits[i] = static_cast<uint8_t>((ctx->count >> ((7 - i) * 8)) & 0xFF);
    }
    size_t index = static_cast<size_t>((ctx->count >> 3) & 63);
    size_t padLen = (index < 56) ? (56 - index) : (120 - index);
    static const uint8_t padding[64] = { 0x80 };
    SHA256_Update(ctx, padding, padLen);
    SHA256_Update(ctx, bits, 8);
    for (int i = 0; i < 8; ++i) {
        digest[i * 4]     = static_cast<uint8_t>((ctx->state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((ctx->state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((ctx->state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>(ctx->state[i] & 0xFF);
    }
}

std::vector<uint8_t> ComputeSHA256(const uint8_t* data, size_t len) {
    SHA256Context ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    std::vector<uint8_t> digest(32);
    SHA256_Final(&ctx, digest.data());
    return digest;
}

std::vector<uint8_t> ComputeHMAC_SHA256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen) {
    uint8_t k0[64] = { 0 };
    if (keyLen > 64) {
        auto d = ComputeSHA256(key, keyLen);
        std::memcpy(k0, d.data(), 32);
    } else {
        std::memcpy(k0, key, keyLen);
    }

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5C;
    }

    SHA256Context ctxInner;
    SHA256_Init(&ctxInner);
    SHA256_Update(&ctxInner, ipad, 64);
    SHA256_Update(&ctxInner, data, dataLen);
    uint8_t innerHash[32];
    SHA256_Final(&ctxInner, innerHash);

    SHA256Context ctxOuter;
    SHA256_Init(&ctxOuter);
    SHA256_Update(&ctxOuter, opad, 64);
    SHA256_Update(&ctxOuter, innerHash, 32);
    std::vector<uint8_t> mac(32);
    SHA256_Final(&ctxOuter, mac.data());
    return mac;
}

// =========================================================================
// CRC16-CCITT & BASE32 (Sem caracteres ambiguos: sem 0, O, 1, I)
// =========================================================================

static const char B32_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

uint16_t ComputeCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>(((crc << 1) ^ 0x1021) & 0xFFFF);
            } else {
                crc = static_cast<uint16_t>((crc << 1) & 0xFFFF);
            }
        }
    }
    return crc;
}

bool DecodeBase32_16CharsTo10Bytes(const std::string& input, uint8_t out[10]) {
    if (input.length() != 16) return false;
    uint64_t high = 0; // primeiros 8 chars (40 bits)
    for (int i = 0; i < 8; ++i) {
        const char* p = std::strchr(B32_ALPHABET, input[i]);
        if (!p) return false;
        high = (high << 5) | static_cast<uint64_t>(p - B32_ALPHABET);
    }
    uint64_t low = 0; // ultimos 8 chars (40 bits)
    for (int i = 8; i < 16; ++i) {
        const char* p = std::strchr(B32_ALPHABET, input[i]);
        if (!p) return false;
        low = (low << 5) | static_cast<uint64_t>(p - B32_ALPHABET);
    }

    out[0] = static_cast<uint8_t>((high >> 32) & 0xFF);
    out[1] = static_cast<uint8_t>((high >> 24) & 0xFF);
    out[2] = static_cast<uint8_t>((high >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((high >> 8) & 0xFF);
    out[4] = static_cast<uint8_t>(high & 0xFF);

    out[5] = static_cast<uint8_t>((low >> 32) & 0xFF);
    out[6] = static_cast<uint8_t>((low >> 24) & 0xFF);
    out[7] = static_cast<uint8_t>((low >> 16) & 0xFF);
    out[8] = static_cast<uint8_t>((low >> 8) & 0xFF);
    out[9] = static_cast<uint8_t>(low & 0xFF);
    return true;
}

// Comparação em tempo constante (anti-timing attack)
bool ConstantTimeCompare(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t result = 0;
    for (size_t i = 0; i < len; ++i) {
        result |= (a[i] ^ b[i]);
    }
    return result == 0;
}

// Função para obter a chave secreta temporariamente na stack com XOR anti-reversing
inline void GetSecretKey(uint8_t outKey[59]) {
    for (size_t i = 0; i < 59; ++i) {
        outKey[i] = OBFUSCATED_SECRET[i] ^ static_cast<uint8_t>((0x5A + i) & 0xFF);
    }
}

} // anonymous namespace

// =========================================================================
// OBTENÇÃO ROBUSTA DE HWID NO WINDOWS (SMBIOS UUID + MachineGuid + CPU + VolSerial)
// =========================================================================

static std::string GetSmbiosUuid() {
    DWORD sig = 0x52534D42; // 'RSMB'
    DWORD size = GetSystemFirmwareTable(sig, 0, nullptr, 0);
    if (size == 0) return "";

    std::vector<uint8_t> buf(size);
    if (GetSystemFirmwareTable(sig, 0, buf.data(), size) == 0) return "";

    const uint8_t* data = buf.data();
    size_t len = buf.size();
    size_t idx = 8; // Pula cabeçalho de 8 bytes de RawSMBIOSData
    while (idx + 4 <= len) {
        uint8_t stType = data[idx];
        uint8_t stLen = data[idx + 1];
        if (stLen < 4) break;
        if (stType == 1 && stLen >= 0x19) {
            const uint8_t* rawUuid = &data[idx + 8];
            bool allZero = true;
            bool allFF = true;
            for (int i = 0; i < 16; ++i) {
                if (rawUuid[i] != 0x00) allZero = false;
                if (rawUuid[i] != 0xFF) allFF = false;
            }
            if (!allZero && !allFF) {
                std::stringstream ss;
                for (int i = 0; i < 16; ++i) {
                    ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(rawUuid[i]);
                }
                return ss.str();
            }
            break;
        }
        size_t nextIdx = idx + stLen;
        while (nextIdx + 1 < len && !(data[nextIdx] == 0 && data[nextIdx + 1] == 0)) {
            nextIdx++;
        }
        idx = nextIdx + 2;
    }
    return "";
}

static std::string GetBaseHardwareRawString() {
    // 1. MachineGuid do Registro
    std::string machineGuid = "00000000-0000-0000-0000-000000000000";
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        char guidBuf[128] = { 0 };
        DWORD bufSize = sizeof(guidBuf);
        if (RegQueryValueExA(hKey, "MachineGuid", nullptr, nullptr, reinterpret_cast<LPBYTE>(guidBuf), &bufSize) == ERROR_SUCCESS) {
            machineGuid = guidBuf;
        }
        RegCloseKey(hKey);
    }

    // 2. CPU Identifier
    std::string cpuId = "AMD64_GENERIC_CPU";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char cpuBuf[128] = { 0 };
        DWORD bufSize = sizeof(cpuBuf);
        if (RegQueryValueExA(hKey, "Identifier", nullptr, nullptr, reinterpret_cast<LPBYTE>(cpuBuf), &bufSize) == ERROR_SUCCESS) {
            cpuId = cpuBuf;
        }
        RegCloseKey(hKey);
    }

    // 3. Volume Serial Number C:
    DWORD volSerial = 0;
    GetVolumeInformationW(L"C:\\", nullptr, 0, &volSerial, nullptr, nullptr, nullptr, 0);

    // Normaliza para lowercase
    std::transform(machineGuid.begin(), machineGuid.end(), machineGuid.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    std::stringstream ssVol;
    ssVol << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << volSerial;

    return machineGuid + "|" + cpuId + "|" + ssVol.str();
}

std::string GetRawHWID_v1() {
    std::string raw = GetBaseHardwareRawString();
    auto hash = ComputeSHA256(reinterpret_cast<const uint8_t*>(raw.data()), raw.length());
    std::stringstream ssHash;
    for (int i = 0; i < 8; ++i) { // 8 bytes = 16 hex chars
        ssHash << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ssHash.str();
}

std::string GetRawHWID_v2() {
    std::string base = GetBaseHardwareRawString();
    std::string smbios = GetSmbiosUuid();
    std::string raw = smbios.empty() ? base : (base + "|" + smbios);
    auto hash = ComputeSHA256(reinterpret_cast<const uint8_t*>(raw.data()), raw.length());
    std::stringstream ssHash;
    for (int i = 0; i < 8; ++i) { // 8 bytes = 16 hex chars
        ssHash << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ssHash.str();
}

std::string GetRawHWID() {
    return GetRawHWID_v2();
}

std::string GetMachineHWID() {
    std::string raw = GetRawHWID();
    if (raw.length() < 16) return "UM2-UNKNOWN-HWID";
    // Formata: UM2-XXXX-XXXX-XXXX-XXXX
    return "UM2-" + raw.substr(0, 4) + "-" + raw.substr(4, 4) + "-" + raw.substr(8, 4) + "-" + raw.substr(12, 4);
}

// =========================================================================
// VALIDAÇÃO DE CHAVE CRIPTOGRÁFICA
// =========================================================================

bool ValidateKey(const std::string& key, const std::string& rawHwid, std::string& outTier) {
    // Normaliza a chave removendo prefixo 'UM2-' e hifens
    std::string clean;
    for (char c : key) {
        if (c != '-' && c != ' ') {
            clean += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    if (clean.rfind("UM2", 0) == 0) {
        clean = clean.substr(3);
    }
    if (clean.length() != 16) {
        return false;
    }

    uint8_t blob[10];
    if (!DecodeBase32_16CharsTo10Bytes(clean, blob)) {
        return false;
    }

    uint8_t macReceived[8];
    std::memcpy(macReceived, blob, 8);
    uint16_t crcReceived = (static_cast<uint16_t>(blob[8]) << 8) | static_cast<uint16_t>(blob[9]);

    // Checagem 1: CRC16 anti-corrupção
    if (ComputeCRC16(macReceived, 8) != crcReceived) {
        return false;
    }

    // Checagem 2: Validação contra HWID local (Tier PRO)
    uint8_t secretBuf[59];
    GetSecretKey(secretBuf);

    std::string payload = rawHwid + ":PRO";
    auto expectedMac = ComputeHMAC_SHA256(
        secretBuf,
        sizeof(secretBuf),
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.length()
    );

    SecureZeroMemory(secretBuf, sizeof(secretBuf));

    if (ConstantTimeCompare(macReceived, expectedMac.data(), 8)) {
        outTier = "PRO_LIFETIME";
        return true;
    }

    return false;
}

bool ValidateKeyForCurrentMachine(const std::string& key, std::string& outTier) {
    std::string hwidV2 = GetRawHWID_v2();
    if (ValidateKey(key, hwidV2, outTier)) {
        return true;
    }
    std::string hwidV1 = GetRawHWID_v1();
    if (hwidV1 != hwidV2 && ValidateKey(key, hwidV1, outTier)) {
        return true;
    }
    return false;
}

// =========================================================================
// CARREGAR E SALVAR LICENÇA
// =========================================================================

static std::wstring GetLicenseFilePath() {
    wchar_t localAppData[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        std::wstring dir = std::wstring(localAppData) + L"\\KaxWav\\UM2";
        CreateDirectoryW((std::wstring(localAppData) + L"\\KaxWav").c_str(), NULL);
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\license.key";
    }
    return L"license.key";
}

LicenseInfo CheckLicense() {
    LicenseInfo info;
    info.rawHwid = GetRawHWID();
    info.hwid = GetMachineHWID();

    std::string keyToTest;

    // 1. Tenta ler do Registro do Windows (HKCU)
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\BehringerUM2\\ASIO", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char keyBuf[128] = { 0 };
        DWORD bufSize = sizeof(keyBuf);
        if (RegQueryValueExA(hKey, "LicenseKey", nullptr, nullptr, reinterpret_cast<LPBYTE>(keyBuf), &bufSize) == ERROR_SUCCESS) {
            keyToTest = keyBuf;
        }
        RegCloseKey(hKey);
    }

    // 2. Se não achou no registro, tenta ler do arquivo local
    if (keyToTest.empty()) {
        std::wstring licPath = GetLicenseFilePath();
        std::ifstream f(licPath);
        if (f.is_open()) {
            std::getline(f, keyToTest);
            // Remove espaços/quebras
            keyToTest.erase(std::remove_if(keyToTest.begin(), keyToTest.end(), [](unsigned char c) { return std::isspace(c); }), keyToTest.end());
        }
    }

    if (!keyToTest.empty()) {
        std::string tier;
        if (ValidateKeyForCurrentMachine(keyToTest, tier)) {
            info.isLicensed = true;
            info.tier = tier;
            info.daysLeft = 0xFFFFFFFF; // Vitalício
            info.licenseKey = keyToTest;
            if (ValidateKey(keyToTest, GetRawHWID_v1(), tier)) {
                info.rawHwid = GetRawHWID_v1();
                info.hwid = "UM2-" + info.rawHwid.substr(0, 4) + "-" + info.rawHwid.substr(4, 4) + "-" + info.rawHwid.substr(8, 4) + "-" + info.rawHwid.substr(12, 4);
            }
            return info;
        }
    }

    info.isLicensed = false;
    info.tier = "DEMO";
    info.daysLeft = 0;
    info.licenseKey = "";
    return info;
}

bool ActivateLicense(const std::string& key, LicenseInfo& outInfo) {
    std::string tier;
    if (!ValidateKeyForCurrentMachine(key, tier)) {
        return false;
    }

    std::string matchedHwid = GetRawHWID_v2();
    if (!ValidateKey(key, matchedHwid, tier)) {
        matchedHwid = GetRawHWID_v1();
    }

    // Grava no Registro do Windows
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BehringerUM2\\ASIO", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "LicenseKey", 0, REG_SZ, reinterpret_cast<const BYTE*>(key.c_str()), static_cast<DWORD>(key.length() + 1));
        RegCloseKey(hKey);
    }

    // Grava no Arquivo Local (%LOCALAPPDATA%\KaxWav\UM2\license.key)
    std::wstring licPath = GetLicenseFilePath();
    std::ofstream f(licPath, std::ios::trunc);
    if (f.is_open()) {
        f << key << std::endl;
        f.close();
    }

    outInfo.isLicensed = true;
    outInfo.tier = tier;
    outInfo.daysLeft = 0xFFFFFFFF;
    outInfo.rawHwid = matchedHwid;
    outInfo.hwid = "UM2-" + matchedHwid.substr(0, 4) + "-" + matchedHwid.substr(4, 4) + "-" + matchedHwid.substr(8, 4) + "-" + matchedHwid.substr(12, 4);
    outInfo.licenseKey = key;
    return true;
}

} // namespace UM2License
