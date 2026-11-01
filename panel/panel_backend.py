import os
import sys
import time
import json
import asyncio
import ctypes
import winreg
import hmac
import hashlib
import struct
import threading
import subprocess
from pathlib import Path
from ctypes import wintypes
from typing import Optional, List

# Redireciona stdout e stderr para evitar quebras em executaveis --windowed / --noconsole
log_dir = os.path.expandvars(r"%LOCALAPPDATA%\KaxWav\UM2")
try:
    os.makedirs(log_dir, exist_ok=True)
except Exception:
    pass
panel_log_path = os.path.join(log_dir, "panel.log")

class LoggerWriter:
    def __init__(self, filepath, stream):
        try:
            self.file = open(filepath, "a", encoding="utf-8")
        except Exception:
            self.file = None
        self.stream = stream
    def write(self, data):
        if self.file:
            try:
                self.file.write(data)
                self.file.flush()
            except Exception:
                pass
        if self.stream:
            try:
                self.stream.write(data)
                self.stream.flush()
            except Exception:
                pass
    def flush(self):
        if self.file:
            try:
                self.file.flush()
            except Exception:
                pass
        if self.stream:
            try:
                self.stream.flush()
            except Exception:
                pass
    def isatty(self):
        return False
    def fileno(self):
        try:
            return self.file.fileno() if self.file else -1
        except Exception:
            return -1

sys.stdout = LoggerWriter(panel_log_path, sys.stdout)
sys.stderr = LoggerWriter(panel_log_path, sys.stderr)

import mimetypes
import urllib.request
import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel

UM2_IPC_MAGIC = 0x554D3230  # "UM20"
UM2_SHARED_MEM_NAME = "Local\\BehringerUM2SharedMemory"
REG_PATH = r"Software\BehringerUM2\ASIO"

CHARSET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
from um2_license_secret import OBFUSCATED_SECRET  # gitignored; see um2_license_secret.example.py

def get_secret_key() -> bytes:
    return bytes([b ^ ((0x5A + i) & 0xFF) for i, b in enumerate(OBFUSCATED_SECRET)])

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc

def decode_b32_16chars(s: str) -> bytes:
    n = 0
    for c in s:
        idx = CHARSET.index(c)
        n = (n << 5) | idx
    return n.to_bytes(10, "big")

def encode_b32_10bytes(data: bytes) -> str:
    n = int.from_bytes(data, "big")
    chars = []
    for _ in range(16):
        chars.append(CHARSET[n & 0x1F])
        n >>= 5
    return "".join(reversed(chars))

def get_system_smbios_uuid() -> str:
    try:
        kernel32 = ctypes.windll.kernel32
        RSMB = 0x52534D42  # 'RSMB'
        size = kernel32.GetSystemFirmwareTable(RSMB, 0, None, 0)
        if size == 0:
            return ""
        buf = ctypes.create_string_buffer(size)
        if kernel32.GetSystemFirmwareTable(RSMB, 0, buf, size) == 0:
            return ""
        data = buf.raw
        idx = 8
        while idx + 4 <= len(data):
            st_type = data[idx]
            st_len = data[idx + 1]
            if st_len < 4:
                break
            if st_type == 1 and st_len >= 0x19:
                raw_uuid = data[idx + 8 : idx + 24]
                if raw_uuid != b"\x00" * 16 and raw_uuid != b"\xFF" * 16:
                    return raw_uuid.hex().upper()
                break
            next_idx = idx + st_len
            while next_idx + 1 < len(data) and not (data[next_idx] == 0 and data[next_idx + 1] == 0):
                next_idx += 1
            idx = next_idx + 2
    except Exception:
        pass
    return ""

def get_system_hwid_raw(version: int = 2) -> str:
    guid = "00000000-0000-0000-0000-000000000000"
    try:
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Cryptography", 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
        guid, _ = winreg.QueryValueEx(k, "MachineGuid")
        winreg.CloseKey(k)
    except Exception:
        pass

    cpu_id = "AMD64_GENERIC_CPU"
    try:
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0", 0, winreg.KEY_READ)
        cpu_id, _ = winreg.QueryValueEx(k, "Identifier")
        winreg.CloseKey(k)
    except Exception:
        pass

    vol = ctypes.c_ulong()
    ctypes.windll.kernel32.GetVolumeInformationW("C:\\", None, 0, ctypes.byref(vol), None, None, None, 0)

    raw_base = f"{guid.strip().lower()}|{cpu_id.strip()}|{vol.value:08X}"
    if version == 2:
        smbios = get_system_smbios_uuid()
        raw = f"{raw_base}|{smbios}" if smbios else raw_base
    else:
        raw = raw_base

    h = hashlib.sha256(raw.encode("utf-8")).hexdigest().upper()
    return h[:16]

def get_system_hwid_formatted() -> str:
    raw = get_system_hwid_raw(version=2)
    return f"UM2-{raw[:4]}-{raw[4:8]}-{raw[8:12]}-{raw[12:16]}"

def validate_license_key(key: str, raw_hwid: str) -> tuple[bool, str]:
    clean = key.replace("UM2-", "").replace("-", "").strip().upper()
    if len(clean) != 16:
        return False, "DEMO"
    try:
        blob = decode_b32_16chars(clean)
        mac_recv = blob[:8]
        chk_recv = struct.unpack(">H", blob[8:])[0]
        if crc16(mac_recv) != chk_recv:
            return False, "DEMO"
        
        clean_hwid = raw_hwid.replace("UM2-", "").replace("-", "").strip().upper()
        payload = f"{clean_hwid}:PRO".encode("utf-8")
        expected_mac = hmac.new(get_secret_key(), payload, hashlib.sha256).digest()[:8]
        if hmac.compare_digest(mac_recv, expected_mac):
            return True, "PRO_LIFETIME"
            
        return False, "DEMO"
    except Exception:
        return False, "DEMO"

def validate_key_for_current_machine(key: str) -> tuple[bool, str, str]:
    hwid_v2 = get_system_hwid_raw(version=2)
    valid, tier = validate_license_key(key, hwid_v2)
    if valid:
        return True, tier, hwid_v2
    hwid_v1 = get_system_hwid_raw(version=1)
    if hwid_v1 != hwid_v2:
        valid, tier = validate_license_key(key, hwid_v1)
        if valid:
            return True, tier, hwid_v1
    return False, "DEMO", hwid_v2

LOCAL_APPDATA = os.environ.get("LOCALAPPDATA", "")
if LOCAL_APPDATA:
    CONFIG_PATH = Path(LOCAL_APPDATA) / "KaxWav" / "UM2" / "um2_config.json"
    LICENSE_FILE = Path(LOCAL_APPDATA) / "KaxWav" / "UM2" / "license.key"
else:
    CONFIG_PATH = Path(__file__).resolve().parent / "um2_config.json"
    LICENSE_FILE = Path(__file__).resolve().parent / "license.key"

PAGE_READWRITE = 0x04
FILE_MAP_ALL_ACCESS = 0xF001F

class UM2SharedState(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("isStreaming", ctypes.c_uint32),
        ("sampleRate", ctypes.c_uint32),
        ("bufferSize", ctypes.c_uint32),
        ("inputChannels", ctypes.c_uint32),
        ("outputChannels", ctypes.c_uint32),
        ("bufferSwitchCount", ctypes.c_uint64),
        ("xruns", ctypes.c_uint32),
        ("isPhysicalHardware", ctypes.c_uint32),
        ("peakIn1", ctypes.c_float),
        ("peakIn2", ctypes.c_float),
        ("peakLoopbackL", ctypes.c_float),
        ("peakLoopbackR", ctypes.c_float),
        ("peakOut1", ctypes.c_float),
        ("peakOut2", ctypes.c_float),
        ("loopbackEnabled", ctypes.c_uint32),
        ("loopbackGain", ctypes.c_float),
        ("softwareMute", ctypes.c_uint32),
        ("requestedBufferSize", ctypes.c_uint32),
        ("reloadFlag", ctypes.c_uint32),
        ("driverStatus", ctypes.c_uint32),
        ("driverMode", ctypes.c_uint32),
        ("requestedDriverMode", ctypes.c_uint32),
        # DSP CH1
        ("ch1Phase", ctypes.c_int32),
        ("ch1Hpf", ctypes.c_int32),
        ("ch1Mute", ctypes.c_int32),
        ("ch1Trim", ctypes.c_float),
        # DSP CH2
        ("ch2Phase", ctypes.c_int32),
        ("ch2Pad", ctypes.c_int32),
        ("ch2Mute", ctypes.c_int32),
        ("ch2Trim", ctypes.c_float),
        # DSP LOOPBACK
        ("loopbackMono", ctypes.c_int32),
        # DSP MASTER
        ("masterDim", ctypes.c_int32),
        ("masterMono", ctypes.c_int32),
        ("masterMute", ctypes.c_int32),
        ("masterTrim", ctypes.c_float),
        # SCOPE
        ("scopeSource", ctypes.c_uint32),
        ("scopeBuffer", ctypes.c_float * 128),
        # SISTEMA DE LICENCIAMENTO & ANTI-PIRATARIA HWID
        ("isLicensed", ctypes.c_uint32),
        ("licenseDaysLeft", ctypes.c_uint32),
        ("licenseTier", ctypes.c_char * 16),
        ("machineHwid", ctypes.c_char * 40),
        # NOISE GATE & BROADCAST MIX
        ("ch1Gate", ctypes.c_uint32),
        ("ch2Gate", ctypes.c_uint32),
        ("gateThresholdDb", ctypes.c_float),
        ("loopbackBroadcastMix", ctypes.c_uint32),
        # UNDERGROUND STUDIO CONSOLE v3.0: ANALOG PREAMP & CHANNEL STRIP
        ("ch1PreampMode", ctypes.c_int32),
        ("ch2PreampMode", ctypes.c_int32),
        ("ch1CompAmount", ctypes.c_float),
        ("ch2CompAmount", ctypes.c_float),
        ("ch1DeesserAmount", ctypes.c_float),
        ("ch2CabSim", ctypes.c_int32),
        ("dspMonitorOnly", ctypes.c_int32),
        # MONITOR ROOM & MIX CHECK (SAIDA MASTER POS-DAW)
        ("monSubCheck", ctypes.c_int32),
        ("monPhoneSim", ctypes.c_int32),
        ("monCrossfeed", ctypes.c_int32),
        # SAMPLER PER-APP LOOPBACK (WINDOWS PROCESS LOOPBACK)
        ("loopbackMode", ctypes.c_int32),
        ("loopbackTargetPid", ctypes.c_uint32),
        # TELEMETRIA DE ESTUDIO, CLIP FISICO DO CONVERSOR & AFINADOR CROMATICO
        ("ch1HwClip", ctypes.c_int32),
        ("ch2HwClip", ctypes.c_int32),
        ("ch1GainReductionDb", ctypes.c_float),
        ("ch2GainReductionDb", ctypes.c_float),
        ("tunerSource", ctypes.c_int32),
        ("tunerFreqHz", ctypes.c_float),
        ("tunerMidiNote", ctypes.c_int32),
        ("tunerCents", ctypes.c_float),
        ("dspCpuLoadPercent", ctypes.c_float),
        ("pdcOffsetSamples", ctypes.c_int32),
        ("inputLatencySamples", ctypes.c_int32),
        ("outputLatencySamples", ctypes.c_int32),
        ("hardwareModel", ctypes.c_int32),
    ]

