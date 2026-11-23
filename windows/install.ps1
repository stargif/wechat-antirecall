# 安装：给 Weixin.dll 打补丁。补丁是确定的，打之前核对原文件，打之后核对结果，对不上就不动。
#   install.cmd                                  自动查找微信
#   install.cmd -Check                           只检查能不能装，不改任何东西
#   install.cmd -Path "D:\Tencent\Weixin"        指定微信目录
param(
    [string]$Path,
    [switch]$Check,
    [switch]$Yes,
    [switch]$Elevated,
    [string]$PatchDir = (Join-Path $PSScriptRoot 'patches'),
    [string]$BackupDir = (Join-Path $env:ProgramData 'WeChatAntiRecall'),
    [switch]$AllowRunning
)
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

function Say($m) { Write-Host "· $m" -ForegroundColor Cyan }
function Ok($m) { Write-Host "√ $m" -ForegroundColor Green }
function Die($m) { Write-Host "× $m" -ForegroundColor Red; exit 1 }

function Sha([byte[]]$b) {
    $h = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($h.ComputeHash($b)) -replace '-', '').ToLower() } finally { $h.Dispose() }
}
function ShaFile($p) { return (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() }
function Unhex($s) {
    $b = New-Object byte[] ($s.Length / 2)
    for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($s.Substring(2 * $i, 2), 16) }
    return , $b
}
function Same([byte[]]$a, [int]$at, [byte[]]$b) {
    for ($i = 0; $i -lt $b.Length; $i++) { if ($a[$at + $i] -ne $b[$i]) { return $false } }
    return $true
}

# 所有补丁，按版本号
function Load-Patches($dir) {
    $r = @{}
    Get-ChildItem -LiteralPath $dir -Filter *.json | ForEach-Object {
        $j = [IO.File]::ReadAllText($_.FullName) | ConvertFrom-Json
        $r[$j.version] = $j
    }
    return $r
}

# 可能的微信安装目录
function Find-Roots($path) {
    if ($path) { return @($path) }
    $c = @()
    foreach ($k in 'HKCU:\Software\Tencent\Weixin', 'HKLM:\SOFTWARE\Tencent\Weixin', 'HKLM:\SOFTWARE\WOW6432Node\Tencent\Weixin') {
        $v = (Get-ItemProperty -LiteralPath $k -ErrorAction SilentlyContinue).InstallPath
        if ($v) { $c += $v }
    }
    foreach ($b in $env:ProgramFiles, ${env:ProgramFiles(x86)}) { if ($b) { $c += (Join-Path $b 'Tencent\Weixin') } }
    return @($c | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -Unique)
}

# 目录下所有 Weixin.dll：安装根目录、版本目录、或 DLL 本身都认
function Find-Dlls($roots) {
    $r = @()
    foreach ($x in $roots) {
        if (Test-Path -LiteralPath $x -PathType Leaf) { $r += (Resolve-Path -LiteralPath $x).ProviderPath; continue }
        $d = Join-Path $x 'Weixin.dll'
        if (Test-Path -LiteralPath $d) { $r += (Resolve-Path -LiteralPath $d).ProviderPath }
        Get-ChildItem -LiteralPath $x -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $d = Join-Path $_.FullName 'Weixin.dll'
            if (Test-Path -LiteralPath $d) { $r += $d }
        }
    }
    return @($r | Select-Object -Unique)
}

function Assert-Closed {
    if (-not $AllowRunning -and (Get-Process -Name Weixin -ErrorAction SilentlyContinue)) {
        Die '微信还在运行。请在托盘图标上点右键退出（只关窗口不算），再重新运行'
    }
}

