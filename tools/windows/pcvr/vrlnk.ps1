$w = New-Object -ComObject WScript.Shell
foreach ($p in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) {
  $l = $w.CreateShortcut((Join-Path $p 'LAS Recomp VR.lnk'))
  $l.TargetPath = 'F:\LAS Recomp VR\lasrecomp.exe'; $l.Arguments = 'extracted --play --vr'
  $l.WorkingDirectory = 'F:\LAS Recomp VR'; $l.IconLocation = 'F:\LAS Recomp VR\lasrecomp.exe,0'; $l.Save()
  '{0}: {1} {2}' -f (Join-Path $p 'LAS Recomp VR.lnk'), $l.TargetPath, $l.Arguments
}