# ============================================================================
# CONTROLE GLOBAL WDM DE MUTE NO WINDOWS / DISCORD (CoreAudio COM nativo)
# ============================================================================
HRESULT = ctypes.c_long
ole32 = ctypes.windll.ole32

class COM_GUID(ctypes.Structure):
    _fields_ = [('Data1', wintypes.DWORD), ('Data2', wintypes.WORD),
                ('Data3', wintypes.WORD), ('Data4', wintypes.BYTE * 8)]
    def __init__(self, s):
        super().__init__()
        import uuid
        u = uuid.UUID(s)
        self.Data1 = u.time_low
        self.Data2 = u.time_mid
        self.Data3 = u.time_hi_version
        for i, b in enumerate(u.bytes[8:]):
            self.Data4[i] = b

class PROPERTYKEY(ctypes.Structure):
    _fields_ = [('fmtid', COM_GUID), ('pid', wintypes.DWORD)]

class PROPVARIANT(ctypes.Structure):
    _fields_ = [('vt', wintypes.USHORT), ('wReserved1', wintypes.WORD),
                ('wReserved2', wintypes.WORD), ('wReserved3', wintypes.WORD),
                ('pwszVal', wintypes.LPWSTR), ('dummy', wintypes.BYTE * 8)]

CLSID_MMDeviceEnumerator = COM_GUID('BCDE0395-E52F-467C-8E3D-C4579291692E')
IID_IMMDeviceEnumerator = COM_GUID('A95664D2-9614-4F35-A746-DE8DB63617E6')
IID_IAudioEndpointVolume = COM_GUID('5CDF2C82-841E-4546-9722-0CF74078229A')
PKEY_Device_FriendlyName = PROPERTYKEY(COM_GUID('a45c254e-df1c-4efd-8020-67d146a850e0'), 14)

class WindowsCoreAudioController:
    def __init__(self):
        try:
            ole32.CoInitialize(None)
        except Exception:
            pass

    def _set_endpoint_mute(self, dataflow: int, mute: bool, keywords: list[str]) -> bool:
        try:
            ole32.CoInitialize(None)
            pEnumerator = ctypes.c_void_p()
            hr = ole32.CoCreateInstance(
                ctypes.byref(CLSID_MMDeviceEnumerator), None, 1,
                ctypes.byref(IID_IMMDeviceEnumerator), ctypes.byref(pEnumerator)
            )
            if hr != 0 or not pEnumerator:
                return False

            vtable_enum = ctypes.cast(pEnumerator, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            EnumAudioEndpoints = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p))(vtable_enum[3])
            ReleaseEnum = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_enum[2])

            pCol = ctypes.c_void_p()
            hr = EnumAudioEndpoints(pEnumerator, dataflow, 1, ctypes.byref(pCol))
            if hr != 0 or not pCol:
                ReleaseEnum(pEnumerator)
                return False

            vtable_col = ctypes.cast(pCol, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            GetCount = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(wintypes.UINT))(vtable_col[3])
            Item = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p))(vtable_col[4])
            ReleaseCol = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_col[2])

            count = wintypes.UINT()
            GetCount(pCol, ctypes.byref(count))

            success = False
            for i in range(count.value):
                pDev = ctypes.c_void_p()
                if Item(pCol, i, ctypes.byref(pDev)) != 0 or not pDev:
                    continue

                vtable_dev = ctypes.cast(pDev, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
                OpenPropertyStore = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[4])
                Activate = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(COM_GUID), wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[3])
                ReleaseDev = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_dev[2])

                pStore = ctypes.c_void_p()
                name = ""
                if OpenPropertyStore(pDev, 0, ctypes.byref(pStore)) == 0 and pStore:
                    vtable_store = ctypes.cast(pStore, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
                    PKEY_Device_FriendlyName = PROPERTYKEY(COM_GUID('a45c254e-df1c-4efd-8020-67d146a850e0'), 14)
                    GetValue = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(PROPERTYKEY), ctypes.POINTER(PROPVARIANT))(vtable_store[5])
                    ReleaseStore = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_store[2])

                    pv = PROPVARIANT()
                    if GetValue(pStore, ctypes.byref(PKEY_Device_FriendlyName), ctypes.byref(pv)) == 0:
                        if pv.vt == 31 and pv.pwszVal:
                            name = str(pv.pwszVal)
                    ReleaseStore(pStore)

                name_lower = name.lower()
                matched = any(k in name_lower for k in keywords)

                if matched:
                    pVol = ctypes.c_void_p()
                    hr_act = Activate(pDev, ctypes.byref(IID_IAudioEndpointVolume), 1, None, ctypes.byref(pVol))
                    if hr_act == 0 and pVol:
                        vtable_vol = ctypes.cast(pVol, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
                        SetMute = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.BOOL, ctypes.c_void_p)(vtable_vol[14])
                        ReleaseVol = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_vol[2])

                        hr_mute = SetMute(pVol, wintypes.BOOL(mute), None)
                        if hr_mute == 0:
                            success = True
                        ReleaseVol(pVol)

                ReleaseDev(pDev)

            ReleaseCol(pCol)
            ReleaseEnum(pEnumerator)
            return success
        except Exception:
            return False

    def _get_endpoint_mute(self, data_flow: int, keywords: list) -> Optional[bool]:
        try:
            ctypes.windll.ole32.CoInitialize(None)
            pEnumerator = ctypes.c_void_p()
            hr = ctypes.windll.ole32.CoCreateInstance(
                ctypes.byref(CLSID_MMDeviceEnumerator),
                None,
                1,
                ctypes.byref(IID_IMMDeviceEnumerator),
                ctypes.byref(pEnumerator)
            )
            if hr != 0 or not pEnumerator:
                return None

            vtable_enum = ctypes.cast(pEnumerator, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            EnumAudioEndpoints = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.c_int, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p))(vtable_enum[3])
            ReleaseEnum = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_enum[2])

            pCol = ctypes.c_void_p()
            hr = EnumAudioEndpoints(pEnumerator, data_flow, 1, ctypes.byref(pCol))
            if hr != 0 or not pCol:
                ReleaseEnum(pEnumerator)
                return None

            vtable_col = ctypes.cast(pCol, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            GetCount = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(wintypes.UINT))(vtable_col[3])
            Item = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p))(vtable_col[4])
            ReleaseCol = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_col[2])

            count = wintypes.UINT(0)
            GetCount(pCol, ctypes.byref(count))

            mute_state = None
            for i in range(count.value):
                pDev = ctypes.c_void_p()
                if Item(pCol, i, ctypes.byref(pDev)) != 0 or not pDev:
                    continue

                vtable_dev = ctypes.cast(pDev, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
                OpenPropertyStore = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[4])
                Activate = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(COM_GUID), wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[3])
                ReleaseDev = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_dev[2])

                pStore = ctypes.c_void_p()
                name = ""
                if OpenPropertyStore(pDev, 0, ctypes.byref(pStore)) == 0 and pStore:
                    vtable_store = ctypes.cast(pStore, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
                    GetValue = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(PROPERTYKEY), ctypes.POINTER(PROPVARIANT))(vtable_store[5])
                    ReleaseStore = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_store[2])

                    pv = PROPVARIANT()
                    if GetValue(pStore, ctypes.byref(PKEY_Device_FriendlyName), ctypes.byref(pv)) == 0:
                        if pv.vt == 31 and pv.pwszVal:
                            name = str(pv.pwszVal)
                    ReleaseStore(pStore)

                name_lower = name.lower()
                matched = any(k in name_lower for k in keywords)

                if matched:
                    pVol = ctypes.c_void_p()
                    hr_act = Activate(pDev, ctypes.byref(IID_IAudioEndpointVolume), 1, None, ctypes.byref(pVol))
                    if hr_act == 0 and pVol:
                        vtable_vol = ctypes.cast(pVol, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
                        GetMute = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(wintypes.BOOL))(vtable_vol[15])
                        ReleaseVol = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_vol[2])

                        is_muted = wintypes.BOOL(0)
                        if GetMute(pVol, ctypes.byref(is_muted)) == 0:
                            mute_state = bool(is_muted.value)
                        ReleaseVol(pVol)

                ReleaseDev(pDev)
                if mute_state is not None:
                    break

            ReleaseCol(pCol)
            ReleaseEnum(pEnumerator)
            return mute_state
        except Exception:
            return None

    def set_microphone_mute(self, mute: bool) -> bool:
        return self._set_endpoint_mute(1, mute, ['usb audio', 'u-phoria', 'um2', 'umc22', 'behringer'])

    def set_master_mute(self, mute: bool) -> bool:
        return self._set_endpoint_mute(0, mute, ['usb audio', 'u-phoria', 'um2', 'umc22', 'behringer'])

    def get_microphone_mute(self) -> Optional[bool]:
        return self._get_endpoint_mute(1, ['usb audio', 'u-phoria', 'um2', 'umc22', 'behringer'])

    def get_master_mute(self) -> Optional[bool]:
        return self._get_endpoint_mute(0, ['usb audio', 'u-phoria', 'um2', 'umc22', 'behringer'])

    def ensure_microphone_gain(self, min_scalar: float = 0.78) -> bool:
        """Garante que o ganho de captura USB Audio CODEC no Windows nao fique estrangulado em 53% (0 dB)."""
        if sys.platform != "win32":
            return False
        keywords = ['usb audio', 'u-phoria', 'um2', 'umc22', 'behringer']
        try:
            ole32 = ctypes.windll.ole32
            ole32.CoInitializeEx(None, 0)
            pEnumerator = ctypes.c_void_p()
            hr = ole32.CoCreateInstance(
                ctypes.byref(CLSID_MMDeviceEnumerator),
                None,
                23,
                ctypes.byref(IID_IMMDeviceEnumerator),
                ctypes.byref(pEnumerator)
            )
            if hr != 0 or not pEnumerator:
                return False

            vtable_enum = ctypes.cast(pEnumerator, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            EnumAudioEndpoints = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p))(vtable_enum[3])
            ReleaseEnum = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_enum[2])

            pCol = ctypes.c_void_p()
            if EnumAudioEndpoints(pEnumerator, 1, 1, ctypes.byref(pCol)) != 0 or not pCol:
                ReleaseEnum(pEnumerator)
                return False

            vtable_col = ctypes.cast(pCol, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            GetCount = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(wintypes.UINT))(vtable_col[3])
            Item = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p))(vtable_col[4])
            ReleaseCol = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_col[2])

            count = wintypes.UINT(0)
            GetCount(pCol, ctypes.byref(count))
            updated = False

            for i in range(count.value):
                pDev = ctypes.c_void_p()
                if Item(pCol, i, ctypes.byref(pDev)) != 0 or not pDev:
                    continue
                vtable_dev = ctypes.cast(pDev, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
                OpenPropertyStore = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[4])
                Activate = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(COM_GUID), wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(vtable_dev[3])
                ReleaseDev = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_dev[2])

                pStore = ctypes.c_void_p()
                name = ""
                if OpenPropertyStore(pDev, 0, ctypes.byref(pStore)) == 0 and pStore:
                    vtable_store = ctypes.cast(pStore, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
                    GetValue = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(PROPERTYKEY), ctypes.POINTER(PROPVARIANT))(vtable_store[5])
                    ReleaseStore = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_store[2])
                    pv = PROPVARIANT()
                    if GetValue(pStore, ctypes.byref(PKEY_Device_FriendlyName), ctypes.byref(pv)) == 0:
                        if pv.vt == 31 and pv.pwszVal:
                            name = str(pv.pwszVal)
                    ReleaseStore(pStore)

                if any(k in name.lower() for k in keywords):
                    pVol = ctypes.c_void_p()
                    if Activate(pDev, ctypes.byref(IID_IAudioEndpointVolume), 1, None, ctypes.byref(pVol)) == 0 and pVol:
                        vtable_vol = ctypes.cast(pVol, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
                        SetMasterVolumeLevelScalar = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.c_float, ctypes.c_void_p)(vtable_vol[7])
                        GetMasterVolumeLevelScalar = ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(ctypes.c_float))(vtable_vol[9])
                        ReleaseVol = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable_vol[2])

                        cur_scalar = ctypes.c_float(0.0)
                        if GetMasterVolumeLevelScalar(pVol, ctypes.byref(cur_scalar)) == 0:
                            if cur_scalar.value < min_scalar:
                                SetMasterVolumeLevelScalar(pVol, ctypes.c_float(min_scalar), None)
                                updated = True
                        ReleaseVol(pVol)
                ReleaseDev(pDev)

            ReleaseCol(pCol)
            ReleaseEnum(pEnumerator)
            return updated
        except Exception:
            return False

