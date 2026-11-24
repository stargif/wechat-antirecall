# 安装 / 还原脚本的测试（CI 用，不需要微信）：造一个假的 Weixin.dll 和对应补丁，走一遍各种情况
$ErrorActionPreference = 'Stop'
$here = Split-Path $PSScriptRoot
$shell = (Get-Process -Id $PID).Path
$t = Join-Path ([IO.Path]::GetTempPath()) ('wxar 测试 ' + [Guid]::NewGuid().ToString('N'))
$root = Join-Path $t 'Tencent\微信 Weixin'; $ver = Join-Path $root '9.9.9.9'; $pd = Join-Path $t 'patches'; $bk = Join-Path $t 'backup'
New-Item -ItemType Directory -Force -Path $ver, $pd | Out-Null
$dll = Join-Path $ver 'Weixin.dll'
$script:bad = 0
function Check($name, $cond) { if ($cond) { Write-Host "ok   $name" } else { $script:bad++; Write-Host "FAIL $name" } }
function Sha($b) { $h = [Security.Cryptography.SHA256]::Create(); return ([BitConverter]::ToString($h.ComputeHash([byte[]]$b)) -replace '-', '').ToLower() }
function Hash { return (Get-FileHash -LiteralPath $dll).Hash.ToLower() }
function Run($script, [string[]]$more) {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $here $script), '-Path', $root, '-PatchDir', $pd, '-BackupDir', $bk, '-AllowRunning') + $more
    $o = & $shell @a 2>&1 | Out-String
    $script:code = $LASTEXITCODE
    return $o
}
# 在受限语言模式里跑（应用控制策略下脚本的环境）
function Limited($script, [string[]]$more) {
    $q = @($root, $pd, $bk) | ForEach-Object { "'" + $_ + "'" }
    $c = "`$ExecutionContext.SessionState.LanguageMode = 'ConstrainedLanguage'; & '$(Join-Path $here $script)' -Path $($q[0]) -PatchDir $($q[1]) -BackupDir $($q[2]) -AllowRunning $($more -join ' '); exit `$LASTEXITCODE"
    $o = & $shell -NoProfile -ExecutionPolicy Bypass -EncodedCommand ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($c))) 2>&1 | Out-String
    $script:code = $LASTEXITCODE
    return $o
}
[byte[]]$src = New-Object byte[] 300000
(New-Object Random 7).NextBytes($src)
[byte[]]$out = New-Object byte[] 300544
[Array]::Copy($src, $out, 300000)
foreach ($i in 0..6) { $out[1000 + $i] = 0xAA }
foreach ($i in 0..511) { $out[300032 + $i] = [byte]($i -band 255) }
[IO.File]::WriteAllBytes($dll, $src)
$old = ([BitConverter]::ToString($src, 1000, 7) -replace '-', '').ToLower()
$j = @{ version = '9.9.9.9'; file = 'Weixin.dll'; size = 300000; sha256 = (Sha $src); patched_size = 300544; patched_sha256 = (Sha $out)
    edits = @(@{ offset = 1000; expected = $old; data = 'aaaaaaaaaaaaaa' }); append = @{ offset = 300032; data = [Convert]::ToBase64String($out, 300032, 512) } }
[IO.File]::WriteAllText((Join-Path $pd '9.9.9.9.json'), ($j | ConvertTo-Json -Depth 5))

