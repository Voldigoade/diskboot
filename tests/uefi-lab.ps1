param(
    [Parameter(Mandatory)][string]$PreparedVhdx,
    [ValidateSet('On', 'Off')][string]$SecureBoot = 'On',
    [UInt64]$MemoryBytes = 12GB
)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Output 'SKIP: UEFI lab requires Hyper-V management permission. No VM was created.'
    exit 77
}
Get-VMHost | Out-Null
$source = [IO.Path]::GetFullPath($PreparedVhdx)
if ([IO.Path]::GetExtension($source) -ne '.vhdx' -or !(Test-Path -LiteralPath $source -PathType Leaf)) { throw 'A prepared, detached VHDX file is required' }
if ((Get-VHD -Path $source).Attached) { throw 'Detach prepared media before testing' }
if ($MemoryBytes -lt 8GB) { throw 'Guest memory must cover the profile RAM preflight; minimum lab allocation is 8 GiB' }
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('diskboot-uefi-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$copy = Join-Path $testRoot 'target.vhdx'
Copy-Item -LiteralPath $source -Destination $copy
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
$vmName = 'DiskBoot-' + [guid]::NewGuid().ToString('N')
New-VM -Name $vmName -Generation 2 -MemoryStartupBytes $MemoryBytes -VHDPath $copy -Path $testRoot | Out-Null
Set-VM -Name $vmName -AutomaticCheckpointsEnabled $false
Set-VMFirmware -VMName $vmName -EnableSecureBoot $SecureBoot -SecureBootTemplate MicrosoftUEFICertificateAuthority
$firmware = Get-VMFirmware -VMName $vmName
if ($firmware.SecureBoot.ToString() -ne $SecureBoot) { throw 'Guest Secure Boot setting was not verified' }
$record = [ordered]@{
    VM = $vmName
    Generation = 2
    MemoryBytes = $MemoryBytes
    SecureBoot = $firmware.SecureBoot.ToString()
    SecureBootTemplate = $firmware.SecureBootTemplate
    SourceSha256 = $sourceHash
    Source = $source
    CopiedDisk = $copy
    BootResult = 'Not observed'
    InstallerResult = 'Not observed'
}
$record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'environment.json') -Encoding utf8
Start-VM -Name $vmName
Write-Output "Disposable Gen2 VM started: $vmName"
Write-Output "Evidence directory: $testRoot"
Write-Output 'Observe the VM console. A running VM is not a successful boot result.'
Write-Output 'Record bootloader versions, ISO hash, installer source release, installation into the planned space, and protected test-disk hashes before claiming support.'
Write-Output "After inspection: Stop-VM -Name $vmName; Remove-VM -Name $vmName"