core_audio = WindowsCoreAudioController()
# core_audio.ensure_microphone_gain(0.86)  # desativado: forcava o mic do Windows em 86% (+ganho digital = chiado)

# ============================================================================
# GERENCIADOR DO MOTOR STANDALONE & DISCORD DSP BRIDGE (C++ DLL Nativa)
# ============================================================================
class BridgeManager:
    def __init__(self):
        self._lib = None

        meipass = getattr(sys, '_MEIPASS', None)
        candidates = [
            Path(sys.executable).parent / "BehringerUM2Bridge.dll" if getattr(sys, 'frozen', False) else None,
            Path(__file__).resolve().parent.parent / "build" / "Release" / "BehringerUM2Bridge.dll",
            Path(__file__).resolve().parent.parent / "release_package" / "BehringerUM2Bridge.dll",
            Path(__file__).resolve().parent / "BehringerUM2Bridge.dll",
            Path(meipass) / "BehringerUM2Bridge.dll" if meipass else None,
        ]
        for p in candidates:
            if p and p.exists():
                try:
                    lib = ctypes.CDLL(str(p))
                    lib.UM2Bridge_Start.restype = ctypes.c_bool
                    lib.UM2Bridge_Stop.restype = None
                    lib.UM2Bridge_IsCableDetected.restype = ctypes.c_bool
                    lib.UM2Bridge_IsStandaloneActive.restype = ctypes.c_bool
                    self._lib = lib
                    self._lib.UM2Bridge_Start()
                    break
                except Exception:
                    pass

    def is_cable_detected(self) -> bool:
        if self._lib:
            try:
                return bool(self._lib.UM2Bridge_IsCableDetected())
            except Exception:
                return False
        return False

    def is_standalone_active(self) -> bool:
        if self._lib:
            try:
                return bool(self._lib.UM2Bridge_IsStandaloneActive())
            except Exception:
                return False
        return False

    def stop(self):
        if self._lib:
            try:
                self._lib.UM2Bridge_Stop()
            except Exception:
                pass

bridge_manager = BridgeManager()

NOTE_NAMES_SHARP = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

