#!/bin/sh
# Inside The Dream : les raccourcis (menu des applications et Bureau) vers ce dossier, le jeu ajoute a Steam sur un Steam Deck
# (bibliotheque, onglet Non-Steam), puis le jeu. Le dossier peut etre place n'importe ou : relancer ce fichier apres
# l'avoir deplace.
D=$(cd "$(dirname "$0")" && pwd)
chmod +x "$D/Inside The Dream.sh" "$D/Inside The Dream.AppImage"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$APPS"
ENTRY="$APPS/inside-the-dream.desktop"
cat > "$ENTRY" <<EOF
[Desktop Entry]
Type=Application
Name=Inside The Dream
Comment=Mod de vue subjective pour The Legend of Zelda: Link's Awakening, construit sur LAS Recomp
Exec="$D/Inside The Dream.sh"
Path=$D
Icon=$D/icon.png
Terminal=false
Categories=Game;
EOF
chmod +x "$ENTRY"
DESK=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")
if [ -d "$DESK" ]; then cp "$ENTRY" "$DESK/Inside The Dream.desktop" && chmod +x "$DESK/Inside The Dream.desktop"; fi
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS" >/dev/null 2>&1
# Steam Deck : steamos-add-to-steam ajoute un jeu non-Steam (Steam doit tourner, ce qui est le cas en mode Bureau)
if command -v steamos-add-to-steam >/dev/null 2>&1; then
    steamos-add-to-steam "$D/Inside The Dream.sh" && echo "Inside The Dream ajoute a Steam (bibliotheque, onglet Non-Steam)."
else
    echo "Pour le mode Jeu : dans Steam, Ajouter un jeu > Ajouter un jeu non-Steam, et choisir \"$D/Inside The Dream.sh\"."
fi
exec "$D/Inside The Dream.sh"
