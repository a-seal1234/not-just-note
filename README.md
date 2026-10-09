# Not Just Note

**Write · Draw · Annotate · Focus**

Not Just Note is an Android note-taking and PDF annotation app for focused work on a
tablet. It combines Krita's drawing canvas with a workspace for handwritten notes and
multi-page PDF notebooks.

This is an independently maintained Krita fork. The current release candidate targets
Android tablets; the source also retains Krita's desktop builds for Linux and Windows.
It began as a personal-use project and is maintained as time allows, so updates may be
infrequent and there is no fixed release schedule. It is not an official Krita or KDE
release and is not endorsed by the Krita Foundation.

## What you can do

- Write and draw freely with a stylus on the canvas.
- Import PDFs as notebooks and annotate their pages with ink layers.
- Manage notebook pages: insert pages from PDFs, add blank or image pages, reorder,
  duplicate, rotate, delete, resize, and merge pages or notebooks.
- Use the app's tablet workspace and light or dark theme.

## Platforms

The current `0.1.7-rc1` release candidate is an Android arm64 APK. The shared drawing and
PDF notebook code is also part of the desktop builds, so Linux and Windows remain build
targets for this source tree. The Linux desktop build and shared desktop code have been
validated in development, including the Not Just Note desktop identity. A Windows Actions
workflow is available for a self-hosted Windows runner with the LLVM/clang-cl 21 and Visual
Studio build toolchain; no Windows package has yet been produced or validated. No packaged
desktop release is currently provided.
The Xiaomi Focus Pen Pro integration and tablet-specific shell are Android-only.

## Xiaomi Focus Pen Pro

On the Xiaomi Pad 8, Not Just Note connects to Xiaomi's built-in pen service and supports
the Focus Pen Pro's squeeze, double-press, slide-up, slide-down, and barrel-rotation
inputs. The four gesture actions can be changed under **Settings → Configure Krita → Pen**.

Default gesture actions are:

- **Squeeze:** open the popup palette
- **Double press:** erase
- **Slide up / down:** increase / decrease brush size
- **Barrel rotation:** drive brush angle; the Pen settings can enable this for every
  brush preset and reverse its direction

Enable each gesture in the tablet's own pen settings first. Rotation can be enabled
globally in the app's Pen settings, or per brush preset with Krita's Rotation sensor.
The integration reads from the Xiaomi system service already on the device; no Xiaomi
SDK is bundled in the APK. Hardware-specific behavior has been verified on Xiaomi Pad 8
with Focus Pen Pro; other devices have not been verified. Xiaomi's haptic feedback and
hover preview are system features and are not app features.

## Project status

- **Current candidate:** `0.1.7-rc1`
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
