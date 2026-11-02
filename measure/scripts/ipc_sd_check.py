"""Security descriptor of the driver's named sections (plan D: an elevated host must leave them openable by the
non-elevated panel). Prints the SDDL (owner, DACL, integrity label) and checks: Medium label with no-write-up,
and an ACE for the current user. Exit 0 = ok, 1 = section missing, 2 = wrong descriptor.

  python measure/scripts/ipc_sd_check.py
"""
import ctypes
import sys
from ctypes import wintypes

k32 = ctypes.windll.kernel32
adv = ctypes.windll.advapi32
k32.OpenFileMappingW.restype = wintypes.HANDLE
k32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
adv.GetSecurityInfo.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.DWORD, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
adv.ConvertSecurityDescriptorToStringSecurityDescriptorW.argtypes = [ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                                                     ctypes.POINTER(wintypes.LPWSTR), ctypes.c_void_p]
k32.LocalFree.argtypes = [ctypes.c_void_p]

READ_CONTROL = 0x00020000
SE_KERNEL_OBJECT = 6
OWNER, DACL, LABEL = 0x1, 0x4, 0x10
SECTIONS = ["Local\\BehringerUM2SharedMemory", "Local\\BehringerUM2Telemetry"]


def user_sid_string():
    import subprocess
    out = subprocess.run(["whoami", "/user", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
    return out.strip().split(",")[-1].strip('"')


def sddl_of(name):
    h = k32.OpenFileMappingW(READ_CONTROL, False, name)
    if not h:
        return None, ctypes.GetLastError()
    psd = ctypes.c_void_p()
    rc = adv.GetSecurityInfo(wintypes.HANDLE(h), SE_KERNEL_OBJECT, OWNER | DACL | LABEL, None, None, None, None,
                             ctypes.byref(psd))
    k32.CloseHandle(wintypes.HANDLE(h))
    if rc != 0:
        return None, rc
    s = wintypes.LPWSTR()
    adv.ConvertSecurityDescriptorToStringSecurityDescriptorW(psd, 1, OWNER | DACL | LABEL, ctypes.byref(s), None)
    out = s.value
    k32.LocalFree(psd)
    return out, 0


def main():
    sid = user_sid_string()
    worst = 0
    for name in SECTIONS:
        sddl, err = sddl_of(name)
        if sddl is None:
            print(f"{name}: ausente/sem acesso (erro {err})")
            worst = max(worst, 1)
            continue
        ok = ("ML;;NW;;;ME" in sddl) and (sid in sddl)
        print(f"{name}: {'OK' if ok else 'ERRADO'}  {sddl}")
        if not ok:
            worst = 2
    return worst


if __name__ == "__main__":
    sys.exit(main())
