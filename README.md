<p align="center">
  <img src="store/cover.png" alt="wxl-modern-m2" width="640">
</p>

# wxl-modern-m2

[Build compatibility and release gate](BUILDING.md)

**Loads models authored for newer versions of the game, natively.**

A [WarcraftXL](https://github.com/WarcraftXL/wxl-core) extension. The client's model loader only
understands the format it originally shipped with; a model authored later isn't rejected politely, it
simply can't be read at all. This module reads that newer file **directly** and fills the client's own
model runtime with it, with no conversion step, no intermediate copy on disk, and no version rewrite.
The model stays exactly what it is; the client just ends up holding it in the shape it always expected.

See [`store/description.md`](store/description.md) for the full write-up (what gets fixed on the way in,
what's intentionally out of scope for now, and the safety/interface contract).

## Highlights

- **Direct native read**: no more host-side pre-conversion
- **Record normalization**: particle emitters and cameras that grew wider over time are rewritten in
  place, driven by a small table (adding a new record type is one entry, not a new code path).
- **Bone-budget splitting**: rigs with more bones than the client can draw in one pass are partitioned
  into pieces that each fit, instead of silently clamped and deformed.
- **Shadow-camera fix**: detects and corrects the bone-flag combination that otherwise sends the shadow
  pass down a path that never refreshes the pose.
- **Effect and hit-test fixes**: particle/ribbon blending and triangle hit-testing corrected so effects
  render as intended and clicking selects the right thing.
- **Extended sequence resolution**: modern animation IDs above the Wrath AnimationData ceiling resolve against the loaded M2, while an optional movement extension can own the final result through the versioned `wxl.m2-animation` interface.
- **Input validation**: unsupported or malformed input is rejected in the validated loader paths. Runtime smoke testing is still required.

## Requirements

WarcraftXL on a 3.3.5a client, build 12340. The module refuses to load against anything else rather than
guessing, and says so in the log.

## Building

This source snapshot follows WXL integration commit `50c2f0d`. Build it with the exact core revision in [BUILDING.md](BUILDING.md), Modern BLP support, and intended M2/DB2 data. Pull requests and `main` run a Win32 build; only an explicit version tag can publish a release. Package contents and the runtime route still need validation before tagging.

## License

GPL-3.0-or-later. See the license header in every source file.

## Integration and release checks

Build `wxl-modern-m2` as a Win32 Release target against the exact core and shared M2/BLP source APIs used by this snapshot. The current workflow stages only the DLL; `wxl-modern-m2.cfg`, modern M2/skin/skeleton files, textures, and compatible DB2 catalogs are separate reviewed inputs. The modern loader can be evaluated without replacing the original Wrath model files; retail equipment and spell presentation additionally need matching data and provider services.

Test one original Wrath model and one optional HD model, then inspect body textures, helmet/hair geosets, draw/sheath, shadows, and particle/ribbon effects. Run a Dalaran/Stormwind return route and inspect crash/memory logs. Keep a full DLL/config/asset rollback. The pinned clean-checkout target compiles, but this large source snapshot still needs staged review, runtime acceptance, and reconciliation with the existing WarcraftXL Modern M2 PR before release.

## Credits

The WXL core ABI and original module interfaces come from WarcraftXL contributors. The local v1.1 integration commits in this snapshot are attributed to Furioz in the integration history. Preserve source-file notices and the GPL-3.0-or-later `LICENSE` when redistributing source or binaries.
