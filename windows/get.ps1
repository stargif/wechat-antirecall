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
    if (-not $Dir) { $Dir = Join-Path $env:LOCALAPPDATA 'WeChatAntiRecall\windows' }
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('wxar-' + [Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        $file = Join-Path $tmp 'main.zip'
        if ($Zip -match '^https://') {
            Write-Host '· 正在从 GitHub 下载' -ForegroundColor Cyan
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri $Zip -OutFile $file
        } else { Copy-Item -LiteralPath $Zip -Destination $file }
        Expand-Archive -LiteralPath $file -DestinationPath (Join-Path $tmp 'x')
        $top = Get-ChildItem -LiteralPath (Join-Path $tmp 'x') -Directory | Select-Object -First 1
        if (-not $top) { throw '下载的内容不完整' }
        $src = Join-Path $top.FullName 'windows'
        foreach ($n in 'install.ps1', 'uninstall.ps1', 'uninstall.cmd', 'patches') {
            if (-not (Test-Path -LiteralPath (Join-Path $src $n))) { throw '下载的内容不完整' }
        }
        New-Item -ItemType Directory -Force -Path (Split-Path $Dir) | Out-Null
        if (Test-Path -LiteralPath $Dir) { Remove-Item -LiteralPath $Dir -Recurse -Force }
        Copy-Item -LiteralPath $src -Destination $Dir -Recurse
    } catch {
        Write-Host "× 没有下载成功，没有改动微信：$($_.Exception.Message)" -ForegroundColor Red
        Write-Host '  可以打开 https://github.com/stargif/wechat-antirecall ，点 Code → Download ZIP 手动下载'
        return
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "· 已下载到 $Dir" -ForegroundColor Cyan
    $more = @(); if ($Check) { $more += '-Check' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Dir 'install.ps1') @more
}
