#!/usr/bin/env python3
"""Cosmic Destruction GTK3 launcher (GPL-3.0)."""
from pathlib import Path
import os
import re
import sys
import threading

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib, Gdk
from model import Library

STYLE = b'''
window { background: #07100e; color: #eaf3eb; }
label { color: #eaf3eb; }
.hero { background: linear-gradient(125deg, #173e26, #0a1713); border: 1px solid #285c38; border-radius: 20px; padding: 28px; }
.brand { color: #a8f356; font-size: 15px; font-weight: 800; letter-spacing: 3px; }
.title { color: #ffffff; font-size: 34px; font-weight: 900; }
.subtitle { color: #a7bbae; font-size: 14px; }
.status { color: #b4dc91; font-size: 13px; }
.card { background: #101e19; border: 1px solid #294032; border-radius: 14px; padding: 20px; }
button { color: #e6f2e8; background: #183324; border: 1px solid #3d6145; border-radius: 9px; padding: 11px 20px; }
button:hover { background: #244a30; }
button.primary { color: #0a190c; background: #9be244; border: 1px solid #b3f668; font-weight: 900; font-size: 15px; }
button.primary:hover { background: #b5f269; }
button:disabled { opacity: 0.45; }
progressbar trough { background: #1c3325; border-radius: 5px; min-height: 6px; }
progressbar progress { background: #9be244; border-radius: 5px; }
textview, textview text { background: #08120d; color: #a9c4b0; font-family: monospace; font-size: 11px; }
combobox button { padding: 6px 12px; }
'''


def label(text, style=None, wrap=False):
    widget = Gtk.Label(label=text, xalign=0)
    widget.set_line_wrap(wrap)
    if style: widget.get_style_context().add_class(style)
    return widget


