# GoldenDict-Lite v1.1.0

轻量级桌面词典，基于 [TauriCPP](https://gitee.com/masonwu21/tauri-cpp) 框架构建，支持 MDX/MDD 词典解析引擎，实现零外部依赖、单 EXE 部署的现代词典应用。

## 特性

- **单 EXE 部署** — WebView2Loader 静态链接，前端资源内嵌，无需安装运行时
- **MDX/MDD 词典支持** — 兼容 .mdx 词典格式，支持加密词典和 MDD 多媒体资源
- **多卷 MDD 支持** — 自动加载 .mdd、.1.mdd、.2.mdd 等多卷资源文件
- **MDD 音频播放** — 词典内 sound:// 链接自动替换为可点击播放的音频
- **多词典同时查询** — 递归扫描 dictionary/ 目录，一次查询显示所有词典结果
- **实时自动补全** — 输入即搜，候选词即时显示
- **词典折叠/展开** — 查询结果按词典分组，可折叠/展开每个词典的解释
- **词典排序** — 底部词典栏支持拖拽调整顺序，查询结果按词典顺序显示
- **排序持久化** — 词典顺序保存到 dict_order.json，重启后自动恢复
- **中文路径支持** — 完整支持中文目录和中文文件名的词典
- **GBK/UTF-8 编码兼容** — 自动检测并转换 GBK 编码的词典标题和内容
- **词典图标** — 自动加载同名 .png 作为词典标识
- **SQLite 缓存** — 词典索引缓存到本地数据库，二次启动秒加载
- **系统托盘** — 最小化到托盘，双击恢复
- **护眼米白金配色** — 温暖舒适的界面主题
- **纯 C++ 实现** — 无 Qt、无 Electron、无 Rust，纯 Win32 + WebView2
- **全静态链接** — zlib、WebView2Loader、CRT 均为静态链接，无外部 DLL

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
│         │         │  SQLite3      │               │
│         │         └──────┬───────┘               │
│         │                │                        │
│  ┌──────▼────────────────▼──────────────────────┐ │
│  │            DictionaryManager                  │ │
│  │  - 递归扫描 dictionary/ 目录                  │ │
│  │  - 词典加载/缓存 (SQLite)                     │ │
│  │  - 前缀匹配 (prefixMatch)                     │ │
│  │  - 词条查询 (lookup)                          │ │
│  │  - MDD 资源查找 (多卷)                        │ │
│  │  - 音频播放 (base64 编码)                     │ │
│  │  - 词典排序持久化                              │ │
│  │  - CSS 样式独立作用域                          │ │
│  │  - 图标加载 (base64)                          │ │
│  └──────────────────────────────────────────────┘ │
│                                                   │
│  ┌──────────────────────────────────────────────┐ │
│  │              Frontend (HTML/CSS/JS)           │ │
│  │  - 搜索栏 + 自动补全                          │ │
│  │  - 多词典卡片式结果展示 (可折叠)               │ │
│  │  - 底部词典栏 (图标 + 名称 + 词条数, 可拖拽)   │ │
│  │  - 音频播放 (data-sound 事件委托)              │ │
│  └──────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
```

### 与 goldendict-ng 的对比

| 特性 | GoldenDict-Lite | goldendict-ng |
|------|----------------|---------------|
| **UI 框架** | TauriCPP + WebView2 | Qt 6 + WebEngine |
| **词典格式** | MDX/MDD | MDX, DSL, BGL, StarDict, ZIM 等 15+ |
| **语言** | C++17 (纯标准库 + Win32) | C++17 (Qt 6 生态) |
| **依赖** | WebView2 (Win10/11 自带) | Qt 6, OpenSSL, XZ, FFmpeg 等 |
| **二进制大小** | ~500KB | ~50MB+ |
| **内存占用** | ~300MB (单词典) | ~200MB |
| **启动速度** | <1s | 2-5s |
| **前端保护** | 前端资源编译进 EXE，内存加载 | WebEngine 渲染 |
| **跨平台** | Windows | Windows, macOS, Linux |
| **安装** | 单 EXE，无需安装 | 需安装 Qt 运行时 |

### 技术决策

- **去除 Qt 依赖**: QFile → std::fopen/_wfopen, QDataStream → 手动字节序读取, QByteArray → std::vector, QString → std::string, QDomDocument → std::regex, QMutex → std::mutex
- **编码转换**: iconv → Windows API (MultiByteToWideChar/WideCharToMultiByte) + Iconv::ensureUtf8 (GBK→UTF-8 自动检测)
- **中文路径**: fopen → openFileWide (CP_ACP + _wfopen), fseek → _fseeki64
- **静态链接**: zlib static, WebView2LoaderStatic, 静态 CRT, 均通过 vcpkg x64-windows-static 安装
- **缓存层**: 自研 DictCache (SQLite3)，缓存 headwords、record blocks、style sheets、元数据

## 目录结构

```
GoldenDict-Lite/
├── CMakeLists.txt              # 顶层构建配置
├── build.ps1                   # 构建脚本
├── src/
│   ├── main.cpp                # 应用入口 + Bridge 命令注册
│   ├── dictionary_manager.*    # 词典管理器 (加载/查询/排序/资源)
│   ├── dict_cache.*            # SQLite 缓存层
│   ├── goldendict/             # 从 goldendict-ng 提取的解析器 (无 Qt)
│   │   ├── mdictparser.*       # MDICT 格式核心解析
│   │   ├── iconv.*             # 编码转换 (Win API + ensureUtf8)
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
│   └── js/app.js               # 搜索/词典栏/音频/折叠/拖拽
├── tests/
│   └── test_bugfixes.cpp       # 单元测试 (38 tests)
├── third_party/
│   └── sqlite3/                # SQLite3 amalgamation
├── dictionary/                 # 词典目录 (放入 .mdx + .mdd + .css + .png)
├── TauriCPP/                   # TauriCPP 框架
└── goldendict-ng-26.6.0/       # 原版源码 (仅用于参考)
```

## 编译方法

### 前置条件

- Windows 10/11
- Visual Studio 2022 (C++ Desktop Development workload)
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

# 4. 运行单元测试
.\build\Release\test_bugfixes.exe
```

### 运行

```powershell
# 确保 Release 目录下有 icon.ico
copy src\icon.ico build\Release\
# 运行
.\build\Release\GoldenDictLite.exe
```

## 使用方法

1. 将 `.mdx` 词典文件放入 `dictionary/` 目录（支持子目录递归扫描）
2. 词典可配套同名资源文件：
   ```
   dictionary/
   ├── mydict/
   │   ├── mydict.mdx          # 词典数据
   │   ├── mydict.mdd          # 多媒体资源 (可选)
   │   ├── mydict.1.mdd        # 多卷 MDD (可选)
   │   ├── mydict.css          # 词典样式 (可选)
   │   └── mydict.png          # 词典图标 (可选)
   ```
3. 启动 GoldenDict-Lite，程序自动扫描并加载所有词典
4. 首次加载会建立 SQLite 缓存，后续启动秒加载
5. 在搜索框输入词汇，实时查看所有词典的释义
6. 支持候选词自动补全，回车或点击直接查询
7. 查询结果按词典分组，点击标题可折叠/展开
8. 底部词典栏可拖拽调整顺序，查询结果按此顺序排列
9. 词典内发音链接可直接点击播放音频
10. 点击最小化按钮可最小化到系统托盘，双击托盘图标恢复

## v1.1.0 更新内容

- 新增 MDD 多媒体资源支持（音频、图片等）
- 新增多卷 MDD 文件加载（.1.mdd, .2.mdd, ...）
- 新增 sound:// 链接自动替换为可点击播放的音频
- 新增词典折叠/展开功能
- 新增底部词典栏拖拽排序
- 新增词典排序持久化（dict_order.json）
- 新增 SQLite 缓存层，二次启动秒加载
- 新增 38 个单元测试
- 修复中文路径/中文文件名词典无法加载的问题
- 修复 GBK 编码词典标题乱码问题
- 修复查询含 GBK 编码内容时 JSON 序列化崩溃的问题
- 修复 ::tolower/toupper 对 signed char 的未定义行为
- 修复 std::stoi 在 substituteStylesheet 中的异常崩溃
- 修复 fseek 32 位偏移量限制（改用 _fseeki64）
- 修复 nlohmann::json::dump() 对非 UTF-8 字符串的 type_error.316 崩溃
- 全静态链接：zlib、WebView2Loader、CRT 均为静态链接，无外部 DLL 依赖

## 依赖库

所有依赖均静态链接，无运行时 DLL：

| 库 | 用途 | 版本 |
|----|------|------|
| WebView2Loader | WebView2 静态加载器 | 1.0.3240 |
| zlib | MDX 压缩/解压 | 1.3.1 |
| SQLite3 | 词典索引缓存 | 3.45.0 |
| nlohmann/json | JSON 通信 | 3.11.3 |
| Windows API | 编码转换、文件操作 | Win10+ |

## 许可证

MIT

## 致谢

- [goldendict-ng](https://github.com/xiaoyifang/goldendict-ng) — MDICT 解析器来源
- [TauriCPP](https://gitee.com/masonwu21/tauri-cpp) — WebView2 桌面框架
- [WebView2](https://developer.microsoft.com/en-us/microsoft-edge/webview2/) — 微软 WebView2