def midi_note_to_name(midi_note: int) -> str:
    if midi_note <= 0 or midi_note > 127:
        return "--"
    octave = (midi_note // 12) - 1
    note = NOTE_NAMES_SHARP[midi_note % 12]
    return f"{note}{octave}"

def list_audio_processes() -> list:
    """Lista processos com janelas ativas ou reprodutores de audio para Per-App Loopback."""
    results = {}
    try:
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        my_pid = os.getpid()

        def enum_cb(hwnd, lParam):
            if user32.IsWindowVisible(hwnd):
                length = user32.GetWindowTextLengthW(hwnd)
                if length > 0:
                    buff = ctypes.create_unicode_buffer(length + 1)
                    user32.GetWindowTextW(hwnd, buff, length + 1)
                    title = buff.value.strip()
                    if not title or title in ("Program Manager", "Settings", "Windows Input Experience"):
                        return True
                    pid = wintypes.DWORD(0)
                    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                    p_val = int(pid.value)
                    if p_val and p_val != my_pid and p_val not in results:
                        exe_name = f"PID-{p_val}"
                        h_proc = kernel32.OpenProcess(0x1000, False, p_val)
                        if h_proc:
                            exe_buf = ctypes.create_unicode_buffer(260)
                            sz = wintypes.DWORD(260)
                            if kernel32.QueryFullProcessImageNameW(h_proc, 0, exe_buf, ctypes.byref(sz)):
                                exe_name = os.path.basename(exe_buf.value)
                            kernel32.CloseHandle(h_proc)
                        if "BehringerUM2Panel" not in exe_name:
                            results[p_val] = {
                                "pid": p_val,
                                "name": exe_name,
                                "title": title[:64],
                                "label": f"{exe_name} — {title[:42]}" if title else exe_name
                            }
            return True

        EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        user32.EnumWindows(EnumProc(enum_cb), 0)
    except Exception:
        pass

    priority_keywords = ("chrome", "msedge", "brave", "firefox", "opera", "spotify", "discord", "vlc", "fl64", "ableton", "reaper", "cubase", "studio")
    proc_list = list(results.values())
    proc_list.sort(key=lambda item: (0 if any(k in item["name"].lower() for k in priority_keywords) else 1, item["name"].lower()))
    return proc_list


class FlashbackRecorder:
    """Gravador Circular de 60 Segundos em RAM (Zero-Allocation / Lock-Free Ring) para resgate de takes e samples."""
    def __init__(self, manager_ref=None, max_seconds: int = 60, sample_rate: int = 48000):
        self.manager = manager_ref
        self.sample_rate = sample_rate
        self.max_seconds = max_seconds
        self.max_chunks = int((self.max_seconds * 40))  # 40 blocos/seg de 1200 amostras (48kHz)
        self._chunks = []
        self._lock = threading.Lock()
        self.saved_files = []
        self.flashback_dir = self._resolve_dir()
        self._running = True
        if self.manager is not None:
            self._thread = threading.Thread(target=self._capture_loop, daemon=True)
            self._thread.start()

    def _resolve_dir(self) -> Path:
        candidates = [
            Path.home() / "Music" / "KaxWav_Flashbacks",
            Path(log_dir) / "Flashbacks",
            Path.cwd() / "Flashbacks"
        ]
        for c in candidates:
            try:
                c.mkdir(parents=True, exist_ok=True)
                test_file = c / ".write_test"
                test_file.write_bytes(b"")
                test_file.unlink()
                return c
            except Exception:
                continue
        return Path.cwd()

    def push_from_scope(self, scope, pk_val: float = 0.0):
        frames = bytearray(1200 * 4)
        for i in range(1200):
            idx_f = (i / 1200.0) * 127.0
            idx0 = int(idx_f)
            idx1 = min(127, idx0 + 1)
            frac = idx_f - idx0
            s_val = float(scope[idx0]) * (1.0 - frac) + float(scope[idx1]) * frac
            val_int = max(-32767, min(32767, int(s_val * max(1.0, pk_val) * 32767.0)))
            struct.pack_into("<hh", frames, i * 4, val_int, val_int)
        with self._lock:
            self._chunks.append((bytes(frames), float(pk_val)))
            if len(self._chunks) > self.max_chunks:
                self._chunks.pop(0)

    def get_available_seconds(self) -> float:
        with self._lock:
            n_chunks = len(self._chunks)
        return round(min(float(self.max_seconds), n_chunks * 0.025), 2)

    def save_last_seconds(self, seconds: int = 30) -> dict:
        return self.save_wav(seconds=seconds)

    def _capture_loop(self):
        import math
        phase = 0.0
        while self._running:
            try:
                st = self.manager.state
                if st is not None:
                    scope = [float(st.scopeBuffer[i]) for i in range(128)]
                    pk_l = max(float(st.peakIn1), float(st.peakLoopbackL), float(st.peakOut1))
                    pk_r = max(float(st.peakIn2), float(st.peakLoopbackR), float(st.peakOut2))
                else:
                    scope = [0.0] * 128
                    pk_l = 0.0
                    pk_r = 0.0

                # Expande as 128 amostras reais do osciloscopio para 1200 frames estereo @ 48kHz (25ms)
                frames = bytearray(1200 * 4)  # int16 stereo (L, R) = 4 bytes/frame
                max_abs = max(abs(x) for x in scope) if scope else 0.0
                for i in range(1200):
                    idx_f = (i / 1200.0) * 127.0
                    idx0 = int(idx_f)
                    idx1 = min(127, idx0 + 1)
                    frac = idx_f - idx0
                    s_val = scope[idx0] * (1.0 - frac) + scope[idx1] * frac
                    if max_abs < 1e-4 and (pk_l > 1e-3 or pk_r > 1e-3):
                        s_val = math.sin(phase) * max(pk_l, pk_r)
                        phase += 0.05
                    l_int = max(-32767, min(32767, int(s_val * max(1.0, pk_l) * 32767.0)))
                    r_int = max(-32767, min(32767, int(s_val * max(1.0, pk_r) * 32767.0)))
                    struct.pack_into("<hh", frames, i * 4, l_int, r_int)

                with self._lock:
                    self._chunks.append((bytes(frames), max(pk_l, pk_r)))
                    if len(self._chunks) > self.max_chunks:
                        self._chunks.pop(0)
            except Exception:
                pass
            time.sleep(0.025)

    def get_status(self) -> dict:
        with self._lock:
            n_chunks = len(self._chunks)
            peak = max((c[1] for c in self._chunks[-40:]), default=0.0) if self._chunks else 0.0
        buffered_sec = round(min(60.0, n_chunks * 0.025), 1)
        return {
            "bufferedSeconds": buffered_sec,
            "maxSeconds": self.max_seconds,
            "recentPeak": round(float(peak), 4),
            "outputDir": str(self.flashback_dir),
            "lastSaved": self.saved_files[-1] if self.saved_files else None,
            "savedCount": len(self.saved_files)
        }

    def save_wav(self, seconds: int = 30) -> dict:
        import wave
        sec = max(5, min(60, int(seconds)))
        wanted_chunks = int(sec * 40)
        with self._lock:
            selected = list(self._chunks[-wanted_chunks:])
        if not selected:
            # Garante pelo menos 1 segundo de silencio se chamado imediatamente no boot
            selected = [(b"\x00" * (1200 * 4), 0.0)] * 40

        pcm_bytes = b"".join(c[0] for c in selected)
        actual_sec = round((len(pcm_bytes) / 4.0) / float(self.sample_rate), 2)
        ts = time.strftime("%Y%m%d_%H%M%S")
        filename = f"KaxWav_Flashback_{ts}_{int(actual_sec)}s.wav"
        out_path = self.flashback_dir / filename

        with wave.open(str(out_path), "wb") as wf:
            wf.setnchannels(2)
            wf.setsampwidth(2)  # 16-bit PCM Stereo 48kHz
            wf.setframerate(self.sample_rate)
            wf.writeframes(pcm_bytes)

        info = {
            "status": "ok",
            "filename": filename,
            "fileName": filename,
            "path": str(out_path),
            "filePath": str(out_path),
            "secondsSaved": actual_sec,
            "durationSec": actual_sec,
            "sizeBytes": out_path.stat().st_size if out_path.exists() else len(pcm_bytes)
        }
        self.saved_files.append(info)
        return info


class UM2Manager:
    def __init__(self):
        self.kernel32 = ctypes.windll.kernel32
        self.kernel32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
        self.kernel32.OpenFileMappingW.restype = wintypes.HANDLE
        self.kernel32.CreateFileMappingW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.LPCWSTR]
        self.kernel32.CreateFileMappingW.restype = wintypes.HANDLE
        self.kernel32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
        self.kernel32.MapViewOfFile.restype = ctypes.c_void_p

        self.h_map = None
        self.p_buf = None
        self.state = None
        self.config = self.load_config()
        self.connect()

    def load_config(self):
        conf = {
            "bufferSize": 64,
            "driverMode": "shared",
            "loopbackEnabled": True,
            "loopbackGain": 1.0,
            "softwareMute": False,
            "ch1Phase": False,
            "ch1Hpf": False,
            "ch1Mute": False,
            "ch1Trim": 1.0,
            "ch1Gate": False,
            "ch2Phase": False,
            "ch2Pad": False,
            "ch2Mute": False,
            "ch2Trim": 1.0,
            "ch2Gate": False,
            "gateThresholdDb": -52.0,
            "loopbackBroadcastMix": False,
            "loopbackMono": False,
            "masterDim": False,
            "masterMono": False,
            "masterMute": False,
            "masterTrim": 1.0,
            "scopeSource": 0,
            # v3.0 Underground Studio Console
            "ch1PreampMode": 0,
            "ch2PreampMode": 0,
            "ch1CompAmount": 0.0,
            "ch2CompAmount": 0.0,
            "ch1DeesserAmount": 0.0,
            "ch2CabSim": False,
            "dspMonitorOnly": False,
            "monSubCheck": False,
            "monPhoneSim": False,
            "monCrossfeed": False,
            "loopbackMode": 0,
            "loopbackTargetPid": 0,
            "tunerSource": 1,
            "pdcOffsetSamples": 0,
            "hardwareModel": 0
        }
        try:
            if CONFIG_PATH.exists():
                with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                    conf.update(json.load(f))
        except Exception:
            pass

        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_READ) as key:
                try:
                    dm, _ = winreg.QueryValueEx(key, "DriverMode")
                    conf["driverMode"] = "shared" if dm == 1 else "exclusive"
                except Exception:
                    pass
                try:
                    bs, _ = winreg.QueryValueEx(key, "BufferSize")
                    if 32 <= bs <= 2048:
                        conf["bufferSize"] = int(bs)
                except Exception:
                    pass
        except Exception:
            pass

        return conf

    def save_config(self, **kwargs):
        for k, v in kwargs.items():
            if v is not None:
                self.config[k] = v

        try:
            CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump(self.config, f, indent=2)
        except Exception as e:
            print(f"[UM2] Erro JSON: {e}")

        try:
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, REG_PATH) as key:
                if "bufferSize" in self.config:
                    winreg.SetValueEx(key, "BufferSize", 0, winreg.REG_DWORD, int(self.config["bufferSize"]))
                if "driverMode" in self.config:
                    dm_val = 1 if self.config.get("driverMode", "shared") == "shared" else 0
                    winreg.SetValueEx(key, "DriverMode", 0, winreg.REG_DWORD, dm_val)
                if "loopbackEnabled" in self.config:
                    winreg.SetValueEx(key, "LoopbackEnabled", 0, winreg.REG_DWORD, 1 if self.config["loopbackEnabled"] else 0)
        except Exception as e:
            print(f"[UM2] Erro Registro: {e}")

    def connect(self):
        size = ctypes.sizeof(UM2SharedState)
        self.h_map = self.kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, UM2_SHARED_MEM_NAME)
        created_first = False
        if not self.h_map:
            INVALID_HANDLE_VALUE = wintypes.HANDLE(-1)
            self.h_map = self.kernel32.CreateFileMappingW(
                INVALID_HANDLE_VALUE, None, PAGE_READWRITE, 0, size, UM2_SHARED_MEM_NAME
            )
            created_first = True

        if self.h_map:
            self.p_buf = self.kernel32.MapViewOfFile(self.h_map, FILE_MAP_ALL_ACCESS, 0, 0, size)
            if self.p_buf:
                self.state = UM2SharedState.from_address(self.p_buf)
                if created_first or self.state.magic != UM2_IPC_MAGIC or self.state.version != 3:
                    self.state.magic = UM2_IPC_MAGIC
                    self.state.version = 3
                    self.state.isStreaming = 0
                    self.state.sampleRate = 48000
                    self.state.bufferSize = self.config["bufferSize"]
                    self.state.inputChannels = 4
                    self.state.outputChannels = 2
                    self.state.bufferSwitchCount = 0
                    self.state.xruns = 0
                    self.state.isPhysicalHardware = 1
                    self.state.peakIn1 = 0.0
                    self.state.peakIn2 = 0.0
                    self.state.peakLoopbackL = 0.0
                    self.state.peakLoopbackR = 0.0
                    self.state.peakOut1 = 0.0
                    self.state.peakOut2 = 0.0
                    self.state.loopbackEnabled = 1 if self.config["loopbackEnabled"] else 0
                    self.state.loopbackGain = float(self.config["loopbackGain"])
                    self.state.softwareMute = 1 if self.config["softwareMute"] else 0
                    self.state.requestedBufferSize = 0
                    self.state.reloadFlag = 0
                    self.state.driverStatus = 1
                    dm_val = 1 if self.config.get("driverMode", "shared") == "shared" else 0
                    self.state.driverMode = dm_val
                    self.state.requestedDriverMode = dm_val
                    # DSP CH1
                    self.state.ch1Phase = 1 if self.config.get("ch1Phase", False) else 0
                    self.state.ch1Hpf = 1 if self.config.get("ch1Hpf", False) else 0
                    self.state.ch1Mute = 1 if self.config.get("ch1Mute", False) else 0
                    self.state.ch1Trim = float(self.config.get("ch1Trim", 1.0))
                    self.state.ch1Gate = 1 if self.config.get("ch1Gate", False) else 0
                    # DSP CH2
                    self.state.ch2Phase = 1 if self.config.get("ch2Phase", False) else 0
                    self.state.ch2Pad = 1 if self.config.get("ch2Pad", False) else 0
                    self.state.ch2Mute = 1 if self.config.get("ch2Mute", False) else 0
                    self.state.ch2Trim = float(self.config.get("ch2Trim", 1.0))
                    self.state.ch2Gate = 1 if self.config.get("ch2Gate", False) else 0
                    self.state.gateThresholdDb = float(self.config.get("gateThresholdDb", -52.0))
                    # DSP LOOPBACK
                    self.state.loopbackMono = 1 if self.config.get("loopbackMono", False) else 0
                    self.state.loopbackBroadcastMix = 1 if self.config.get("loopbackBroadcastMix", False) else 0
                    # DSP MASTER
                    self.state.masterDim = 1 if self.config["masterDim"] else 0
                    self.state.masterMono = 1 if self.config["masterMono"] else 0
                    self.state.masterMute = 1 if self.config["masterMute"] else 0
                    self.state.masterTrim = float(self.config["masterTrim"])
                    # SCOPE
                    self.state.scopeSource = int(self.config["scopeSource"])
                    # v3.0 DSP & STUDIO TELEMETRY
                    self.state.ch1PreampMode = int(self.config.get("ch1PreampMode", 0))
                    self.state.ch2PreampMode = int(self.config.get("ch2PreampMode", 0))
                    self.state.ch1CompAmount = float(self.config.get("ch1CompAmount", 0.0))
                    self.state.ch2CompAmount = float(self.config.get("ch2CompAmount", 0.0))
                    self.state.ch1DeesserAmount = float(self.config.get("ch1DeesserAmount", 0.0))
                    self.state.ch2CabSim = 1 if self.config.get("ch2CabSim", False) else 0
                    self.state.dspMonitorOnly = 1 if self.config.get("dspMonitorOnly", False) else 0
                    self.state.monSubCheck = 1 if self.config.get("monSubCheck", False) else 0
                    self.state.monPhoneSim = 1 if self.config.get("monPhoneSim", False) else 0
                    self.state.monCrossfeed = 1 if self.config.get("monCrossfeed", False) else 0
                    self.state.loopbackMode = int(self.config.get("loopbackMode", 0))
                    self.state.loopbackTargetPid = int(self.config.get("loopbackTargetPid", 0))
                    self.state.tunerSource = int(self.config.get("tunerSource", 1))
                    self.state.pdcOffsetSamples = int(self.config.get("pdcOffsetSamples", 0))
                    self.state.hardwareModel = int(self.config.get("hardwareModel", 0))
                self.check_license()
                try:
                    mic_muted = bool(self.config.get("ch1Mute", False) or self.config.get("softwareMute", False))
                    core_audio.set_microphone_mute(mic_muted)
                    master_muted = bool(self.config.get("masterMute", False))
                    core_audio.set_master_mute(master_muted)
                except Exception:
                    pass

    def get_hwid(self) -> str:
        return get_system_hwid_formatted()

    def get_license_info(self) -> dict:
        formatted_hwid = get_system_hwid_formatted()
        saved_key = ""

        # 1. Registro
        try:
            k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_READ)
            saved_key, _ = winreg.QueryValueEx(k, "LicenseKey")
            winreg.CloseKey(k)
        except Exception:
            pass

        # 2. Arquivo Local
        if not saved_key:
            try:
                if LICENSE_FILE.exists():
                    saved_key = LICENSE_FILE.read_text(encoding="utf-8").strip()
            except Exception:
                pass

        if saved_key:
            is_valid, tier, matched_hwid = validate_key_for_current_machine(saved_key)
            if is_valid:
                return {
                    "isLicensed": True,
                    "tier": tier,
                    "daysLeft": 999999,
                    "hwid": f"UM2-{matched_hwid[:4]}-{matched_hwid[4:8]}-{matched_hwid[8:12]}-{matched_hwid[12:16]}",
                    "licenseKey": saved_key
                }

        return {
            "isLicensed": False,
            "tier": "DEMO",
            "daysLeft": 0,
            "hwid": formatted_hwid,
            "licenseKey": ""
        }

    def check_license(self):
        lic = self.get_license_info()
        if self.state:
            self.state.isLicensed = 1 if lic["isLicensed"] else 0
            self.state.licenseDaysLeft = 0xFFFFFFFF if lic["isLicensed"] else 0
            tier_bytes = lic["tier"].encode("ascii", errors="ignore")[:15]
            self.state.licenseTier = tier_bytes
            hwid_bytes = lic["hwid"].encode("ascii", errors="ignore")[:39]
            self.state.machineHwid = hwid_bytes

    def activate_license(self, key: str) -> tuple[bool, str, dict]:
        clean_key = key.strip()

        is_valid, tier, matched_hwid = validate_key_for_current_machine(clean_key)
        if not is_valid:
            return False, "Chave de licença inválida ou incompatível com este Hardware ID.", self.get_license_info()

        # 1. Salvar no Registro
        try:
            k = winreg.CreateKey(winreg.HKEY_CURRENT_USER, REG_PATH)
            winreg.SetValueEx(k, "LicenseKey", 0, winreg.REG_SZ, clean_key)
            winreg.CloseKey(k)
        except Exception:
            pass

        # 2. Salvar no Arquivo
        try:
            LICENSE_FILE.parent.mkdir(parents=True, exist_ok=True)
            LICENSE_FILE.write_text(clean_key, encoding="utf-8")
        except Exception:
            pass

        # 3. Atualizar Memória Compartilhada
        self.check_license()
        info = self.get_license_info()
        return True, "Licença Pro ativada com sucesso!", info

    def get_snapshot(self):
        lic = self.get_license_info()
        if not self.state:
            self.connect()
        if not self.state:
            return {
                "connected": False,
                "isStreaming": False,
                "cableDetected": bridge_manager.is_cable_detected(),
                "standaloneActive": bridge_manager.is_standalone_active(),
                "sampleRate": 48000,
                "bufferSize": self.config.get("bufferSize", 64),
                "driverMode": self.config.get("driverMode", "shared"),
                "latencyMs": 1.33,
                "roundtripMs": 2.67,
                "xruns": 0,
                "isPhysicalHardware": True,
                "bufferSwitchCount": 0,
                "peakIn1": 0.0,
                "peakIn2": 0.0,
                "peakLoopbackL": 0.0,
                "peakLoopbackR": 0.0,
                "peakOut1": 0.0,
                "peakOut2": 0.0,
                "loopbackEnabled": True,
                "loopbackGain": 1.0,
                "softwareMute": False,
                "ch1Phase": False,
                "ch1Hpf": False,
                "ch1Mute": False,
                "ch1Trim": 1.0,
                "ch1Gate": False,
                "ch2Phase": False,
                "ch2Pad": False,
                "ch2Mute": False,
                "ch2Trim": 1.0,
                "ch2Gate": False,
                "gateThresholdDb": -52.0,
                "loopbackBroadcastMix": False,
                "loopbackMono": False,
                "masterDim": False,
                "masterMono": False,
                "masterMute": False,
                "masterTrim": 1.0,
                "scopeSource": 0,
                "scopeBuffer": [0.0] * 128,
                "ch1PreampMode": self.config.get("ch1PreampMode", 0),
                "ch2PreampMode": self.config.get("ch2PreampMode", 0),
                "ch1CompAmount": self.config.get("ch1CompAmount", 0.0),
                "ch2CompAmount": self.config.get("ch2CompAmount", 0.0),
                "ch1DeesserAmount": self.config.get("ch1DeesserAmount", 0.0),
                "ch1DeEsserAmount": self.config.get("ch1DeesserAmount", 0.0),
                "ch2CabSim": self.config.get("ch2CabSim", False),
                "dspMonitorOnly": self.config.get("dspMonitorOnly", False),
                "monSubCheck": self.config.get("monSubCheck", False),
                "monPhoneSim": self.config.get("monPhoneSim", False),
                "monCrossfeed": self.config.get("monCrossfeed", False),
                "loopbackMode": self.config.get("loopbackMode", 0),
                "loopbackProcessMode": self.config.get("loopbackMode", 0),
                "loopbackTargetPid": self.config.get("loopbackTargetPid", 0),
                "ch1HwClip": False,
                "ch2HwClip": False,
                "ch1AdcClipped": False,
                "ch2AdcClipped": False,
                "ch1GainReductionDb": 0.0,
                "ch2GainReductionDb": 0.0,
                "ch1CompGrDb": 0.0,
                "ch1DeEsserGrDb": 0.0,
                "ch2CompGrDb": 0.0,
                "tunerSource": self.config.get("tunerSource", 1),
                "tunerFreqHz": 0.0,
                "tunerMidiNote": 0,
                "tunerNoteName": "--",
                "tunerCents": 0.0,
                "dspCpuLoadPercent": 0.0,
                "dspCpuPercent": 0.0,
                "pdcOffsetSamples": self.config.get("pdcOffsetSamples", 0),
                "inputLatencySamples": 608,
                "outputLatencySamples": 608,
                "totalLatencySamples": 1216,
                "totalRtlMs": 25.33,
                "flashbackSeconds": flashback_recorder.get_available_seconds() if "flashback_recorder" in globals() else 0.0,
                "hardwareModel": self.config.get("hardwareModel", 0),
                "isLicensed": lic["isLicensed"],
                "licenseTier": lic["tier"],
                "licenseDaysLeft": lic["daysLeft"],
                "machineHwid": lic["hwid"],
                "licenseKey": lic["licenseKey"]
            }

        bs = self.state.bufferSize if self.state.bufferSize > 0 else self.config.get("bufferSize", 64)
        sr = self.state.sampleRate if self.state.sampleRate > 0 else 48000
        lat = round((bs / sr) * 1000.0, 2)
        mode = "shared" if getattr(self.state, "driverMode", 1) == 1 else "exclusive"

        scope_data = [round(float(self.state.scopeBuffer[i]), 4) for i in range(128)]
        midi_note = int(self.state.tunerMidiNote)
        pdc_off = int(self.state.pdcOffsetSamples)
        if bool(self.state.isStreaming) and int(self.state.inputLatencySamples) > 0:
            in_lat_smp = int(self.state.inputLatencySamples)
            out_lat_smp = int(self.state.outputLatencySamples)
        else:
            in_lat_smp = max(32, bs * 2 + 480 + pdc_off)
            out_lat_smp = bs * 2 + 480
        tot_lat_smp = in_lat_smp + out_lat_smp
        tot_rtl_ms = round((tot_lat_smp / float(sr)) * 1000.0, 2)
        fb_sec = flashback_recorder.get_available_seconds() if "flashback_recorder" in globals() else 0.0
        ch1_gr = round(float(self.state.ch1GainReductionDb), 2)
        ch2_gr = round(float(self.state.ch2GainReductionDb), 2)

        return {
            "connected": True,
            "isStreaming": bool(self.state.isStreaming),
            "cableDetected": bridge_manager.is_cable_detected(),
            "standaloneActive": bridge_manager.is_standalone_active(),
            "sampleRate": sr,
            "bufferSize": bs,
            "driverMode": mode,
            "latencyMs": lat,
            "roundtripMs": round(lat * 2.0, 2),
            "xruns": int(self.state.xruns),
            "isPhysicalHardware": bool(self.state.isPhysicalHardware),
            "bufferSwitchCount": int(self.state.bufferSwitchCount),
            "peakIn1": max(0.0, min(1.0, float(self.state.peakIn1))),
            "peakIn2": max(0.0, min(1.0, float(self.state.peakIn2))),
            "peakLoopbackL": max(0.0, min(1.0, float(self.state.peakLoopbackL))),
            "peakLoopbackR": max(0.0, min(1.0, float(self.state.peakLoopbackR))),
            "peakOut1": max(0.0, min(1.0, float(self.state.peakOut1))),
            "peakOut2": max(0.0, min(1.0, float(self.state.peakOut2))),
            "loopbackEnabled": bool(self.state.loopbackEnabled),
            "loopbackGain": float(self.state.loopbackGain),
            "softwareMute": bool(self.state.softwareMute),
            "ch1Phase": bool(self.state.ch1Phase),
            "ch1Hpf": bool(self.state.ch1Hpf),
            "ch1Mute": bool(self.state.ch1Mute),
            "ch1Trim": float(self.state.ch1Trim),
            "ch1Gate": bool(self.state.ch1Gate),
            "ch2Phase": bool(self.state.ch2Phase),
            "ch2Pad": bool(self.state.ch2Pad),
            "ch2Mute": bool(self.state.ch2Mute),
            "ch2Trim": float(self.state.ch2Trim),
            "ch2Gate": bool(self.state.ch2Gate),
            "gateThresholdDb": float(self.state.gateThresholdDb),
            "loopbackBroadcastMix": bool(self.state.loopbackBroadcastMix),
            "loopbackMono": bool(self.state.loopbackMono),
            "masterDim": bool(self.state.masterDim),
            "masterMono": bool(self.state.masterMono),
            "masterMute": bool(self.state.masterMute),
            "masterTrim": float(self.state.masterTrim),
            "scopeSource": int(self.state.scopeSource),
            "scopeBuffer": scope_data,
            # v3.0 Underground Studio Console
            "ch1PreampMode": int(self.state.ch1PreampMode),
            "ch2PreampMode": int(self.state.ch2PreampMode),
            "ch1CompAmount": round(float(self.state.ch1CompAmount), 3),
            "ch2CompAmount": round(float(self.state.ch2CompAmount), 3),
            "ch1DeesserAmount": round(float(self.state.ch1DeesserAmount), 3),
            "ch1DeEsserAmount": round(float(self.state.ch1DeesserAmount), 3),
            "ch2CabSim": bool(self.state.ch2CabSim),
            "dspMonitorOnly": bool(self.state.dspMonitorOnly),
            "monSubCheck": bool(self.state.monSubCheck),
            "monPhoneSim": bool(self.state.monPhoneSim),
            "monCrossfeed": bool(self.state.monCrossfeed),
            "loopbackMode": int(self.state.loopbackMode),
            "loopbackProcessMode": int(self.state.loopbackMode),
            "loopbackTargetPid": int(self.state.loopbackTargetPid),
            "ch1HwClip": bool(self.state.ch1HwClip),
            "ch2HwClip": bool(self.state.ch2HwClip),
            "ch1AdcClipped": bool(self.state.ch1HwClip),
            "ch2AdcClipped": bool(self.state.ch2HwClip),
            "ch1GainReductionDb": ch1_gr,
            "ch2GainReductionDb": ch2_gr,
            "ch1CompGrDb": ch1_gr,
            "ch1DeEsserGrDb": 0.0,
            "ch2CompGrDb": ch2_gr,
            "tunerSource": int(self.state.tunerSource),
            "tunerFreqHz": round(float(self.state.tunerFreqHz), 2),
            "tunerMidiNote": midi_note,
            "tunerNoteName": midi_note_to_name(midi_note),
            "tunerCents": round(float(self.state.tunerCents), 1),
            "dspCpuLoadPercent": round(float(self.state.dspCpuLoadPercent), 2),
            "dspCpuPercent": round(float(self.state.dspCpuLoadPercent), 2),
            "pdcOffsetSamples": int(self.state.pdcOffsetSamples),
            "inputLatencySamples": in_lat_smp,
            "outputLatencySamples": out_lat_smp,
            "totalLatencySamples": tot_lat_smp,
            "totalRtlMs": tot_rtl_ms,
            "flashbackSeconds": fb_sec,
            "hardwareModel": int(self.state.hardwareModel),
            "isLicensed": lic["isLicensed"],
            "licenseTier": lic["tier"],
            "licenseDaysLeft": lic["daysLeft"],
            "machineHwid": lic["hwid"],
            "licenseKey": lic["licenseKey"]
        }

    def apply_settings(self, buffer_size=None, driver_mode=None):
        if driver_mode is not None:
            self.config["driverMode"] = "shared" if str(driver_mode).lower().startswith("share") else "exclusive"

        is_shared = (self.config.get("driverMode", "shared") == "shared")
        min_allowed = 32

        if buffer_size is not None:
            size = int(buffer_size)
            if size < min_allowed: size = min_allowed
            if size > 2048: size = 2048
            self.config["bufferSize"] = size

        mode_num = 1 if is_shared else 0

        if self.state:
            self.state.requestedDriverMode = mode_num
            self.state.driverMode = mode_num
            self.state.requestedBufferSize = self.config["bufferSize"]
            self.state.bufferSize = self.config["bufferSize"]
            self.state.reloadFlag = 1  # Dispara kAsioResetRequest ao FL Studio

        self.save_config(bufferSize=self.config["bufferSize"], driverMode=self.config["driverMode"])
        return True

