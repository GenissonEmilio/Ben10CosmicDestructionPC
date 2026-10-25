"""Linux launcher process boundary. GPL-3.0; importer keeps its MIT license."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
APP_ID = 'io.github.GenissonEmilio.Ben10CosmicDestructionPC'


def clean_environment():
    env = dict(os.environ)
    for name in ('PYTHONHOME', 'PYTHONPATH', 'GI_TYPELIB_PATH', 'GIO_EXTRA_MODULES',
                 'GTK_PATH', 'LD_LIBRARY_PATH', 'LD_LIBRARY_PATH_ORIG'):
        env.pop(name, None)
    return env


class Library:
    def __init__(self, directory=None, kit=None):
        data = Path(os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share')))
        if not data.is_absolute(): data = Path.home() / '.local/share'
        self.directory = Path(directory or data / 'Ben10CosmicDestructionPC').resolve()
        self.kit = Path(kit or os.environ.get('COSMIC_COMPILER', ROOT / 'artifacts/compiler')).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.work = self.directory / 'work'
        self.player = self.directory / 'player'
        self.player.mkdir(exist_ok=True)

    def ready(self):
        boot, marker = self.work / 'disc/SLUS_219.38', self.work / 'build.json'
        try:
            info = json.loads(marker.read_text())
            compiler = json.loads((self.kit / 'manifest.json').read_text())
            return (boot.is_file() and (self.work / 'game').is_file() and bool(info.get('sha256'))
                    and info.get('compiler_id') == compiler.get('build_id'))
        except (OSError, ValueError, TypeError):
            return False

    def prepare(self, source, report):
        if not (self.kit / 'bin/ps2_recomp').is_file():
            raise ValueError('O kit do compilador está ausente. Reinstale o pacote completo.')
        if not Path(source).is_file():
            raise ValueError('O disco selecionado não foi encontrado.')
        command = [sys.executable, str(ROOT / 'tools/prepare_game.py'), str(source),
                   '--kit', str(self.kit), '--work', str(self.work), '--jobs', '1']
        # The compiler also works on distributions without systemd. Where
        # available, bound its process tree without imposing limits on the game.
        env = clean_environment()
        if shutil.which('systemctl') and subprocess.run(['systemctl', '--user', 'show-environment'], env=env,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            command = ['systemd-run', '--user', '--scope', '--quiet',
                       '--property=CPUQuota=200%', '--property=MemoryMax=2500M',
                       '--property=MemorySwapMax=0', *command]
        with (self.directory / 'last-prepare.log').open('w') as log:
            process = subprocess.Popen(command, env=env, cwd=self.directory, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, errors='replace',
                                       start_new_session=True)
            for line in process.stdout:
                log.write(line); log.flush()
                report(line.rstrip())
            if process.wait():
                raise ValueError('A preparação não terminou. Abra os detalhes para consultar o erro.')
        if not self.ready(): raise ValueError('A preparação terminou sem produzir um executável válido.')

    def launch(self, display=0, fullscreen=True):
        if not self.ready(): raise ValueError('Prepare seu disco antes de jogar.')
        from import_disc import EXPECTED, sha256
        boot = self.work / 'disc/SLUS_219.38'
        if sha256(boot) != EXPECTED[boot.name][1]:
            raise ValueError('O executável do disco foi alterado; importe sua cópia novamente.')
        marker = json.loads((self.work / 'build.json').read_text())
        game = self.work / 'game'
        if hashlib.sha256(game.read_bytes()).hexdigest() != marker['sha256']:
            raise ValueError('O executável compilado foi alterado; prepare o jogo novamente.')
        settings = self.player / 'cosmic_settings.cfg'
        settings.write_text(f'fullscreen={int(fullscreen)}\nscreen={int(display)}\n')
        env = clean_environment()
        for name in list(env):
            if name.startswith(('COSMIC_PAD_', 'COSMIC_SHOT_', 'COSMIC_EXIT_')):
                env.pop(name)
        env.update(LD_LIBRARY_PATH=str(self.kit / 'lib'), PS2X_IOP_NATIVE='1',
                   PS2X_GS_WORKER='1', COSMIC_SAVE_DIR=str(self.player / 'saves'))
        env.setdefault('LP_NUM_THREADS', '2')
        with (self.directory / 'last-game.log').open('w') as log:
            return subprocess.Popen([str(game), str(boot)], env=env, cwd=self.player,
                                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
