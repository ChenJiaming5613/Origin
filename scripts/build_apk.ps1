<#
.SYNOPSIS
    一键构建 Origin 的 Android APK，不需要打开 Android Studio。

.DESCRIPTION
    把 Android Studio 在背后替你做的环境准备全部显式化：
      1. 定位 JDK        —— JAVA_HOME 未设时用 Android Studio 自带的 JBR
      2. 定位 Android SDK —— ANDROID_HOME 未设时从 local.properties / 默认路径推断
      3. 生成 local.properties（它是 gitignore 的，新机器 clone 下来没有）
      4. 检查 submodule 是否就位（third_party 全空时 CMake 的报错很难看懂）
      5. 检查 Vulkan SDK  —— shader 构建期用 dxc 编译，宿主机必须有
      6. 调 gradlew 出包，并从**正确的**目录取产物
      7. 可选：校验 16KB 对齐 / 安装 / 启动 / 抓 logcat

    兼容 Windows PowerShell 5.1（不使用 7+ 才有的 && || ?: ?? 语法）。

.PARAMETER Config
    Debug（默认）或 Release。
    Release 在 app/build.gradle 里没有配 signingConfig，AGP 产出的是
    app-release-unsigned.apk 无法安装；脚本会自动用 debug keystore 补签
    （仅供本机测试，不能分发）。

.PARAMETER Clean
    构建前跑一次 gradle clean。
    慎用：native 会连 SDL3 一起全量重编，单 ABI 约 5~10 分钟。

.PARAMETER Install
    构建成功后用 adb 安装到已连接设备（-r 覆盖安装）。

.PARAMETER Run
    安装后立刻启动 App（隐含 -Install）。

.PARAMETER Logcat
    启动后跟随打印本 App 日志（隐含 -Run）。按 Ctrl+C 退出。

.PARAMETER Verify
    额外校验产物：APK 内 .so 的 zip 对齐 + ELF LOAD 段 16KB 对齐。

.PARAMETER GradleArgs
    透传给 gradlew 的额外参数，例如 --info、--offline。

.PARAMETER LogFile
    把完整输出（含 gradle 原始输出）另存一份，便于 CI 归档或事后排查。
    控制台照常打印。

.EXAMPLE
    .\scripts\build_apk.ps1
    出一个 debug APK。

.EXAMPLE
    .\scripts\build_apk.ps1 -Run -Logcat
    出包、装到手机、启动、跟随日志。

.EXAMPLE
    .\scripts\build_apk.ps1 -Config Release -Verify
    出 release 包（自动补签）并校验 16KB 对齐。
#>

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Debug',

    [switch] $Clean,
    [switch] $Install,
    [switch] $Run,
    [switch] $Logcat,
    [switch] $Verify,

    [string[]] $GradleArgs = @(),
    [string]   $LogFile = ''
)

# 刻意用 Continue 而不是 Stop。
# 这个脚本主要在调外部命令行工具，而 java / gradle / adb 都会把**正常信息**
# 写到 stderr（例如 `java -version` 的版本号就在 stderr）。在 Stop 之下，
# 这些行会被 PowerShell 包成 ErrorRecord 当成终止性错误，脚本会在
# "打印一下 JDK 版本" 这种无害的地方直接崩掉。
# 因此这里统一靠显式检查 $LASTEXITCODE / Test-Path 来判断成败。
$ErrorActionPreference = 'Continue'

# -----------------------------------------------------------------------------
# 控制台输出编码
# -----------------------------------------------------------------------------
# 中文 Windows 的控制台默认代码页是 936(GBK)。交互运行时看不出问题，
# 但一旦把 stdout 重定向或走管道（CI 里收集日志、`... | Tee-Object`、
# 从别的工具调用本脚本），非 ASCII 字符就会按 GBK 编码写出去，
# 再被按 UTF-8 读回来即变成「¶¨Î» JDK」这类乱码。
#
# 注意只改本进程的 Console.OutputEncoding，不动系统代码页（不用 chcp），
# 所以不会影响用户的其他终端会话。
# 用 UTF8Encoding($false) 而非 [Text.Encoding]::UTF8 —— 后者会在**每次**
# 写入时带 BOM，输出里会散落 \ufeff。
try {
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
} catch {
    # 无控制台宿主（如某些 CI runner）时会抛，忽略即可，不影响构建
}

