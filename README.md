# Wide Scan Application

Windows desktop scanner application for controlling Contex wide-format Scanners through the low level `ctx_scan_2000.dll` sdk library.

The application provides a graphical interface for configuring scan dimensions and resolution, previewing scans, and running continuous batch scans. Scanned documents can be saved as JPEG files with the configured DPI metadata.

## Features

* Wide-format color scanning
* Live scan preview with vertical scrolling
* Automated batch scanning
* Configurable scan resolution
* Configurable scan width and height
* Configurable output directory
* Serial-number-based output filenames
* Activity/status logging in the application window

## Requirements

The project is intended to be built as a Windows application and requires:

* Windows
* Contex compatible scanner supported by `ctx_scan_2000.dll`
* `ctx_scan_2000.dll` library for linking
* MinGW/G++ with Windows support
* JPEG library (`libjpeg`)

The provided Makefile links against:

* `ctx_scan_2000.dll`
* `libjpeg`

## Building

From a MinGW/MSYS-style environment, run:

```bash
make
```
## Running

Launch:

```text
widescan.exe
```

The application opens the **Wide Scan Application** dialog.

The application initializes its configuration from `settings.ini` located next to the executable. If a setting is missing, built-in defaults are used.

## Configuration

The application stores configuration under the `[ScannerSettings]` section of `settings.ini`.



### Configuration parameters

| Setting        |      Default | Description                                 |
| -------------- | -----------: | ------------------------------------------- |
| `DPI`          |        `200` | Scan resolution                             |
| `Width`        |       `1360` | Scan width in millimeters                   |
| `Height`       |      `20000` | Scan height in millimeters                  |
| `OutputDir`    |        empty | Directory where JPEG files are written      |
| `BaseFilename` | `ScanOutput` | Base name used when generating output files |

Settings are automatically saved when the UI values are synchronized.

## User Interface

The main window contains:

* **Resolution (DPI)** — selectable values are 100, 200, 300, 400, and 500 DPI.
* **Width (mm)** — scan width.
* **Height (mm)** — scan height.
* **Output Folder** — destination for generated JPEG files.
* **Select...** — opens a Windows folder-selection dialog.
* **Serial Number** — base filename for saved scans.
* **Logger** — displays scanner/application status messages.
* **Start** — starts automated batch scanning.
* **Close Batch** — requests cancellation of the active batch.

## Input Validation

Before a scan starts, the application validates the configured values.

### Width

Width must be between:

```text
21 mm and 1370 mm
```

### Height

Height must be between:

```text
2000 mm and 30000 mm
```

### Serial Number

The serial number:

* Must not be empty.
* May contain alphanumeric characters.
* May contain `-`.
* May not contain other characters.

For example:

```text
ABC123
ABC-123
SCAN-2026-001
```

The validation is performed before either preview or batch scanning starts.

## Output Files

When a base filename and output directory are configured, batch scans are written as:

```text
<base>-01.jpg
<base>-02.jpg
<base>-03.jpg
...
```

For example, with:

```text
OutputDir=C:\Scans
BaseFilename=JOB-12345
```

the generated files are:

```text
C:\Scans\JOB-12345-01.jpg
C:\Scans\JOB-12345-02.jpg
C:\Scans\JOB-12345-03.jpg
```

## Cancellation

While a preview or batch scan is active, the **Close Batch** button becomes available.

Pressing it sets the cancellation flag and signals the worker thread to stop.

During a batch operation, cancellation can occur while:

* Waiting for media
* Waiting for scanner status
* Receiving image data
* Processing a document

The application attempts to clean up temporary files and release the scanner when the operation ends.

## Scanner Shutdown

At the end of an operation, the application:

1. Closes the preview temporary file.
2. Releases the scanner unit if reserved.
3. Closes the scanner.
4. Closes the scanner library.
5. Resets the scanning state.
6. Re-enables the UI controls.

This cleanup is centralized in `CloseAndExit()`.

## Troubleshooting

### Scanner not detected

Check that:

* The scanner is connected and powered on.
* `ctx_scan_2000.dll` is available to the executable.
* No other application has exclusive control of the scanner.

The application reports scanner discovery and reservation failures in the Logger.

### Reservation conflict

If another process has reserved the scanner, the application logs a reservation error and terminates the scan operation.

### Invalid dimensions

Verify that:

```text
Width: 21–1370 mm
Height: 2000–30000 mm
```

are satisfied.

### Invalid serial number

Use only:

```text
A-Z
a-z
0-9
-
```

and make sure the field is not empty.

### Output files are not created

Verify that:

* An output directory is configured.
* The application has permission to write there.
* A valid serial number/base filename is configured.
* The scan actually produced image rows.
