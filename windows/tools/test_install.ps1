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
$o = Run 'uninstall.ps1' @('-Yes'); Check '没有任何原文件：说明去处' ($code -ne 0 -and $o -match '重装微信')
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
if (-not ($cmdCode -ne 0 -and $o -match '4\.2\.0\.53')) { Write-Host $o }
Check 'install.cmd 与末尾反斜杠' ($cmdCode -ne 0 -and $o -match '4\.2\.0\.53')
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