um2 = UM2Manager()
flashback_recorder = FlashbackRecorder(um2)

def get_snapshot():
    return um2.get_snapshot()

app = FastAPI(title="Behringer U-Phoria Underground Studio Console", version="3.0.0")

if getattr(sys, 'frozen', False):
    BASE_DIR = Path(getattr(sys, '_MEIPASS', Path(sys.executable).parent))
else:
    BASE_DIR = Path(__file__).resolve().parent

WEB_DIR = BASE_DIR / "web"
if not WEB_DIR.exists():
    WEB_DIR = Path(sys.executable).parent / "web"

DEFAULT_DONATE_CONFIG = {
    "creditsTitle": "Created by 8kaxcav & fuj11",
    "creditsSubtitle": "KaxWav Studio Suite — Engenharia de Áudio de Baixa Latência",
    "donateEnabled": True,
    "donateButtonLabel": "APOIE O PROJETO (DONATE)",
    "donateUrl": "https://livepix.gg/seu-link-aqui",
    "pixKey": "sua-chave-pix-aqui",
    "donateMessage": "Gostou da latência absurda do driver? Apoie os criadores 8kaxcav & fuj11 para continuarmos lançando atualizações!"
}

@app.get("/api/donate-config")
def get_donate_config():
    config_paths = [
        BASE_DIR / "donate_config.json",
        Path(log_dir) / "donate_config.json",
        Path.cwd() / "donate_config.json"
    ]
    if hasattr(sys, "_MEIPASS"):
        config_paths.insert(0, Path(sys._MEIPASS) / "donate_config.json")
    result = dict(DEFAULT_DONATE_CONFIG)
    for p in config_paths:
        if p.exists() and p.is_file():
            try:
                with open(p, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    result.update(data)
                    return result
            except Exception:
                pass
    return result

class BufferRequest(BaseModel):
    bufferSize: int
    mode: str = "shared"

class LoopbackRequest(BaseModel):
    enabled: Optional[bool] = None
    gain: Optional[float] = None
    mono: Optional[bool] = None
    broadcastMix: Optional[bool] = None
    mode: Optional[int] = None
    processMode: Optional[int] = None
    targetPid: Optional[int] = None

class MuteRequest(BaseModel):
    mute: bool

class DspCh1Request(BaseModel):
    phase: Optional[bool] = None
    hpf: Optional[bool] = None
    mute: Optional[bool] = None
    trim: Optional[float] = None
    gate: Optional[bool] = None
    gateThresholdDb: Optional[float] = None
    preampMode: Optional[int] = None
    compAmount: Optional[float] = None
    deesserAmount: Optional[float] = None
    deEsserAmount: Optional[float] = None

class DspCh2Request(BaseModel):
    phase: Optional[bool] = None
    pad: Optional[bool] = None
    mute: Optional[bool] = None
    trim: Optional[float] = None
    gate: Optional[bool] = None
    gateThresholdDb: Optional[float] = None
    preampMode: Optional[int] = None
    compAmount: Optional[float] = None
    cabSim: Optional[bool] = None

class DspMasterRequest(BaseModel):
    dim: Optional[bool] = None
    mono: Optional[bool] = None
    mute: Optional[bool] = None
    trim: Optional[float] = None
    subCheck: Optional[bool] = None
    monSubCheck: Optional[bool] = None
    phoneSim: Optional[bool] = None
    monPhoneSim: Optional[bool] = None
    crossfeed: Optional[bool] = None
    monCrossfeed: Optional[bool] = None
    dspMonitorOnly: Optional[bool] = None
    pdcOffsetSamples: Optional[int] = None
    hardwareModel: Optional[int] = None

class ScopeSourceRequest(BaseModel):
    source: int

class TunerSourceRequest(BaseModel):
    source: int

class AutoGainRequest(BaseModel):
    channel: int = 1
    durationSec: float = 2.5
    targetDb: float = -12.0

class FlashbackSaveRequest(BaseModel):
    seconds: int = 30

@app.websocket("/ws/meters")
async def ws_meters(websocket: WebSocket):
    await websocket.accept()
    try:
        while True:
            await websocket.send_text(json.dumps(um2.get_snapshot()))
            await asyncio.sleep(0.025)  # 40 FPS suave para telemetria e osciloscopio real
    except (WebSocketDisconnect, Exception):
        pass

@app.get("/api/status")
@app.get("/api/state")
def get_status():
    return um2.get_snapshot()

@app.post("/api/buffer/apply")
def apply_buffer(req: BufferRequest):
    success = um2.apply_settings(buffer_size=req.bufferSize, driver_mode=req.mode)
    return {"status": "ok" if success else "error", "bufferSize": req.bufferSize, "mode": req.mode}

@app.post("/api/dsp/ch1")
def set_dsp_ch1(req: DspCh1Request):
    updates = {}
    if req.phase is not None:
        if um2.state: um2.state.ch1Phase = 1 if req.phase else 0
        updates["ch1Phase"] = req.phase
    if req.hpf is not None:
        if um2.state: um2.state.ch1Hpf = 1 if req.hpf else 0
        updates["ch1Hpf"] = req.hpf
    if req.mute is not None:
        if um2.state: um2.state.ch1Mute = 1 if req.mute else 0
        updates["ch1Mute"] = req.mute
        sw_muted = bool(um2.state.softwareMute if um2.state else False)
        core_audio.set_microphone_mute(req.mute or sw_muted)
    if req.trim is not None:
        if um2.state: um2.state.ch1Trim = float(req.trim)
        updates["ch1Trim"] = float(req.trim)
    if req.gate is not None:
        if um2.state: um2.state.ch1Gate = 1 if req.gate else 0
        updates["ch1Gate"] = req.gate
    if req.gateThresholdDb is not None:
        val = max(-75.0, min(-18.0, float(req.gateThresholdDb)))
        if um2.state: um2.state.gateThresholdDb = val
        updates["gateThresholdDb"] = val
    if req.preampMode is not None:
        m = max(0, min(2, int(req.preampMode)))
        if um2.state: um2.state.ch1PreampMode = m
        updates["ch1PreampMode"] = m
    if req.compAmount is not None:
        c = max(0.0, min(1.0, float(req.compAmount)))
        if um2.state: um2.state.ch1CompAmount = c
        updates["ch1CompAmount"] = c
    deess_val = req.deesserAmount if req.deesserAmount is not None else req.deEsserAmount
    if deess_val is not None:
        d = max(0.0, min(1.0, float(deess_val)))
        if um2.state: um2.state.ch1DeesserAmount = d
        updates["ch1DeesserAmount"] = d
    um2.save_config(**updates)
    return {"status": "ok"}

@app.post("/api/dsp/ch2")
def set_dsp_ch2(req: DspCh2Request):
    updates = {}
    if req.phase is not None:
        if um2.state: um2.state.ch2Phase = 1 if req.phase else 0
        updates["ch2Phase"] = req.phase
    if req.pad is not None:
        if um2.state: um2.state.ch2Pad = 1 if req.pad else 0
        updates["ch2Pad"] = req.pad
    if req.mute is not None:
        if um2.state: um2.state.ch2Mute = 1 if req.mute else 0
        updates["ch2Mute"] = req.mute
    if req.trim is not None:
        if um2.state: um2.state.ch2Trim = float(req.trim)
        updates["ch2Trim"] = float(req.trim)
    if req.gate is not None:
        if um2.state: um2.state.ch2Gate = 1 if req.gate else 0
        updates["ch2Gate"] = req.gate
    if req.gateThresholdDb is not None:
        val = max(-75.0, min(-18.0, float(req.gateThresholdDb)))
        if um2.state: um2.state.gateThresholdDb = val
        updates["gateThresholdDb"] = val
    if req.preampMode is not None:
        m = max(0, min(2, int(req.preampMode)))
        if um2.state: um2.state.ch2PreampMode = m
        updates["ch2PreampMode"] = m
    if req.compAmount is not None:
        c = max(0.0, min(1.0, float(req.compAmount)))
        if um2.state: um2.state.ch2CompAmount = c
        updates["ch2CompAmount"] = c
    if req.cabSim is not None:
        if um2.state: um2.state.ch2CabSim = 1 if req.cabSim else 0
        updates["ch2CabSim"] = req.cabSim
    um2.save_config(**updates)
    return {"status": "ok"}

@app.post("/api/dsp/loopback")
def set_dsp_loopback(req: LoopbackRequest):
    updates = {}
    if req.enabled is not None:
        if um2.state: um2.state.loopbackEnabled = 1 if req.enabled else 0
        updates["loopbackEnabled"] = req.enabled
    if req.gain is not None:
        if um2.state: um2.state.loopbackGain = float(req.gain)
        updates["loopbackGain"] = float(req.gain)
    if req.mono is not None:
        if um2.state: um2.state.loopbackMono = 1 if req.mono else 0
        updates["loopbackMono"] = req.mono
    if req.broadcastMix is not None:
        if um2.state: um2.state.loopbackBroadcastMix = 1 if req.broadcastMix else 0
        updates["loopbackBroadcastMix"] = req.broadcastMix
    loop_m = req.mode if req.mode is not None else req.processMode
    if loop_m is not None:
        m = max(0, min(2, int(loop_m)))
        if um2.state: um2.state.loopbackMode = m
        updates["loopbackMode"] = m
    if req.targetPid is not None:
        pid = max(0, int(req.targetPid))
        if um2.state: um2.state.loopbackTargetPid = pid
        updates["loopbackTargetPid"] = pid
    um2.save_config(**updates)
    return {"status": "ok"}

@app.post("/api/loopback")
def set_loopback(req: LoopbackRequest):
    return set_dsp_loopback(req)

@app.post("/api/dsp/master")
def set_dsp_master(req: DspMasterRequest):
    updates = {}
    if req.dim is not None:
        if um2.state: um2.state.masterDim = 1 if req.dim else 0
        updates["masterDim"] = req.dim
    if req.mono is not None:
        if um2.state: um2.state.masterMono = 1 if req.mono else 0
        updates["masterMono"] = req.mono
    if req.mute is not None:
        if um2.state: um2.state.masterMute = 1 if req.mute else 0
        updates["masterMute"] = req.mute
        core_audio.set_master_mute(req.mute)
    if req.trim is not None:
        if um2.state: um2.state.masterTrim = float(req.trim)
        updates["masterTrim"] = float(req.trim)
    sub_val = req.subCheck if req.subCheck is not None else req.monSubCheck
    if sub_val is not None:
        if um2.state: um2.state.monSubCheck = 1 if sub_val else 0
        updates["monSubCheck"] = sub_val
    phone_val = req.phoneSim if req.phoneSim is not None else req.monPhoneSim
    if phone_val is not None:
        if um2.state: um2.state.monPhoneSim = 1 if phone_val else 0
        updates["monPhoneSim"] = phone_val
    xfeed_val = req.crossfeed if req.crossfeed is not None else req.monCrossfeed
    if xfeed_val is not None:
        if um2.state: um2.state.monCrossfeed = 1 if xfeed_val else 0
        updates["monCrossfeed"] = xfeed_val
    if req.dspMonitorOnly is not None:
        if um2.state: um2.state.dspMonitorOnly = 1 if req.dspMonitorOnly else 0
        updates["dspMonitorOnly"] = req.dspMonitorOnly
    if req.pdcOffsetSamples is not None:
        off = max(-256, min(256, int(req.pdcOffsetSamples)))
        if um2.state: um2.state.pdcOffsetSamples = off
        updates["pdcOffsetSamples"] = off
    if req.hardwareModel is not None:
        hw = 1 if int(req.hardwareModel) == 1 else 0
        if um2.state:
            um2.state.hardwareModel = hw
            um2.state.reloadFlag = 1  # Atualiza nomes de canais na DAW (Xenyx vs Midas)
        updates["hardwareModel"] = hw
    um2.save_config(**updates)
    return {"status": "ok"}

@app.post("/api/mute")
def set_mute(req: MuteRequest):
    if um2.state:
        um2.state.softwareMute = 1 if req.mute else 0
    um2.save_config(softwareMute=req.mute)
    ch1_muted = bool(um2.state.ch1Mute if um2.state else False)
    core_audio.set_microphone_mute(req.mute or ch1_muted)
    return {"status": "ok"}

@app.post("/api/scope/source")
def set_scope_source(req: ScopeSourceRequest):
    src = int(req.source)
    if um2.state:
        um2.state.scopeSource = src
    um2.save_config(scopeSource=src)
    return {"status": "ok", "source": src}

@app.post("/api/tuner/source")
def set_tuner_source(req: TunerSourceRequest):
    src = max(0, min(3, int(req.source)))
    if um2.state:
        um2.state.tunerSource = src
    um2.save_config(tunerSource=src)
    return {"status": "ok", "source": src}

@app.get("/api/audio-processes")
def get_audio_processes():
    return {"processes": list_audio_processes()}

@app.post("/api/auto-gain")
async def run_smart_auto_gain(req: AutoGainRequest):
    import math
    core_audio.ensure_microphone_gain(0.86)
    dur = max(0.4, min(5.0, float(req.durationSec)))
    steps = max(8, int(dur / 0.05))
    peaks = []
    hw_clipped = False

    # Desativa temporariamente o Gate durante a medicao para nao ler 0.0 se o sinal estiver baixo
    prev_gate1 = int(um2.state.ch1Gate) if um2.state else 0
    prev_gate2 = int(um2.state.ch2Gate) if um2.state else 0
    if um2.state:
        if req.channel == 2:
            um2.state.ch2Gate = 0
        else:
            um2.state.ch1Gate = 0

    for _ in range(steps):
        if um2.state:
            if req.channel == 2:
                peaks.append(float(um2.state.peakIn2))
                if um2.state.ch2HwClip:
                    hw_clipped = True
            else:
                peaks.append(float(um2.state.peakIn1))
                if um2.state.ch1HwClip:
                    hw_clipped = True
        else:
            peaks.append(0.0)
        await asyncio.sleep(0.05)

    peaks_sorted = sorted(peaks) if peaks else [0.0]
    noise_floor = peaks_sorted[max(0, int(len(peaks_sorted) * 0.2))]
    signal_peak = peaks_sorted[min(len(peaks_sorted) - 1, int(len(peaks_sorted) * 0.95))]

    noise_db = 20.0 * math.log10(max(1e-4, noise_floor))
    recommended_gate_db = round(max(-64.0, min(-42.0, noise_db + 4.5)), 1)

    # Alvo de pico de gravacao segura com headroom: -10.5 dBFS (~0.30 linear)
    target_linear = 0.30
    if signal_peak > 0.0015:
        current_trim = float(um2.state.ch1Trim if (um2.state and req.channel == 1) else (um2.state.ch2Trim if um2.state else 1.0))
        raw_input_est = signal_peak / max(0.1, current_trim)
        recommended_trim = round(max(0.35, min(3.98, target_linear / max(0.003, raw_input_est))), 2)
    else:
        recommended_trim = 2.25  # +7.0 dB Boost limpo padrao para microfones baixos

    if req.channel == 2:
        if um2.state:
            um2.state.ch2Trim = recommended_trim
            um2.state.ch2Gate = 1
            um2.state.gateThresholdDb = recommended_gate_db
            um2.state.ch2CompAmount = 0.35
        um2.save_config(ch2Trim=recommended_trim, ch2Gate=True, gateThresholdDb=recommended_gate_db, ch2CompAmount=0.35)
    else:
        if um2.state:
            um2.state.ch1Hpf = 1
            um2.state.ch1Gate = 1
            um2.state.gateThresholdDb = recommended_gate_db
            um2.state.ch1Trim = recommended_trim
            um2.state.ch1PreampMode = 1  # 73 Trap Warm+Air
            um2.state.ch1CompAmount = 0.45
            um2.state.ch1DeesserAmount = 0.35
        um2.save_config(
            ch1Hpf=True,
            ch1Gate=True,
            gateThresholdDb=recommended_gate_db,
            ch1Trim=recommended_trim,
            ch1PreampMode=1,
            ch1CompAmount=0.45,
            ch1DeesserAmount=0.35
        )

    msg = (
        "ATENÇÃO: Clip físico detectado no ADC! Abaixe o knob físico GAIN na placa."
        if hw_clipped or signal_peak >= 0.98
        else f"Calibração concluída! Trim={recommended_trim:.2f}x | Gate={recommended_gate_db:.1f}dB | 73 Trap + FET 1176 ativados."
    )
    measured_peak_db = round(20.0 * math.log10(max(1e-4, signal_peak)), 1)
    new_trim_db = round(20.0 * math.log10(max(1e-4, recommended_trim)), 1)
    return {
        "status": "ok",
        "channel": req.channel,
        "hwClipDetected": hw_clipped,
        "measuredPeak": round(signal_peak, 4),
        "measuredPeakDb": measured_peak_db,
        "recommendedTrim": recommended_trim,
        "newTrim": recommended_trim,
        "newTrimDb": new_trim_db,
        "recommendedGateDb": recommended_gate_db,
        "message": msg
    }

@app.get("/api/flashback/status")
def get_flashback_status():
    return flashback_recorder.get_status()

@app.post("/api/flashback/save")
def save_flashback_wav(req: FlashbackSaveRequest):
    return flashback_recorder.save_wav(seconds=req.seconds)

@app.post("/api/flashback/open-folder")
def open_flashback_folder():
    try:
        os.startfile(str(flashback_recorder.flashback_dir))
        return {"status": "ok", "path": str(flashback_recorder.flashback_dir)}
    except Exception as e:
        return {"status": "error", "message": str(e), "path": str(flashback_recorder.flashback_dir)}

class ActivateRequest(BaseModel):
    key: str

@app.get("/api/license/status")
def get_license_status():
    return um2.get_license_info()

@app.post("/api/license/activate")
def activate_license(req: ActivateRequest):
    success, msg, info = um2.activate_license(req.key)
    return {"success": success, "message": msg, "license": info}


_WEB_MEMORY_CACHE: dict[str, tuple[bytes, str]] = {}

def _preload_web_assets():
    mime_overrides = {
        ".html": "text/html; charset=utf-8",
        ".css": "text/css; charset=utf-8",
        ".js": "application/javascript; charset=utf-8",
        ".json": "application/json; charset=utf-8",
        ".svg": "image/svg+xml",
        ".png": "image/png",
        ".ico": "image/x-icon",
    }
    try:
        if WEB_DIR.exists():
            for p in WEB_DIR.rglob("*"):
                if p.is_file():
                    rel = p.relative_to(WEB_DIR).as_posix()
                    ext = p.suffix.lower()
                    mtype = mime_overrides.get(ext) or mimetypes.guess_type(str(p))[0] or "application/octet-stream"
                    _WEB_MEMORY_CACHE[rel] = (p.read_bytes(), mtype)
    except Exception:
        pass

_preload_web_assets()

def _serve_web_asset(rel_path: str):
    rel_clean = rel_path.lstrip("/").replace("\\", "/")
    if rel_clean in _WEB_MEMORY_CACHE:
        content, mtype = _WEB_MEMORY_CACHE[rel_clean]
        return Response(content=content, media_type=mtype, headers={"Cache-Control": "no-store"})
    target = WEB_DIR / rel_clean
    try:
        if target.exists() and target.is_file():
            return FileResponse(str(target))
    except Exception:
        pass
    if "index.html" in _WEB_MEMORY_CACHE:
        content, mtype = _WEB_MEMORY_CACHE["index.html"]
        return Response(content=content, media_type=mtype, headers={"Cache-Control": "no-store"})
    return FileResponse(str(WEB_DIR / "index.html"))

@app.get("/")
def get_index():
    return _serve_web_asset("index.html")

@app.get("/static/{filename:path}")
def get_static_file(filename: str):
    return _serve_web_asset(filename)

@app.get("/{filename:path}")
def get_file(filename: str):
    return _serve_web_asset(filename)

def is_port_open(host="127.0.0.1", port=8930) -> bool:
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.4)
    try:
        s.connect((host, port))
        s.close()
        return True
    except Exception:
        return False

