# GoldenDict-Lite

轻量级桌面词典，基于 [Lutra](https://github.com/nicereply/TauriCPP)（TauriCPP）框架构建， 支持MDX词典解析引擎，实现零外部依赖、单 EXE 部署的现代词典应用。

## 特性

- **单 EXE 部署** — WebView2Loader 静态链接，前端资源内嵌，无需安装运行时
- **MDX 词典支持** — 兼容 GoldenDict 的 .mdx 词典格式，支持加密词典
- **多词典同时查询** — 自动扫描 `dictionary/`目录，一次查询显示所有词典结果
- **实时自动补全** — 输入即搜，候选词即时显示
- **词典图标** — 自动加载同名 .png/.ico 作为词典标识
- **系统托盘** — 最小化到托盘，双击恢复
- **护眼米白金配色** — 温暖舒适的界面主题
- **纯 C++ 实现** — 无 Qt、无 Electron、无 Rust，纯 Win32 + WebView2
- **全静态链接** — zlib、WebView2Loader 均为静态链接，无外部 DLL

## 架构

```
┌──────────────────────────────────────────────────┐
│                 GoldenDictLite.exe                │
│                                                   │
│  ┌─────────────┐  ┌──────────────┐               │
│  │  TauriCPP   │  │  MDX Parser  │               │
│  │  (WebView2) │  │  (C++ 静态库) │               │
│  │             │──│              │               │
│  │  Bridge     │  │  mdictparser │               │
│  │  VirtualFS  │  │  iconv (Win) │               │
│  │  Window     │  │  zlib        │               │
│  └──────┬──────┘  │  RIPEMD128   │               │
│         │         └──────┬───────┘               │
│         │                │                        │
│  ┌──────▼────────────────▼──────────────────────┐ │
│  │            DictionaryManager                  │ │
│  │  - 词典加载/扫描                               │ │
│  │  - 前缀匹配 (prefixMatch)                     │ │
│  │  - 词条查询 (lookup)                          │ │
│  │  - CSS 样式独立作用域                          │ │
│  │  - 图标加载 (base64)                          │ │
│  └──────────────────────────────────────────────┘ │
│                                                   │
│  ┌──────────────────────────────────────────────┐ │
│  │              Frontend (HTML/CSS/JS)           │ │
│  │  - 搜索栏 + 自动补全                          │ │
│  │  - 多词典卡片式结果展示                        │ │
│  │  - 底部词典栏 (图标 + 名称 + 词条数)          │ │
│  └──────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
```

### 与 goldendict-ng-26.6.0 的对比

| 特性 | GoldenDict-Lite | goldendict-ng |
|------|----------------|---------------|
| **UI 框架** | Lutra (TauriCPP) + WebView2 | Qt 6 + WebEngine |
| **词典格式** | MDX | MDX, DSL, BGL, StarDict, ZIM 等 15+ |
| **语言** | C++17 (纯标准库 + Win32) | C++17 (Qt 6 生态) |
| **依赖** | WebView2 (Win10/11 自带) | Qt 6, OpenSSL, XZ, FFmpeg 等 |
| **二进制大小** | ~500KB | ~50MB+ |
| **内存占用** | ~300MB (单词典) | ~200MB |
| **启动速度** | <1s | 2-5s |
| **前端保护** | 前端资源编译进 EXE，内存加载 | WebEngine 渲染 |
| **跨平台** | Windows | Windows, macOS, Linux |
| **安装** | 单 EXE，无需安装 | 需安装 Qt 运行时 |

### 技术决策

- **去除 Qt 依赖**: QFile → std::fopen, QDataStream → 手动字节序读取, QByteArray → std::vector, QString → std::string, QDomDocument → std::regex, QMutex → std::mutex
- **编码转换**: iconv → Windows API (MultiByteToWideChar/WideCharToMultiByte)
- **内存映射**: QFile::map → Windows CreateFileMapping/MapViewOfFile
- **静态链接**: zlibstatic, WebView2LoaderStatic, 均通过 vcpkg x64-windows-static 安装

## 目录结构

```
GoldenDict-Lite/
├── CMakeLists.txt              # 顶层构建配置
├── build.ps1                   # 构建脚本
├── goldendict.png              # 应用图标
├── src/
│   ├── main.cpp                # 应用入口
│   ├── dictionary_manager.*    # 词典管理器
│   ├── goldendict/             # 从 goldendict-ng 提取的解析器 (无 Qt)
│   │   ├── mdictparser.*       # MDICT 格式核心解析
│   │   ├── iconv.*             # 编码转换 (Win API)
│   │   ├── decompress.*        # zlib 解压
│   │   ├── ripemd.*            # RIPEMD-128 加密
│   │   ├── htmlescape.*        # HTML 转义
│   │   ├── text.*              # UTF-8/32 转换
│   │   ├── filetype.*          # 文件类型检测
│   │   ├── sptr.hh             # 智能指针
│   │   └── ex.hh               # 异常宏
│   ├── icon.ico                # 应用图标
│   └── icon.rc                 # 资源文件
├── frontend/                   # 前端界面
│   ├── index.html
│   ├── css/style.css           # 米白金主题
│   └── js/app.js
├── dictionary/                 # 词典目录 (放入 .mdx + .css + .png)
├── bing/                       # 示例词典
│   ├── concise-bing.mdx
│   └── concise-bing.css
├── TauriCPP/                   # Lutra (TauriCPP) 框架
└── goldendict-ng-26.6.0/       # 原版源码 (仅用于参考)
```

## 编译方法

### 前置条件

- Windows 10/11
- Visual Studio 2019+ (C++ Desktop Development workload)
- [vcpkg](https://vcpkg.io) (classic mode, installed at `C:\vcpkg`)
- CMake 3.15+
- Python 3.7+ (资源打包)

### 首次编译

```powershell
# 1. 安装依赖 (首次)
vcpkg install zlib:x64-windows-static webview2:x64-windows-static

# 2. 配置
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static

# 3. 编译
cmake --build build --config Release
```

### 使用 build.ps1

```powershell
.\build.ps1              # 编译 Release
.\build.ps1 -Clean       # 清理并重新编译
.\build.ps1 -SetupDeps   # 首次安装依赖
```

### 运行

```powershell
# 确保 Release 目录下有 icon.ico
copy src\icon.ico build\Release\
# 运行
.\build\Release\GoldenDictLite.exe
```

## 使用方法

1. 将 `.mdx` 词典文件放入 `dictionary/` 目录
2. 词典可配套同名 `.css` 和 `.png` 文件：
   ```
   dictionary/
   ├── mydict.mdx          # 词典数据
   ├── mydict.css           # 词典样式 (可选)
   └── mydict.png           # 词典图标 (可选)
   ```
3. 启动 GoldenDict-Lite，程序自动扫描并加载所有词典
4. 在搜索框输入词汇，实时查看所有词典的释义
5. 支持候选词自动补全，回车或点击直接查询
6. 点击最小化按钮可最小化到系统托盘，双击托盘图标恢复

## 依赖库

所有依赖均静态链接，无运行时 DLL：

| 库 | 用途 | 版本 |
|----|------|------|
| WebView2Loader | WebView2 静态加载器 | 1.0.3240 |
| zlib | MDX 压缩/解压 | 1.3.1 |
| nlohmann/json | JSON 通信 | 3.11.3 |
| Windows API | 编码转换、内存映射 | Win10+ |

## 许可证

MIT

## 致谢

- [goldendict-ng](https://github.com/xiaoyifang/goldendict-ng) — MDICT 解析器来源
- [TauriCPP](https://gitee.com/masonwu21/tauri-cpp) (Lutra) — WebView2 桌面框架
- [WebView2](https://developer.microsoft.com/en-us/microsoft-edge/webview2/) — 微软 WebView2