# -----------------------------------------------------------------------------
# 修正 PATHEXT
# -----------------------------------------------------------------------------

# 某些机器上 PATHEXT 被改坏（本工程开发机实测只剩 ".CPL"）。后果很隐蔽：
# PowerShell 判断文件是否"可执行"依赖 PATHEXT，缺了 .EXE 之后它把 java.exe
# 当成**文档**，于是 `& $exe ... | ...` 会抛
#   "无法在管道中间运行文档" (CantActivateDocumentInPipeline)
# 而不带管道的 `& $exe` 却能跑，极易误判成别的问题。
# 缺 .BAT/.CMD 还会影响 gradlew.bat。
# 只改本进程（及子进程）的环境变量，不动注册表、不影响系统。
$RequiredExt = @('.COM', '.EXE', '.BAT', '.CMD')
$CurrentExt  = @()
if (-not [string]::IsNullOrWhiteSpace($env:PATHEXT)) {
    $CurrentExt = @($env:PATHEXT.Split(';') | Where-Object { $_ -ne '' })
}
$MissingExt = @($RequiredExt | Where-Object { $CurrentExt -notcontains $_ })
$script:PathExtWasFixed = ($MissingExt.Count -gt 0)
if ($script:PathExtWasFixed) {
    $env:PATHEXT = (@($RequiredExt + $CurrentExt) | Select-Object -Unique) -join ';'
}

# -----------------------------------------------------------------------------
# 日志
# -----------------------------------------------------------------------------

# 用 Start-Transcript 而不是给每个 Write-* 手动 tee：
# 前者能把 gradle 这类**外部进程**的原始输出一起抓到，后者抓不到。
$script:Transcribing = $false
if (-not [string]::IsNullOrWhiteSpace($LogFile)) {
    try {
        Start-Transcript -Path $LogFile -Force | Out-Null
        $script:Transcribing = $true
    } catch {
        Write-Host ("无法写日志文件 " + $LogFile + "：" + $_.Exception.Message) -ForegroundColor Yellow
    }
}

function Stop-Log {
    if ($script:Transcribing) {
        try { Stop-Transcript | Out-Null } catch { }
        $script:Transcribing = $false
    }
}

# -----------------------------------------------------------------------------
# 输出辅助
# -----------------------------------------------------------------------------

$script:StepIndex = 0

function Write-Step([string] $Message) {
    $script:StepIndex++
    Write-Host ''
    Write-Host ("[{0}] {1}" -f $script:StepIndex, $Message) -ForegroundColor Cyan
}

function Write-Ok([string] $Message) {
    Write-Host ("    OK   " + $Message) -ForegroundColor Green
}

function Write-Info([string] $Message) {
    Write-Host ("         " + $Message) -ForegroundColor DarkGray
}

function Write-Warn2([string] $Message) {
    Write-Host ("    WARN " + $Message) -ForegroundColor Yellow
}

function Fail([string] $Message) {
    Write-Host ''
    Write-Host ("失败: " + $Message) -ForegroundColor Red
    Stop-Log
    exit 1
}

