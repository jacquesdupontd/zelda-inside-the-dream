# Install on Mac

## Install

- Download `InsideTheDream-v0.1.0-macos.dmg`, open it, and drag Inside The Dream into Applications.
- The first time you open it, macOS will probably refuse, because the app doesn't come from the App Store. Open System
  Settings, go to Privacy & Security, scroll down, and click Open Anyway.

On an Apple Silicon Mac with a Retina screen, we recommend setting Internal resolution to 50% in Settings > Video.

## The first launch

The first time you open Inside The Dream, it shows a page called **Set up the game**:
1. Click **Choose the folder** and pick the folder with your `exefs` and `romfs`.
2. Click **Set up the game**.

The app copies what it needs from your files, then rebuilds the game's code for your own computer. This only happens once,
and it takes a few minutes: about seven on a recent desktop PC, a bit more on a laptop. Your own files are never changed.
When it's done, the start screen lets you play.

To play in VR, turn your headset on and start SteamVR first. Then click **PLAY IN VR** on the start screen. That is
Windows only.

## Your saves and settings

Updating or reinstalling the app keeps your saves. They live in the `save` folder, inside the app's data folder:
- on Windows, the app's own folder, which is `%LOCALAPPDATA%\Programs\Port0r\InsideTheDream` if you used the installer. If
  you use the zip, unzip new versions into the same folder;
- on Linux, `~/.local/share/lasrecomp`;
- on a Mac, `~/Library/Application Support/com.peiikai.lasrecomp`.
