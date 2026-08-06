#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <vector>
#include <mutex>
#include <fstream>
#include <random>
#include <chrono>
#include <string>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ---- OFFSET STRUCT (AUTO-UPDATED) ----
struct Offsets {
    uintptr_t UWorld;
    uintptr_t GameInstance;
    uintptr_t LocalPlayer;
    uintptr_t EntityList;
    uintptr_t EntityCount;
    uintptr_t Position;
    uintptr_t Head;
    uintptr_t ViewMatrix;
    uintptr_t WeaponData;
    uintptr_t ShotOrigin;
    uintptr_t ShotRotation;
    uintptr_t Name;
    uintptr_t Camera;
} g_Offsets;

// ---- GLOBALS ----
HANDLE g_hProcess = NULL;
uintptr_t g_BaseAddress = 0;
bool g_ShowMenu = true;
bool g_AimbotEnabled = true, g_ESPEnabled = true, g_MagicBullet = false;
float g_AimbotFOV = 30.0f, g_Smoothing = 15.0f, g_ESPMaxDist = 300.0f;
bool g_ESPBoxes = true, g_ESPHealth = true, g_ESPNames = true, g_ESPDistance = true;
bool g_SilentAim = false;
float g_BoxColor[3] = {0.0f,1.0f,0.0f};
float g_HealthColor[3] = {1.0f,0.0f,0.0f};
float g_NameColor[3] = {1.0f,1.0f,1.0f};
int g_BoneTarget = 0;
std::mutex g_DataMutex;

struct Vector3 { float x, y, z; };
struct Vector2 { float x, y; };
struct Player {
    uintptr_t address;
    Vector3 position, headPosition;
    float health;
    bool isAlive, isVisible;
    Vector2 screenPos, headScreenPos;
    std::string name;
};
std::vector<Player> g_Players;

// ---- DEFAULT OFFSETS (FALLBACK) ----
void SetDefaultOffsets() {
    g_Offsets.UWorld        = 0x4A8B1A0;
    g_Offsets.GameInstance  = 0x1B8;
    g_Offsets.LocalPlayer   = 0x308;
    g_Offsets.EntityList    = 0x1A0;
    g_Offsets.EntityCount   = 0x198;
    g_Offsets.Position      = 0x1A0;
    g_Offsets.Head          = 0x1C0;
    g_Offsets.ViewMatrix    = 0x5C0;
    g_Offsets.WeaponData    = 0x7C0;
    g_Offsets.ShotOrigin    = 0x30;
    g_Offsets.ShotRotation  = 0x7E0;
    g_Offsets.Name          = 0x1A0;
    g_Offsets.Camera        = 0x4A0;
}

// ---- PATTERN SCANNER ----
uintptr_t FindPattern(uintptr_t start, SIZE_T size, const char* pattern, const char* mask) {
    BYTE* buffer = new BYTE[size];
    memcpy(buffer, (void*)start, size);
    SIZE_T patternLen = strlen(mask);
    for (SIZE_T i = 0; i < size - patternLen; i++) {
        bool found = true;
        for (SIZE_T j = 0; j < patternLen; j++) {
            if (mask[j] == 'x' && buffer[i + j] != (BYTE)pattern[j]) { found = false; break; }
        }
        if (found) { delete[] buffer; return start + i; }
    }
    delete[] buffer;
    return 0;
}

// ---- AUTO-SCAN OFFSETS ----
void ScanOffsets() {
    printf("[*] Scanning for offsets...\n");
    uintptr_t base = (uintptr_t)GetModuleHandle(L"KnivesOut.exe");
    if (!base) { printf("[-] Game module not found. Using defaults.\n"); return; }

    // Pattern for UWorld (update if game patches)
    uintptr_t uWorldAddr = FindPattern(base, 0x1000000, "\x48\x8B\x0D\x00\x00\x00\x00\x48\x85\xC9\x74\x0F", "xxx????xxxxx");
    if (uWorldAddr) {
        g_Offsets.UWorld = uWorldAddr - base;
        printf("[+] UWorld found at 0x%llX\n", g_Offsets.UWorld);
    } else {
        printf("[-] UWorld pattern not found. Using default.\n");
    }
    printf("[*] Offsets scan complete.\n");
}

