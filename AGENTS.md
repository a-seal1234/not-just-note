# Agent guidance for the Not Just Note Krita fork

This checkout is based on Krita commit `9a24ae0d8c070884fb013594191002e1a1b39a19` (2026-09-18). Keep fork work easy to review and, where intended, easy to send upstream.

## Authority and precedence

- Follow repository-boundary, preservation, and upstream-write restrictions in the parent `../AGENTS.md` as well as any higher-level instructions.
- Explicit requirements in the current task determine the requested behavior and scope.
- For code intended to remain close to or be submitted upstream, follow the matching upstream `HACKING`, `.clang-format`, `.clang-tidy`, and contribution template linked below. These define upstream code and review expectations.
- This file adds fork architecture and maintainability rules. For new or changed code, upstream conventions take precedence over incidental local style differences. In untouched legacy code, preserve surrounding style; do not reformat it just to make it uniform.
- If a task explicitly requires a deliberate deviation, keep it scoped, explain it in the change, and preserve upstream behavior elsewhere.

## Upstream references

Use the versions in this checkout as the source of truth; the links below pin the upstream base revision for review:

- [HACKING](https://invent.kde.org/graphics/krita/-/blob/9a24ae0d8c070884fb013594191002e1a1b39a19/HACKING) — Krita-specific coding rules.
- [`.clang-format`](https://invent.kde.org/graphics/krita/-/blob/9a24ae0d8c070884fb013594191002e1a1b39a19/.clang-format) and [`.clang-tidy`](https://invent.kde.org/graphics/krita/-/blob/9a24ae0d8c070884fb013594191002e1a1b39a19/.clang-tidy) — formatter and static-analysis configuration.
- [Merge request template](https://invent.kde.org/graphics/krita/-/blob/9a24ae0d8c070884fb013594191002e1a1b39a19/.gitlab/merge_request_templates/merge_request_template.md), [contribution notes](https://invent.kde.org/graphics/krita/-/blob/9a24ae0d8c070884fb013594191002e1a1b39a19/.github/CONTRIBUTING.md), and [README](https://invent.kde.org/graphics/krita/-/blob/9a24ae0d8c070884fb013594191002e1a1b39a19/README.md) — build, runtime, test, licensing, review, and current project policy. Krita development and reviews happen on [KDE Invent](https://invent.kde.org/graphics/krita), not GitHub pull requests.
- KDE’s [Frameworks C++ style](https://community.kde.org/Policies/Frameworks_Coding_Style), [CMake style](https://community.kde.org/Policies/CMake_Coding_Style), and [commit policy](https://community.kde.org/Policies/Commit_Policy) are referenced by upstream guidance.

## Coding conventions

- **C++ and formatting:** Use four spaces, the checked-in `.clang-format`, and the existing project language/build settings. Avoid `auto` when it hides a type; use it mainly for iterators/range loops or template-dependent types. Use `nullptr` in new code. Keep includes out of headers when forward declarations suffice; use initializer lists and the upstream member initialization style.
- **Qt:** Prefer Qt types and established Krita/Qt APIs. Use `Q_FOREACH` for Qt containers and range-based loops for STL containers. Match the file’s signal/slot syntax; do not introduce the new connection syntax piecemeal, and prefer ordinary functions over unnecessary lambdas.
- **Naming:** Use `camelBack` for functions and local/parameter names, `m_` only for class members, and `x()` / `setX(...)` for accessors. Use `Kis` names for library classes/files; plugin filenames need not use that prefix. Do not introduce new `Ko` classes. Follow existing `slot...` and `sig...` prefixes.
- **Ownership:** Make ownership and lifetime explicit. Follow the smart-pointer guidance in the checked-in `HACKING`: use Krita/Qt shared-pointer types consistently; reserve `QScopedPointer` for the documented simple d-pointer case and use `std::unique_ptr` for other unique ownership. Avoid raw owning pointers.
- **CMake:** Follow the KDE CMake style and the nearest `CMakeLists.txt`: consistent command casing, spaces (no tabs), and empty closing commands such as `endif()`. Prefer existing target-scoped dependency and test-registration patterns; avoid global flags or unrelated build-system changes. For optional Qt patch APIs, use the feature-detection approach documented in `HACKING`.
- **Tests and review:** For a bug fix, add or update a focused regression test when practical. Before an upstream MR, build Krita, run it through the affected behavior, and run relevant tests; describe any checks that could not be run. Keep each commit buildable with a clear message, preserve licensing/attribution, and prepare documentation changes for user-visible features.
- **Python:** Follow PEP 8 as directed by upstream `HACKING`; its 79-character line limit is optional where it harms readability.

## Fork Architecture & Upstream Merge Policy

- Prefer additive changes over modifying existing upstream behavior.
- Prefer standalone modules, extensions, and adapters.
- Reuse existing Krita infrastructure before implementing replacements.
- When core modifications are necessary, keep them minimal and isolated.
- Avoid unrelated refactoring and repository-wide formatting changes.
- Preserve compatibility with upstream behavior unless the task explicitly requires otherwise.
- Keep fork-specific modifications identifiable and independently maintainable, with clear boundaries and descriptions.

## Source conflicts and time-sensitive rules

- The older online Modern C++ guide conflicts with this checkout’s `HACKING` on `nullptr` and smart-pointer choices. Follow the matching checkout’s `HACKING`; do not apply the older page over it.
- `.clang-tidy` enables broad modernization checks, including checks that can suggest style changes. Treat diagnostics as review input, not permission for automatic or repository-wide fixes; where a suggestion conflicts with `HACKING`, follow `HACKING`.
- The project CMake configuration requires C++17 while `.clang-format` sets its formatting parser to C++14. The build configuration governs language availability; retain both upstream settings and do not use this difference to trigger unrelated edits.
- The matching upstream `README.md` states an AI-development moratorium through October 2026. Recheck the current upstream policy before preparing any upstream contribution; this time-limited notice may change.
