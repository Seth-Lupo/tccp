# tccp installer for Windows

$repo = "sethlupo/tccp"
$binary = "tccp-windows-x64.exe"
$url = "https://github.com/$repo/releases/latest/download/$binary"
$installDir = "$env:LOCALAPPDATA\tccp"

Write-Host "Downloading $binary..."
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Invoke-WebRequest -Uri $url -OutFile "$installDir\tccp.exe"

# Add to PATH if not already there
$currentPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($currentPath -notlike "*$installDir*") {
    Write-Host "Adding $installDir to your PATH..."
    [Environment]::SetEnvironmentVariable("PATH", "$currentPath;$installDir", "User")
    Write-Host "Restart your terminal for PATH changes to take effect."
}

Write-Host "Done. Run 'tccp --version' to verify."