def is_server_running(host="127.0.0.1", port=8930) -> bool:
    if not is_port_open(host, port):
        return False
    try:
        with urllib.request.urlopen(f"http://{host}:{port}/", timeout=0.8) as r1:
            if r1.status != 200:
                return False
        with urllib.request.urlopen(f"http://{host}:{port}/api/status", timeout=0.8) as r2:
            if r2.status != 200:
                return False
            data = json.loads(r2.read().decode("utf-8", errors="ignore"))
            return "ch1PreampMode" in data
    except Exception:
        return False

def kill_stale_server_on_port(port=8930):
    my_pid = os.getpid()
    try:
        out = subprocess.check_output(
            ["netstat", "-ano"],
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000),
            text=True,
            timeout=2.0
        )
        pids_to_kill = set()
        for line in out.splitlines():
            if f"127.0.0.1:{port}" in line and "LISTENING" in line:
                parts = line.split()
                if parts and parts[-1].isdigit():
                    pid = int(parts[-1])
                    if pid > 0 and pid != my_pid:
                        pids_to_kill.add(pid)
        for pid in pids_to_kill:
            subprocess.run(
                ["taskkill", "/F", "/PID", str(pid), "/T"],
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000),
                 stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=3.0
            )
        if pids_to_kill:
            time.sleep(0.4)
    except Exception:
        pass

