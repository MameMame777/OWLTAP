---
title: LGPL Distribution Policy
tags: [licensing, legal, distribution, lgpl]
date: 2026-04-20
category: Project/Policy
status: Draft
---

# LGPL Distribution Policy

This document is a project policy note, not legal advice.

## Current Facts

- The repository root is licensed under MIT.
- Dear ImGui and ImPlot are MIT licensed.
- GLFW uses the zlib/libpng license.
- `libftdi` is LGPL-2.1-only.
- `libusb` is LGPL v2.1 or later.
- Linux builds link system `libftdi1` and `libusb-1.0`.
- Windows builds currently compile vendored `libftdi` and `libusb` source.

## Project Policy

### 1. Public Source Release

Allowed, with these conditions:

- Do not publish vendor-proprietary BSDL files.
- Keep BSDL/BSD files ignored unless their redistribution terms are verified.
- Keep third-party notices in the repository.
- Include the LGPL 2.1 and GPL v2 license texts in the repository for source-only release.

### 2. Linux Binary Distribution

Conditionally allowed, with these requirements:

- Prefer dynamic/system linking to LGPL libraries.
- Include license texts and notices for LGPL components.
- Preserve the user's ability to replace the LGPL-covered libraries.
- Document the exact external library dependencies used by the build.

### 3. Windows Binary Distribution

Not approved under the current build layout.

Reason:

- The current Windows build compiles vendored `libftdi` and `libusb` source into the build output.
- That creates additional LGPL compliance work which is not yet implemented or documented.

Before shipping Windows binaries, do one of the following:

1. Switch to dynamic linking with separately replaceable LGPL libraries.
2. Prepare a distribution process that satisfies LGPL obligations for the shipped binaries.

## Practical Release Rule

Until the Windows LGPL path is resolved, the safe default is:

- Public source release: yes.
- Linux binaries: only after packaging review.
- Windows binaries: no.

## Release Checklist

- Confirm no proprietary BSDL/BSD files are included.
- Include `LICENSE` and `THIRD_PARTY_NOTICES.md`.
- Include `THIRD_PARTY_LICENSES/LGPL-2.1.txt`.
- Include `THIRD_PARTY_LICENSES/GPL-2.0.txt`.
- Include the relevant LGPL license texts when distributing binaries.
- Re-check `libftdi` and `libusb` linkage mode for the target platform.
- Re-review this policy before the first public binary release.