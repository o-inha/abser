# Abser

A small, portable Windows x64 audio browser. Open a folder and audition its audio files with a click or the arrow keys. Designed for a tall, narrow window beside your DAW.

## Run

Extract the ZIP and launch **Abser.exe**. No installer, administrator access, or separate runtime is required. Keep the extracted folder wherever you prefer.

Supported audio: WAV/WAVE, AIFF/AIFC (PCM and floating point), MP3, FLAC, and Ogg Vorbis. Only files directly inside the chosen folder are listed.

## Use

- Paste or type a folder path and press **Enter**, or use the folder icon to choose a directory.
- Click a filename or use **Up/Down** to select and immediately play it. Switching files stops the previous one.
- Press **Space** to pause or resume. At the end of a file, Space plays it again.
- Click a file's **Cat** cell to choose one of ten colours or clear its category.
- The colour checkboxes above the list control which categories appear. Unassigned files have their own filter.
- Click a column heading to sort; click it again to reverse. Drag column dividers to resize. Scroll horizontally to reach columns beyond the current window width.
- Adjust playback volume with the slider at the bottom.

Shortcuts: **Ctrl+L** focuses the path, **Ctrl+O** opens the folder chooser, **F5** refreshes the directory, **C** opens the selected file's category picker, and **Escape** stops playback while the list has focus. You can also drop a folder or audio file onto the window to open its directory.

Categories and preferences are stored in **%LOCALAPPDATA%\Abser**. Abser does not write metadata into your audio folders. Copying the app to another machine does not copy its categories; copy the Abser settings folder separately if needed.

Duration metadata is read in the background. Playback streams audio directly and does not wait for waveform generation or loudness analysis. Actual switching latency also depends on the storage device, audio driver, and file format.

## Build

Windows with Visual Studio 2022 C++ desktop tools and CMake 3.20 or newer:

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/Abser
Compress-Archive -Path dist/Abser -DestinationPath dist/Abser-Windows-x64.zip
```

The Windows GitHub Actions workflow builds, tests, and packages the same portable ZIP on each push or pull request. Download it from the successful workflow run's **Artifacts** section.

A MinGW-w64 toolchain file is also supplied at `cmake/mingw-x64.cmake`. The native C++ catalogue tests can be built on Linux.

## License

MIT. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
