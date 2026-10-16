param([Parameter(Mandatory)][string]$Driver)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Output 'SKIP: VHD integration requires an elevated process. No disk was changed.'
    exit 77
}
if (!(Get-Command New-VHD -ErrorAction SilentlyContinue)) {
    Write-Output 'SKIP: Hyper-V VHD tools are unavailable. No Windows feature was enabled.'
    exit 77
}
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('diskboot-vhd-' + [guid]::NewGuid().ToString('N'))
$vhdPath = Join-Path $testRoot 'target.vhdx'
New-Item -ItemType Directory -Path $testRoot | Out-Null
$attached = $false
$success = $false
function Assert-VirtualTarget($Disk) {
    $mapping = Get-VHD -Path $vhdPath
    if (!$mapping.Attached -or $mapping.DiskNumber -ne $Disk.Number -or $Disk.IsBoot -or $Disk.IsSystem) { throw 'Virtual disk mapping or Windows protection check failed' }
    if ($Disk.BusType -notin @('File Backed Virtual', 'Virtual')) { throw 'Refusing nonvirtual test target' }
    $fresh = Get-Disk -Number $mapping.DiskNumber
    if ($fresh.UniqueId -ne $Disk.UniqueId -or $fresh.Path -ne $Disk.Path -or $fresh.Size -ne $Disk.Size) { throw 'Virtual disk identity changed' }
}
try {
    New-VHD -Path $vhdPath -SizeBytes 40GB -Dynamic -LogicalSectorSizeBytes 512 -PhysicalSectorSizeBytes 4096 | Out-Null
    Mount-VHD -Path $vhdPath -NoDriveLetter | Out-Null
    $attached = $true
    $disk = Get-Disk -Number (Get-VHD -Path $vhdPath).DiskNumber
    Assert-VirtualTarget $disk
    $disk | Initialize-Disk -PartitionStyle GPT
    $disk = Get-Disk -Number (Get-VHD -Path $vhdPath).DiskNumber
    if ($disk.PartitionStyle -ne 'GPT') { throw 'GPT initialization was not verified' }
    Assert-VirtualTarget $disk
    $partition = New-Partition -DiskNumber $disk.Number -Size 32GB -AssignDriveLetter
    $actual = Get-Partition -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber
    if ($actual.Size -ne 32GB -or $actual.Guid -ne $partition.Guid) { throw 'Test partition creation mismatch' }
    Assert-VirtualTarget $disk
    $partition | Format-Volume -FileSystem NTFS -NewFileSystemLabel 'DiskBoot test data' -Confirm:$false | Out-Null
    $volume = $partition | Get-Volume
    if ($volume.FileSystem -ne 'NTFS' -or $volume.FileSystemLabel -ne 'DiskBoot test data') { throw 'NTFS test setup verification failed' }
    $sentinel = "$($partition.DriveLetter):\sentinel.bin"
    [IO.File]::WriteAllBytes($sentinel, [Text.Encoding]::UTF8.GetBytes('DiskBoot data preservation sentinel'))
    $sentinelHash = (Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash
    $guid = $partition.Guid.ToString().Trim('{}').ToLowerInvariant()
    $driverOutput = & $Driver $vhdPath $guid (Join-Path $testRoot 'journal.json')
    if ($LASTEXITCODE -ne 0) { throw "Native integration driver failed: $driverOutput" }
    $driverOutput | Set-Content -LiteralPath (Join-Path $testRoot 'result.json') -Encoding utf8
    if ((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $sentinelHash) { throw 'Data sentinel changed' }
    $after = Get-Partition -DiskNumber (Get-VHD -Path $vhdPath).DiskNumber
    $source = @($after | Where-Object { $_.Guid.ToString().Trim('{}').ToLowerInvariant() -eq $guid })
    if ($source.Count -ne 1 -or $source[0].Size -ne 16GB) { throw 'Exact source shrink was not verified' }
    if (@($after | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' }).Count -ne 1) { throw 'Test ESP missing' }
    Write-Output 'PASS: VHDX mapping, NTFS shrink, new FAT32 ESP, staging partition, range verification, and preserved data sentinel. No boot claim.'
    $success = $true
} finally {
    if ($attached) { Dismount-VHD -Path $vhdPath }
    if ($success) {
        $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
        $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (!$resolvedTestRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or !(Split-Path $resolvedTestRoot -Leaf).StartsWith('diskboot-vhd-')) { throw 'Cleanup path is outside test scope' }
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    } else { Write-Output "Failure evidence retained at $testRoot" }
}