$o = Run 'install.ps1' @('-Check'); Check '只检查' ($code -eq 0 -and $o -match '可以安装' -and (Hash) -eq (Sha $src))
$o = Run 'uninstall.ps1' @('-Yes'); Check '没装时还原' ($code -eq 0 -and $o -match '不用还原')
$o = Run 'install.ps1' @('-Yes'); Check '安装' ($code -eq 0 -and $o -match '安装完成' -and (Hash) -eq (Sha $out))
Check '备份正确' ((Get-FileHash -LiteralPath (Join-Path $bk '9.9.9.9\Weixin.dll')).Hash.ToLower() -eq (Sha $src))
Check '没有残留文件' (@(Get-ChildItem -LiteralPath $ver).Count -eq 1)
$o = Run 'install.ps1' @('-Yes'); Check '重复安装' ($code -eq 0 -and $o -match '已经装好')
Remove-Item -LiteralPath $bk -Recurse -Force
$o = Run 'uninstall.ps1' @('-Yes'); Check '没有备份也能还原' ($code -eq 0 -and $o -match '已还原' -and (Hash) -eq (Sha $src))
$f = [IO.File]::Open($dll, 'Open', 'Read', 'Read')
$o = Run 'install.ps1' @('-Yes'); $f.Dispose(); Check '文件被占用：失败且不改动' ($code -ne 0 -and (Hash) -eq (Sha $src) -and @(Get-ChildItem -LiteralPath $ver).Count -eq 1)
[byte[]]$x = $src.Clone(); $x[5] = $x[5] -bxor 1; [IO.File]::WriteAllBytes($dll, $x)
$o = Run 'install.ps1' @('-Yes'); Check '被改过的文件拒绝' ($code -ne 0 -and $o -match '没有支持的版本' -and (Hash) -eq (Sha $x))
[IO.File]::WriteAllBytes($dll, $src)
$j.edits[0].expected = '00000000000000'; [IO.File]::WriteAllText((Join-Path $pd '9.9.9.9.json'), ($j | ConvertTo-Json -Depth 5))
$o = Run 'install.ps1' @('-Yes'); Check '原始字节不符拒绝' ($code -ne 0 -and $o -match '不是预期的' -and (Hash) -eq (Sha $src))
$j.edits[0].expected = $old; $j.edits[0].data = 'bbbbbbbbbbbbbb'; [IO.File]::WriteAllText((Join-Path $pd '9.9.9.9.json'), ($j | ConvertTo-Json -Depth 5))
$o = Run 'install.ps1' @('-Yes'); Check '结果不符拒绝' ($code -ne 0 -and $o -match '与登记的不一致' -and (Hash) -eq (Sha $src))
# DLL 丢失或损坏，用备份放回
$j.edits[0].data = 'aaaaaaaaaaaaaa'; [IO.File]::WriteAllText((Join-Path $pd '9.9.9.9.json'), ($j | ConvertTo-Json -Depth 5))
$o = Run 'install.ps1' @('-Yes'); Check '再次安装' ($code -eq 0 -and (Hash) -eq (Sha $out))
Remove-Item -LiteralPath $dll -Force
$o = Run 'uninstall.ps1' @('-Yes'); Check 'DLL 被删后还原' ($code -eq 0 -and $o -match '已还原' -and (Hash) -eq (Sha $src))
[IO.File]::WriteAllBytes($dll, $x)
$o = Run 'uninstall.ps1' @('-Yes'); Check 'DLL 损坏后还原' ($code -eq 0 -and (Hash) -eq (Sha $src))
Remove-Item -LiteralPath $bk -Recurse -Force; Move-Item -LiteralPath $dll -Destination "$dll.wxar-old"
$o = Run 'uninstall.ps1' @('-Yes'); Check '中断残留的原文件能放回' ($code -eq 0 -and (Hash) -eq (Sha $src) -and @(Get-ChildItem -LiteralPath $ver).Count -eq 1)
Remove-Item -LiteralPath $dll -Force
$o = Run 'uninstall.ps1' @('-Yes'); Check '没有任何原文件：说明去处' ($code -ne 0 -and $o -match '覆盖安装')
[IO.File]::WriteAllBytes($dll, $src)
# 另有别的版本时提醒
$v2 = Join-Path $root '9.9.9.10'; New-Item -ItemType Directory -Force -Path $v2 | Out-Null; [IO.File]::WriteAllBytes((Join-Path $v2 'Weixin.dll'), $x)
$o = Run 'install.ps1' @('-Check'); Check '提醒还有别的版本' ($code -eq 0 -and $o -match '还有别的版本')
Remove-Item -LiteralPath $v2 -Recurse -Force
# 双击时的默认参数：不传 -PatchDir / -BackupDir
$o = & $shell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'install.ps1') -Path $root -Check 2>&1 | Out-String
Check '默认补丁目录' ($LASTEXITCODE -ne 0 -and $o -match '没有支持的版本' -and $o -match '4\.2\.0\.53')
# 走 install.cmd，且路径以反斜杠结尾（cmd 会多传一个引号）
$ascii = Join-Path $env:TEMP ('wxar-' + [Guid]::NewGuid().ToString('N'))
$av = Join-Path $ascii 'We ixin\1.2.3.4'; New-Item -ItemType Directory -Force -Path $av | Out-Null; [IO.File]::WriteAllBytes((Join-Path $av 'Weixin.dll'), $x)
$bat = Join-Path $ascii 'run.cmd'
[IO.File]::WriteAllText($bat, "@call `"$(Join-Path $here 'install.cmd')`" -Path `"$ascii\We ixin\`" -Check <nul`r`n@exit /b %errorlevel%`r`n", [Text.Encoding]::ASCII)
$ErrorActionPreference = 'Continue'
$o = cmd /c $bat 2>&1 | Out-String
$ErrorActionPreference = 'Stop'
$cmdCode = $LASTEXITCODE; Remove-Item -LiteralPath $ascii -Recurse -Force
if (-not ($cmdCode -ne 0 -and $o -match '末尾不要带反斜杠')) { Write-Host $o }
Check 'install.cmd 与末尾反斜杠' ($cmdCode -ne 0 -and $o -match '末尾不要带反斜杠')
# 智能应用控制：开启时拒绝（已装好的也一样），评估时提醒，关闭、没有这个值或没有这个键时照常
$ci = 'HKCU:\Software\wxar-test-' + [Guid]::NewGuid().ToString('N'); New-Item -Path $ci | Out-Null
try {
    New-ItemProperty -LiteralPath $ci -Name VerifiedAndReputablePolicyState -PropertyType DWord -Value 1 | Out-Null
    $o = Run 'install.ps1' @('-Check', '-CiKey', $ci); Check '智能应用控制开启：检查就拒绝' ($code -ne 0 -and $o -match '智能应用控制' -and $o -notmatch '可以安装')
    $o = Run 'install.ps1' @('-Yes', '-CiKey', $ci); Check '智能应用控制开启：不安装' ($code -ne 0 -and $o -match '没有改动' -and (Hash) -eq (Sha $src) -and -not (Test-Path -LiteralPath $bk))
    [IO.File]::WriteAllBytes($dll, $out)
    $o = Run 'install.ps1' @('-Yes', '-CiKey', $ci); Check '智能应用控制开启：已装好的提示还原' ($code -ne 0 -and $o -match 'uninstall\.cmd' -and (Hash) -eq (Sha $out))
    # 受限语言模式里控制台编码设不了，只核对英文部分
    $o = Limited 'install.ps1' @('-Yes', '-CiKey', $ci); Check '受限语言模式：智能应用控制的提示照样能看到' ($code -ne 0 -and $o -match 'uninstall\.cmd' -and (Hash) -eq (Sha $out))
    $o = Limited 'uninstall.ps1' @('-Yes'); Check '受限语言模式：还原给出手动办法' ($code -ne 0 -and $o -match 'Weixin\.dll' -and $o -match '2\. ' -and (Hash) -eq (Sha $out))
    [IO.File]::WriteAllBytes($dll, $src)
    $o = Limited 'install.ps1' @('-Yes', '-CiKey', "$ci-none"); Check '受限语言模式：不安装' ($code -ne 0 -and $o -match 'PowerShell' -and (Hash) -eq (Sha $src) -and -not (Test-Path -LiteralPath $bk))
    Set-ItemProperty -LiteralPath $ci -Name VerifiedAndReputablePolicyState -Value 2
    $o = Run 'install.ps1' @('-Check', '-CiKey', $ci); Check '智能应用控制评估：提醒' ($code -eq 0 -and $o -match '评估模式' -and $o -match '可以安装')
    Set-ItemProperty -LiteralPath $ci -Name VerifiedAndReputablePolicyState -Value 0
    $o = Run 'install.ps1' @('-Check', '-CiKey', $ci); Check '智能应用控制关闭：照常' ($code -eq 0 -and $o -notmatch '智能应用控制' -and $o -match '可以安装')
    Remove-ItemProperty -LiteralPath $ci -Name VerifiedAndReputablePolicyState
    $o = Run 'install.ps1' @('-Check', '-CiKey', $ci); Check '没有智能应用控制这个值：照常' ($code -eq 0 -and $o -notmatch '智能应用控制' -and $o -match '可以安装')
    $o = Run 'install.ps1' @('-Check', '-CiKey', "$ci-none"); Check '没有智能应用控制：照常' ($code -eq 0 -and $o -notmatch '智能应用控制' -and $o -match '可以安装')
} finally { Remove-Item -LiteralPath $ci -Recurse -Force }
# 真实控制台（不重定向）：跑完以后中文结果必须留在屏幕上。中文系统上 chcp 会清屏，英文系统复现不了，所以另查没有用它
foreach ($n in 'install.cmd', 'uninstall.cmd') { Check "$n 不切换代码页" ([IO.File]::ReadAllText((Join-Path $here $n)) -notmatch 'chcp') }
$con = Join-Path $env:TEMP ('wxar-' + [Guid]::NewGuid().ToString('N')); $cv = Join-Path $con 'Weixin\9.9.9.9'; $cp = Join-Path $con 'patches'
New-Item -ItemType Directory -Force -Path $cv, $cp | Out-Null
[IO.File]::WriteAllBytes((Join-Path $cv 'Weixin.dll'), $src); [IO.File]::WriteAllText((Join-Path $cp '9.9.9.9.json'), ($j | ConvertTo-Json -Depth 5))
[IO.File]::WriteAllText((Join-Path $con 'screen.ps1'), @'
param($bat, $out, $cp)
chcp $cp | Out-Null
cmd /c $bat
$r = $host.UI.RawUI
$b = $r.GetBufferContents((New-Object Management.Automation.Host.Rectangle 0, 0, ($r.BufferSize.Width - 1), $r.CursorPosition.Y))
$s = for ($y = 0; $y -lt $b.GetLength(0); $y++) { -join $(for ($x = 0; $x -lt $b.GetLength(1); $x++) { if ($b[$y, $x].BufferCellType -ne 'Trailing') { $b[$y, $x].Character } }) }
[IO.File]::WriteAllText($out, (($s | ForEach-Object { $_.TrimEnd() }) -join "`n"))
'@)
foreach ($n in 'install', 'uninstall') { foreach ($page in 936, 437, 65001) {
    $bat = Join-Path $con "$n.bat"; $seen = Join-Path $con "$n-$page.txt"; $x2 = if ($n -eq 'install') { '-Check' } else { '-Yes' }
    [IO.File]::WriteAllText($bat, "@call `"$(Join-Path $here "$n.cmd")`" -Path `"$con\Weixin`" -PatchDir `"$cp`" $x2 <nul`r`n", [Text.Encoding]::ASCII)
    Start-Process $shell -WindowStyle Hidden -Wait -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $con 'screen.ps1')`" `"$bat`" `"$seen`" $page"
    $o = if (Test-Path -LiteralPath $seen) { [IO.File]::ReadAllText($seen) } else { '(没有输出)' }
    if ($page -ne 936) { $o = $o -replace '([\u2E80-\uFFEF])\1', '$1' }  # 非中文代码页下每个汉字占的两格都读成同一个字
    $want = if ($n -eq 'install') { '可以安装' } else { '不用还原' }
    if ($o -notmatch $want) { Write-Host "---- 屏幕内容`n$o`n----" }
    Check "真实控制台（代码页 $page）：$n.cmd 的结果留在屏幕上" ($o -match $want)
} }
Remove-Item -LiteralPath $con -Recurse -Force
# 一键安装：用本地 zip 代替下载，按 irm | iex 的方式（整段文本）执行
$g = Join-Path $t 'get'; $gd = Join-Path $g '目标 dir\windows'; New-Item -ItemType Directory -Force -Path (Join-Path $g 'wechat-antirecall-main') | Out-Null
Copy-Item -LiteralPath $here -Destination (Join-Path $g 'wechat-antirecall-main\windows') -Recurse
Compress-Archive -Path (Join-Path $g 'wechat-antirecall-main') -DestinationPath (Join-Path $g 'a.zip')
[IO.File]::WriteAllText((Join-Path $g 'bad.zip'), 'not a zip')
function Get-It($zip) {
    $c = "[Console]::OutputEncoding = [Text.Encoding]::UTF8; & ([scriptblock]::Create([IO.File]::ReadAllText('$(Join-Path $here 'get.ps1')'))) -Zip '$zip' -Dir '$gd' -Check; 'STILL-HERE'"
    return (& $shell -NoProfile -ExecutionPolicy Bypass -EncodedCommand ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($c))) 2>&1 | Out-String)
}
$b3 = [IO.File]::ReadAllBytes((Join-Path $here 'get.ps1'))
Check 'get.ps1 没有 BOM' ($b3[0] -ne 0xEF)
$o = Get-It (Join-Path $g 'a.zip'); Check '一键安装：下载并运行' ($o -match '已下载到' -and $o -match '找不到微信' -and $o -match 'STILL-HERE' -and (Test-Path -LiteralPath (Join-Path $gd 'uninstall.cmd')))
[IO.File]::WriteAllText((Join-Path $gd 'old.txt'), 'x')
$o = Get-It (Join-Path $g 'bad.zip'); Check '一键安装：下载坏了不动已有的' ($o -match '没有下载成功' -and $o -match 'STILL-HERE' -and (Test-Path -LiteralPath (Join-Path $gd 'old.txt')))
$o = Get-It (Join-Path $g 'a.zip'); Check '一键安装：再来一次会换成新的' ($o -match '已下载到' -and -not (Test-Path -LiteralPath (Join-Path $gd 'old.txt')) -and (Test-Path -LiteralPath (Join-Path $gd 'install.ps1')))
Check '一键安装：没有留下临时文件' (@(Get-ChildItem -LiteralPath ([IO.Path]::GetTempPath()) -Directory -Filter 'wxar-*').Count -eq 0)
$root = Join-Path $t 'none'; $o = Run 'install.ps1' @('-Yes'); Check '找不到微信' ($code -ne 0 -and $o -match '找不到微信')
# 真实补丁文件的格式
foreach ($pf in Get-ChildItem -LiteralPath (Join-Path $here 'patches') -Filter *.json) {
    $r = [IO.File]::ReadAllText($pf.FullName) | ConvertFrom-Json
    $n = [Convert]::FromBase64String($r.append.data).Length
    Check "$($pf.Name) 尺寸自洽" ($r.append.offset + $n -eq $r.patched_size -and $r.append.offset -ge $r.size -and @($r.edits | Where-Object { $_.expected.Length -ne $_.data.Length }).Count -eq 0)
    $sorted = @($r.edits | Sort-Object offset); $over = 0
    for ($i = 1; $i -lt $sorted.Count; $i++) { if ($sorted[$i - 1].offset + $sorted[$i - 1].data.Length / 2 -gt $sorted[$i].offset) { $over++ } }
    Check "$($pf.Name) 改动不重叠" ($over -eq 0)
}
Remove-Item -LiteralPath $t -Recurse -Force
if ($bad) { Write-Host "$bad 项失败"; exit 1 } else { Write-Host '全部通过' }
