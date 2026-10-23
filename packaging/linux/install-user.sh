#!/bin/sh
# Explicit, per-user installation. Does not change themes or default file apps.
set -eu
SOURCE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DATA=${XDG_DATA_HOME:-"$HOME/.local/share"}
CONFIG=${XDG_CONFIG_HOME:-"$HOME/.config"}
INTEGRATION_ONLY=no
AUTOSTART=no
for option in "$@"; do
    case "$option" in
        --integration-only) INTEGRATION_ONLY=yes ;;
        --autostart) AUTOSTART=yes ;;
        *) echo "Uso: install-user.sh [--autostart | --integration-only]" >&2; exit 2 ;;
    esac
done
install_integration() {
    effect="$DATA/kwin/effects/mandarina-shift"
    mkdir -p "$effect"
    cp -R "$SOURCE/share/kwin/effects/mandarina-shift/." "$effect/"
}
if [ "$INTEGRATION_ONLY" = yes ]; then
    install_integration
    echo "Integração do próprio Maracujá instalada. Reabra o Maracujá para ativar o gesto."
    exit 0
fi
VERSION=$(cat "$SOURCE/VERSION")
case "$VERSION" in *[!0-9A-Za-z.-]*|'') echo "Versão inválida" >&2; exit 1 ;; esac
TARGET="$HOME/.local/lib/maracuja/$VERSION"
if [ -e "$TARGET" ]; then
    echo "Essa versão já existe em $TARGET; não foi sobrescrita." >&2
    exit 1
fi
mkdir -p "$(dirname "$TARGET")" "$HOME/.local/bin"
cp -R "$SOURCE" "$TARGET"
if [ -x "$HOME/.local/bin/maracuja" ]; then
    "$HOME/.local/bin/maracuja" --quit >/dev/null 2>&1 || true
fi
mkdir -p "$DATA/applications" "$DATA/icons/hicolor/scalable/apps" "$DATA/kio/servicemenus"
# Fixed launcher files are owned by this app. File names stay compatible with
# previous Maracujá/Mandarina installs; user preferences and sources stay intact.
"$TARGET/runtime/python/bin/python3" -I - "$TARGET" "$HOME/.local/bin" "$DATA" "$CONFIG" "$AUTOSTART" <<'PY'
import os, pathlib, shlex, sys
target, binaries, data, config = map(pathlib.Path, sys.argv[1:5])
launcher = binaries / 'maracuja'
launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(str(target / 'AppRun')) + ' "$@"\n')
launcher.chmod(0o755)
legacy = binaries / 'mandarina'
legacy.write_text('#!/bin/sh\nexec ' + shlex.quote(str(launcher)) + ' "$@"\n')
legacy.chmod(0o755)
arg = str(launcher).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$').replace('%', '%%')
arg = ('"' + arg + '"').replace('\\', '\\\\')
desktop = '[Desktop Entry]\nType=Application\nName=Maracujá\nComment=Conversor local de arquivos\nExec=' + arg + ' %U\nIcon=org.winux.Maracuja\nCategories=Utility;Graphics;\nTerminal=false\n'
(data / 'applications/org.winux.Mandarina.desktop').write_text(desktop)
service = '[Desktop Entry]\nType=Service\nMimeType=all/allfiles;\nX-KDE-ServiceTypes=KonqPopupMenu/Plugin\nActions=convert;\n\n[Desktop Action convert]\nName=Converter com Maracujá…\nIcon=org.winux.Maracuja\nExec=' + arg + ' --wheel %U\n'
menu = data / 'kio/servicemenus/mandarina-convert.desktop'
menu.write_text(service)
menu.chmod(0o755)
if sys.argv[5] == 'yes':
    (config / 'autostart').mkdir(parents=True, exist_ok=True)
    (config / 'autostart/org.winux.Mandarina.desktop').write_text(desktop.replace(' %U\n', ' --background\n'))
PY
cp "$SOURCE/org.winux.Maracuja.svg" "$DATA/icons/hicolor/scalable/apps/org.winux.Maracuja.svg"
# Existing pinned launchers can still reference the previous icon ID.
cp "$SOURCE/org.winux.Maracuja.svg" "$DATA/icons/hicolor/scalable/apps/org.winux.Mandarina.svg"
install_integration
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DATA/applications" || true
echo "Maracujá $VERSION instalado para este usuário. Comando: $HOME/.local/bin/maracuja"
