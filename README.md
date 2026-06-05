<div align="center">

  # linux-devmgmt
  <i>The Windows Device Manager, on Linux</i>

  <p>
    A faithful recreation of the Windows Device Manager built with Qt6 and real hardware backends via sysfs/procfs. Best enjoyed with <a href="https://github.com/aeroshell-desktop/aerothemeplasma">AeroThemePlasma</a>, but looks great on regular KDE as well.
  </p>

</div>
<br>

> [!NOTE]
> **Built for CachyOS / Arch Linux.** Also packaged as a Nix flake for NixOS.<br>Some features (DKMS uninstall, driver date lookup) depend on `dkms` and `pacman`. Other distros may need minor adjustments.

| | | |
|:-------------------------:|:-------------------------:|:-------------------------:|
| <img src="./screenshots/main.png" alt="A list of device categories with some of them expanded."> | <img src="./screenshots/properties.png" alt="A device properties window open, showing information about an AMD GPU"> | <img src="./screenshots/driver.png" alt="The Driver tab of the Properties window, shwoing the version and date of said GPU driver"> |
| <img src="./screenshots/olddriver.png" alt="The same Driver tab of the Properties window, but this time showing an older unmaintained driver's date and version."> | <img src="./screenshots/details.png" alt="The Details tab, shwoing a dropdown with a large text area."> | <img src="./screenshots/resources.png" alt="The Resources tab, shwoing a information about a deviecs resources like IRQ and similar."> |


## Installation

### Arch / CachyOS (AUR)

```bash
yay -S linux-devmgmt
```

### Pre-built binaries

Download the latest binary for your architecture from the [Releases](https://github.com/actuallyaridan/linux-devmgmt/releases/latest) page and place it somewhere in your `$PATH`:

```bash
chmod +x devmgmt-x86_64
sudo mv devmgmt-x86_64 /usr/local/bin/devmgmt
```

## Building

### Arch / CachyOS

```bash
sudo pacman -S qt6-base cmake
cmake -B build
cmake --build build -j
./build/devmgmt
```

### Nix

```bash
nix build
./result/bin/devmgmt

# or run directly
nix run
```

A dev shell with Qt Creator and GDB is also available:

```bash
nix develop
```

## Runtime dependencies

On Arch, you need these installed separately. The Nix flake handles all of them automatically.

| Tool | Purpose |
|------|---------|
| `pkexec` | Privilege escalation for enable/disable/uninstall |
| `modinfo` | Driver details |
| `bluetoothctl` | Bluetooth device disconnect |
| `dkms` | *(optional)* DKMS driver management |
| `pacman` | *(optional)* Package date lookup |

## Features

- Full two-level device tree (by type) backed by real sysfs/procfs data
- Per-device Properties dialog with General, Driver, Details, and Resources tabs
- Enable / disable devices via kernel module blacklisting
- Uninstall DKMS drivers
- Update Driver wizard
- Driver Details viewer (`modinfo` output)
- Scan for hardware changes
