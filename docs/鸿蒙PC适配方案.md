# cube-shell 鸿蒙PC（HarmonyOS 5 PC）适配方案

> 目标：将 cube-shell C++/Qt6 版本移植到鸿蒙PC（MateBook Pro / Fold 等，HarmonyOS 5，
> 麒麟 X90 ARM64），产出可签名分发的 HAP 应用。
>
> 本文档是**可落地实施方案**，含功能取舍、技术障碍对策、分阶段步骤与 CMake 改造设计。

---

## 0. 一页结论

- 技术是**可行的**：Qt 官方自 6.5 起支持 OpenHarmony（技术预览），6.8+ 趋于可用；
  社区已有大量 "Qt 应用适配鸿蒙PC" 的验证先例。本项目是纯 Qt6 Widgets + C++17 + CMake，
  属于适配友好型架构。
- 鸿蒙PC 形态对我们是**利好**：桌面窗口/键鼠/菜单正是 Qt Widgets 主场，UI 几乎不用重做。
- 但**鸿蒙PC 仍是手机同款沙箱应用模型**：无本地 shell、禁止 exec 子进程、文件访问受限。
  因此功能上必须是**子集**：`SSH/SFTP 远程终端 + 设备管理 + AI 助手` 为第一期主线，
  串口/RDP 为第二期，本地终端/frp/本地 CLI 类工具永久砍掉。
- 最大工程风险：**OpenSSL/libssh2 等 C 依赖用 OHOS NDK 交叉编译**（中等难度、有先例、可控）。

---

## 1. 平台事实（鸿蒙PC 适配前必须建立的认知）

| 事实 | 含义 |
|---|---|
| 芯片为麒麟 X90（ARM64） | 交叉编译目标 `aarch64-linux-ohos`，所有 C/C++ 依赖须用 OHOS NDK clang 重编 |
| 应用模型 = HAP + 沙箱（与手机相同） | 第三方应用**不能 exec 子进程**（QProcess 不可用）、**无本地 shell** |
| 文件系统沙箱 | 不能全盘整盘浏览；用系统 FilePicker 或申请受限权限 |
| 设备类型为 "2in1"（PC/平板二合一） | DevEco Studio 模拟器选 2in1 镜像；module.json5 的 deviceTypes 需包含 |
| 密钥存储为 HUKS | macOS Keychain / Win DPAPI 均无对应物，Secrets.cpp 需新增分支 |
| Qt 模块子集 | qtbase（Core/Gui/Widgets/Network/Concurrent）可用；**无 QtSerialPort**；Multimedia 不完整 |
| 分发给 AppGallery（PC 端） | 需要华为开发者账号 + 签名证书 |

---

## 2. 现有代码依赖盘点（按适配难度分类）

### ✅ 第一类：直接可用（核心价值的全部）

- **UI 层**（`src/ui/`）：纯 Qt6 Widgets（QMainWindow/QDialog/QMenu/QTabWidget）
- **终端渲染**（`src/qtermwidget/`）：Vt102 模拟 + CPU 光栅绘制，不依赖 GPU/PTY
- **AI / Hermes / Claude 面板**：QNetworkAccessManager + SSE + QtConcurrent（纯 HTTP）
- **设备配置 / 分组 / 主题 / 国际化**：QSettings/JSON/QTranslator

### ⚠️ 第二类：需移植或替换（工作量集中区）

| 模块 | 现状 | 对策 |
|---|---|---|
| SSH/SFTP（`src/core/ssh/`） | libssh2 + 原生 POSIX socket（`SshClient.cpp` 直接 getaddrinfo/socket/connect） | libssh2/openssl/zlib 用 OHOS NDK 交叉编译；鸿蒙 musl libc 提供完整 socket API，源码基本不动 |
| 密钥存储（`Secrets.cpp`） | macOS Keychain / Win DPAPI | 新增 `Q_OS_OHOS` 分支：一期用沙箱内加密文件，二期接 HUKS NDK |
| 语音输入（`VoiceInput.cpp`） | QAudioSource（Qt Multimedia） | 一期关闭（新增 CUBESHELL_WITH_VOICE=OFF）；二期改鸿蒙原生录音 + NAPI 桥接 |
| 本地文件浏览（`local_file_browser_widget.cpp`） | 全盘浏览 + `QProcess::startDetached(open/explorer/xdg-open)` | 改为系统 FilePicker 选文件 → SFTP 上传/下载 |
| 串口（`src/core/serial/`，二期） | QSerialPort | 自研 `OhosSerialBackend`：USB Host NDK + CH340/CP210x/FTDI 用户态驱动（参照 usb-serial-for-android 的 C 实现）；SerialBridge 只认字节流，后端可替换 |
| RDP（二期） | FreeRDP 库后端 + 自绘帧 | 交叉编译 FreeRDP/WinPR 到 OHOS（有 Android 移植先例 aFreeRDP）；RdpClient 架构不依赖 X11，移植面小 |

