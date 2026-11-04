# Как участвовать

**cleanlinux-boot** — сервисы dinit и помощник загрузки CleanLinux на C, часть [CleanLinux](https://github.com/vovawees/CleanLinux). *English [below](#english).*

## С чего начать

- Что входит в базу, а что ставится сверху — [SCOPE.md](https://github.com/vovawees/CleanLinux/blob/main/docs/SCOPE.md); как всё устроено — [ARCHITECTURE.md](https://github.com/vovawees/CleanLinux/blob/main/docs/ARCHITECTURE.md).
- Что дальше — [ROADMAP.md](https://github.com/vovawees/CleanLinux/blob/main/docs/ROADMAP.md), issues с меткой `roadmap` и общая доска [CleanLinux roadmap](https://github.com/users/vovawees/projects/1).
- Крупную правку сначала обсудите в issue, чтобы не делать работу дважды.

## Проверки

Хостингового CI нет: проверки запускаются локально, одной командой (нужен shellcheck 0.11 или новее).

`tools/check.sh` — сборка с `-Werror` и `make check` на gcc и clang, shellcheck, `make install` во временный `DESTDIR`.

Быстрые проверки можно запускать автоматически перед каждым `git push`:

```sh
git config core.hooksPath tools/hooks   # перед push: tools/check.sh --quick
```

## Правила

- C17, libkmod, libblkid, libzstd; стиль — как в соседнем коде, табы.
- Каждая команда работает и с `--root DIR` — так её проверяют unit-тесты без root (`tests/run.sh`).
- Сервис dinit — декларативный файл в `services/`; логика — в `cleanlinux-boot`, а не в shell.
- Релиз: `VERSION` в Makefile, коммит `X.Y.Z`, тег `vX.Y.Z`, `make dist` → tarball в GitHub Release; затем рецепт `cleanlinux-boot` в CleanLinux-Recipes.

## Коммиты и pull request'ы

- Одна логическая правка — один коммит. Сообщение — на английском, в стиле истории: `область: что изменилось`
  (`bootstrap: …`, `docs: …`), подробности — в теле письма.
- Ветка от `main`, pull request в `main`; `Closes #N` (или `Closes vovawees/CleanLinux#N`) закрывает issue при слиянии.
- Документация меняется в том же pull request'е, что и поведение.

## English

cleanlinux-boot is part of [CleanLinux](https://github.com/vovawees/CleanLinux). There is no hosted CI: run `tools/check.sh` (shellcheck 0.11+)
before every push (`git config core.hooksPath tools/hooks` runs the quick checks automatically). C17 with libkmod, libblkid and libzstd; every command supports `--root DIR` and is unit-tested without root; services are declarative dinit files; releases are tags vX.Y.Z with the `make dist` tarball attached.
Commit messages are in English (`area: what changed`); pull requests target `main`, and `Closes #N` closes the issue.
See the [roadmap](https://github.com/vovawees/CleanLinux/blob/main/docs/ROADMAP.md) and the [project board](https://github.com/users/vovawees/projects/1).