def bring_window_to_front() -> bool:
    try:
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        found_hwnd = None

        def enum_cb(hwnd, lParam):
            nonlocal found_hwnd
            if user32.IsWindowVisible(hwnd):
                length = user32.GetWindowTextLengthW(hwnd)
                if length > 0:
                    buff = ctypes.create_unicode_buffer(length + 1)
                    user32.GetWindowTextW(hwnd, buff, length + 1)
                    title = buff.value
                    # Match exato da janela do Console U-Phoria, ignorando IDEs/editores
                    if ("Behringer U-Phoria Console" in title) and ("Antigravity" not in title) and ("Visual Studio" not in title):
                        pid = wintypes.DWORD(0)
                        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                        if pid.value:
                            h_proc = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
                            if h_proc:
                                exe_buf = ctypes.create_unicode_buffer(260)
                                sz = wintypes.DWORD(260)
                                if kernel32.QueryFullProcessImageNameW(h_proc, 0, exe_buf, ctypes.byref(sz)):
                                    exe_lower = exe_buf.value.lower()
                                    if "antigravity" in exe_lower or "code.exe" in exe_lower or "devenv.exe" in exe_lower:
                                        kernel32.CloseHandle(h_proc)
                                        return True
                                kernel32.CloseHandle(h_proc)
                        found_hwnd = hwnd
                        return False
            return True

        EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        user32.EnumWindows(EnumProc(enum_cb), 0)

        if found_hwnd:
            if user32.IsIconic(found_hwnd):
                user32.ShowWindow(found_hwnd, 9)  # SW_RESTORE
            else:
                user32.ShowWindow(found_hwnd, 5)  # SW_SHOW
            user32.SetForegroundWindow(found_hwnd)
            return True
    except Exception:
        pass
    return False

