"""The RecompLauncher manifest (recomplauncher.json) for the Halo 2 Recomp Windows release, and the SDK ZIP that carries
it (after Marvel vs. Capcom 2's recomplauncher_manifest.py).

    python scripts/recomplauncher_manifest.py --version 1.0.0 [--build build-release] [--out installer/Output]

writes Halo2-Recomp-v<version>-Windows-x64.zip (the game, its downloader, the importer, the settings program, Insignia
Setup, the updater, SDL3, the default key map and recomplauncher.json) and its .sha256.
Level 3 of the RecompLauncher guide: prepare (headless halo2_import --prepare), game.update (title update 1.5,
optional: skipped, halo2_import makes it from the disc with the patches/ in the ZIP).
The Inno Setup installer stays the choice for players who do not use the launcher.
"""
import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = '{install}/data'        # data beside the game: settings.ini, keymap.ini, saves, identity, logs (portable)

TU_PATCHES = ('default.xbe.tu15.h2p', 'downloader.xbe.tu15.h2p')

# The files in the ZIP: name in the ZIP <- name in the build folder (or the source tree, for keymap.ini).
PROGRAMS = {
    'halo2_recomp.exe': 'halo2_recomp.exe',
    'halo2_downloader.exe': 'halo2_downloader.exe',
    'halo2_import.exe': 'halo2_import.exe',
    'config.exe': 'Config.exe',
    'Insignia Setup.exe': 'Insignia Setup.exe',
    'Halo2 Updater.exe': 'Halo2 Updater.exe',
    'SDL3.dll': 'SDL3.dll',
}


