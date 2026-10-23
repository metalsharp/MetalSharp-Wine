# Security policy

## Supported versions

Security fixes go into the latest release. Older releases are not patched;
update to the latest one.

| Version | Supported |
|---|---|
| 2.0 | Yes |
| 1.x (VKMT, `legacy-1.x`) | No |

## Reporting a vulnerability

Report vulnerabilities privately through GitHub: on this repository's
**Security** tab, choose **Report a vulnerability**. Do not open a public
issue, pull request or discussion for a vulnerability.

Include what you can of:

- the MetalSharp Wine version (`wine --version`) and which download you use
  (`MetalSharp-Wine.app` or the `.tar.xz` folder);
- the macOS version and Mac model;
- the steps or program that trigger it, and a crash log if there is one;
- what an attacker could achieve.

Do not send passwords, private keys, personal data, whole Wine prefixes or
large archives; the release `SHA256SUMS` identify the files.

You will get a reply within 7 days. Once the report is confirmed, a fix is
prepared privately and released, and the advisory is published with credit to
you unless you would rather stay anonymous.

## Scope

MetalSharp Wine runs Windows programs with the user's own privileges. Wine is
not a sandbox, and a Windows program can do whatever a Mac program run by the
same user can. That is by design and not a vulnerability. In scope:

- the MetalSharp changes to Wine (`patches/wine`, branch `source/wine`) and
  FEX (`patches/fex`, branch `source/fex`): memory-safety bugs, or anything that
  lets one process or prefix reach beyond what Wine itself allows;
- the signed app: its entitlements, its signature and notarization, and
  anything that lets other code load into it or change it;
- the build, staging, signing and release scripts, and the release assets
  (a file that does not match `SHA256SUMS`, a dependency fetched without a
  checksum);
- the bundled components as MetalSharp Wine builds or configures them.

Out of scope, and better reported where they belong (tell us as well if
MetalSharp Wine is affected):

- **Wine itself**: [WineHQ](https://www.winehq.org/).
- **FEX itself**: [FEX-Emu](https://github.com/FEX-Emu/FEX).
- **Mullion / DXMT**: [dttdrv/mullion](https://github.com/dttdrv/mullion).
- **WineMetalGL**: [metalsharp/WineMetalGL](https://github.com/metalsharp/WineMetalGL/security).
- **The MetalSharp app**: [metalsharp/MetalSharp](https://github.com/metalsharp/MetalSharp/security).
- **Other bundled components** (MoltenVK, DXVK, FFmpeg, GStreamer, GnuTLS,
  Wine Mono, Wine Gecko and the rest in `share/licenses`): their upstream
  projects.