// ---- MEMORY FUNCTIONS ----
DWORD GetProcessId(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry = { sizeof(entry) };
    if (Process32FirstW(snap, &entry)) {
        do { if (_wcsicmp(entry.szExeFile, name) == 0) { CloseHandle(snap); return entry.th32ProcessID; } }
        while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return 0;
}

bool ReadMemory(HANDLE hProc, uintptr_t addr, void* buffer, SIZE_T size) {
    SIZE_T bytesRead;
    return ReadProcessMemory(hProc, (LPCVOID)addr, buffer, size, &bytesRead);
}

bool WriteMemory(HANDLE hProc, uintptr_t addr, void* buffer, SIZE_T size) {
    SIZE_T bytesWritten;
    return WriteProcessMemory(hProc, (LPVOID)addr, buffer, size, &bytesWritten);
}

// ---- ESP ----
bool WorldToScreen(Vector3 worldPos, Vector2& screenPos, float viewMatrix[4][4]) {
    screenPos.x = viewMatrix[0][0] * worldPos.x + viewMatrix[0][1] * worldPos.y + viewMatrix[0][2] * worldPos.z + viewMatrix[0][3];
    screenPos.y = viewMatrix[1][0] * worldPos.x + viewMatrix[1][1] * worldPos.y + viewMatrix[1][2] * worldPos.z + viewMatrix[1][3];
    float w = viewMatrix[3][0] * worldPos.x + viewMatrix[3][1] * worldPos.y + viewMatrix[3][2] * worldPos.z + viewMatrix[3][3];
    if (w < 0.001f) return false;
    screenPos.x /= w; screenPos.y /= w;
    screenPos.x = (screenPos.x + 1.0f) * 0.5f * GetSystemMetrics(SM_CXSCREEN);
    screenPos.y = (1.0f - screenPos.y) * 0.5f * GetSystemMetrics(SM_CYSCREEN);
    return true;
}

void UpdateESP(HANDLE hProc, uintptr_t uWorld, float viewMatrix[4][4]) {
    std::lock_guard<std::mutex> lock(g_DataMutex);
    g_Players.clear();
    uintptr_t gameInstance = 0;
    ReadMemory(hProc, uWorld + g_Offsets.GameInstance, &gameInstance, 8);
    if (!gameInstance) return;
    uintptr_t localPlayer = 0;
    ReadMemory(hProc, gameInstance + g_Offsets.LocalPlayer, &localPlayer, 8);
    if (!localPlayer) return;
    uintptr_t entityList = 0;
    ReadMemory(hProc, localPlayer + g_Offsets.EntityList, &entityList, 8);
    if (!entityList) return;
    int count = 0;
    ReadMemory(hProc, localPlayer + g_Offsets.EntityCount, &count, 4);
    for (int i = 0; i < count; i++) {
        uintptr_t entityAddr = 0;
        ReadMemory(hProc, entityList + i * 8, &entityAddr, 8);
        if (!entityAddr) continue;
        Player p;
        p.address = entityAddr;
        ReadMemory(hProc, entityAddr + g_Offsets.Position, &p.position, 12);
        ReadMemory(hProc, entityAddr + g_Offsets.Head, &p.headPosition, 12);
        ReadMemory(hProc, entityAddr + g_Offsets.Health, &p.health, 4);
        ReadMemory(hProc, entityAddr + g_Offsets.Visible, &p.isVisible, 1);
        p.isAlive = p.health > 0;
        char name[64] = { 0 };
        ReadMemory(hProc, entityAddr + g_Offsets.Name, name, 64);
        p.name = std::string(name);
        Vector2 screenPos;
        if (WorldToScreen(p.position, screenPos, viewMatrix)) {
            p.screenPos = screenPos;
            Vector2 headScreen;
            if (WorldToScreen(p.headPosition, headScreen, viewMatrix)) {
                p.headScreenPos = headScreen;
            }
        }
        g_Players.push_back(p);
    }
}

void DrawESP(float viewMatrix[4][4]) {
    if (!g_ESPEnabled) return;
    Vector3 localPos = { 0,0,0 };
    for (auto& p : g_Players) {
        if (p.address == 0 || !p.isAlive) continue;
        if (g_ESPVisibleOnly && !p.isVisible) continue;
        float dist = sqrt(pow(p.position.x - localPos.x, 2) + pow(p.position.y - localPos.y, 2));
        if (dist > g_ESPMaxDist) continue;
        Vector2 screenPos = p.screenPos;
        Vector2 headScreenPos = p.headScreenPos;
        if (screenPos.x == 0 && screenPos.y == 0) continue;
        float height = abs(screenPos.y - headScreenPos.y);
        float width = height * 0.5f;
        if (g_ESPBoxes) {
            ImVec2 topLeft(headScreenPos.x - width/2, headScreenPos.y);
            ImVec2 bottomRight(headScreenPos.x + width/2, screenPos.y);
            ImGui::GetWindowDrawList()->AddRect(topLeft, bottomRight, IM_COL32(g_BoxColor[0]*255, g_BoxColor[1]*255, g_BoxColor[2]*255, 255));
        }
        if (g_ESPHealth) {
            float healthPct = p.health / 100.0f;
            ImVec2 barPos(headScreenPos.x - width/2 - 6, headScreenPos.y);
            ImGui::GetWindowDrawList()->AddRectFilled(barPos, { barPos.x+4, barPos.y+height }, IM_COL32(0,0,0,200));
            ImGui::GetWindowDrawList()->AddRectFilled(barPos, { barPos.x+4, barPos.y+height*healthPct }, IM_COL32(g_HealthColor[0]*255, g_HealthColor[1]*255, g_HealthColor[2]*255, 255));
        }
        if (g_ESPNames) {
            ImGui::GetWindowDrawList()->AddText(ImVec2(headScreenPos.x - 20, headScreenPos.y - 15), IM_COL32(g_NameColor[0]*255, g_NameColor[1]*255, g_NameColor[2]*255, 255), p.name.c_str());
        }
        if (g_ESPDistance) {
            char distStr[32]; sprintf_s(distStr, "%.0fm", dist);
            ImGui::GetWindowDrawList()->AddText(ImVec2(headScreenPos.x - 10, screenPos.y + 2), IM_COL32(255,255,255,255), distStr);
        }
    }
}

// ---- AIMBOT ----
void Aimbot(HANDLE hProc, uintptr_t localPlayer, float viewMatrix[4][4]) {
    if (!g_AimbotEnabled) return;
    Vector3 localPos;
    ReadMemory(hProc, localPlayer + g_Offsets.Position, &localPos, 12);
    Player target;
    float closestDist = g_AimbotFOV;
    for (auto& p : g_Players) {
        if (p.address == localPlayer || !p.isAlive) continue;
        Vector2 screenPos = p.screenPos;
        if (screenPos.x == 0 && screenPos.y == 0) continue;
        float dist = sqrt(pow(screenPos.x - GetSystemMetrics(SM_CXSCREEN)/2, 2) + pow(screenPos.y - GetSystemMetrics(SM_CYSCREEN)/2, 2));
        if (dist < closestDist) { closestDist = dist; target = p; }
    }
    if (target.address == 0) return;
    Vector3 aimPos;
    switch (g_BoneTarget) {
        case 0: aimPos = target.headPosition; break;
        case 1: aimPos = target.neckPosition; break;
        case 2: aimPos = target.chestPosition; break;
        case 3: aimPos = target.pelvisPosition; break;
        default: aimPos = target.headPosition;
    }
    float dx = aimPos.x - localPos.x;
    float dy = aimPos.y - localPos.y;
    float dz = aimPos.z - localPos.z;
    float pitch = atan2(dz, sqrt(dx*dx + dy*dy)) * (180.0f / 3.14159265f);
    float yaw = atan2(dy, dx) * (180.0f / 3.14159265f);
    uintptr_t cameraManager = 0;
    ReadMemory(hProc, localPlayer + g_Offsets.Camera, &cameraManager, 8);
    if (cameraManager) {
        if (g_SilentAim) {
            uintptr_t weaponData = 0;
            ReadMemory(hProc, localPlayer + g_Offsets.WeaponData, &weaponData, 8);
            if (weaponData) {
                Vector3 angles = { pitch, yaw, 0 };
                WriteMemory(hProc, weaponData + g_Offsets.ShotRotation, &angles, 12);
            }
        } else {
            Vector3 angles = { pitch, yaw, 0 };
            WriteMemory(hProc, cameraManager + 0x2C0, &angles, 12);
        }
    }
}

// ---- MAGIC BULLET ----
void MagicBullet(HANDLE hProc, uintptr_t localPlayer) {
    if (!g_MagicBullet) return;
    Player target;
    float closestDist = 300.0f;
    for (auto& p : g_Players) {
        if (p.address == localPlayer || !p.isAlive) continue;
        float dist = sqrt(pow(p.position.x - target.position.x, 2) + pow(p.position.y - target.position.y, 2));
        if (dist < closestDist) { closestDist = dist; target = p; }
    }
    if (target.address == 0) return;
    uintptr_t weaponData = 0;
    ReadMemory(hProc, localPlayer + g_Offsets.WeaponData, &weaponData, 8);
    if (!weaponData) return;
    Vector3 hitPos = target.headPosition;
    WriteMemory(hProc, weaponData + g_Offsets.ShotOrigin, &hitPos, 12);
}

// ---- MENU ----
void DrawMenu() {
    if (!g_ShowMenu) return;
    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("MINEHEX v1.0", &g_ShowMenu,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(ImVec2(winPos.x, winPos.y), ImVec2(winPos.x + winSize.x, winPos.y + 32), IM_COL32(10, 20, 30, 255));
    draw->AddRect(ImVec2(winPos.x, winPos.y), ImVec2(winPos.x + winSize.x, winPos.y + 32), IM_COL32(0, 255, 255, 100), 0.0f, 0, 2.0f);
    draw->AddText(ImVec2(winPos.x + 12, winPos.y + 8), IM_COL32(0, 255, 255, 255), "MINEHEX v1.0 [ Knives Out ]");
    ImGui::SetCursorPos(ImVec2(winSize.x - 30, 6));
    if (ImGui::Button("X", ImVec2(22, 22))) { g_ShowMenu = false; }

    ImGui::SetCursorPosY(36);
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0, 1, 1, 1), "STATUS: ");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0, 1, 0, 1), "ACTIVE");
    ImGui::SameLine(0, 30); ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "|");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 1, 0, 1), "Players: %d", (int)g_Players.size());
    ImGui::SameLine(0, 30); ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "|");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0, 1, 1, 1), "FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Separator();

    ImGui::TextColored(ImVec4(0, 1, 1, 1), "QUICK PRESETS");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.2f, 0.4f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.4f, 0.8f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.6f, 1.0f, 1.0f));

    if (ImGui::Button(" LEGIT ", ImVec2(80, 28))) {
        g_AimbotFOV = 15; g_Smoothing = 25; g_AimbotEnabled = true;
        g_ESPEnabled = true; g_MagicBullet = false; g_SilentAim = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(" RAGE ", ImVec2(80, 28))) {
        g_AimbotFOV = 180; g_Smoothing = 2; g_AimbotEnabled = true;
        g_ESPEnabled = true; g_MagicBullet = true; g_SilentAim = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(" STEALTH ", ImVec2(80, 28))) {
        g_AimbotFOV = 10; g_Smoothing = 30; g_AimbotEnabled = true;
        g_ESPEnabled = true; g_MagicBullet = false; g_SilentAim = false;
    }
    ImGui::PopStyleColor(3);
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.05f, 0.1f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.0f, 0.4f, 0.8f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.0f, 0.6f, 1.0f, 1.0f));

    if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("🎯 Aimbot")) {
            ImGui::Checkbox("Enable Aimbot", &g_AimbotEnabled);
            ImGui::SameLine();
            if (g_AimbotEnabled) ImGui::TextColored(ImVec4(0,1,0,1), "ON"); else ImGui::TextColored(ImVec4(1,0,0,1), "OFF");
            ImGui::SliderFloat("FOV", &g_AimbotFOV, 0, 360);
            ImGui::SliderFloat("Smoothing", &g_Smoothing, 1, 30);
            ImGui::Checkbox("Silent Aim", &g_SilentAim);
            ImGui::Combo("Bone Target", &g_BoneTarget, "Head\0Neck\0Chest\0Pelvis\0");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("👁️ ESP")) {
            ImGui::Checkbox("Enable ESP", &g_ESPEnabled);
            ImGui::SameLine();
            if (g_ESPEnabled) ImGui::TextColored(ImVec4(0,1,0,1), "ON"); else ImGui::TextColored(ImVec4(1,0,0,1), "OFF");
            ImGui::Checkbox("Boxes", &g_ESPBoxes);
            ImGui::Checkbox("Health Bars", &g_ESPHealth);
            ImGui::Checkbox("Names", &g_ESPNames);
            ImGui::Checkbox("Distance", &g_ESPDistance);
            ImGui::SliderFloat("Max Distance", &g_ESPMaxDist, 50, 500);
            ImGui::ColorEdit3("Box Color", g_BoxColor);
            ImGui::ColorEdit3("Health Color", g_HealthColor);
            ImGui::ColorEdit3("Name Color", g_NameColor);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("✨ Magic")) {
            ImGui::Checkbox("Enable Magic Bullet", &g_MagicBullet);
            ImGui::SameLine();
            if (g_MagicBullet) ImGui::TextColored(ImVec4(0,1,0,1), "ON"); else ImGui::TextColored(ImVec4(1,0,0,1), "OFF");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("⚙️ Settings")) {
            if (ImGui::Button("Save Config", ImVec2(120, 30))) {
                std::ofstream f("minehex.cfg", std::ios::binary);
                f.write((char*)&g_AimbotEnabled, sizeof(bool));
                f.write((char*)&g_AimbotFOV, sizeof(float));
                f.write((char*)&g_Smoothing, sizeof(float));
                f.write((char*)&g_ESPEnabled, sizeof(bool));
                f.write((char*)&g_MagicBullet, sizeof(bool));
                f.write((char*)&g_ESPMaxDist, sizeof(float));
                f.close();
            }
            ImGui::SameLine();
            if (ImGui::Button("Load Config", ImVec2(120, 30))) {
                std::ifstream f("minehex.cfg", std::ios::binary);
                if (f.is_open()) {
                    f.read((char*)&g_AimbotEnabled, sizeof(bool));
                    f.read((char*)&g_AimbotFOV, sizeof(float));
                    f.read((char*)&g_Smoothing, sizeof(float));
                    f.read((char*)&g_ESPEnabled, sizeof(bool));
                    f.read((char*)&g_MagicBullet, sizeof(bool));
                    f.read((char*)&g_ESPMaxDist, sizeof(float));
                    f.close();
                }
            }
            ImGui::Text("Hotkeys:");
            ImGui::Text("F1 - Aimbot   F2 - ESP   F3 - Magic");
            ImGui::Text("INS - Toggle Menu");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleColor(3);

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1), "Made with ❤️ by MINEHEX | Use at your own risk.");
    ImGui::End();
}

