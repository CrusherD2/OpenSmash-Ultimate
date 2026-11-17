# Response to the review on #345

Keep Windows shipping the WinForms OpenSmash.exe until the owner accepts parity.
No legacy frontend or test deletion, shortcut switch, or production release workflow
change belongs in the additive app PR.

## Review units

1. Add the opt-in application, its artifact-only workflow, build helpers and tests.
   Use a distinct executable name; preserve current launchers, backends and tests.
2. Review the community/remap/settings ports, update compatibility, problem report,
   leaderboard pictures, renderer_menu and shared-setting refresh as follow-ups.
   These ports are implemented locally, but still need separate review units.
3. Only after acceptance and manual validation, switch defaults and shortcuts and
   remove old frontends. That final switch is intentionally not implemented yet.

The parity implementation is saved on feature/avalonia-parity, based on the
additive app branch. Keep it separate from the step-a PR; open parity follow-ups
only after the additive app is accepted.

## Independent fixes

- fix/glslang-older-toolset (.gg/wt/glslang-fallback): compatibility symbols only
  when needed; preserve main's existing CRT-library repair. Mock retry test passes;
  real glslang integration needs the external glslang dependencies.
- fix/plugin-rebuild-isolation (.gg/wt/plugin-rebuild-fixes): preserve original mod
  NROs, update generated copies and detect compiler-inlined hook targets. Plugin,
  compiler-hook and Skyline regression tests pass.
- #399 (training hot reload) and #400 (Vulkan wait budget) are already merged.
- Main already contains the macOS hot-reload compile stub from #399. Do not bring
  back the old branch's removal of it. Script relocations and shortcut changes
  are not standalone check fixes and stay out of these independent branches.

## Still required before replacing production

Owner review; real Windows old-install/no-SDK updates against release assets;
Linux/macOS installation and Play; real controllers; live Discord sign-in and
pictures; mod toggling; and the final small release/shortcut switch PR. Headless
and mocked tests cannot prove these hardware, network and shipping paths.
