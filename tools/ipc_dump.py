"""Le a memoria compartilhada do UM2 (layout do painel) e imprime os campos relevantes."""
import sys, ctypes
from ctypes import wintypes
sys.path.insert(0, r"C:\driver xd\behringer-um2\panel")
import importlib.util
spec = importlib.util.spec_from_file_location("pb_struct", r"C:\driver xd\behringer-um2\tools\_ipc_struct.py")
k = ctypes.windll.kernel32
k.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
k.OpenFileMappingW.restype = wintypes.HANDLE
k.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
k.MapViewOfFile.restype = ctypes.c_void_p
src = open(r"C:\driver xd\behringer-um2\panel\panel_backend.py", encoding="utf-8").read()
start = src.index("class UM2SharedState(ctypes.Structure):")
end = src.index("# ====", start)
ns = {"ctypes": ctypes}
exec(src[start:end], ns)
S = ns["UM2SharedState"]
h = k.OpenFileMappingW(0xF001F, False, "Local\\BehringerUM2SharedMemory")
if not h:
    print("mapping nao existe"); sys.exit(1)
p = k.MapViewOfFile(h, 0xF001F, 0, 0, ctypes.sizeof(S))
st = S.from_address(p)
for f in sys.argv[1:] or ["magic", "version", "extMagic", "ch1NoiseReduction", "ch2NoiseReduction", "rawCapture",
                          "ch1NoiseFloorDb", "ch1NrReductionDb", "inputLatencySamples", "outputLatencySamples", "isStreaming"]:
    v = getattr(st, f)
    print(f"{f} = {hex(v) if f.endswith('agic') else v}")