// ---- RENDER LOOP ----
void RenderLoop() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// ---- HOOK ----
typedef HRESULT (WINAPI* PresentFunc)(IDXGISwapChain*, UINT, UINT);
PresentFunc oPresent = nullptr;
HWND g_GameHWND = nullptr;

HRESULT WINAPI HookedPresent(IDXGISwapChain* pSwapChain, UINT Sync, UINT Flags) {
    if (!g_GameHWND) {
        pSwapChain->GetHWND(&g_GameHWND);
        if (g_GameHWND) {
            ImGui::CreateContext();
            ImGui_ImplWin32_Init(g_GameHWND);
            ImGui_ImplDX11_Init(pSwapChain, nullptr);
        }
    }
    RenderLoop();
    return oPresent(pSwapChain, Sync, Flags);
}

void InstallHook() {
    IDXGISwapChain* swapChain = nullptr;
    D3D_FEATURE_LEVEL featureLevel;
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = 1;
    sd.BufferDesc.Height = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = GetForegroundWindow();
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swapChain, nullptr, nullptr, nullptr);
    if (SUCCEEDED(hr) && swapChain) {
        void** vtable = *(void***)swapChain;
        oPresent = (PresentFunc)vtable[8];
        DWORD oldProtect;
        VirtualProtect(&vtable[8], sizeof(uintptr_t), PAGE_READWRITE, &oldProtect);
        vtable[8] = &HookedPresent;
        VirtualProtect(&vtable[8], sizeof(uintptr_t), oldProtect, &oldProtect);
        swapChain->Release();
    }
}

