# primary-iso-loader

Just a small fine workaround to load an ISO installation software into an OpenStack like instance while keeping the primary disk the main disk to install the base operating system.

## OpenShift Container Platform 4

The Assisted Installer boots a discovery ISO, then writes Red Hat Enterprise Linux CoreOS onto a disk you select. On an OpenStack-like cloud the primary disk of an instance is fixed after the first start, and the firmware boots that disk. Using the discovery ISO as the primary disk starts the agent, but then the installer has no separate disk left for the operating system.

Import `primary-boot.qcow2` as the primary disk instead. It is a sparse 120 GiB UEFI disk whose only content is a bootloader. Attach the discovery ISO as a second volume or CD, and do not mark that volume as a boot disk. The bootloader loads the ISO's own GRUB configuration, including the Assisted Installer kernel arguments. The installer can then use the 120 GiB disk for Red Hat Enterprise Linux CoreOS.

The virtual size is 120 GiB so the disk satisfies the Assisted Installer installation-disk minimum. The qcow2 file is about 2 MiB because the unused space stays unallocated.

The same bootloader also starts a Red Hat Enterprise Linux installation DVD.

Assisted Installer documentation for OpenShift Container Platform 4: [Installing OpenShift Container Platform with the Assisted Installer](https://docs.redhat.com/en/documentation/assisted_installer_for_openshift_container_platform/2026/html/installing_openshift_container_platform_with_the_assisted_installer/index).

## First boot prints the MAC address

The Assisted Installer host inventory pairs each machine with its NIC MAC address. On a cloud instance that address is assigned when the instance is created, and the public IP is attached to the same NIC. You need both before the discovery agent starts, so the installer can match the host to the address you entered.

Boot the instance once with only `primary-boot.qcow2` attached. GRUB does not find an ISO, so it prints the virtio NIC address and waits:

```text
No discovery or installation ISO found.
Attach the ISO and reboot.
MAC address: 52:54:00:ab:cd:ef
```

Copy that address into the Assisted Installer and map it to the public IP of the instance. Then attach the discovery ISO and reboot. The MAC screen is only shown when the ISO is absent. The next boot loads the discovery menu.

## Import the image

1. Import `primary-boot.qcow2` with UEFI firmware. Leave Secure Boot disabled. The GRUB binary in this image is not enrolled with Secure Boot.
2. Create the instance from that image and attach its public IP. Do not boot the instance from the discovery ISO.
3. Start it and read `MAC address:` from the console. Enter that MAC in the Assisted Installer, mapped to the public IP.
4. Attach the discovery ISO as a non-boot CD or second volume, then reboot.
5. The console should show the discovery menu. In the Assisted Installer, select the 120 GiB disk as the installation disk.

The firmware must present the ISO as `cd0`. The configuration reconnects PCI and SCSI devices first, because some firmware connects only the boot disk. A later CD is ignored.

## Rebuild

The prebuilt image was produced with GNU GRUB 2.12 (`grub2-efi-x64-2.12-64.fc44`). Rebuild it with:

```bash
./build-primary-boot-disk.sh --force
```

Required commands: `qemu-img`, `parted`, `mkfs.vfat`, `grub2-mkimage`, `grub2-script-check`, `mmd`, `mcopy`, `truncate`, `gcc`, `ld`, and `python3`.

## License

The files in this repository are under the Apache License 2.0, except the GNU GRUB EFI binary inside `primary-boot.qcow2`. GNU GRUB is GPL-3.0-or-later. That binary was produced by `grub2-mkimage` from Fedora package `grub2-efi-x64-2.12-64.fc44`; the corresponding source is that Fedora source package. Run the build script to replace the binary.