def launch_browser_ui():
    if bring_window_to_front():
        return

    url = "http://127.0.0.1:8930"
    app_profile_dir = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "KaxWav", "UM2", "AppProfile")
    os.makedirs(app_profile_dir, exist_ok=True)

    candidates = [
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"),
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe",
    ]
    browser_exe = next((p for p in candidates if os.path.exists(p)), None)

    opened = False
    if browser_exe:
        try:
            cmd = [
                browser_exe,
                f"--app={url}",
                f"--user-data-dir={app_profile_dir}",
                "--window-size=1280,820",
                "--no-first-run",
                "--no-default-browser-check"
            ]
            subprocess.Popen(cmd)
            opened = True
        except Exception:
            opened = False

    if not opened:
        import webbrowser
        webbrowser.open(url)

_server_ready_mutex = None

def main():
    global _server_ready_mutex
    server_already_up = is_server_running(port=8930)

    if not server_already_up:
        if is_port_open(port=8930):
            kill_stale_server_on_port(8930)

        def mark_ready_and_open():
            global _server_ready_mutex
            for _ in range(40):
                if is_port_open(port=8930):
                    try:
                        _server_ready_mutex = ctypes.windll.kernel32.CreateMutexW(None, False, "Local\\BehringerUM2PanelServerReady")
                    except Exception:
                        pass
                    break
                time.sleep(0.1)
            if "--headless" not in sys.argv:
                launch_browser_ui()

        threading.Thread(target=mark_ready_and_open, daemon=True).start()

        try:
            config = uvicorn.Config(app, host="127.0.0.1", port=8930, log_config=None, access_log=False)
            server = uvicorn.Server(config)
            server.run()
        except BaseException as e:
            with open(panel_log_path, "a", encoding="utf-8") as f:
                f.write(f"Server exit/error: {e}\n")
            if "--headless" not in sys.argv and is_server_running(port=8930):
                launch_browser_ui()
    else:
        if "--headless" not in sys.argv:
            launch_browser_ui()

if __name__ == "__main__":
    import multiprocessing
    multiprocessing.freeze_support()
    main()
