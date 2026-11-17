# Experimental Avalonia launcher (step a)

An opt-in app alongside the shipping Windows and web launchers, not a replacement.
Existing release workflows, installers, shortcuts and frontend tests are unchanged.

Run with the .NET 8 SDK:

```
dotnet run --project OpenSmash.Avalonia/OpenSmash.csproj
```

Publish an experimental package:

```
python tools/publish_avalonia.py --rid win-x64
```

Use `linux-x64` or `osx-arm64` on those platforms. Packages go under
`dist/avalonia/`. The executable is `OpenSmash.Avalonia`, not `OpenSmash`.
To explicitly install that experimental binary into your checkout, run
`python tools/build_avalonia.py --rid win-x64`.

## Parity ports

- #377/#320: community, Discord account/presence, controller remapping and game-default settings.
- #383/#420/#427: stable release channel, legacy WindowSmash commit mapping, validated
  installer refresh before `-UpdateOnly`, and the existing installer's checksum-verified
  production launcher update. Production release workflows remain unchanged.
- #431: Report a problem opens the bug form with the game version and the build log folder.
- #432: Discord leaderboard pictures, bounded downloads, trusted CDN only, initials fallback.
- #435/#438: shared SettingDefaults source, renderer_menu compatibility, changed-only writes,
  and refresh when the game's settings change while the launcher stays open.

The desktop suite covers these ports; old frontend tests remain and still run independently.
The experimental executable never downloads a production executable over itself: updating the
shipping Windows launcher is still the existing installer's job. An experimental source
build can rebuild itself; there is no published experimental self-update asset yet.

## Landing order and manual validation

Submit the additive app/workflow first, then the parity ports for review. Only after
owner acceptance should a final PR change shipping workflows/shortcuts and remove old
frontends. That final switch is deliberately not part of this branch.

Still required on real systems: install then Play; enable/disable a mod; update an old
archive; real-controller remapping on Linux/macOS; Discord sign-in/avatar downloads;
and a no-SDK Windows update with published release assets. Headless/mock checks do not
substitute for those runs. No claim of complete parity or release readiness is made.

Compatibility setup aliases are additive: the layout helper never removes old
entry points or rewrites existing installer backends. Deployment uses its own
executable name and leaves the shipping Windows launcher untouched.
