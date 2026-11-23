# 还原：把 Weixin.dll 变回官方原文件。一般不需要备份——按补丁反推，结果必须与官方文件的校验和一致；
# DLL 丢失或损坏时才用备份。
#   uninstall.cmd
#   uninstall.cmd -Path "D:\Tencent\Weixin"
param(
    [string]$Path,
    [switch]$Yes,
    [switch]$Elevated,
    [string]$PatchDir,
    [string]$BackupDir,
    [switch]$AllowRunning
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PatchDir) { $PatchDir = Join-Path $here 'patches' }
if (-not $BackupDir) { $BackupDir = Join-Path $env:ProgramData 'WeChatAntiRecall' }
# cmd 里写成 -Path "D:\Weixin\" 时，\" 会被当成引号，连后面的参数一起吞进来；不猜，直接说明
if ($Path -and $Path.Contains('"')) {
    Write-Host '× 路径末尾不要带反斜杠，例如：-Path "D:\Tencent\Weixin"' -ForegroundColor Red
    exit 1
}
if ($Path) { $Path = $Path.Trim().TrimEnd('\') }
# 没预料到的错误也要让人看得见（管理员窗口会立刻关闭）
trap {
    Write-Host "× 出错了：$($_.Exception.Message)" -ForegroundColor Red
    if ($Elevated) { Read-Host '按回车关闭这个窗口' | Out-Null }
    exit 1
}
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

function Say($m) { Write-Host "· $m" -ForegroundColor Cyan }
function Ok($m) { Write-Host "√ $m" -ForegroundColor Green }
function Warn($m) { Write-Host "! $m" -ForegroundColor Yellow }
function Die($m) {
    Write-Host "× $m" -ForegroundColor Red
    if ($Elevated) { Read-Host '按回车关闭这个窗口' | Out-Null }
    exit 1
}

function Sha([byte[]]$b) {
    $h = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($h.ComputeHash($b)) -replace '-', '').ToLower() } finally { $h.Dispose() }
}
function ShaFile($p) {
    $h = [Security.Cryptography.SHA256]::Create()
    $f = [IO.File]::Open($p, 'Open', 'Read', 'ReadWrite, Delete')
    try { return ([BitConverter]::ToString($h.ComputeHash($f)) -replace '-', '').ToLower() } finally { $f.Dispose(); $h.Dispose() }
}
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
    if ($p.ExitCode -eq 0) { Ok '还原完成' } else { Write-Host '× 还原没有完成' -ForegroundColor Red }
    exit $p.ExitCode
}

# 用新内容替换 DLL：先写临时文件并核对，再一步替换；任何一步失败都还原
function Swap-Dll($dll, [byte[]]$data, $sha) {
    $tag = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $new = "$dll.wxar-new-$tag"; $old = "$dll.wxar-old"
    $moved = $false
    try {
        if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Force }
        [IO.File]::WriteAllBytes($new, $data)
        if ((ShaFile $new) -ne $sha) { throw '写出的文件校验不一致' }
        if (Test-Path -LiteralPath $dll) { [IO.File]::Replace($new, $dll, $old); $moved = $true }
        else { [IO.File]::Move($new, $dll) }
        # 杀毒软件最可能在这时删除或锁住新文件
        if ((ShaFile $dll) -ne $sha) { throw '替换后校验不一致' }
    } catch {
        $why = $_.Exception.Message
        Remove-Item -LiteralPath $new -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $old) {
            try {
                if (Test-Path -LiteralPath $dll) { Remove-Item -LiteralPath $dll -Force }
                [IO.File]::Move($old, $dll)
            } catch {
                Die "替换失败（$why），也没能自动放回。原文件在：$old`n  请把它改名为 Weixin.dll，或运行 uninstall.cmd"
            }
            if ($moved) { Die "替换失败，已放回原文件：$why`n  如果杀毒软件有提示，多半是它拦下了" }
        }
        Die "替换失败，微信没有改动：$why"
    }
    Remove-Item -LiteralPath $old -Force -ErrorAction SilentlyContinue
}

$patches = Load-Patches $PatchDir
$roots = @(Find-Roots $Path)
$dlls = @(Find-Dlls $roots)

$todo = $null; $clean = $false
foreach ($d in $dlls) {
    $sha = ShaFile $d
    $hit = $patches.Values | Where-Object { $_.patched_sha256 -eq $sha } | Select-Object -First 1
    if ($hit) { $todo = @{ dll = $d; patch = $hit }; break }
    if ($patches.Values | Where-Object { $_.sha256 -eq $sha }) { $clean = $true }
}
if (-not $todo) {
    if ($clean) { Ok '现在就是官方原文件，不用还原'; exit 0 }
    # DLL 被杀毒软件删了或被改坏了：有校验一致的原文件就放回去
    $dirs = @()
    foreach ($x in $roots) {
        if ($x -like '*\Weixin.dll') { $dirs += (Split-Path $x) } else { $dirs += $x }
        if (Test-Path -LiteralPath $x -PathType Container) { $dirs += @(Get-ChildItem -LiteralPath $x -Directory | ForEach-Object { $_.FullName }) }
    }
    foreach ($v in $patches.Keys) {
        foreach ($d in @($dirs | Where-Object { (Split-Path $_ -Leaf) -eq $v -and (Test-Path -LiteralPath $_) } | Select-Object -Unique)) {
            foreach ($f in (Join-Path $d 'Weixin.dll.wxar-old'), (Join-Path (Join-Path $BackupDir $v) 'Weixin.dll')) {
                if (-not $todo -and (Test-Path -LiteralPath $f) -and (ShaFile $f) -eq $patches[$v].sha256) {
                    $todo = @{ dll = (Join-Path $d 'Weixin.dll'); patch = $patches[$v]; from = $f }
                }
            }
        }
    }
    if (-not $todo) { Die "没有找到本工具打过补丁的 Weixin.dll。备份在 $BackupDir，也可以直接重装微信（聊天记录不受影响）" }
    Warn "Weixin.dll 不见了或被改过，将用校验一致的原文件放回：$($todo.from)"
}

$dll = $todo.dll; $p = $todo.patch
Say "将还原微信 $($p.version)：$dll"
if ($todo.from) {
    [byte[]]$out = [IO.File]::ReadAllBytes($todo.from)
} else {
    [byte[]]$cur = [IO.File]::ReadAllBytes($dll)
    [byte[]]$out = New-Object byte[] $p.size
    [Array]::Copy($cur, $out, [int]$p.size)
    foreach ($e in $p.edits) {
        $want = Unhex $e.expected
        [Array]::Copy($want, 0, $out, [int]$e.offset, $want.Length)
    }
}
if ((Sha $out) -ne $p.sha256) { Die '还原结果与官方文件不一致，没有改动' }

Assert-Closed
if (-not $Yes) {
    $a = Read-Host '继续？(y/N)'
    if ($a -notmatch '^[yY]') { Die '已取消' }
}
Ensure-Admin (Split-Path $dll) $MyInvocation.MyCommand.Path "-Yes -Path `"$dll`" -PatchDir `"$PatchDir`" -BackupDir `"$BackupDir`""
Assert-Closed
Swap-Dll $dll $out $p.sha256
Ok '已还原为官方原文件'
if ($Elevated) { Start-Sleep -Seconds 4 }
