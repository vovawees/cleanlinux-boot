# cleanlinux-boot

Загрузка [CleanLinux](https://github.com/vovawees/CleanLinux) под [dinit](https://github.com/davmac314/dinit):
декларативные сервисы и один небольшой помощник на C вместо shell-скриптов. Плюс всё, что нужно ядру на
пути к экрану входа: ядро и записи systemd-boot в ESP, маленький initramfs для тех, кому он нужен.

*English summary: dinit services and a small C helper for booting CleanLinux — API filesystems, fsck,
fstab, udev coldplug, modules via libkmod, sysctl, hostname, random seed, clock, tmpfiles, sysusers, getty on VTs
and serial consoles, swap on zram, backlight, power settings, clean shutdown; kernels into the EFI system partition with systemd-boot entries; an
optional initramfs (LUKS, modular storage). C17, libkmod, libblkid, libzstd. MIT.*

## Порядок загрузки

```
early-mounts ─┬─ early-udevd ── early-udev-trigger ── early-udev-settle ─┐
              ├─ early-clock ───────────────────────────────────────────┤
              ├─ early-modules, early-hostname                          │
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
| `seed load\|save` | seed в `/var/lib/cleanlinux/random-seed`: подмешивается в пул без учёта как энтропия, сразу обновляется |
| `clock load\|save` | RTC в локальном времени (`LOCAL` в adjtime), часы не раньше сборки и не раньше последнего выключения |
| `machine-id` | `/etc/machine-id` при первой загрузке |
| `tmpfiles` | общее подмножество `tmpfiles.d(5)`: `d D f w L z r R`, модификаторы `! - +` |
| `sysusers` | системные пользователи и группы из `sysusers.d(5)` (`u g m`, `r` принимается): недостающие дописываются в `passwd`, `shadow`, `group`, `gshadow` со свободными ID из `SYS_UID_MIN..SYS_UID_MAX`; учётные записи заблокированы |
| `getty TTY`, `consoles` | agetty на VT и на последовательном порту (скорость из `console=`); serial-getty на каждую последовательную консоль ядра |
| `kernel sync\|list` | ядра `/usr/lib/modules/*/vmlinuz` в ESP, записи `loader/entries/cleanlinux-*.conf`, systemd-boot как `EFI/BOOT/BOOT<ARCH>.EFI` |
| `initramfs` | initramfs только там, где без него нельзя |
| `zram start\|stop\|--print` | swap в сжатой памяти размером с RAM (не больше 8 ГиБ, zstd, приоритет 100) — если нет swap на диске; zswap при этом выключается |
| `backlight load\|save` | яркость каждой подсветки: сохраняется при выключении, восстанавливается при загрузке (не темнее 5%) |
| `power [--print]` | настройки питания из `power.d`: профиль платформы, governor и EPP процессора, пороги заряда, лимиты мощности |
| `shutdown-hook` | swap off, umount, корень только для чтения |

`--root DIR` выполняет команду для другого корня (установщик, сборка образа, тесты).

## Ядро и загрузчик

`cleanlinux-boot kernel sync` находит смонтированный ESP (`/efi`, `/boot` или `/boot/efi`, vfat) или берёт
`--esp DIR`, копирует каждое установленное ядро в `ESP/cleanlinux/<версия>/linux` и пишет запись
systemd-boot. Командная строка ядра — `/etc/kernel/cmdline`, затем фрагменты `kernel/cmdline.d/*.conf`
(`/etc`, `/run`, `/usr/lib` — их ставят профили железа) и `/usr/lib/modules/<версия>/cmdline` из профиля
сборки ядра. Записи и ядра, которых больше нет, удаляются; неизменившиеся файлы не переписываются.
burn запускает это сам после каждой транзакции, которая затронула ядро, systemd-boot или настройки
initramfs (`/usr/share/burn/triggers/cleanlinux-boot.trigger`).

Так же burn вызывает `cleanlinux-boot sysusers` после установки пакета с файлом в `/usr/lib/sysusers.d`
(`cleanlinux-sysusers.trigger`): пользователь `dhcpcd` или `messagebus` появляется вместе с пакетом, а
сервис `early-sysusers` при загрузке добавляет тех, кого почему-то нет.

## initramfs

Нужен только для зашифрованного корня или модульных драйверов накопителя. Он включается файлом в
`/etc/cleanlinux/initramfs.d/` (или `/usr/lib/cleanlinux/initramfs.d/` из пакета профиля):

```
modules=nvme dm-crypt
files=/usr/bin/cryptsetup
```

Внутри — `cleanlinux-init` (PID 1: API ФС, модули и coldplug, `rd.luks.uuid=`, ожидание `root=` по
`/dev/…`, `UUID=`, `LABEL=`, `PARTUUID=`, `PARTLABEL=`, `rootfstype=`, `rootflags=`, `ro`/`rw`, `init=`,
`rd.break`), его библиотеки, модули с собственным индексом (`depmod -b`) и их firmware. Архив cpio
отсортирован и датирован `SOURCE_DATE_EPOCH` — одинаковый вход даёт одинаковые байты.

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