### ❌ 第三类：永久砍掉（沙箱物理限制，与形态无关）

- **本地终端**（KPty/Pty/ConPtyProcess）：无 forkpty/posix_openpt 可用。鸿蒙上 SSH 终端
  复用 `SerialBridge` 已验证的模式：`Session::runEmptyPTY()` + 手动接管 emulation 字节流，
  **终端渲染能力 100% 保留**
- **FrpManager / FrpInstaller**（QProcess 跑 frpc/frps 二进制）
- **ClaudeCodeBackend / HermesBackend 的本地 CLI 驱动**（QProcess 跑 claude CLI）；
  AI 对话面板本身（HTTP）不受影响
- **DockerManager 的本地 docker CLI 调用**；若是走 SSH 管理远程 Docker 的部分则保留
- **UrlSchemeRegistrar / WindowsIntegration / FinderIntegration / UpdateDownloader 的安装器调起**

---

## 3. 分阶段实施步骤

### 阶段 0：最小可行性验证（1-3 天，最先做）

目标：**在鸿蒙PC 真机/模拟器上跑通一个 Qt Widgets 窗口 + 一条 SSH 连接**，验证整条技术链成色，
再决定是否全面铺开。

1. 装通环境（见阶段 1），先跑通 Qt 官方 widgets 示例的 HAP
2. 用 OHOS NDK 编一个最小 libssh2 demo：连一台服务器、执行 `uname -a`、打印结果
3. 两者合一：一个 QPushButton 触发 SSH 连接，结果显示在 QLabel 上 → 出 HAP 真机验证

**只有阶段 0 通过，才进入后续阶段。**

### 阶段 1：交叉编译环境（2-4 天）

1. DevEco Studio 内安装 OpenHarmony SDK + Native NDK（API 12+），记下
   `OHOS_SDK_HOME`（如 `~/Library/OpenHarmony/Sdk`）
2. 获取 Qt for OpenHarmony 6.8+：
   - 路线 A（推荐）：找官方/商业预编译包
   - 路线 B：自行交叉编译 qtbase（只需 Core/Gui/Widgets/Network/Concurrent），
     关闭 serialport/multimedia 等无用模块
3. 编写 CMake toolchain 文件（见 §4.2 模板）并用最小 CMake 工程验证

### 阶段 2：C 依赖库交叉编译（2-4 天）

按依赖序编译，全部 `-fPIC`、静态库、目标 `aarch64-linux-ohos`：

```
zlib → OpenSSL（no-asm 或适配 ARM64 asm）→ libssh2 → yaml-cpp
```

两种组织方式任选：
- **方式 A（简单可控）**：写 `cmake/ohos-deps.sh` 逐个 configure/make install 到
  `third_party/ohos/aarch64/`，CMake 里用 `find_library` + HINTS 指过去
  （项目现有 libssh2 查找逻辑已支持 `LIBSSH2_LIBRARY/LIBSSH2_INCLUDE_DIR` 直传，改动最小）
- **方式 B（与现有 Windows 流程对齐）**：为 vcpkg 新增 `community/aarch64-linux-ohos.cmake`
  triplet + overlay

### 阶段 3：代码裁剪与条件编译（1-2 周，核心工作量）

1. 顶层 CMakeLists 新增平台与功能开关：

```cmake
option(CUBESHELL_PLATFORM_OHOS  "Build for HarmonyOS PC (OpenHarmony)" OFF)
option(CUBESHELL_WITH_LOCALPTY  "Local shell terminal (forkpty/ConPty)" ON)   # OHOS 下强制 OFF
option(CUBESHELL_WITH_VOICE     "Voice input (Qt Multimedia)"           ON)   # OHOS 下强制 OFF
option(CUBESHELL_WITH_LOCALPROC "Local subprocess tools (frp/docker/cli)" ON) # OHOS 下强制 OFF

if(CUBESHELL_PLATFORM_OHOS)
    set(CUBESHELL_WITH_SERIAL   OFF)
    set(CUBESHELL_WITH_RDP      OFF)
    set(CUBESHELL_WITH_LOCALPTY OFF)
    set(CUBESHELL_WITH_VOICE    OFF)
    set(CUBESHELL_WITH_LOCALPROC OFF)
    add_compile_definitions(CUBESHELL_PLATFORM_OHOS=1)
endif()
```

2. 用 `Q_OS_OHOS`（Qt on OHOS 定义；编译器层面是 `__OHOS__`）在源码中摘除/替换：