class Launcher(Gtk.Window):
    def __init__(self):
        super().__init__(title='Ben 10 — Cosmic Destruction PC')
        self.set_wmclass('ben10-cosmic-destruction-pc', 'Ben10CosmicDestructionPC')
        self.set_default_size(960, 700)
        self.set_size_request(760, 620)
        self.library, self.source, self.busy, self.game = Library(), None, False, None
        self.connect('delete-event', self.closing)
        self.connect('destroy', Gtk.main_quit)
        css = Gtk.CssProvider(); css.load_from_data(STYLE)
        Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18, margin=28)
        self.add(root)
        hero = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        hero.get_style_context().add_class('hero')
        hero.pack_start(label('BEN 10  /  ULTIMATE ALIEN', 'brand'), False, False, 0)
        hero.pack_start(label('COSMIC DESTRUCTION', 'title'), False, False, 0)
        hero.pack_start(label('Sua aventura, agora no PC.', 'subtitle'), False, False, 0)
        hero.pack_start(label('Linux • SLUS-21938 • Em desenvolvimento', 'status'), False, False, 5)
        root.pack_start(hero, False, False, 0)
        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        card.get_style_context().add_class('card')
        card.pack_start(label('SEU DISCO', 'brand'), False, False, 0)
        row = Gtk.Box(spacing=16)
        self.file = label('Selecione sua cópia de Cosmic Destruction para PS2.', 'subtitle', True)
        row.pack_start(self.file, True, True, 0)
        self.choose = Gtk.Button(label='Selecionar disco'); self.choose.connect('clicked', self.select_file)
        row.pack_end(self.choose, False, False, 0); card.pack_start(row, False, False, 0)
        card.pack_start(label('ISO, 7Z, ZIP ou RAR • A importação é necessária apenas na primeira preparação.', 'subtitle', True), False, False, 0)
        root.pack_start(card, False, False, 0)
        options = Gtk.Box(spacing=14)
        self.display = Gtk.ComboBoxText()
        for name in ('16:9 com bordas', 'Preencher a tela', 'Pixels inteiros'): self.display.append_text(name)
        self.display.set_active(0)
        options.pack_start(label('Imagem', 'subtitle'), False, False, 0)
        options.pack_start(self.display, False, False, 0)
        self.fullscreen = Gtk.CheckButton(label='Tela cheia'); self.fullscreen.set_active(True)
        options.pack_start(self.fullscreen, False, False, 0)
        controls = Gtk.Button(label='Controles'); controls.connect('clicked', self.controls)
        options.pack_end(controls, False, False, 0); root.pack_start(options, False, False, 0)
        self.status = label('Disco preparado. Pronto para iniciar.' if self.library.ready() else 'Escolha seu disco para começar.', 'subtitle', True)
        root.pack_start(self.status, False, False, 0)
        self.progress = Gtk.ProgressBar(); root.pack_start(self.progress, False, False, 0)
        self.play = Gtk.Button(label='JOGAR' if self.library.ready() else 'PREPARAR JOGO')
        self.play.get_style_context().add_class('primary'); self.play.connect('clicked', self.activate)
        self.play.set_sensitive(self.library.ready()); root.pack_start(self.play, False, False, 0)
        expander = Gtk.Expander(label='Detalhes da preparação')
        scroll = Gtk.ScrolledWindow(); scroll.set_min_content_height(100)
        self.log = Gtk.TextView(editable=False, cursor_visible=False, wrap_mode=Gtk.WrapMode.WORD_CHAR)
        scroll.add(self.log); expander.add(scroll); root.pack_start(expander, True, True, 0)
        root.pack_end(label('Os arquivos importados e seus saves ficam neste computador.', 'subtitle'), False, False, 0)
        GLib.timeout_add_seconds(1, self.poll)

    def select_file(self, _):
        dialog = Gtk.FileChooserDialog(title='Escolha seu disco de PS2', parent=self, action=Gtk.FileChooserAction.OPEN)
        dialog.add_buttons('Cancelar', Gtk.ResponseType.CANCEL, 'Selecionar', Gtk.ResponseType.OK)
        types = Gtk.FileFilter(); types.set_name('Disco de PS2 / arquivo compactado')
        for extension in ('iso', '7z', 'zip', 'rar'):
            types.add_pattern('*.' + extension); types.add_pattern('*.' + extension.upper())
        dialog.add_filter(types)
        if dialog.run() == Gtk.ResponseType.OK:
            self.source = dialog.get_filename(); self.file.set_text(Path(self.source).name)
            self.play.set_label('PREPARAR JOGO'); self.play.set_sensitive(True)
        dialog.destroy()

    def append(self, line):
        buffer = self.log.get_buffer()
        buffer.insert(buffer.get_end_iter(), line + '\n')
        if buffer.get_line_count() > 500:
            buffer.delete(buffer.get_start_iter(), buffer.get_iter_at_line(100))
        match = re.search(r'Compilando: (\d+)/(\d+)', line)
        if match:
            self.progress.set_fraction(int(match[1]) / int(match[2]))
            self.status.set_text('Compilando o jogo para este PC…')
        elif line.startswith('Dispatch:'):
            self.status.set_text('Preparando o código do jogo…')
        return False

    def activate(self, _):
        if self.busy: return
        if self.source is None and self.library.ready():
            try:
                self.game = self.library.launch(self.display.get_active(), self.fullscreen.get_active())
                self.busy = True; self.status.set_text('Jogo em execução. Seus saves são gravados automaticamente.')
                self.sensitivity(False)
            except Exception as error: self.status.set_text(str(error))
            return
        self.busy = True; self.sensitivity(False); self.progress.set_fraction(0)
        self.status.set_text('Importando e verificando seu disco… A primeira preparação pode demorar.')
        def worker():
            try:
                self.library.prepare(self.source, lambda line: GLib.idle_add(self.append, line))
                GLib.idle_add(self.prepared, None)
            except Exception as error: GLib.idle_add(self.prepared, str(error))
        threading.Thread(target=worker, daemon=False).start()

    def prepared(self, error):
        self.busy = False; self.sensitivity(True)
        self.status.set_text(error or 'Preparação concluída. Você pode iniciar o jogo.')
        if not error:
            self.source = None; self.play.set_label('JOGAR'); self.progress.set_fraction(1)
        return False

    def sensitivity(self, enabled):
        for widget in (self.play, self.choose, self.display, self.fullscreen): widget.set_sensitive(enabled)

    def poll(self):
        if self.game is not None and self.game.poll() is not None:
            code = self.game.returncode; self.game = None; self.busy = False; self.sensitivity(True)
            self.status.set_text('Jogo encerrado.' if code == 0 else 'O jogo encerrou com erro. Consulte last-game.log na pasta do aplicativo.')
        return True

    def controls(self, _):
        dialog = Gtk.MessageDialog(transient_for=self, modal=True, message_type=Gtk.MessageType.INFO,
                                   buttons=Gtk.ButtonsType.CLOSE, text='Controles')
        dialog.format_secondary_text('W A S D: mover\nI J K L: analógico direito\nSetas: direcional\nX / Espaço: X\nZ: Quadrado • C: Círculo • V: Triângulo\nQ / E: L1 / R1\n1 / 3: L2 / R2\nEnter: Start • Tab: Select\nF11: alternar tela cheia\n\nControles USB/Bluetooth compatíveis são detectados automaticamente.')
        dialog.run(); dialog.destroy()

    def closing(self, *_):
        if self.busy:
            self.status.set_text('Aguarde a preparação terminar ou encerre o jogo antes de fechar o launcher.')
            return True
        return False


if __name__ == '__main__':
    Launcher().show_all()
    Gtk.main()
