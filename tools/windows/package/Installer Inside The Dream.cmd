@echo off
rem Inside The Dream : les raccourcis (Bureau et menu Demarrer) vers ce dossier, puis l'ecran d'accueil du jeu.
rem Le dossier peut etre place n importe ou : relancer ce fichier apres l avoir deplace.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d = (Get-Location).Path; $w = New-Object -ComObject WScript.Shell; foreach ($p in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) { $o = Join-Path $p 'LAS Recomp.lnk'; if (Test-Path $o) { Remove-Item $o }; $l = $w.CreateShortcut((Join-Path $p 'Inside The Dream.lnk')); $l.TargetPath = (Join-Path $d 'lasrecomp.exe'); $l.Arguments = 'extracted'; $l.WorkingDirectory = $d; $l.IconLocation = (Join-Path $d 'lasrecomp.exe') + ',0'; $l.Save() }"
start "" "%~dp0lasrecomp.exe" extracted
