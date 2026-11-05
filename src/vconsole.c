/* SPDX-License-Identifier: MIT */
/* vconsole: the keyboard map and the font of the virtual terminals.
 *
 * /etc/vconsole.conf, as elsewhere: KEYMAP=ru, FONT=cyr-sun16 (FONT_MAP=, FONT_UNIMAP= when the
 * font needs them); vconsole.keymap= and vconsole.font= on the kernel command line win (a live
 * system started for another language). Every terminal gets the Unicode keyboard mode (the
 * kernel's console is UTF-8), the map is loaded once (it is shared by all of them) and the font
 * goes onto each one. Without the file nothing changes: the kernel's map is "us". */
#define _GNU_SOURCE
#include "boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VTS 6

struct vconsole {
	char *keymap, *font, *font_map, *font_unimap;
};

/* KEY=value lines, quotes and comments as in a shell; a later line wins */
static void parse(struct vconsole *v, char *text)
{
	for (char *save = NULL, *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		l = trim(l);
		char *eq = strchr(l, '=');
		if (!*l || *l == '#' || !eq)
			continue;
		*eq = '\0';
		char *k = trim(l), *val = trim(eq + 1);
		size_t n = strlen(val);
		if (n >= 2 && (val[0] == '"' || val[0] == '\'') && val[n - 1] == val[0]) {
			val[n - 1] = '\0';
			val++;
		}
		char **slot = !strcmp(k, "KEYMAP")	  ? &v->keymap
			      : !strcmp(k, "FONT")	  ? &v->font
			      : !strcmp(k, "FONT_MAP")	  ? &v->font_map
			      : !strcmp(k, "FONT_UNIMAP") ? &v->font_unimap
							  : NULL;
		if (!slot)
			continue;
		free(*slot);
		*slot = *val ? xstrdup(val) : NULL;
	}
}

static void override(char **slot, const char *key)
{
	char *v = cmdline_value(key);
	if (!v)
		return;
	free(*slot);
	*slot = *v ? v : NULL;
	if (!*slot)
		free(v);
}

/* a name for loadkeys and setfont: no options, no paths outside their data */
static bool plain_name(const char *s)
{
	if (!s || !*s || *s == '-')
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("._+-/", *s)))
			return false;
	return true;
}

static int act(bool dry, char *const argv[])
{
	if (dry) {
		for (size_t i = 0; argv[i]; i++)
			printf("%s%s", i ? " " : "", argv[i]);
		putchar('\n');
		return 0;
	}
	return run(argv);
}

int cmd_vconsole(int argc, char **argv)
{
	bool dry = argc > 1 && !strcmp(argv[1], "--print");
	if (argc > 2 || (argc == 2 && !dry))
		die("usage: vconsole [--print]");
	struct vconsole v = {0};
	char *p = rpath("/etc/vconsole.conf");
	char *text = read_file(p, NULL);
	free(p);
	if (text)
		parse(&v, text);
	free(text);
	override(&v.keymap, "vconsole.keymap");
	override(&v.font, "vconsole.font");
	int rc = 0;
	const char *names[] = {v.keymap, v.font, v.font_map, v.font_unimap};
	for (size_t i = 0; i < ARRAY_LEN(names); i++)
		if (names[i] && !plain_name(names[i])) {
			warnx("vconsole.conf: not a map or font name: %s", names[i]);
			rc = 1;
		}
	if (rc)
		goto out;
	/* --print shows the commands by name, installed or not */
	char *kbd_mode = dry ? xstrdup("kbd_mode") : find_program("kbd_mode");
	char *loadkeys = dry ? xstrdup("loadkeys") : find_program("loadkeys");
	char *setfont = dry ? xstrdup("setfont") : find_program("setfont");
	for (int n = 1; n <= VTS && (v.keymap || v.font) && !(v.font && !setfont && n > 1); n++) {
		char *tty = xasprintf("/dev/tty%d", n), *dev = rpath(tty);
		bool there = exists(dev);
		free(dev);
		if (!there && !dry) {
			free(tty);
			continue;
		}
		if (kbd_mode) {
			char *a[] = {kbd_mode, "-u", "-C", tty, NULL};
			if (act(dry, a) != 0)
				rc = 1;
		}
		if (v.font && !setfont) {
			warnx("FONT=%s, but setfont (kbd) is not installed", v.font);
			rc = 1;
		} else if (v.font) {
			char *a[10] = {setfont, "-C", tty, v.font};
			size_t k = 4;
			if (v.font_map)
				a[k++] = "-m", a[k++] = v.font_map;
			if (v.font_unimap)
				a[k++] = "-u", a[k++] = v.font_unimap;
			a[k] = NULL;
			if (act(dry, a) != 0) {
				warnx("setfont %s failed on %s", v.font, tty);
				rc = 1;
			}
		}
		free(tty);
	}
	if (v.keymap) {
		if (!loadkeys) {
			warnx("KEYMAP=%s, but loadkeys (kbd) is not installed", v.keymap);
			rc = 1;
		} else {
			char *a[] = {loadkeys, "-q", "-u", v.keymap, NULL};
			if (act(dry, a) != 0) {
				warnx("loadkeys %s failed", v.keymap);
				rc = 1;
			}
		}
	}
	free(kbd_mode);
	free(loadkeys);
	free(setfont);
out:
	free(v.keymap);
	free(v.font);
	free(v.font_map);
	free(v.font_unimap);
	return rc;
}
