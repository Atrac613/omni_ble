# Release Checklist

Use this checklist when publishing a new `omni_ble` release to both GitHub and
pub.dev.

## Before You Tag

1. Update the version in `pubspec.yaml`, `ios/omni_ble.podspec`, and
   `macos/omni_ble.podspec`.
2. Add release notes to `CHANGELOG.md`.
3. Make sure `README.md`, `LICENSE`, and repository metadata still match the
   package status and supported platforms.
4. Confirm the validated scenarios in
   [validated_support_matrix.md](validated_support_matrix.md) are accurate for
   the release you are about to publish.

## Verification

Run the standard release verification set:

```bash
flutter analyze
flutter test
cd example && flutter test
cd example && flutter test integration_test -d macos -r compact
cd example && flutter build apk --debug
cd example && flutter build macos
dart pub publish --dry-run
```

If the release touched a specific native backend, prefer running an additional
host-side build or smoke test on that target before publishing.

## GitHub Release

1. Ensure `git status` is clean.
2. Commit the release-prep changes.
3. Tag the version, for example `v0.1.0`.
4. Push the branch and tag to GitHub.
5. Create a GitHub release whose title matches the tag and whose notes are
   based on the matching `CHANGELOG.md` section.

## pub.dev Release

1. Run `dart pub publish --dry-run` from a clean working tree and confirm there
   are no warnings you want to address first.
2. Run `dart pub publish` when the package contents and version are final.
3. If this is the first upload for the package, publish from the Google account
   that should become the initial uploader.
4. If the package should live under a verified publisher later, transfer it
   after the first successful upload.

## After Publishing

1. Verify the new version page on pub.dev.
2. Verify the GitHub release page shows the same version and notes.
3. If needed, update any downstream examples, announcements, or validation
   docs that should point at the new public release.