| 位置 | 改造 |
|---|---|
| `src/app/main.cpp` | 摘 QFileOpenEvent/URL Scheme 过滤器、QLockFile 单实例；保 QApplication 主流程 |
| `src/qtermwidget/` | KPty/Pty/ConPtyProcess 本地后端整体不编译（新增 CMake 条件），Session 保留 runEmptyPTY 路径 |
| `src/core/ssh/SshBridge` 接线处 | SSH 终端按 SerialBridge 模式接：空 PTY + emulation.sendData ↔ SSH channel |
| `src/core/config/Secrets.cpp` | 新增 OHOS 分支（一期：沙箱加密文件） |
| `src/ui/local_file_browser_widget.cpp` | QProcess::startDetached 分支替换为 FilePicker/禁用 |
| `src/core/forwarder/*`、`docker/*`、`claude_code/ClaudeCodeBackend`、`hermes/HermesBackend` | CUBESHELL_WITH_LOCALPROC 宏摘除，UI 入口同步隐藏 |
| `src/core/update/Update*` | 鸿蒙版走 AppGallery 更新，摘安装器调起 |
| `src/core/platform/*Integration*` | 已有 stub 模式，补 OHOS 分支即可 |

3. UI 菜单按宏隐藏已砍功能的入口（现有 CUBESHELL_WITH_SERIAL 宏摘除串口入口的模式照搬）

### 阶段 4：打包、签名、上架（3-5 天）

1. DevEco Studio 建 **Native C++ (ArkTS + napi)** 壳工程，deviceTypes 含 `2in1`
2. Qt 编译产物（cube-shell + Qt 动态库 + 资源）作为 Native 部分嵌入；
   Qt for OHOS 已封装事件循环与 AbilityStage 生命周期对接
3. `module.json5` 申请权限：`ohos.permission.INTERNET`（必需）、
   `ohos.permission.GET_NETWORK_INFO`；文件访问按需
4. 资源部署：color-schemes / kb-layouts / i18n .qm 打入 HAP 资源目录，
   运行期探测路径补 OHOS 分支
5. 签名（调试证书 → 发布证书）→ 出 HAP → 真机调测 → AppGallery PC 端上架

> **进度（2026-08-06）**：未签名 HAP 已可一键盘出并**在模拟器跑起来**——
> `./scripts/build-ohos-app.sh --clean --hap`，产物
> `build-ohos/hap/entry/build/default/outputs/default/entry-default-unsigned.hap`（约 117 MB，
> 内含 `libcube-shell.so` + Qt6 Core/Gui/Widgets/Network + 第三方依赖库）。
> 已验证在 Huawei 2in1 模拟器（API 24）上 `hdc install` + `aa start` 后进入前台、UI 响应输入。
> 签名/上架仍待做。
>
> **运行到模拟器/设备**：
> ```bash
> HDC=~/Library/OpenHarmony/Sdk/23/toolchains/hdc
> $HDC install -r build-ohos/hap/entry/build/default/outputs/default/entry-default-unsigned.hap
> $HDC shell aa start -b org.qtproject.example.cube_shell -a QAbility   # 启动
> $HDC shell aa force-stop org.qtproject.example.cube_shell             # 停止
> ```
>
> **关键踩坑 ①（hvigor `00303168 SDK component missing`）**：hvigorw 只认
> 「完整 HarmonyOS SDK」布局——`<root>/<stage>/{openharmony,hms}/<component>` 且组件根带
> `sdk-pkg.json`。单独下载的 API 23 SDK（`~/Library/OpenHarmony/Sdk/23`）只有
> `oh-uni-package.json`、无此布局，hvigor 扫不到。解决：把 `DEVECO_SDK_HOME` 指向
> **DevEco Studio 自带的完整 SDK** `/Applications/DevEco-Studio.app/Contents/sdk`
> （API 26，含资源编译依赖 `toolchains/lib/libimage_transcoder_shared.dylib`）。
> 生成的 `build-profile.json5` 无需手改（`compatibleSdkVersion 6.1.0(23)` 保留，
> `compileSdkVersion` 留空由 hvigor 取 SDK 的 26.0.0）。脚本已内置该默认值。
>
> **关键踩坑 ②（启动即崩 jscrash `handleAbilityStageOnCreate of undefined`）**：
> Qt for OHOS 的预编译 `libQt6Gui/Network/Core.so` 动态依赖一批第三方库，但 Qt 安装包
> 与鸿蒙系统都不附带，导致 `libqohos.so` dlopen 失败、`import qpa` 得到 undefined：
>   - Gui → `libfontconfig` `libfreetype` `libpng16`；Network → `libbrotlidec/common`；
>     Core → `libicui18n/uc/data`（ICU **78**，符号后缀 `_78`，版本必须匹配）
>   - 由 `scripts/build-ohos-qt-deps.sh` 用 OHOS NDK 交叉编译，并以
>     「裸名 + SONAME 名」两套文件名拷进 `entry/libs/arm64-v8a/`（Qt 按裸名 dlopen，
>     这批库彼此按 SONAME 名引用，OHOS 按文件名加载，两种都得在）。
>   - `build-ohos-app.sh --hap` 已自动调用该脚本注入并重打 HAP。
>
> **关键踩坑 ③（`--clean` 后 harmonydeployqt 报 `uv_cwd ENOENT`）**：hvigor 缓存了
> 被删的 `build-ohos/hap` 旧路径。脚本在 `--clean --hap` 时会一并 `--stop-daemon` +
> 清 `~/.hvigor`。