# 把 "C:\Users\x" 统一成正斜杠，写进 local.properties / 打印都更清爽
function Normalize-Path([string] $Path) {
    return $Path.Replace('\', '/')
}

# 调外部程序并捕获全部输出（stdout + stderr）为字符串数组。
#
# 两条约束决定了必须封装成函数：
#   1. 绝不能写成 `& $exe ... | Select-...` —— PATHEXT 异常时会报
#      "无法在管道中间运行文档"。先赋值给变量、再对变量做管道就没问题。
#   2. 返回值统一转成 string[]，调用方可以放心用 -match 过滤，
#      不必关心 stderr 行其实是 ErrorRecord 对象。
# 退出码放在 $script:LastNativeExit 里，供调用方判断成败。
function Invoke-Native([string] $Exe, [string[]] $Arguments = @()) {
    $raw = & $Exe @Arguments 2>&1
    $script:LastNativeExit = $LASTEXITCODE
    if ($null -eq $raw) { return @() }
    return @($raw | ForEach-Object { $_.ToString() })
}

# -----------------------------------------------------------------------------
# 路径与开关归一化
# -----------------------------------------------------------------------------

# $PSScriptRoot 是 scripts/，工程根在它上一级
$RepoRoot    = Split-Path -Parent $PSScriptRoot
$AndroidRoot = Join-Path $RepoRoot 'android'

if ($Logcat) { $Run = $true }
if ($Run)    { $Install = $true }

$ConfigLower = $Config.ToLower()

Write-Host ''
Write-Host '=======================================================' -ForegroundColor White
Write-Host (" Origin Android 构建  |  配置: {0}" -f $Config) -ForegroundColor White
Write-Host '=======================================================' -ForegroundColor White
Write-Info ("工程根: " + (Normalize-Path $RepoRoot))
if ($script:PathExtWasFixed) {
    Write-Info ("已临时补全 PATHEXT（原值缺少 " + ($MissingExt -join ',') + "）")
}

# -----------------------------------------------------------------------------
# 1. 定位 JDK
# -----------------------------------------------------------------------------

Write-Step '定位 JDK'

function Test-JdkDir([string] $Dir) {
    if ([string]::IsNullOrWhiteSpace($Dir)) { return $false }
    return (Test-Path (Join-Path $Dir 'bin\java.exe'))
}

$JdkHome = $null

# 顺序有讲究：先尊重用户已设的 JAVA_HOME，再退到 Android Studio 自带的 JBR。
# JBR 是 Android Studio 用来跑 Gradle 的那一个，版本必然与 AGP 匹配，
# 比系统里随便一个 JDK 更安全。
$JdkCandidates = @()
if (-not [string]::IsNullOrWhiteSpace($env:JAVA_HOME)) {
    $JdkCandidates += $env:JAVA_HOME
}
$JdkCandidates += @(
    'C:\Program Files\Android\Android Studio\jbr'
    'C:\Program Files\Android\Android Studio Preview\jbr'
    (Join-Path $env:LOCALAPPDATA 'Programs\Android Studio\jbr')
)
# 兜底：扫常见 JDK 安装位置，取版本号最大的
foreach ($root in @('C:\Program Files\Java', 'C:\Program Files\Eclipse Adoptium')) {
    if (Test-Path $root) {
        $dirs = @(Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
                  Sort-Object Name -Descending)
        foreach ($d in $dirs) { $JdkCandidates += $d.FullName }
    }
}

foreach ($c in $JdkCandidates) {
    if (Test-JdkDir $c) { $JdkHome = $c; break }
}

if ($null -eq $JdkHome) {
    Fail @"
找不到 JDK。gradlew 必须有 JAVA_HOME 才能启动。
解决办法（任选其一）：
  - 安装 Android Studio（自带 JBR，脚本会自动找到）
  - 装一个 JDK 17 或 21，然后设 JAVA_HOME
  - 在当前终端临时指定后再跑本脚本:
      `$env:JAVA_HOME = 'C:/path/to/jdk'
"@
}

$env:JAVA_HOME = $JdkHome
$JavaExe = Join-Path $JdkHome 'bin\java.exe'
$JavaOut = Invoke-Native $JavaExe @('-version')
Write-Ok ("JDK: " + (Normalize-Path $JdkHome))
if ($JavaOut.Count -gt 0) { Write-Info $JavaOut[0] }

# -----------------------------------------------------------------------------
# 2. 定位 Android SDK
# -----------------------------------------------------------------------------

Write-Step '定位 Android SDK'

function Test-SdkDir([string] $Dir) {
    if ([string]::IsNullOrWhiteSpace($Dir)) { return $false }
    return (Test-Path (Join-Path $Dir 'platform-tools'))
}

$SdkRoot        = $null
$LocalPropsPath = Join-Path $AndroidRoot 'local.properties'

$SdkCandidates = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT)

# local.properties 里的 sdk.dir 是 Java properties 格式，冒号和反斜杠都转义过
# （形如 C\:\\Users\\me\\AppData\\Local\\Android\\Sdk），必须反转义才能用。
if (Test-Path $LocalPropsPath) {
    $line = Select-String -Path $LocalPropsPath -Pattern '^\s*sdk\.dir\s*=' -ErrorAction SilentlyContinue |
            Select-Object -First 1
    if ($null -ne $line) {
        $raw = ($line.Line -split '=', 2)[1].Trim()
        $SdkCandidates += $raw.Replace('\:', ':').Replace('\\', '\')
    }
}

$SdkCandidates += (Join-Path $env:LOCALAPPDATA 'Android\Sdk')

foreach ($c in $SdkCandidates) {
    if (Test-SdkDir $c) { $SdkRoot = $c; break }
}

if ($null -eq $SdkRoot) {
    Fail @"
找不到 Android SDK。
解决办法：设 ANDROID_HOME 指向 SDK 根目录，或在
  android/local.properties
里写一行 sdk.dir=<路径>（Java properties 格式，反斜杠要转义）。
"@
}

$env:ANDROID_HOME     = $SdkRoot
$env:ANDROID_SDK_ROOT = $SdkRoot
Write-Ok ("SDK: " + (Normalize-Path $SdkRoot))

# local.properties 是 gitignore 的，新机器 clone 下来不会有 ——
# 没有它 Gradle 直接报 "SDK location not found"，所以这里补写一份。
if (-not (Test-Path $LocalPropsPath)) {
    $escaped = $SdkRoot.Replace('\', '\\').Replace(':', '\:')
    @(
        '# 由 scripts/build_apk.ps1 自动生成。此文件不纳入版本管理。'
        ("sdk.dir=" + $escaped)
    ) | Set-Content -Path $LocalPropsPath -Encoding ASCII
    Write-Ok '已生成 android/local.properties'
}

# 常用工具目录，后面多处要用
$BuildToolsDir = $null
$btCandidates = @(Get-ChildItem -Path (Join-Path $SdkRoot 'build-tools') -Directory -ErrorAction SilentlyContinue |
                  Sort-Object Name -Descending)
if ($btCandidates.Count -gt 0) { $BuildToolsDir = $btCandidates[0].FullName }

$AdbExe = Join-Path $SdkRoot 'platform-tools\adb.exe'

# -----------------------------------------------------------------------------
# 3. 检查 submodule
# -----------------------------------------------------------------------------

Write-Step '检查 submodule'

# 每个库挑一个哨兵文件。只看目录存在没用 —— submodule 未 init 时目录是
# 存在但为空的，那种情况下 CMake 会在 add_subdirectory 处报错，
# 错误信息跟"没拉 submodule"毫无关联，很难一眼看出来。
$Sentinels = [ordered]@{
    'SDL'            = 'CMakeLists.txt'
    'volk'           = 'CMakeLists.txt'
    'Vulkan-Headers' = 'CMakeLists.txt'
    'spdlog'         = 'CMakeLists.txt'
    'SPIRV-Reflect'  = 'spirv_reflect.c'
    'stb'            = 'stb_image.h'
    'glm'            = 'glm/glm.hpp'
    'imgui'          = 'imgui.cpp'
    'tinygltf'       = 'tiny_gltf_v3.c'
}

$MissingSubs = @()
foreach ($name in $Sentinels.Keys) {
    $probe = Join-Path $RepoRoot ('third_party/' + $name + '/' + $Sentinels[$name])
    if (-not (Test-Path $probe)) { $MissingSubs += $name }
}

if ($MissingSubs.Count -gt 0) {
    Fail @"
以下 submodule 未就位: $($MissingSubs -join ', ')
请在工程根目录执行:
  git submodule update --init --recursive
"@
}
Write-Ok ("{0} 个 submodule 全部就位" -f $Sentinels.Count)

# -----------------------------------------------------------------------------
# 4. 检查 shader 编译器
# -----------------------------------------------------------------------------

Write-Step '检查 shader 编译器 (dxc)'

# shader 是**构建期**在宿主机上编的，产物 .spv 打进 APK，设备上不需要
# 任何编译器。但宿主机必须有 dxc —— NDK 的 shader-tools 里只有 glslc，
# 没有 dxc，所以必须装 Vulkan SDK。
$DxcPath  = $null
$DxcHints = @()
if (-not [string]::IsNullOrWhiteSpace($env:VULKAN_SDK)) {
    $DxcHints += (Join-Path $env:VULKAN_SDK 'Bin\dxc.exe')
}
if (Test-Path 'C:\VulkanSDK') {
    $sdks = @(Get-ChildItem -Path 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue |
              Sort-Object Name -Descending)
    foreach ($s in $sdks) { $DxcHints += (Join-Path $s.FullName 'Bin\dxc.exe') }
}
foreach ($h in $DxcHints) {
    if (Test-Path $h) { $DxcPath = $h; break }
}

if ($null -eq $DxcPath) {
    Fail @"
找不到 dxc.exe（HLSL -> SPIR-V 编译器）。
shader 在构建期编译，所以宿主机必须装 Vulkan SDK：
  https://vulkan.lunarg.com/sdk/home#windows
注意 Android NDK 的 shader-tools 里只有 glslc，没有 dxc。
"@
}
Write-Ok ("dxc: " + (Normalize-Path $DxcPath))

# -----------------------------------------------------------------------------
# 5. 调 Gradle 出包
# -----------------------------------------------------------------------------

Write-Step ("Gradle 构建 (assemble{0})" -f $Config)

$GradlewBat = Join-Path $AndroidRoot 'gradlew.bat'
if (-not (Test-Path $GradlewBat)) {
    Fail ("找不到 gradle wrapper: " + (Normalize-Path $GradlewBat))
}

Push-Location $AndroidRoot
try {
    if ($Clean) {
        Write-Info 'gradle clean（native 会全量重编，SDL3 较慢）'
        & $GradlewBat 'clean' @GradleArgs
        if ($LASTEXITCODE -ne 0) { Fail 'gradle clean 失败' }
    }

    $task = 'assemble' + $Config
    $sw   = [System.Diagnostics.Stopwatch]::StartNew()

    # 刻意不捕获输出：gradle 的进度和错误要实时可见。
    & $GradlewBat $task @GradleArgs
    $gradleExit = $LASTEXITCODE

    $sw.Stop()

    if ($gradleExit -ne 0) {
        Fail ("gradle {0} 失败（退出码 {1}）。上面的输出里有具体原因。" -f $task, $gradleExit)
    }
    Write-Ok ("构建完成，耗时 {0:N1} 秒" -f $sw.Elapsed.TotalSeconds)
}
finally {
    Pop-Location
}

# -----------------------------------------------------------------------------
# 6. 取产物
# -----------------------------------------------------------------------------

Write-Step '定位 APK 产物'

# ⚠️ 必须从 build/outputs/ 取，不能看 build/intermediates/apk/。
# intermediates 下那个同名文件是历史构建的残留，AGP 不保证覆盖它 ——
# 本工程曾因此被一个 1.1GB 的旧包误导过。
$OutDir = Join-Path $AndroidRoot ('app/build/outputs/apk/' + $ConfigLower)
if (-not (Test-Path $OutDir)) {
    Fail ("产物目录不存在: " + (Normalize-Path $OutDir))
}

$apks = @(Get-ChildItem -Path $OutDir -Filter '*.apk' -ErrorAction SilentlyContinue |
          Sort-Object LastWriteTime -Descending)
if ($apks.Count -eq 0) {
    Fail ("在 " + (Normalize-Path $OutDir) + " 下没找到 apk")
}
$Apk = $apks[0]

# Release 没配 signingConfig，AGP 出的是 *-unsigned.apk，装不上去。
# 用 debug keystore 补签，仅供本机测试。
if ($Apk.Name -like '*unsigned*') {
    Write-Warn2 ("产物未签名: " + $Apk.Name)

    $ApkSigner = $null
    $ZipAlign  = $null
    if ($null -ne $BuildToolsDir) {
        $ApkSigner = Join-Path $BuildToolsDir 'apksigner.bat'
        $ZipAlign  = Join-Path $BuildToolsDir 'zipalign.exe'
    }
    $DebugKeystore = Join-Path $env:USERPROFILE '.android\debug.keystore'

    if ($null -ne $ApkSigner -and (Test-Path $ApkSigner) -and (Test-Path $DebugKeystore)) {
        $Signed = Join-Path $OutDir ('app-' + $ConfigLower + '-debugsigned.apk')

        # 先 zipalign 再签名：apksigner 不做对齐，而 16KB 对齐必须在签名前完成
        if (Test-Path $ZipAlign) {
            $null = Invoke-Native $ZipAlign @('-f', '-P', '16', '4', $Apk.FullName, $Signed)
        } else {
            Copy-Item -Path $Apk.FullName -Destination $Signed -Force
        }

        $null = Invoke-Native $ApkSigner @(
            'sign',
            '--ks', $DebugKeystore,
            '--ks-pass', 'pass:android',
            '--key-pass', 'pass:android',
            '--ks-key-alias', 'androiddebugkey',
            $Signed
        )
        if ($script:LastNativeExit -ne 0) { Fail 'apksigner 签名失败' }

        $Apk = Get-Item $Signed
        Write-Ok '已用 debug keystore 补签（仅本机测试，不可分发）'
    } else {
        Write-Warn2 '找不到 apksigner 或 debug.keystore，跳过签名 —— 该 APK 无法安装'
    }
}

$SizeMB = [Math]::Round($Apk.Length / 1MB, 2)
Write-Ok ("APK: " + (Normalize-Path $Apk.FullName))
Write-Info ("体积: {0} MB" -f $SizeMB)

# assets/ 会被原样打进 APK。本工程曾因为把 3GB 样例素材放在 assets/ 下，
# 打出过 1.1GB 的包。这里给个早期预警，免得又悄悄变大。
if ($SizeMB -gt 100) {
    Write-Warn2 ("APK 偏大（{0} MB）。检查 assets/ 下是否混进了大体积素材。" -f $SizeMB)
    Write-Warn2 '参考素材应放在 sample-assets/（与 assets/ 平级，不参与打包）。'
}

# -----------------------------------------------------------------------------
# 7. 可选校验
# -----------------------------------------------------------------------------

if ($Verify) {
    Write-Step '校验 16 KB 页面对齐'

    # 这件事有两层，必须分开看：
    #   zip 层 —— APK 内 .so 条目的对齐，AGP >= 8.5.1 自动处理
    #   ELF 层 —— .so 自身 LOAD 段的对齐，靠链接器标志（见根 CMakeLists）
    if ($null -ne $BuildToolsDir) {
        $ZipAlign = Join-Path $BuildToolsDir 'zipalign.exe'
        if (Test-Path $ZipAlign) {
            $zaOut = Invoke-Native $ZipAlign @('-c', '-P', '16', '-v', '4', $Apk.FullName)
            $bad   = @($zaOut | Where-Object { $_ -match 'BAD' })
            if ($bad.Count -eq 0) {
                Write-Ok 'zip 层对齐通过'
            } else {
                Write-Warn2 'zip 层对齐有问题:'
                foreach ($b in $bad) { Write-Info $b }
            }
        }
    }

    $ndkDirs = @(Get-ChildItem -Path (Join-Path $SdkRoot 'ndk') -Directory -ErrorAction SilentlyContinue |
                 Sort-Object Name -Descending)
    if ($ndkDirs.Count -gt 0) {
        $ReadElf = Join-Path $ndkDirs[0].FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
        $SoDir   = Join-Path $AndroidRoot ('app/build/intermediates/merged_native_libs/' + $ConfigLower + '/merge' + $Config + 'NativeLibs/out/lib/arm64-v8a')
        if ((Test-Path $ReadElf) -and (Test-Path $SoDir)) {
            foreach ($so in @(Get-ChildItem -Path $SoDir -Filter '*.so')) {
                $hdrs  = Invoke-Native $ReadElf @('--program-headers', $so.FullName)
                $loads = @($hdrs | Where-Object { $_ -match '^\s+LOAD' })
                $good  = @($loads | Where-Object { $_ -match '0x4000' })
                if ($loads.Count -gt 0 -and $loads.Count -eq $good.Count) {
                    Write-Ok ("{0}: {1}/{1} 个 LOAD 段为 0x4000" -f $so.Name, $loads.Count)
                } else {
                    Write-Warn2 ("{0}: 仅 {1}/{2} 个 LOAD 段为 0x4000 —— 检查 CMakeLists 里的 max-page-size" -f $so.Name, $good.Count, $loads.Count)
                }
            }
        }
    }
}

# -----------------------------------------------------------------------------
# 8. 安装 / 启动 / 日志
# -----------------------------------------------------------------------------

$PackageId  = 'com.origin.app'
$ActivityId = 'org.libsdl.app.SDLActivity'   # SDL 提供的 Activity，见 AndroidManifest.xml

if ($Install) {
    Write-Step 'adb 安装'

    if (-not (Test-Path $AdbExe)) {
        Fail ("找不到 adb: " + (Normalize-Path $AdbExe))
    }

    # 只数 "device" 状态的行；unauthorized / offline 不算可用设备
    $devOut  = Invoke-Native $AdbExe @('devices')
    $devices = @($devOut | Where-Object { $_ -match '\sdevice$' })
    if ($devices.Count -eq 0) {
        Fail @"
没有可用设备。请检查：
  - USB 已连接，且手机上已确认"允许 USB 调试"
  - 开发者选项里 USB 调试已打开
  - adb devices 看到的状态是 device（不是 unauthorized / offline）
"@
    }
    Write-Info ("检测到 {0} 台设备" -f $devices.Count)

    $insOut = Invoke-Native $AdbExe @('install', '-r', $Apk.FullName)
    foreach ($l in $insOut) { Write-Info $l }
    if ($script:LastNativeExit -ne 0) {
        # 最常见的原因是签名不一致（debug 与 release 互换）
        Write-Warn2 ("若提示签名冲突，先卸载再装: adb uninstall " + $PackageId)
        Fail 'adb install 失败'
    }
    Write-Ok '安装完成'
}

if ($Run) {
    Write-Step '启动 App'

    # 启动前清一次 log，这样 -Logcat 看到的都是本次运行的输出
    $null  = Invoke-Native $AdbExe @('logcat', '-c')
    $amOut = Invoke-Native $AdbExe @('shell', 'am', 'start', '-n', ($PackageId + '/' + $ActivityId))
    foreach ($l in $amOut) { Write-Info $l }
    if ($script:LastNativeExit -ne 0) { Fail 'am start 失败' }
    Write-Ok ('已启动 ' + $PackageId)
}

if ($Logcat) {
    Write-Step 'logcat（Ctrl+C 退出）'

    # SDL 把 stdout/stderr 桥到 tag "SDL"，我们的 spdlog 也走 SDL_Log；
    # 另外带上 Vulkan 校验层与崩溃信息，便于一眼看出问题。
    # 这里不捕获输出，直接流式打印。
    & $AdbExe 'logcat' '-v' 'brief' `
        'SDL:V' 'SDL/APP:V' 'VALIDATION:V' 'vulkan:V' 'AndroidRuntime:E' 'DEBUG:V' '*:S'
}

Write-Host ''
Write-Host '=======================================================' -ForegroundColor Green
Write-Host (" 完成  |  " + (Normalize-Path $Apk.FullName)) -ForegroundColor Green
Write-Host '=======================================================' -ForegroundColor Green
Write-Host ''

Stop-Log
exit 0