// ---- MAIN THREAD ----
DWORD WINAPI MainThread(LPVOID) {
    AllocConsole(); freopen("CONOUT$", "w", stdout);
    printf("[+] MINEHEX v1.0 – Internal DLL loaded.\n");
    printf("[+] Press INS to open menu.\n");

    // Auto-scan offsets
    ScanOffsets();

    g_hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, GetProcessId(L"KnivesOut.exe"));
    if (!g_hProcess) { printf("[-] Game not running.\n"); return 0; }
    g_BaseAddress = (uintptr_t)GetModuleHandle(L"KnivesOut.exe");

    InstallHook();

    float viewMatrix[4][4];
    while (true) {
        uintptr_t uWorld = g_BaseAddress + g_Offsets.UWorld;
        uintptr_t localPlayer = 0;
        ReadMemory(g_hProcess, uWorld + g_Offsets.GameInstance, &localPlayer, 8);
        if (localPlayer) {
            ReadMemory(g_hProcess, localPlayer + g_Offsets.ViewMatrix, viewMatrix, 64);
            UpdateESP(g_hProcess, uWorld, viewMatrix);
            Aimbot(g_hProcess, localPlayer, viewMatrix);
            MagicBullet(g_hProcess, localPlayer);
        }
        if (GetAsyncKeyState(VK_INSERT) & 1) g_ShowMenu = !g_ShowMenu;
        if (GetAsyncKeyState(VK_F1) & 1) g_AimbotEnabled = !g_AimbotEnabled;
        if (GetAsyncKeyState(VK_F2) & 1) g_ESPEnabled = !g_ESPEnabled;
        if (GetAsyncKeyState(VK_F3) & 1) g_MagicBullet = !g_MagicBullet;
        Sleep(10);
    }
    return 0;
}

// ---- DLL ENTRY ----
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    }
    return TRUE;
}