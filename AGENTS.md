# AGENTS.md

## Purpose

This repository contains `omni_ble`, a Flutter plugin that aims to provide a
single Dart-first Bluetooth LE API for central and peripheral roles across
iOS, macOS, Android, Windows, and Linux.

## Repository Map

- `lib/`: public Dart API, shared models, platform interface, and method-channel bridge
- `android/`, `ios/`, `macos/`, `windows/`, `linux/`: native plugin implementations
- `example/`: example app and integration coverage
- `test/`: Dart unit tests for the plugin API and method-channel encoding

## Working Rules

1. Keep the Dart API platform-agnostic whenever possible.
2. When adding a new public API, update `lib/`, the relevant native backends, tests, `README.md`, and the example app in the same change.
3. Do not claim support for a platform feature unless the native implementation exists and verification has been run for that target.
4. Prefer small, additive changes over broad rewrites, especially in the native plugin layers.
5. Treat Android runtime permissions as part of the API surface, not just app-level setup.

## Verification

Run the smallest relevant set first, then expand when the change touches public API or native code.

```bash
flutter analyze
flutter test
cd example && flutter test
cd example && flutter test integration_test
cd example && flutter build apk --debug
```

Use macOS or iOS builds when touching Apple native code, and prefer platform-specific validation for the backend you changed.

## Commit Style

Use English Conventional Commits.
This repository intentionally does not use a checked-in Git commit template file.

Format:

```text
<type>(<scope>): <imperative summary>
```

Rules:

1. Keep the summary short, imperative, and lowercase where natural.
2. Do not end the summary with a period.
3. Prefer a concrete scope such as `android`, `ios`, `macos`, `linux`, `windows`, `api`, `docs`, `example`, or `tests`.
4. Use `!` only for breaking changes.
5. Add a body when the reason or validation would not be obvious from the diff.

Recommended types:

- `feat`
- `fix`
- `docs`
- `refactor`
- `test`
- `build`
- `ci`
- `chore`

Examples:

- `feat(api): add runtime BLE permission API`
- `fix(android): guard permission requests without an activity`
- `docs(readme): document cross-platform permission behavior`
- `test(method-channel): cover permission payload encoding`

## Commit Body Template

When a body helps, keep it compact and use sections like these:

```text
Why:
- explain the problem or motivation

What:
- summarize the main change

Validation:
- list the commands you ran
```

## Before You Commit

1. Make sure generated or local-only artifacts are not accidentally staged.
2. Re-read the commit subject and ensure it describes the user-visible outcome.
3. Include verification in the body when native code or public API changed.
