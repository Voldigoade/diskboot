param([Parameter(Mandatory)][string]$VMName, [Parameter(Mandatory)][string]$Output)
$ErrorActionPreference = 'Stop'
$vm = Get-VM -Name $VMName
if (!$vm.Name.StartsWith('DiskBoot-') -or $vm.Generation -ne 2) { throw 'Capture is restricted to DiskBoot Gen2 laboratory VMs' }
$scope = 'root\virtualization\v2'
$system = Get-WmiObject -Namespace $scope -Class Msvm_ComputerSystem -Filter "Name='$($vm.Id)'"
$settings = @($system.GetRelated('Msvm_VirtualSystemSettingData', 'Msvm_SettingsDefineState', $null, $null, 'SettingData', 'ManagedElement', $false, $null))
if ($settings.Count -ne 1) { throw 'Ambiguous active VM settings' }
$service = Get-WmiObject -Namespace $scope -Class Msvm_VirtualSystemManagementService
$arguments = $service.GetMethodParameters('GetVirtualSystemThumbnailImage')
$arguments.TargetSystem = $settings[0].Path.Path
$arguments.WidthPixels = 800
$arguments.HeightPixels = 600
$result = $service.InvokeMethod('GetVirtualSystemThumbnailImage', $arguments, $null)
if ($result.ReturnValue -ne 0 -or $result.ImageData.Length -ne 800*600*2) { throw 'VM framebuffer capture failed' }
[IO.File]::WriteAllBytes([IO.Path]::GetFullPath($Output), $result.ImageData)
