# SporeCoop — Local Multiplayer Mod (Alpha 1)

SporeCoop is an experimental local co-op mod for SPORE. It is designed to let two SPORE instances on the same PC share a game session, with a future LAN/Radmin VPN mode planned for remote players.

The project is currently **Alpha 1**. It is a development and testing build, not a finished release. Crashes, visual differences, desynchronisation, and unsupported stages are still possible.

## What the mod is for

The mod explores how two players can experience the same SPORE world together. The host and guest instances communicate through a small TCP session server and exchange gameplay state.

Current experimental goals include:

- inviting a second player from the in-game pause menu;
- accepting an invitation and joining the host's saved world;
- displaying a network clone of the other player;
- synchronising creature appearance, position, scale, and species data;
- sharing cell-stage progress, food progress, unlocked parts, and editor state;
- running two isolated SPORE profiles and two ModAPI instances on one PC;
- providing diagnostic console commands such as `coopStatus`, `coopSpawn`, and `coopJoin`.

## Alpha 1 limitations

This version has not been validated across every SPORE stage. The protocol and server are tested automatically, but the tests do not drive the SPORE game engine. Creature-stage transitions, tutorials, quests, editor transitions, and unusual save states may still cause crashes or incomplete synchronisation. Remote play over Radmin VPN/ZeroTier/Hamachi is prepared at the protocol level, but complete world transfer and reliable cross-machine testing are not finished.

## Requirements

- Windows and a working SPORE installation;
- SPORE ModAPI Launcher Kit;
- the same mod build in both game instances;
- PowerShell 5.1 or newer;
- Visual Studio build tools if rebuilding the native DLL.

## Running two local instances

Use:

```powershell
.\Start-TwoSpore.ps1
```

This creates/uses the isolated second profile at `%AppData%\SporeCoop2`, installs the latest DLL in both Launcher Kit folders, starts the session server, and launches host and guest instances. The desktop shortcut `SPORE Coop - 2 окна` runs the same workflow.

The host loads a saved world and uses the pause menu's co-op invitation. The guest accepts the invitation; the guest profile then attempts to load a fresh copy of the host world. `coopJoin` remains available as a diagnostic fallback. `coopSpawn` only creates a manual diagnostic clone and is not required for normal joining.

## Testing

Run the protocol tests with:

```powershell
.\Test-Server.ps1
node .\tests\native-source.test.mjs
```

To rebuild the native probe DLL:

```powershell
.\Build-Probe.ps1 -LauncherRoot 'C:\ProgramData\SPORE ModAPI Launcher Kit'
```

When reporting a problem, include `%TEMP%\SporeCoop.Probe.log` and the exact last action. Useful reports include crashes during growth, duplicate clones, mismatched colours/parts, failed invitations, or quests that do not update for both players.

## Repository status

This repository contains the mod source, session server, launch scripts, tests, and the Spore ModAPI headers used to build it. Generated binaries, local save data, registry backups, and temporary build output are intentionally excluded.

SporeCoop is an independent community project and is not affiliated with or endorsed by Electronic Arts or Maxis.