def manifest():
    def setting(identifier, label, key, choices, default, help_text=None):
        row = {
            'id': identifier, 'label': label,
            'choices': [{'label': name, 'set': {key: str(value)}} for name, value in choices],
            'defaults': {key: str(default)},
        }
        if help_text:
            row['help'] = help_text
        return row

    toggle = [('Off', 0), ('On', 1)]
    # the same settings as config.exe and the F1 menu (src/settings.c: [display] and [input] in settings.ini)
    settings = [
        setting('scale', 'Render scale', 'display/resolution_scale', [('1x', 1), ('2x', 2), ('3x', 3), ('4x', 4), ('5x', 5)], 1,
                'Draws the game sharper; higher needs a faster graphics card.'),
        setting('fullscreen', 'Fullscreen', 'display/fullscreen', toggle, 0, 'Big Picture mode always starts fullscreen.'),
        setting('vsync', 'VSync', 'display/vsync', toggle, 1),
        setting('anisotropy', 'Anisotropic filter', 'display/anisotropy', [('Off', 1), ('2x', 2), ('4x', 4), ('8x', 8), ('16x', 16)], 1),
        setting('fxaa', 'FXAA', 'display/fxaa', toggle, 0),
        setting('msaa', 'MSAA', 'display/msaa', [('Off', 1), ('2x', 2), ('4x', 4), ('8x', 8)], 1),
        setting('widescreen', 'Widescreen', 'display/widescreen',
                [('Off (4:3)', 0), ('Auto (monitor)', 1), ('16:9', 2), ('16:10', 3)], 2,
                'Widens the 3D view; the HUD and menus keep their shape. Takes effect at the next start.'),
        setting('mouse-aim', 'Mouse aim', 'input/mouse_aim', toggle, 0,
                'The mouse turns the player. Off in Xbox Live (controller only there).'),
        setting('mouse-sens', 'Mouse sensitivity', 'input/mouse_sens',
                [('25%', 25), ('50%', 50), ('75%', 75), ('100%', 100), ('125%', 125), ('150%', 150), ('200%', 200),
                 ('250%', 250), ('300%', 300), ('400%', 400), ('500%', 500)], 100),
        setting('mouse-invert', 'Invert mouse', 'input/mouse_invert', toggle, 0),
    ]
    return {
        '$schema': 'https://recomplauncher.com/schema/recomplauncher.json',
        'format': 1,
        'name': 'Halo 2',
        'repo': 'dalves408/halo2-recomp',
        'game': {
            'kind': 'iso_or_folder',
            'label': 'Your Halo 2 (USA, Europe) Xbox disc image, or the folder with its files (the one that holds default.xbe).',
            'check': {'file': 'default.xbe', 'xdvdfs': 'default.xbe'},
            'update': {
                'into': 'data/TitleUpdate',
                'label': "Title update 1.5 (optional): its folder from your console's hard drive (E:\\TDATA\\4D530064\\$u, "
                         'with default.xbe and downloader.xbe). Skip it and the update is made from your disc.',
                'accepts': 'folder',
                'optional': True,
            },
        },
        'start': {'program': ['halo2_recomp.exe']},
        'prepare': {
            'program': ['halo2_import.exe'],
            'args': ['--prepare', '{game}', '{install}/game_files'],
            'label': 'Game files from your disc, with title update 1.5',
            'timeout': 1800,
        },
        'config': [{
            'format': 'ini',
            'file': [{'path': DATA + '/settings.ini'}],
            'new': {key: value for row in settings for key, value in row['defaults'].items()},
        }],
        'settings': settings,
        'saves': [DATA + '/UDATA'],
        'folders': {
            'saves': [{'path': DATA + '/UDATA'}],
            'data': [{'path': DATA}],
            # The game's own log, one file per run, the last three kept (src/h2_diag.c). The launcher puts the newest
            # .log from here in the crash report a player sends.
            'logs': [{'path': DATA + '/logs'}],
            'downloads': [{'path': DATA + '/TDATA'}],
        },
        'actions': [
            {'id': 'open-saves', 'label': 'Open the save folder', 'open': 'saves'},
            {'id': 'open-data', 'label': 'Open the settings folder', 'open': 'data',
             'help': 'settings.ini and keymap.ini (also set by config.exe and F1 in the game), and the console identity '
                     'Insignia Setup saves (Identity).'},
            {'id': 'open-logs', 'label': 'Open the logs folder', 'open': 'logs',
             'help': 'One log per run, the last three kept. Attach the newest to a problem report.'},
            {'id': 'open-downloads', 'label': 'Open the downloaded content folder', 'open': 'downloads',
             'help': 'Map packs and other content the game downloaded from Xbox Live (Insignia).'},
            {'id': 'clear-update-downloads', 'label': 'Delete saved update downloads', 'clear': 'data/Update',
             'help': 'What the in-game updater left behind. The installed game stays.'},
        ],
        'exit_codes': {
            '2': 'The disc image or folder could not be found.',
            '3': 'That file is not an Xbox disc image.',
            '4': 'That is not the supported Halo 2 disc (USA, Europe).',
            '5': 'The disc image is damaged or incomplete.',
            '6': 'The game files could not be written to the install folder.',
            '7': "The title update you picked is not title update 1.5: pick its folder from your console's hard drive "
                 '(E:\\TDATA\\4D530064\\$u), or skip it.',
        },
        'looks': {'graphics': 'd3d11', 'lines': 480},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', required=True, help='release version, e.g. 1.0.0')
    parser.add_argument('--build', type=Path, default=ROOT / 'build-release')
    parser.add_argument('--out', type=Path, default=ROOT / 'installer' / 'Output')
    parser.add_argument('--json-only', action='store_true', help='print recomplauncher.json and exit')
    args = parser.parse_args()
    text = json.dumps(manifest(), indent=2) + '\n'
    if args.json_only:
        sys.stdout.write(text)
        return
    args.out.mkdir(parents=True, exist_ok=True)
    archive = args.out / f'Halo2-Recomp-v{args.version}-Windows-x64.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as package:
        for name, source in PROGRAMS.items():
            package.write(args.build / source, name)
        package.write(ROOT / 'keymap.ini', 'keymap.ini')
        # title update 1.5 made from the disc when the player has none (halo2_import.c; scripts/convert_tu_patches.py)
        for name in TU_PATCHES:
            package.write(ROOT / 'installer' / 'patches' / name, f'patches/{name}')
        package.writestr('recomplauncher.json', text)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n')
    print(f'RecompLauncher ZIP: {archive}\nSHA-256: {digest}')


if __name__ == '__main__':
    main()
