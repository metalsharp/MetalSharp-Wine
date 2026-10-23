## Summary

<!-- What changes and why. Link the issue it fixes, if any. -->

## Testing

<!-- Tick what you ran and note the results. -->

- [ ] Built with `scripts/build-all.sh` (or the steps the change touches)
- [ ] Smoke tests for the change, on each architecture it covers (ARM64, x86_64, i386)
- [ ] Wine conformance units, for changes to ntdll, the loader, wineserver or sync (say which)
- [ ] The signed app (`scripts/sign-app.sh`), for i386 or for changes to signing or entitlements

## Checklist

- [ ] `patches/` matches the `source/*` branch commits, and the submodule pin on `main` is moved
- [ ] Nothing depends on Rosetta or hardware TSO
- [ ] New downloads are pinned and checked against a SHA-256
- [ ] No prefixes, user registries or other personal data
