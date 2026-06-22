#include "tauricpp/app.hpp"
#include "tauricpp/dialog.hpp"
#include "dictionary_manager.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <string>
#include <thread>
#include <atomic>
#include <cstdio>

#define WM_TRAYICON (WM_USER + 1)

static DictionaryManager g_dictManager;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_dictLoaded{false};
static HWND g_hwnd = nullptr;
static NOTIFYICONDATA g_nid = {};
static HICON g_appIcon = nullptr;
static FILE* g_logFile = nullptr;

static void Log(const char* msg) {
    if (g_logFile) { fprintf(g_logFile, "%s\n", msg); fflush(g_logFile); }
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

static void LoadAppIcon(const std::string& exeDir) {
    std::string icoPath = exeDir + "icon.ico";
    g_appIcon = (HICON)LoadImageA(nullptr, icoPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!g_appIcon) g_appIcon = LoadIcon(nullptr, IDI_APPLICATION);
}

static void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_appIcon;
    strcpy_s(g_nid.szTip, "GoldenDict-Lite");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon() { Shell_NotifyIcon(NIM_DELETE, &g_nid); }
static void MinimizeToTray(HWND hwnd) { ShowWindow(hwnd, SW_HIDE); }
static void RestoreFromTray(HWND hwnd) { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string exeDir = exePath;
    exeDir = exeDir.substr(0, exeDir.find_last_of("\\/") + 1);

    g_logFile = fopen((exeDir + "debug.log").c_str(), "w");
    Log("GoldenDict-Lite starting");

    LoadAppIcon(exeDir);
    Log("Icon loaded");

    tauricpp::App::Config config;
    config.window_config.title = "GoldenDict-Lite";
    config.window_config.width = 800;
    config.window_config.height = 600;
    config.window_config.center = true;
    config.window_config.devtools = true;
    config.window_config.bg_color = RGB(250, 248, 242);

    tauricpp::App app(config);
    auto& bridge = app.GetBridge();
    Log("App created");

    std::thread loadThread([exeDir]() {
        Log("Loading dictionaries...");
        g_dictManager.loadDictionaryDir((exeDir + "dictionary").c_str());
        g_dictManager.loadDictionaryDir((exeDir + "bing").c_str());
        std::string msg = "Loaded " + std::to_string(g_dictManager.getDictCount()) + " dicts, " + std::to_string(g_dictManager.getWordCount()) + " words";
        Log(msg.c_str());
        g_dictLoaded = true;
    });
    loadThread.detach();

    bridge.RegisterCommand("query", [](const nlohmann::json& args) -> nlohmann::json {
        std::string word = args.value("word", "");
        if (word.empty()) return {{"html", ""}, {"error", "No word provided"}};
        return {{"html", g_dictManager.lookup(word)}};
    });

    bridge.RegisterCommand("suggest", [](const nlohmann::json& args) -> nlohmann::json {
        std::string prefix = args.value("prefix", "");
        size_t max = args.value("max", 20);
        if (prefix.empty()) return {{"words", std::vector<std::string>()}};
        return {{"words", g_dictManager.prefixMatch(prefix, max)}};
    });

    bridge.RegisterCommand("dict_info", [](const nlohmann::json&) -> nlohmann::json {
        return {
            {"title", g_dictManager.getTitle()},
            {"word_count", g_dictManager.getWordCount()},
            {"dict_count", g_dictManager.getDictCount()},
            {"loaded", g_dictLoaded.load()}
        };
    });

    bridge.RegisterCommand("dict_list", [](const nlohmann::json&) -> nlohmann::json {
        return g_dictManager.getDictList();
    });

    bridge.RegisterCommand("window.minimize", [&app](const nlohmann::json&) -> nlohmann::json {
        MinimizeToTray(app.GetWindow().GetHwnd());
        return {{"success", true}};
    });

    bridge.RegisterCommand("window.close", [&app](const nlohmann::json&) -> nlohmann::json {
        PostMessage(app.GetWindow().GetHwnd(), WM_CLOSE, 0, 0);
        return {{"success", true}};
    });

    app.OnSetup([](tauricpp::App& app) {
        g_hwnd = app.GetWindow().GetHwnd();
        AddTrayIcon(g_hwnd);
        if (g_appIcon) {
            SendMessage(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_appIcon);
            SendMessage(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_appIcon);
        }
        Log("Window icon set");

        app.GetWindow().OnMinimize([]() { MinimizeToTray(g_hwnd); });
        app.GetWindow().OnClose([]() -> bool {
            g_running = false;
            RemoveTrayIcon();
            return true;
        });

        std::thread([]() {
            while (g_running) {
                MSG msg;
                if (PeekMessage(&msg, g_hwnd, WM_TRAYICON, WM_TRAYICON, PM_REMOVE)) {
                    if (msg.lParam == WM_LBUTTONDBLCLK) RestoreFromTray(g_hwnd);
                }
                Sleep(50);
            }
        }).detach();
    });

    int result = app.Run();
    if (g_logFile) fclose(g_logFile);
    return result;
}
