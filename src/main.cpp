#include "tauricpp/app.hpp"
#include "tauricpp/dialog.hpp"
#include "dictionary_manager.hpp"
#include "goldendict/iconv.hh"
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
        try {
            Log("Loading dictionaries...");
            g_dictManager.loadDictionaryDir((exeDir + "dictionary").c_str());
            g_dictManager.finalizeLoading();
            std::string msg = "Loaded " + std::to_string(g_dictManager.getDictCount()) + " dicts, " + std::to_string(g_dictManager.getWordCount()) + " words";
            Log(msg.c_str());
        } catch (const std::exception& e) {
            Log(("Dictionary loading exception: " + std::string(e.what())).c_str());
        } catch (...) {
            Log("Dictionary loading failed with unknown exception");
        }
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
            {"title", Iconv::ensureUtf8(g_dictManager.getTitle())},
            {"word_count", g_dictManager.getWordCount()},
            {"dict_count", g_dictManager.getDictCount()},
            {"loaded", g_dictLoaded.load()},
            {"progress_dict", Iconv::ensureUtf8(g_dictManager.getProgressDict())},
            {"progress_current", g_dictManager.getProgressCurrent()},
            {"progress_total", g_dictManager.getProgressTotal()},
            {"progress_cached", g_dictManager.getProgressCached()}
        };
    });

    bridge.RegisterCommand("dict_list", [](const nlohmann::json&) -> nlohmann::json {
        return g_dictManager.getDictList();
    });

    bridge.RegisterCommand("resource", [](const nlohmann::json& args) -> nlohmann::json {
        std::string dict = args.value("dict", "");
        std::string path = args.value("path", "");
        if (dict.empty() || path.empty()) return {{"data", ""}, {"error", "Missing dict or path"}};
        std::string base64 = g_dictManager.lookupResource(dict, path);
        if (base64.empty()) return {{"data", ""}, {"error", "Resource not found"}};
        return {{"data", base64}};
    });

    bridge.RegisterCommand("mdd_search", [](const nlohmann::json& args) -> nlohmann::json {
        std::string dict = args.value("dict", "");
        std::string pattern = args.value("pattern", "");
        if (dict.empty() || pattern.empty()) return {{"words", nlohmann::json::array()}};
        auto results = g_dictManager.searchMddHeadwords(dict, pattern);
        return {{"words", results}};
    });

    bridge.RegisterCommand("set_dict_order", [](const nlohmann::json& args) -> nlohmann::json {
        auto orderArr = args.value("order", nlohmann::json::array());
        std::vector<std::string> order;
        for (auto& item : orderArr) {
            if (item.is_string()) order.push_back(item.get<std::string>());
        }
        g_dictManager.setDictOrder(order);
        return {{"success", true}};
    });

    bridge.RegisterCommand("get_dict_order", [](const nlohmann::json&) -> nlohmann::json {
        auto order = g_dictManager.getDictOrder();
        return {{"order", order}};
    });

    bridge.RegisterCommand("window.minimize", [&app](const nlohmann::json&) -> nlohmann::json {
        MinimizeToTray(app.GetWindow().GetHwnd());
        return {{"success", true}};
    });

    bridge.RegisterCommand("window.close", [&app](const nlohmann::json&) -> nlohmann::json {
        PostMessage(app.GetWindow().GetHwnd(), WM_CLOSE, 0, 0);
        return {{"success", true}};
    });

    bridge.RegisterCommand("save_png", [](const nlohmann::json& args) -> nlohmann::json {
        std::string base64Data = args.value("data", "");
        std::string filename = args.value("filename", "export.png");
        
        if (base64Data.empty()) {
            return {{"success", false}, {"error", "No data provided"}};
        }
        
        // Remove data URL prefix if present
        size_t commaPos = base64Data.find(',');
        if (commaPos != std::string::npos) {
            base64Data = base64Data.substr(commaPos + 1);
        }
        
        // Decode base64
        static const std::string base64Chars = 
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        
        std::vector<unsigned char> decoded;
        decoded.reserve(base64Data.size() * 3 / 4);
        
        unsigned char val3[3];
        unsigned char val4[4];
        int i = 0, j = 0;
        
        for (size_t k = 0; k < base64Data.size(); ++k) {
            char c = base64Data[k];
            if (c == '=') break;
            
            size_t pos = base64Chars.find(c);
            if (pos == std::string::npos) continue;
            
            val4[i++] = static_cast<unsigned char>(pos);
            if (i == 4) {
                val3[0] = (val4[0] << 2) + ((val4[1] & 0x30) >> 4);
                val3[1] = ((val4[1] & 0xf) << 4) + ((val4[2] & 0x3c) >> 2);
                val3[2] = ((val4[2] & 0x3) << 6) + val4[3];
                
                for (i = 0; i < 3; i++) {
                    decoded.push_back(val3[i]);
                }
                i = 0;
            }
        }
        
        if (i) {
            for (j = i; j < 4; j++) val4[j] = 0;
            val3[0] = (val4[0] << 2) + ((val4[1] & 0x30) >> 4);
            val3[1] = ((val4[1] & 0xf) << 4) + ((val4[2] & 0x3c) >> 2);
            val3[2] = ((val4[2] & 0x3) << 6) + val4[3];
            for (j = 0; j < i - 1; j++) {
                decoded.push_back(val3[j]);
            }
        }
        
        // Save to file in current directory
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string exeDir = exePath;
        exeDir = exeDir.substr(0, exeDir.find_last_of("\\/") + 1);
        
        std::string filePath = exeDir + filename;
        
        // Convert to wide string for Unicode path support
        int wlen = MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0);
        if (wlen <= 0) {
            // Try system default codepage
            wlen = MultiByteToWideChar(CP_ACP, 0, filePath.c_str(), -1, nullptr, 0);
            if (wlen <= 0) {
                return {{"success", false}, {"error", "Cannot convert filename to wide string"}};
            }
        }
        
        std::vector<wchar_t> wFilePath(wlen);
        // Try UTF-8 first, then ACP
        if (MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, wFilePath.data(), wlen) <= 0) {
            if (MultiByteToWideChar(CP_ACP, 0, filePath.c_str(), -1, wFilePath.data(), wlen) <= 0) {
                return {{"success", false}, {"error", "Cannot convert filename"}};
            }
        }
        
        FILE* f = nullptr;
        if (_wfopen_s(&f, wFilePath.data(), L"wb") != 0 || !f) {
            return {{"success", false}, {"error", "Cannot open file for writing"}};
        }
        
        size_t written = fwrite(decoded.data(), 1, decoded.size(), f);
        fclose(f);
        
        if (written != decoded.size()) {
            return {{"success", false}, {"error", "Write incomplete"}};
        }
        
        return {{"success", true}, {"path", filePath}};
    });

    bridge.RegisterCommand("shell_open", [](const nlohmann::json& args) -> nlohmann::json {
        std::string url = args.value("url", "");
        if (url.empty()) return {{"success", false}, {"error", "No URL provided"}};
        int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return {{"success", false}, {"error", "Cannot convert URL"}};
        std::vector<wchar_t> wUrl(wlen);
        MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wUrl.data(), wlen);
        ShellExecuteW(nullptr, L"open", wUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
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