---

## 4. 关键工程模板

### 4.1 构建命令（目标形态）

```bash
cmake -B build-ohos \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-ohos.cmake \
  -DCUBESHELL_PLATFORM_OHOS=ON \
  -DLIBSSH2_INCLUDE_DIR=$PWD/third_party/ohos/aarch64/include \
  -DLIBSSH2_LIBRARY=$PWD/third_party/ohos/aarch64/lib/libssh2.a \
  -DOPENSSL_ROOT_DIR=$PWD/third_party/ohos/aarch64 \
  -DCMAKE_PREFIX_PATH=<Qt-for-OHOS 路径> \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-ohos -j
```

### 4.2 toolchain 模板（`cmake/toolchains/aarch64-linux-ohos.cmake`）

```cmake
set(CMAKE_SYSTEM_NAME OHOS)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# DevEco Studio 安装的 OpenHarmony SDK native 目录
if(NOT DEFINED OHOS_SDK_HOME)
    set(OHOS_SDK_HOME "$ENV{OHOS_SDK_HOME}")
endif()
set(OHOS_NDK "${OHOS_SDK_HOME}/native")
set(OHOS_LLVM "${OHOS_NDK}/llvm")

set(CMAKE_C_COMPILER   "${OHOS_LLVM}/bin/clang")
set(CMAKE_CXX_COMPILER "${OHOS_LLVM}/bin/clang++")

# OHOS NDK 用 --target + --sysroot 指定三元组（版本号随 SDK 变）
set(OHOS_TARGET "aarch64-linux-ohos")
set(OHOS_SYSROOT "${OHOS_NDK}/sysroot")
set(CMAKE_C_FLAGS   "--target=${OHOS_TARGET} --sysroot=${OHOS_SYSROOT}")
set(CMAKE_CXX_FLAGS "--target=${OHOS_TARGET} --sysroot=${OHOS_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH ${OHOS_SYSROOT} ${CMAKE_PREFIX_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

> 注：具体 target 三元组与目录结构以本机 DevEco SDK 实际版本为准（API 12/14/15 有差异），
> 阶段 1 落地时先 `ls $OHOS_SDK_HOME/native` 校准。

---

## 5. 里程碑与风险

| 里程碑 | 内容 | 预估 |
|---|---|---|
| M0 | 鸿蒙PC 上 Qt Widgets 窗口 + libssh2 连接跑通 | 1 周内 |
| M1 | SSH/SFTP 终端 + 设备管理 + AI 助手全功能 HAP | +2~3 周 |
| M2 | 签名、真机调测、AppGallery 上架 | +1 周 |
| M3（二期） | 串口（USB Host 自研后端） | +1~2 周 |
| M4（二期） | RDP（FreeRDP 交叉编译） | +1 周 |

| 风险 | 等级 | 缓解 |
|---|---|---|
| OpenSSL/libssh2 OHOS 交叉编译细节坑 | 中 | 阶段 0 先验证；OpenSSL 有 OHOS 适配先例 |
| Qt for OHOS 个别 Widgets 行为差异 | 中 | M0 用真实主窗口验证，而非 toy demo |
| 上架审核（权限、签名） | 低 | 提前注册开发者账号，按 AppGallery PC 清单准备 |
| 串口自研驱动兼容性 | 中（二期） | 优先支持 CH340/CP210x 两款最主流芯片 |

---

## 6. 参考

- Qt 官方：Qt for HarmonyOS development（wiki.qt.io，6.12 文档含 DevEco 集成步骤）
- Qt 官方：Building Qt for OpenHarmony（wiki.qt.io）
- 社区实战：CSDN/AtomGit「Qt 开源软件适配鸿蒙PC」系列复现指南
- 串口用户态驱动参照：usb-serial-for-android（CH340/CP210x/FTDI/PL2303 协议实现）
- FreeRDP 移动移植先例：aFreeRDP（Android）
