# Wayland & waypipe in diesem iSH-Fork

## Grafische Ausgabe in der App

In der Leiste über der Tastatur gibt es ein Bildschirm-Symbol (oder ⌘⇧D mit
Hardware-Tastatur). Es öffnet einen eingebauten VNC-Viewer, der sich mit
`127.0.0.1:5900` verbindet (änderbar über das Zahnrad). Darüber wird ein
headless Wayland-Compositor angezeigt, der in iSH läuft:

```sh
apk add sway wayvnc foot
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p -m 700 $XDG_RUNTIME_DIR
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 sway &
WAYLAND_DISPLAY=wayland-1 wayvnc 127.0.0.1 5900 &
WAYLAND_DISPLAY=wayland-1 foot &
```

Statt sway geht jeder wlroots-Compositor mit headless-Backend (z.B. `cage`,
`labwc`). Der Viewer bittet den Server, die Auflösung an den Bildschirm
anzupassen (abschaltbar im Zahnrad-Menü).

Bedienung: Tippen = Klick, Ziehen = Ziehen mit linker Taste, Tippen mit zwei
Fingern = Rechtsklick, Wischen mit zwei Fingern = Scrollen. Die Leiste oben hat
Tastatur, Esc, Tab, Ctrl/Alt/Super (einmalig gedrückt halten) und einen Knopf,
der die iOS-Zwischenablage an den Server schickt.

## waypipe

Mit dem lokalen Compositor von oben:

```sh
apk add waypipe openssh-client
WAYLAND_DISPLAY=wayland-1 waypipe ssh user@rechner programm
```

Für waypipe hat der Kernel jetzt `memfd_create` (inkl. Seals), korrektes
fd-Passing über Unix-Sockets (SCM_RIGHTS), `accept4`, `signalfd`,
`timerfd_gettime`, `close_range` und `mremap` für Datei-Mappings.

## IPA

Jeder Push auf `master` oder `claude/**` baut über GitHub Actions
(`.github/workflows/build-ipa.yml`) eine unsignierte `iSH-*-unsigned.ipa`. Sie
liegt als Pre-Release unter "Releases" und als Artifact am Workflow-Lauf und
kann mit AltStore, SideStore, Sideloadly o.ä. installiert werden.
