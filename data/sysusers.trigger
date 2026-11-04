# burn runs this after a transaction that touched sysusers.d: the users and groups packages
# need exist before their files are used ($ROOT: "/" or the target root of burn -r)
path=usr/lib/sysusers.d/*.conf
run=/usr/bin/cleanlinux-boot --root $ROOT sysusers
