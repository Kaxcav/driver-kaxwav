#include <windows.h>
#include <iostream>
#include <string>
#include "../src/UM2License.h"

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::string hwid = UM2License::GetMachineHWID();
    
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, hwid.length() + 1);
        if (hMem) {
            char* p = static_cast<char*>(GlobalLock(hMem));
            if (p) {
                memcpy(p, hwid.c_str(), hwid.length() + 1);
                GlobalUnlock(hMem);
                SetClipboardData(CF_TEXT, hMem);
            }
        }
        CloseClipboard();
    }

    std::cout << "============================================================\n";
    std::cout << "      KAXWAV STUDIO - IDENTIFICADOR DE HARDWARE (HWID)\n";
    std::cout << "============================================================\n\n";
    std::cout << "SEU HARDWARE ID REAL:\n";
    std::cout << ">> " << hwid << " <<\n\n";
    std::cout << "[OK] O codigo acima foi COPIADO automaticamente para o seu Ctrl+V!\n";
    std::cout << "Basta colar (Ctrl+V) no chat para gerar a sua chave Pro.\n\n";
    std::cout << "Pressione qualquer tecla para fechar...\n";
    system("pause >nul");
    return 0;
}
