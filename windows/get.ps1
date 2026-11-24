# 一键安装（Windows）。在 PowerShell 里运行：
#   irm https://raw.githubusercontent.com/stargif/wechat-antirecall/main/windows/get.ps1 | iex
# 从 GitHub 下载本仓库，把 windows 文件夹放到 %LOCALAPPDATA%\WeChatAntiRecall，再运行里面的 install.ps1。
# 这个文件要保持 UTF-8 无 BOM，否则 irm | iex 会报错；也不能用 exit，会关掉用户的窗口。
param(
    [string]$Zip = 'https://github.com/stargif/wechat-antirecall/archive/refs/heads/main.zip',
    [string]$Dir,
    [switch]$Check
)
& {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'
    if ($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') {
        Write-Host '× 系统限制了 PowerShell 脚本（多半是开着智能应用控制），不能安装。没有改动任何东西' -ForegroundColor Red
        return
    }
    if (-not $Dir) { $Dir = Join-Path $env:LOCALAPPDATA 'WeChatAntiRecall\windows' }
    $new = "$Dir.new"; $old = "$Dir.old"
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('wxar-' + [Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        $file = Join-Path $tmp 'main.zip'
        if ($Zip -match '^https://') {
            Write-Host '· 正在从 GitHub 下载' -ForegroundColor Cyan
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri $Zip -OutFile $file
        } else { Copy-Item -LiteralPath $Zip -Destination $file }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($file, (Join-Path $tmp 'x'))
        $top = Get-ChildItem -LiteralPath (Join-Path $tmp 'x') -Directory | Select-Object -First 1
        if (-not $top) { throw '下载的内容不完整' }
        $src = Join-Path $top.FullName 'windows'
        foreach ($n in 'install.ps1', 'uninstall.ps1', 'uninstall.cmd', 'patches') {
            if (-not (Test-Path -LiteralPath (Join-Path $src $n))) { throw '下载的内容不完整' }
        }
        New-Item -ItemType Directory -Force -Path (Split-Path $Dir) | Out-Null
        # 先复制到旁边，再改名换上；失败就改回来，旧的那份（还原要用）一直都在
        if ((Test-Path -LiteralPath $old) -and -not (Test-Path -LiteralPath $Dir)) { [IO.Directory]::Move($old, $Dir) }
        foreach ($d in $new, $old) { if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force } }
        Copy-Item -LiteralPath $src -Destination $new -Recurse
        if (Test-Path -LiteralPath $Dir) { [IO.Directory]::Move($Dir, $old) }
        try { [IO.Directory]::Move($new, $Dir) }
        catch { if (Test-Path -LiteralPath $old) { [IO.Directory]::Move($old, $Dir) }; throw }
        Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue
    } catch {
        Write-Host "× 没有成功，没有改动微信：$($_.Exception.Message)" -ForegroundColor Red
        Write-Host '  可以打开 https://github.com/stargif/wechat-antirecall ，点 Code → Download ZIP 手动下载'
        return
    } finally {
        Remove-Item -LiteralPath $tmp, $new -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "· 已下载到 $Dir" -ForegroundColor Cyan
    $more = @(); if ($Check) { $more += '-Check' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Dir 'install.ps1') @more
}