# 目录不可写就用管理员身份重新运行自己
function Ensure-Admin($dir, $script, $extra) {
    $probe = Join-Path $dir ('wxar-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try { [IO.File]::WriteAllText($probe, ''); Remove-Item -LiteralPath $probe -Force; return } catch { }
    if ($Elevated) { Die "没有权限写入 $dir" }
    Say '需要管理员权限，请在弹出的窗口里点「是」'
    $arg = "-NoProfile -ExecutionPolicy Bypass -File `"$script`" -Elevated $extra"
    try { $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $arg -Wait -PassThru } catch { Die '没有拿到管理员权限，没有改动任何东西' }
    exit $p.ExitCode
}

# 用新内容替换 DLL：先写临时文件并核对，再一步替换；失败就还原
function Swap-Dll($dll, [byte[]]$data, $sha) {
    $new = "$dll.wxar-new"; $old = "$dll.wxar-old"
    foreach ($f in $new, $old) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }
    try {
        [IO.File]::WriteAllBytes($new, $data)
        if ((ShaFile $new) -ne $sha) { throw '写出的文件校验不一致' }
        [IO.File]::Replace($new, $dll, $old)
    } catch {
        $e = $_.Exception.Message
        if (-not (Test-Path -LiteralPath $dll) -and (Test-Path -LiteralPath $old)) { Move-Item -LiteralPath $old -Destination $dll }
        if (Test-Path -LiteralPath $new) { Remove-Item -LiteralPath $new -Force -ErrorAction SilentlyContinue }
        Die "替换失败，微信没有改动：$e"
    }
    if ((ShaFile $dll) -ne $sha) {
        Remove-Item -LiteralPath $dll -Force
        Move-Item -LiteralPath $old -Destination $dll
        Die '替换后校验不一致，已还原'
    }
    Remove-Item -LiteralPath $old -Force -ErrorAction SilentlyContinue
}

$patches = Load-Patches $PatchDir
$dlls = @(Find-Dlls (Find-Roots $Path))
if ($dlls.Count -eq 0) { Die '找不到微信。请用 -Path 指定安装目录，例如：install.cmd -Path "D:\Tencent\Weixin"' }

$todo = $null; $done = $false; $seen = @()
foreach ($d in $dlls) {
    $sha = ShaFile $d
    $hit = $patches.Values | Where-Object { $_.sha256 -eq $sha } | Select-Object -First 1
    if ($hit) { $todo = @{ dll = $d; patch = $hit }; break }
    if ($patches.Values | Where-Object { $_.patched_sha256 -eq $sha }) { $done = $true; Say "已装好：$d" }
    $seen += "$d（$((Get-Item -LiteralPath $d).VersionInfo.FileVersion)）"
}
if (-not $todo) {
    if ($done) { Ok '已经装好了，不用再装'; exit 0 }
    Die ("没有支持的版本。找到的是：`n    " + ($seen -join "`n    ") + "`n  支持的版本：" + (($patches.Keys | Sort-Object) -join '、') + "`n  如果版本号一样，说明文件被别的工具改过，请先还原或重装微信")
}

$dll = $todo.dll; $p = $todo.patch
Say "微信 $($p.version)：$dll"
[byte[]]$src = [IO.File]::ReadAllBytes($dll)
if ($src.Length -ne $p.size) { Die '文件大小不符' }
[byte[]]$out = New-Object byte[] $p.patched_size
[Array]::Copy($src, $out, $src.Length)
foreach ($e in $p.edits) {
    $want = Unhex $e.expected; $data = Unhex $e.data
    if (-not (Same $src $e.offset $want)) { Die ("0x{0:x} 的字节不是预期的，拒绝修改" -f $e.offset) }
    [Array]::Copy($data, 0, $out, [int]$e.offset, $data.Length)
}
$tail = [Convert]::FromBase64String($p.append.data)
[Array]::Copy($tail, 0, $out, [int]$p.append.offset, $tail.Length)
if ((Sha $out) -ne $p.patched_sha256) { Die '补丁结果与登记的不一致，拒绝修改' }
Ok '原文件与补丁结果都核对通过'
if ($Check) { Ok '可以安装（-Check 只检查，没有改动任何东西）'; exit 0 }

Assert-Closed
if (-not $Yes) {
    $a = Read-Host '将修改微信的 Weixin.dll（可用 uninstall.cmd 还原）。建议先用小号。继续？(y/N)'
    if ($a -notmatch '^[yY]') { Die '已取消' }
}
Ensure-Admin (Split-Path $dll) $PSCommandPath "-Yes -Path `"$dll`" -PatchDir `"$PatchDir`" -BackupDir `"$BackupDir`""
Assert-Closed

$bak = Join-Path $BackupDir $p.version
New-Item -ItemType Directory -Force -Path $bak | Out-Null
$copy = Join-Path $bak 'Weixin.dll'
if (-not ((Test-Path -LiteralPath $copy) -and (ShaFile $copy) -eq $p.sha256)) {
    [IO.File]::WriteAllBytes($copy, $src)
    if ((ShaFile $copy) -ne $p.sha256) { Die '备份校验不一致，没有改动微信' }
}
Say "原文件已备份到 $copy"
Swap-Dll $dll $out $p.patched_sha256
Ok "安装完成。打开微信，让别人发一条消息再撤回，看看效果"
Write-Host '  想还原就运行 uninstall.cmd'
if ($Elevated) { Start-Sleep -Seconds 4 }
