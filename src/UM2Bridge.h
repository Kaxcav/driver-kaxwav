#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BEHRINGER_BRIDGE_EXPORTS
#define BRIDGE_API __declspec(dllexport)
#else
#define BRIDGE_API __declspec(dllimport)
#endif

BRIDGE_API bool __cdecl UM2Bridge_Start();
BRIDGE_API void __cdecl UM2Bridge_Stop();
BRIDGE_API bool __cdecl UM2Bridge_IsCableDetected();
BRIDGE_API bool __cdecl UM2Bridge_IsStandaloneActive();

#ifdef __cplusplus
}
#endif
