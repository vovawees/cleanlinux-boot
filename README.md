# cleanlinux-boot

Загрузка [CleanLinux](https://github.com/vovawees/CleanLinux) под [dinit](https://github.com/davmac314/dinit):
декларативные сервисы и один небольшой помощник на C вместо shell-скриптов. Плюс всё, что нужно ядру на
пути к экрану входа: ядро в загрузочный раздел под загрузчик устройства (systemd-boot, U-Boot extlinux,
прошивка Raspberry Pi, Android boot.img), маленький initramfs для тех, кому он нужен, и live-загрузка с флешки (btrfs/ext4).

*English summary: dinit services and a small C helper for booting CleanLinux — API filesystems, fsck,
fstab, udev coldplug, modules via libkmod, sysctl, hostname, console keymap and font, random seed, clock, tmpfiles, sysusers, getty on VTs
and serial consoles, swap on zram, backlight, power settings, clean shutdown; kernels into the boot partition for the
loader of the device (systemd-boot entries, U-Boot extlinux.conf, Raspberry Pi config.txt, Android boot images v0–v2 with
safe flashing), device trees and overlays; an optional initramfs (modular storage, live boot from the btrfs/ext4 partition
of a USB image with layers under a tmpfs overlay, optionally copied to RAM). C17, libkmod, libblkid,
libzstd. MIT.*

## Порядок загрузки

```
early-mounts ─┬─ early-udevd ── early-udev-trigger ── early-udev-settle ─┐
              ├─ early-clock ───────────────────────────────────────────┤
              ├─ early-modules, early-hostname, early-vconsole          │
              └────────────────────────────── early-fsck-root ◄─────────┘
                                                    │
                    early-root-rw (starts-rwfs) ── early-fsck ── early-fstab ─┬─ early-seed
                    early-machine-id, early-sysusers ── early-tmpfiles ◄──────┤  early-sysctl
                                                                              │  zram, backlight, power
                                     system ◄─────────────────────────────────┘
                                       │
              login (getty@tty1..4, early-consoles → serial-getty@ttyS0) ── boot (+ /etc/dinit.d/boot.d)
```

Корень монтирует само ядро (`root=PARTUUID=… ro`), без initramfs: драйверы накопителей и файловая система
корня встроены в ядро CleanLinux. Дальше всё делают сервисы из `/usr/lib/dinit.d`; любой из них
переопределяется файлом с тем же именем в `/etc/dinit.d`. `dinitctl enable <сервис>` добавляет сервис в
`/etc/dinit.d/boot.d`. Если загрузка сорвалась, dinit предлагает `recovery` (`sulogin`).

Выключение: dinit останавливает сервисы (часы и seed сохраняются), затем запускает
`/usr/lib/dinit/shutdown-hook` — ссылку на `cleanlinux-boot`: swap выключается, файловые системы
отмонтируются в обратном порядке, корень перемонтируется только для чтения.

## Команды

| Команда | Что делает |
|---|---|
| `early-mounts` | `/proc`, `/sys`, `/dev` (devtmpfs), `/dev/pts`, `/dev/shm`, `/run`, cgroup2, efivarfs, bpf, … |
| `fsck-root`, `remount-root` | проверка корня, пока он только для чтения (`fsck.mode=skip\|force`, номер прохода из fstab); корень на запись |
| `fsck-all`, `mount-all` | остальные файловые системы fstab (без `_netdev`), swap |
| `udev-trigger` | при необходимости пересобирает hwdb, затем повторяет события `add` для всех устройств |
| `modules`, `coldplug` | модули из `modules-load.d` и `modules-load=` через libkmod; coldplug по `modalias` — для initramfs и систем без udev |
| `sysctl` | `sysctl.d` и `/etc/sysctl.conf`: поздние файлы побеждают, `-ключ` не считается ошибкой |
| `hostname` | имя из `/etc/hostname`, по умолчанию `cleanlinux` |
| `vconsole [--print]` | раскладка и шрифт консоли из `/etc/vconsole.conf` (`KEYMAP=`, `FONT=`, `FONT_MAP=`, `FONT_UNIMAP=`); `vconsole.keymap=` и `vconsole.font=` в командной строке ядра важнее; tty1–6 в режиме Unicode, шрифт на каждый; без файла ничего не меняется (`us`); без консоли, рисующей текст (только serial, `dummy device` в `/sys/class/vtconsole`), шрифт пропускается без ошибки |
| `seed load\|save` | seed в `/var/lib/cleanlinux/random-seed`: подмешивается в пул без учёта как энтропия, сразу обновляется |
| `clock load\|save` | RTC в локальном времени (`LOCAL` в adjtime), часы не раньше сборки и не раньше последнего выключения |
| `machine-id` | `/etc/machine-id` при первой загрузке |
| `tmpfiles` | общее подмножество `tmpfiles.d(5)`: `d D f w L z r R`, модификаторы `! - +` |
| `sysusers` | системные пользователи и группы из `sysusers.d(5)` (`u g m`, `r` принимается): недостающие дописываются в `passwd`, `shadow`, `group`, `gshadow` со свободными ID из `SYS_UID_MIN..SYS_UID_MAX`; учётные записи заблокированы |
| `getty TTY`, `consoles` | agetty на VT и на последовательном порту (скорость из `console=`); serial-getty на каждую последовательную консоль ядра; на tty1 — автоматический вход пользователя из `/etc/cleanlinux/autologin` (не root; `cleanlinux.autologin=0` в строке ядра отключает) |
| `kernel sync\|list\|config` | ядра `/usr/lib/modules/*/vmlinuz` в загрузочный раздел под загрузчики из `boot.d`: записи systemd-boot, `extlinux.conf`, `config.txt` Raspberry Pi, Android boot.img; `config` — итоговые настройки |
| `initramfs` | initramfs только там, где без него нельзя |
| `zram start\|stop\|--print` | swap в сжатой памяти размером с RAM (не больше 8 ГиБ, zstd, приоритет 100) — если нет swap на диске; zswap при этом выключается |
| `backlight load\|save` | яркость каждой подсветки: сохраняется при выключении, восстанавливается при загрузке (не темнее 5%) |
| `power [--print]` | настройки питания из `power.d`: профиль платформы, governor и EPP процессора, пороги заряда, лимиты мощности |
| `shutdown-hook` | swap off, umount, корень только для чтения |

`--root DIR` выполняет команду для другого корня (установщик, сборка образа, тесты).

## Ядро и загрузчик

`cleanlinux-boot kernel sync` находит смонтированный загрузочный раздел (`/efi`, `/boot` или `/boot/efi`,
vfat — ESP) или берёт `--esp DIR`, копирует каждое установленное ядро в `ESP/cleanlinux/<версия>/` и пишет
то, что читает загрузчик устройства. Командная строка ядра — `/etc/kernel/cmdline`, затем фрагменты
`kernel/cmdline.d/*.conf` (`/etc`, `/run`, `/usr/lib` — их ставит пакет `hardware` из `KERNEL_CMDLINE` профиля) и
`/usr/lib/modules/<версия>/cmdline`. Записи, ядра и файлы, которые больше не нужны, удаляются;
неизменившиеся файлы не переписываются. burn запускает это сам после каждой транзакции, которая затронула
ядро, systemd-boot, настройки загрузчика или initramfs (`/usr/share/burn/triggers/cleanlinux-boot.trigger`).

### Загрузчик устройства: `boot.d`

Чем грузится устройство, описывают файлы `cleanlinux/boot.d/*.conf` (`/etc`, `/run`, `/usr/lib`; пакет
`hardware` кладёт `/usr/lib/cleanlinux/boot.d/50-profile.conf` из `BOOTLOADER` и `DEVICE_TREE` профиля,
свои настройки — в `/etc/cleanlinux/boot.d/`, позже по имени — главнее). Без них — systemd-boot, как раньше.

```ini
loader=extlinux                     # systemd-boot | extlinux | rpi | android | none; можно несколько
devicetree=rockchip/rk3566-anbernic-rg353p.dtb   # из /usr/lib/modules/<версия>/dtbs; rockchip/ — весь каталог
overlays=rockchip/overlay/x.dtbo    # оверлеи device tree
default=                            # версия ядра по умолчанию (иначе — самое новое)
timeout=3                           # секунды (extlinux)
image=                              # файл ядра для не-UEFI загрузчиков (иначе Image, если есть, или vmlinuz)
```

| `loader=` | Что пишется |
|---|---|
| `systemd-boot` | `loader/entries/cleanlinux-<версия>.conf` (`linux`, `initrd`, `devicetree`, `devicetree-overlay`, `options`), systemd-boot как `EFI/BOOT/BOOT<ARCH>.EFI` |
| `extlinux` | `extlinux/extlinux.conf` для U-Boot: ядро по умолчанию первым, потом остальные от новых к старым; `linux`, `initrd`, `fdt` (или `fdtdir` для каталога — U-Boot сам возьмёт свой `$fdtfile`), `fdtoverlays`, `append` |
| `rpi` | `config.txt` прошивки Raspberry Pi: `os_prefix=cleanlinux/<версия>/` ядра по умолчанию, `kernel=`, `initramfs … followkernel`, `device_tree=`, `dtoverlay=`; рядом с ядром — `cmdline.txt`, `.dtb` и `overlays/`; свои настройки прошивки — в `usercfg.txt` (он подключается) |
| `android` | `/var/lib/cleanlinux/android/<версия>.img` — Android boot image (ABL Qualcomm и подобные), см. ниже |

`extlinux.conf` и `config.txt`, которые написал не cleanlinux-boot (без его первой строки), не трогаются:
`kernel sync` предупреждает и завершается с ошибкой, пока чужой файл не убран. На arm64 `vmlinuz` — программа
EFI (для systemd-boot), а U-Boot и прошивка Raspberry Pi получают `/usr/lib/modules/<версия>/Image`.

### Android boot image

Для устройств, где ядро грузит загрузчик Android (ABL на Snapdragon и т. п.), `loader=android` собирает
boot.img в формате `mkbootimg` (заголовок версий 0, 1, 2; id — SHA-1 секций, как у AOSP): ядро (`Image`, сжатое
gzip), initramfs, если он нужен, и device tree — отдельной секцией в версии 2 или в конце ядра
(`Image.gz-dtb`) в версиях 0 и 1. Загрузочный раздел ESP для этого не нужен.

```ini
android.header=2                    # 0, 1 или 2 (2 требует devicetree=)
android.pagesize=4096
android.base=0x10000000             # и android.kernel_offset, ramdisk_offset, tags_offset, dtb_offset (как у mkbootimg)
android.os_version=13.0.0           # если загрузчик их проверяет
android.os_patch_level=2024-05
android.compress=gzip               # gzip | none
android.partition=/dev/disk/by-partlabel/boot_a
```

С `android.partition` образ ядра по умолчанию прошивается в раздел, но только на работающей системе (с
`--root` — лишь по `--flash`), только если в разделе уже лежит Android boot image (`ANDROID!`) или он пуст,
только если образ влезает и отличается от записанного. Перед первой прошивкой всё содержимое раздела
сохраняется в `/var/lib/cleanlinux/android/<раздел>.orig` — вернуть стоковое ядро можно
`dd if=/var/lib/cleanlinux/android/boot_a.orig of=/dev/disk/by-partlabel/boot_a`. `--dry-run` показывает, что
было бы записано и прошито.

Так же burn вызывает `cleanlinux-boot sysusers` после установки пакета с файлом в `/usr/lib/sysusers.d`
(`cleanlinux-sysusers.trigger`): пользователь `dhcpcd` или `messagebus` появляется вместе с пакетом, а
сервис `early-sysusers` при загрузке добавляет тех, кого почему-то нет.

## initramfs

Нужен только live-системе и модульным драйверам накопителя. Он включается файлом в
`/etc/cleanlinux/initramfs.d/` (или `/usr/lib/cleanlinux/initramfs.d/` из пакета профиля):

```
modules=nvme                 # загружаются до поиска корня; модуля нет в ядре — ошибка
files=/usr/bin/btrfs         # программы (с их библиотеками) и файлы
optional=panel-foo pwm_bl    # кладутся, если ядро их собрало; грузит coldplug, если есть устройство
compress=zstd                # zstd | none
```

`optional=` — для профилей, где одно ядро обслуживает много плат (драйверы экранов RK3566): нужный
драйвер окажется в initramfs и сообщения загрузки будут видны на экране, а лишние не мешают. Сам по себе
`optional=` initramfs не включает — только дополняет нужный по другой причине.

Внутри — `cleanlinux-init` (PID 1: API ФС, модули и coldplug, ожидание `root=` по `/dev/…`, `UUID=`,
`LABEL=`, `PARTUUID=`, `PARTLABEL=`, `rootfstype=`, `rootflags=`, `ro`/`rw`, `init=`, `rd.break`), его
библиотеки, модули с собственным индексом (`depmod -b`) и их firmware. Архив cpio отсортирован и датирован
`SOURCE_DATE_EPOCH` — одинаковый вход даёт одинаковые байты. Шифрования корня (LUKS) нет: CleanLinux его
не использует.

### Live-система: `root=live:`

Так загружается live-образ CleanLinux с флешки, записанной целиком (сырой GPT-образ: ESP и системный
раздел). `cleanlinux-init` ищет раздел `SPEC` (`LABEL=CL_LIVE_…`, `UUID=`, `PARTUUID=`, `/dev/…`; подходит
тот, на котором есть `@/usr`), монтирует его только для чтения (btrfs со сжатием zstd или ext4 — обычная
файловая система, без squashfs и loop) и собирает корень overlay из слоёв с tmpfs сверху: live-система
пишет в память, носитель не меняется.

```
раздел CL_LIVE_… (btrfs/ext4, ro)
  ├─ live/          слой live-системы (автологин, установщик)  ─┐
  ├─ spins/NAME/    слой сборки (cleanlinux.live.spin=NAME)    ─┤
  ├─ nonfree/       проприетарные драйверы и прошивки          ─┤ lowerdir
  └─ @/             система — ровно то, что ставится на диск   ─┘
                                            tmpfs (upper) ── overlay ── /
```

| Параметр ядра | Что делает |
|---|---|
| `root=live:SPEC` | live-загрузка с раздела `SPEC` |
| `cleanlinux.live.spin=NAME` | добавить слой сборки `spins/NAME` (например рабочий стол labwc) |
| `cleanlinux.live.nonfree=0` | без слоя `nonfree/` (проприетарные драйверы подключены по умолчанию) |
| `cleanlinux.live.toram` | сначала скопировать весь раздел в память (с процентами на экране), если он занимает не больше 70% свободной; после этого носитель можно вынуть |
| `rootwait`, `rootdelay=` | сколько ждать носитель (по умолчанию 30 с; медленные USB-флешки) |

Модули `loop`, `overlay`, `btrfs`, `ext4`, `usb-storage`, `uas` загружаются, если они модулями собраны и
лежат в initramfs (`modules=`/`optional=`). Всё смонтированное остаётся под `/run/cleanlinux/live`
(`medium` — раздел, `rw`, `toram`) и после перехода в корень: `cleanlinux-install` копирует систему из
`/run/cleanlinux/live/medium/@`.

## Память и питание

Настройки железа — текстовые файлы `KEY=VALUE`, а не GUI: `*.conf` в `/etc/cleanlinux/<каталог>`,
`/run/…`, `/usr/local/lib/…` и `/usr/lib/cleanlinux/<каталог>` (туда кладёт свои значения пакет профиля
железа `hardware`); файлы читаются по порядку имён, последнее значение ключа побеждает.

`zram.d` (сервис `zram`):

```
size=100%        # доля RAM или МиБ; 0 или off — без zram (или cleanlinux.zram= в командной строке ядра)
max=8192         # потолок в МиБ, 0 — без потолка
algorithm=zstd
priority=100
```

Если в fstab есть swap на диске, zram не создаётся: перед дисковым swap работает zswap (он включён в ядре
CleanLinux по умолчанию).

`power.d` (сервис `power`, после того как udev загрузил драйверы):

```
platform_profile=balanced        # /sys/firmware/acpi/platform_profile (варианты: platform_profile_choices)
governor=powersave               # scaling_governor каждого cpufreq policy
epp=balance_power                # energy_performance_preference (amd-pstate, intel_pstate в режиме active)
charge_limit=80                  # charge_control_end_threshold каждой батареи
charge_start=70                  # charge_control_start_threshold
firmware.ppt_pl1_spl=15          # атрибут firmware-attributes (TDP у asus-armoury, lenovo-wmi, …)
rapl_long=15                     # Intel RAPL: длительный (PL1) и короткий (PL2) лимит пакета, Вт
rapl_short=25
```

Чего железо не умеет, то пропускается с сообщением в журнале; неизвестный ключ — ошибка сервиса.

## Сборка

```
make            # cleanlinux-boot, cleanlinux-init
make check      # unit-тесты в отдельном корне, без root
make install DESTDIR=… PREFIX=/usr
```

Зависимости: libkmod, libblkid (util-linux), libzstd; во время работы — dinit, eudev, util-linux (agetty,
fsck, mount, swapon, mkswap, sulogin, hwclock), kmod.
