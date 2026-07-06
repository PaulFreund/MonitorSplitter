#pragma once

#include "Layout.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace MonitorSplitter
{
static constexpr size_t kSyntheticEdidSize = 128;

inline WORD EncodeEdidManufacturerId(char first, char second, char third)
{
    return static_cast<WORD>(((first - 'A' + 1) << 10) |
                             ((second - 'A' + 1) << 5) |
                             (third - 'A' + 1));
}

inline bool IsSyntheticEdidChecksumValid(const std::array<BYTE, kSyntheticEdidSize>& edid)
{
    BYTE sum = 0;
    for (const auto value : edid)
    {
        sum = static_cast<BYTE>(sum + value);
    }

    return sum == 0;
}

inline void SetEdidTextDescriptor(std::array<BYTE, kSyntheticEdidSize>& edid, size_t offset, BYTE tag, const std::string& text)
{
    edid[offset + 0] = 0x00;
    edid[offset + 1] = 0x00;
    edid[offset + 2] = 0x00;
    edid[offset + 3] = tag;
    edid[offset + 4] = 0x00;

    for (size_t i = 0; i < 13; i++)
    {
        edid[offset + 5 + i] = 0x20;
    }

    const size_t count = std::min<size_t>(text.size(), 12);
    for (size_t i = 0; i < count; i++)
    {
        edid[offset + 5 + i] = static_cast<BYTE>(text[i]);
    }
    edid[offset + 5 + count] = 0x0A;
}

inline bool IsAsciiAlphaNumeric(wchar_t value)
{
    return (value >= L'0' && value <= L'9') ||
           (value >= L'A' && value <= L'Z') ||
           (value >= L'a' && value <= L'z');
}

inline std::string SanitizeEdidNameBase(const std::wstring& value)
{
    std::string sanitized;
    for (const auto ch : value)
    {
        if (IsAsciiAlphaNumeric(ch))
        {
            sanitized.push_back(static_cast<char>(ch));
        }
    }
    return sanitized;
}

inline std::string SanitizeEdidNameBase(const std::string& value)
{
    std::string sanitized;
    for (const auto ch : value)
    {
        const unsigned char valueByte = static_cast<unsigned char>(ch);
        if ((valueByte >= '0' && valueByte <= '9') ||
            (valueByte >= 'A' && valueByte <= 'Z') ||
            (valueByte >= 'a' && valueByte <= 'z'))
        {
            sanitized.push_back(static_cast<char>(valueByte));
        }
    }
    return sanitized;
}

inline std::string EdidIndexToken(size_t index)
{
    if (index < 26)
    {
        return std::string(1, static_cast<char>('A' + index));
    }

    char token[8] = {};
    sprintf_s(token, "%u", static_cast<unsigned>(index + 1));
    return token;
}

inline DWORD UpdateEdidIdentityHash(DWORD hash, BYTE value)
{
    hash ^= static_cast<DWORD>(value);
    hash *= 16777619u;
    return hash;
}

inline DWORD UpdateEdidIdentityHash(DWORD hash, DWORD value)
{
    for (size_t shift = 0; shift < 32; shift += 8)
    {
        hash = UpdateEdidIdentityHash(hash, static_cast<BYTE>((value >> shift) & 0xFF));
    }
    return hash;
}

inline DWORD UpdateEdidIdentityHash(DWORD hash, const std::string& value)
{
    for (const char ch : value)
    {
        hash = UpdateEdidIdentityHash(hash, static_cast<BYTE>(ch));
    }
    return hash;
}

inline DWORD EdidIdentityHash(const MonitorMode& mode, size_t count, size_t index, const std::string& baseName)
{
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(count);
    UNREFERENCED_PARAMETER(baseName);
    return 0x4D535000u + static_cast<DWORD>(index) + 1u;
}

inline WORD EdidProductCodeFromIdentity(DWORD identityHash, size_t index)
{
    UNREFERENCED_PARAMETER(identityHash);
    return static_cast<WORD>(0x5000u + static_cast<DWORD>(index));
}

inline std::string EdidMonitorNameFromBase(const std::string& baseName, size_t index)
{
    std::string base = SanitizeEdidNameBase(baseName);
    if (base.empty())
    {
        base = "MSplit";
    }

    const std::string suffix = "_" + EdidIndexToken(index);
    const size_t maxBaseLength = suffix.size() >= 12 ? 1 : 12 - suffix.size();
    if (base.size() > maxBaseLength)
    {
        base.resize(maxBaseLength);
    }

    return base + suffix;
}

inline std::wstring WidenAscii(const std::string& value)
{
    std::wstring widened;
    widened.reserve(value.size());
    for (const auto ch : value)
    {
        widened.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    return widened;
}

inline std::string EdidMonitorName(size_t count, size_t index, const std::string& baseName = {})
{
    if (!baseName.empty())
    {
        return EdidMonitorNameFromBase(baseName, index);
    }

    if (count == 1)
    {
        return "MSplit";
    }
    if (count == 2)
    {
        return index == 0 ? "MSplit Left" : "MSplit Right";
    }
    if (count == 3)
    {
        if (index == 0)
        {
            return "MSplit Left";
        }
        if (index == 1)
        {
            return "MSplit Ctr";
        }
        return "MSplit Right";
    }

    char name[14] = {};
    sprintf_s(name, "MSplit %u", static_cast<unsigned>(index + 1));
    return name;
}

inline std::wstring EdidMonitorNameWide(size_t count, size_t index, const std::wstring& baseName = {})
{
    return WidenAscii(EdidMonitorName(count, index, SanitizeEdidNameBase(baseName)));
}

inline std::wstring EdidIndexSuffixWide(size_t index)
{
    return WidenAscii("_" + EdidIndexToken(index));
}

inline bool FillDetailedTiming(
    std::array<BYTE, kSyntheticEdidSize>& edid,
    size_t offset,
    const MonitorMode& mode)
{
    if (mode.Width == 0 ||
        mode.Height == 0 ||
        mode.Width > 4095 ||
        mode.Height > 4095 ||
        mode.Refresh == 0)
    {
        return false;
    }

    const DWORD hBlank = 160;
    const DWORD vBlank = 45;
    const DWORD hSyncOffset = 48;
    const DWORD hSyncPulse = 32;
    const DWORD vSyncOffset = 3;
    const DWORD vSyncPulse = 5;
    const DWORD hTotal = mode.Width + hBlank;
    const DWORD vTotal = mode.Height + vBlank;
    const ULONGLONG pixelClock10KHz =
        (static_cast<ULONGLONG>(hTotal) * static_cast<ULONGLONG>(vTotal) * mode.Refresh + 9999) / 10000;

    if (hBlank > 4095 ||
        vBlank > 4095 ||
        hSyncOffset > 1023 ||
        hSyncPulse > 1023 ||
        vSyncOffset > 63 ||
        vSyncPulse > 63 ||
        pixelClock10KHz == 0 ||
        pixelClock10KHz > 0xFFFF)
    {
        return false;
    }

    const DWORD widthMm = std::max<DWORD>(160, std::min<DWORD>(4095, (mode.Width * 254 + 480) / 960));
    const DWORD heightMm = std::max<DWORD>(90, std::min<DWORD>(4095, (mode.Height * 254 + 480) / 960));

    edid[offset + 0] = static_cast<BYTE>(pixelClock10KHz & 0xFF);
    edid[offset + 1] = static_cast<BYTE>((pixelClock10KHz >> 8) & 0xFF);
    edid[offset + 2] = static_cast<BYTE>(mode.Width & 0xFF);
    edid[offset + 3] = static_cast<BYTE>(hBlank & 0xFF);
    edid[offset + 4] = static_cast<BYTE>(((mode.Width >> 8) & 0x0F) << 4 | ((hBlank >> 8) & 0x0F));
    edid[offset + 5] = static_cast<BYTE>(mode.Height & 0xFF);
    edid[offset + 6] = static_cast<BYTE>(vBlank & 0xFF);
    edid[offset + 7] = static_cast<BYTE>(((mode.Height >> 8) & 0x0F) << 4 | ((vBlank >> 8) & 0x0F));
    edid[offset + 8] = static_cast<BYTE>(hSyncOffset & 0xFF);
    edid[offset + 9] = static_cast<BYTE>(hSyncPulse & 0xFF);
    edid[offset + 10] = static_cast<BYTE>(((vSyncOffset & 0x0F) << 4) | (vSyncPulse & 0x0F));
    edid[offset + 11] = static_cast<BYTE>(((hSyncOffset >> 8) & 0x03) << 6 |
                                          ((hSyncPulse >> 8) & 0x03) << 4 |
                                          ((vSyncOffset >> 4) & 0x03) << 2 |
                                          ((vSyncPulse >> 4) & 0x03));
    edid[offset + 12] = static_cast<BYTE>(widthMm & 0xFF);
    edid[offset + 13] = static_cast<BYTE>(heightMm & 0xFF);
    edid[offset + 14] = static_cast<BYTE>(((widthMm >> 8) & 0x0F) << 4 | ((heightMm >> 8) & 0x0F));
    edid[offset + 15] = 0x00;
    edid[offset + 16] = 0x00;
    edid[offset + 17] = 0x1E;
    return true;
}

inline bool BuildSyntheticEdid(
    const MonitorMode& mode,
    size_t count,
    size_t index,
    std::array<BYTE, kSyntheticEdidSize>& edid,
    const std::string& edidNameBase = {})
{
    edid = {};

    static const BYTE header[] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    std::memcpy(edid.data(), header, sizeof(header));

    const WORD manufacturer = EncodeEdidManufacturerId('M', 'S', 'P');
    edid[8] = static_cast<BYTE>((manufacturer >> 8) & 0xFF);
    edid[9] = static_cast<BYTE>(manufacturer & 0xFF);
    const DWORD identityHash = EdidIdentityHash(mode, count, index, edidNameBase);
    const WORD productCode = EdidProductCodeFromIdentity(identityHash, index);
    edid[10] = static_cast<BYTE>(productCode & 0xFF);
    edid[11] = static_cast<BYTE>((productCode >> 8) & 0xFF);
    const DWORD serial = identityHash;
    edid[12] = static_cast<BYTE>(serial & 0xFF);
    edid[13] = static_cast<BYTE>((serial >> 8) & 0xFF);
    edid[14] = static_cast<BYTE>((serial >> 16) & 0xFF);
    edid[15] = static_cast<BYTE>((serial >> 24) & 0xFF);
    edid[16] = 1;
    edid[17] = 36;
    edid[18] = 1;
    edid[19] = 4;
    edid[20] = 0xA5;
    edid[21] = static_cast<BYTE>(std::max<DWORD>(1, std::min<DWORD>(255, (mode.Width * 254 + 4800) / 9600)));
    edid[22] = static_cast<BYTE>(std::max<DWORD>(1, std::min<DWORD>(255, (mode.Height * 254 + 4800) / 9600)));
    edid[23] = 0x78;
    edid[24] = 0x0A;

    const BYTE chromaticity[] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
    std::memcpy(&edid[25], chromaticity, sizeof(chromaticity));

    edid[35] = 0x00;
    edid[36] = 0x00;
    edid[37] = 0x00;
    for (size_t i = 38; i <= 53; i++)
    {
        edid[i] = 0x01;
    }

    if (!FillDetailedTiming(edid, 54, mode))
    {
        return false;
    }

    char serialText[13] = {};
    sprintf_s(serialText, "MSP%08X", static_cast<unsigned>(identityHash));
    SetEdidTextDescriptor(edid, 72, 0xFF, serialText);
    SetEdidTextDescriptor(edid, 90, 0xFC, EdidMonitorName(count, index, edidNameBase));

    edid[108] = 0x00;
    edid[109] = 0x00;
    edid[110] = 0x00;
    edid[111] = 0xFD;
    edid[112] = 0x00;
    edid[113] = 30;
    edid[114] = static_cast<BYTE>(std::min<DWORD>(240, std::max<DWORD>(mode.Refresh, 60)));
    const DWORD verticalTotal = mode.Height + 45;
    const DWORD horizontalKHz = std::max<DWORD>(1, std::min<DWORD>(255, (verticalTotal * mode.Refresh + 500) / 1000));
    edid[115] = static_cast<BYTE>(horizontalKHz);
    edid[116] = static_cast<BYTE>(horizontalKHz);
    const DWORD descriptorPixelClock10KHz = static_cast<DWORD>(edid[54]) | (static_cast<DWORD>(edid[55]) << 8);
    const DWORD pixelClock10MHz = std::max<DWORD>(1, std::min<DWORD>(255, (descriptorPixelClock10KHz + 999) / 1000));
    edid[117] = static_cast<BYTE>(pixelClock10MHz);
    edid[118] = 0x00;
    edid[119] = 0x0A;
    for (size_t i = 120; i <= 125; i++)
    {
        edid[i] = 0x20;
    }

    edid[126] = 0;
    edid[127] = 0;
    BYTE sum = 0;
    for (size_t i = 0; i < 127; i++)
    {
        sum = static_cast<BYTE>(sum + edid[i]);
    }
    edid[127] = static_cast<BYTE>((256 - sum) & 0xFF);
    return true;
}

inline bool DecodeSyntheticEdidMode(
    const BYTE* edid,
    size_t size,
    MonitorMode& mode)
{
    if (edid == nullptr || size < kSyntheticEdidSize)
    {
        return false;
    }

    static const BYTE header[] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (std::memcmp(edid, header, sizeof(header)) != 0)
    {
        return false;
    }

    BYTE sum = 0;
    for (size_t i = 0; i < kSyntheticEdidSize; i++)
    {
        sum = static_cast<BYTE>(sum + edid[i]);
    }
    if (sum != 0)
    {
        return false;
    }

    const WORD manufacturer = static_cast<WORD>((edid[8] << 8) | edid[9]);
    if (manufacturer != EncodeEdidManufacturerId('M', 'S', 'P'))
    {
        return false;
    }

    const size_t offset = 54;
    const DWORD pixelClock10KHz = static_cast<DWORD>(edid[offset + 0]) |
                                  (static_cast<DWORD>(edid[offset + 1]) << 8);
    const DWORD hActive = static_cast<DWORD>(edid[offset + 2]) |
                          ((static_cast<DWORD>(edid[offset + 4]) & 0xF0) << 4);
    const DWORD hBlank = static_cast<DWORD>(edid[offset + 3]) |
                         ((static_cast<DWORD>(edid[offset + 4]) & 0x0F) << 8);
    const DWORD vActive = static_cast<DWORD>(edid[offset + 5]) |
                          ((static_cast<DWORD>(edid[offset + 7]) & 0xF0) << 4);
    const DWORD vBlank = static_cast<DWORD>(edid[offset + 6]) |
                         ((static_cast<DWORD>(edid[offset + 7]) & 0x0F) << 8);

    if (pixelClock10KHz == 0 || hActive == 0 || vActive == 0)
    {
        return false;
    }

    const ULONGLONG denominator = static_cast<ULONGLONG>(hActive + hBlank) * static_cast<ULONGLONG>(vActive + vBlank);
    if (denominator == 0)
    {
        return false;
    }

    mode.Width = hActive;
    mode.Height = vActive;
    mode.Refresh = static_cast<DWORD>((static_cast<ULONGLONG>(pixelClock10KHz) * 10000 + (denominator / 2)) / denominator);
    return mode.Refresh != 0;
}
}
