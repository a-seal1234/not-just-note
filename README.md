# Not Just Note

**Write · Draw · Annotate · Focus**

Not Just Note is an Android note-taking and PDF annotation app for focused work on a
tablet. It combines Krita's drawing canvas with a workspace for handwritten notes and
multi-page PDF notebooks.

This is an independent, Android-focused Krita fork. It is not an official Krita or KDE
release and is not endorsed by the Krita Foundation.

## What you can do

- Write and draw freely with a stylus on the canvas.
- Import PDFs as notebooks and annotate their pages with ink layers.
- Manage notebook pages: insert pages from PDFs, add blank or image pages, reorder,
  duplicate, rotate, delete, resize, and merge pages or notebooks.
- Use the app's tablet workspace and light or dark theme.

## Project status

- **Current candidate:** `0.1.6-rc1`
- **Device tested:** Xiaomi Pad 8 with Xiaomi Focus Pen Pro
- **Test build:** Android arm64 debug APK (`com.njn.debug`). This is a development build,
  not the public release package.
- **Release package ID:** `com.njn`

The app is in early testing. It has been tested primarily on the Xiaomi Pad 8; other
Android devices have not been verified. Encrypted or password-protected PDFs are not
supported. Large notebooks can use substantial memory, so keep a backup of important work.

## Download

There is no public installable release yet. When the first release is ready, its signed
APK and installation notes will be published on the
[GitHub Releases page](https://github.com/pongkhunwuttijarat-droid/not-just-note/releases).

The release notes will identify the source revision, Android ABI, package ID, signing
status, and upgrade considerations. Builds signed for direct distribution can only be
updated by a build signed with the same release key.

## Source and upstream

The active product work is on the [`pen-pdf` branch](https://github.com/pongkhunwuttijarat-droid/not-just-note/tree/pen-pdf).
This project is based on [Krita](https://krita.org/), whose source is maintained by the
[Krita community on KDE Invent](https://invent.kde.org/graphics/krita). This repository
contains fork-specific Android, stylus, workspace, and PDF notebook changes.

Krita's name and logo are used only to identify the upstream project. “Krita” and its
logo are trademarks of the Krita Foundation. Not Just Note is independently maintained
and is not affiliated with or endorsed by KDE or the Krita Foundation. See the
[Krita trademark policy](https://krita.org/en/posts/2013/krita-trademark-policy/).

## License

Krita is licensed under the GNU General Public License, version 3. Individual files and
bundled components may have different compatible licenses. See [`COPYING`](COPYING) and
[`LICENSES/`](LICENSES/) for the notices that apply to this source tree.
