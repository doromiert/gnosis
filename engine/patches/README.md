# CEF source fixes

`cef-directory-picker.patch` targets CEF commit `c1a097e`, used by the pinned
150.0.22 SDK. It preserves folder selection when Chromium's File System Access
API reaches CEF's native dialog handler. The original conversion handles upload
folders but drops both ordinary folder modes, reporting an open-file request.

The patch applies to the pinned source. A real `showDirectoryPicker()` gesture
was observed arriving in `Browser::OnFileDialog` as `FILE_DIALOG_OPEN` (0),
and opening a GTK file chooser. This is separate from the upload confirmation
crash triggered by `<input webkitdirectory>`.

This source patch is **not applied to the prebuilt SDK by package.nix**. It needs
a rebuilt CEF binary and an end-to-end directory-handle/portal regression before
the directory-picker issue can be marked fixed. The running app still has it.
