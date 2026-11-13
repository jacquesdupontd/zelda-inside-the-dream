@echo off
rem Inside The Dream straight into a PC VR headset (OpenXR: SteamVR with Steam Link, Quest Link / Air Link, Virtual
rem Desktop...), without the start screen - which has PLAY IN VR too. Set up the game files once on the start screen first.
cd /d "%~dp0"
start "" "%~dp0lasrecomp.exe" extracted --play --vr
