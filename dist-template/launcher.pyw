#!/usr/bin/env python3
"""Settings launcher: edits hmbtn.ini next to this file and starts the game.

Needs Python 3 with tkinter (part of the python.org installer). Double-click it, or run
"python launcher.pyw". Only options you change are written; comments in hmbtn.ini stay.
English, Portuguese or Spanish, as the system until a flag in the header picks one (kept in hmbtn.ini).
"""
import os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, 'frozen', False) else __file__))  # also as Launcher.exe
INI = os.path.join(HERE, 'hmbtn.ini')
EXE = os.path.join(HERE, 'HarvestMoonBTN.exe' if os.name == 'nt' else 'HarvestMoonBTN')
# the launcher's release: tools/release.py publishes it on GitHub as v<VERSION>, with Launcher.exe attached;
# a Launcher.exe finds newer ones there, updates itself and rebuilds the game
VERSION = '1.0.0'
RELEASES = 'https://api.github.com/repos/d4rksp4rt4n/HarvestMoonBTN-Recomp-Enhanced/releases/latest'
LAUNCHER_EXE = 'HarvestMoonBTN-Launcher.exe'
GAME_FILES = ('SLUS_011.15', 'A_FILE.HDT', 'A_FILE.BIN')    # what build.py reads from the disc (extract/)
# the build scripts and sources for the Build page: inside Launcher.exe (tools/build_launcher.py), or the
# checkout this folder was built from (dist/<name>); WORK gets the downloads, your game's files and the build
if getattr(sys, 'frozen', False):
    SRC, WORK = os.path.join(getattr(sys, '_MEIPASS', ''), 'src'), os.path.join(HERE, 'build-files')
else:
    SRC = WORK = os.path.dirname(os.path.dirname(HERE))
if not os.path.isfile(os.path.join(SRC, 'build.py')):
    SRC = None

if getattr(sys, 'frozen', False) and sys.argv[1:2] and sys.argv[1].endswith('.py'):
    # "Launcher.exe script.py ...": the Build page runs the build scripts with the Python this exe carries
    import runpy, traceback
    import collections, concurrent.futures, glob, hashlib, json, platform, shutil, tarfile, urllib.request, \
        zipfile, zlib  # noqa: F401  (what the scripts import, so that PyInstaller packs it)
    if sys.stdout:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace', line_buffering=True)
        sys.stderr.reconfigure(encoding='utf-8', errors='replace', line_buffering=True)
    sys.argv = sys.argv[1:]
    sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[0])))
    try:
        runpy.run_path(sys.argv[0], run_name='__main__')
    except SystemExit:
        raise
    except BaseException:
        traceback.print_exc()                   # to the Build page, not a window of its own
        sys.exit(1)
    sys.exit()

BUTTONS = ['up', 'down', 'left', 'right', 'cross', 'circle', 'square', 'triangle',
           'l1', 'r1', 'l2', 'r2', 'start', 'select']
# (page, ini section, key, kind)   kind: file, dir, spinN (1..N), bool, bool:1 (on unless set to 0),
# text, choice:a|b|c, edit:a|b|c (a choice that can also be typed)
OPTIONS = [
    ('video', 'video', 'upscale', 'choice:1|2|3|4'),
    ('video', 'video', 'widescreen', 'choice:off|16:9|16:10|21:9'),
    ('video', 'video', 'pgxp', 'bool'),
    ('video', 'video', 'crackfix', 'bool:1'),
    ('video', 'video', 'perspective', 'bool'),
    ('video', 'video', 'interp', 'bool'),
    ('video', 'video', 'texturepack', 'bool'),
    ('video', 'video', 'dumptextures', 'bool'),
    ('video', 'video', 'drawthreads', 'choice:auto|0|1|2|3|4'),
    ('video', 'video', 'scale', 'spin8'),
    ('video', 'video', 'fullscreen', 'bool'),
    ('video', 'video', 'scaling', 'choice:sharp-bilinear|sharp|smooth'),
    ('video', 'video', 'aspect43', 'bool'),
    ('game', 'general', 'disc', 'file'),
    ('game', 'general', 'savedir', 'dir'),
    ('game', 'behaviour', 'autoslot', 'bool'),
    ('game', 'behaviour', 'fastcard', 'bool'),
    ('game', 'behaviour', 'quickui', 'bool'),
    ('game', 'behaviour', 'fastload', 'bool'),
    ('game', 'behaviour', 'fastcd', 'choice:0|1|8|16'),
    ('game', 'behaviour', 'textspeed', 'spin16'),
    ('game', 'behaviour', 'skipscenes', 'bool'),
    ('game', 'behaviour', 'fastforward', 'bool'),
    ('game', 'behaviour', 'overclock', 'edit:100|150|200|300'),
    ('game', 'behaviour', 'emptybag', 'bool'),
    ('game', 'behaviour', 'record', 'bool'),
] + [('keyboard', 'keyboard', 'key_' + b, 'text') for b in BUTTONS] \
  + [('controller', 'controller', 'pad_' + b, 'text') for b in BUTTONS] \
  + [('controller', 'controller', 'stick_as_dpad', 'bool')] \
  + [('coop', 'netplay', 'coop_address', 'text'), ('coop', 'netplay', 'coop_port', 'text'),
     ('coop', 'netplay', 'coop_delay', 'choice:3|2|4|5|6|8'), ('coop', 'netplay', 'coop_p2', 'bool')]   + [('p2', 'video', 'p2view_upscale', 'choice:auto|1|2|3|4')] \
  + [('cheats', 'cheats', 'cheat_money', 'edit:0|10000|100000|1000000|9999999'), ('cheats', 'cheats', 'cheat_stamina', 'bool'),
     ('cheats', 'cheats', 'cheat_clock', 'bool'), ('cheats', 'cheats', 'cheat_bag', 'bool'),
     ('cheats', 'cheats', 'cheat_items', 'bool'), ('cheats', 'cheats', 'cheat_tools', 'bool')]
COOP_PORT = '7845'

# Video presets by processor: the picture is drawn by the processor (no graphics card needed), and
# the internal resolution is what costs. Every effect stays on; only the resolution changes.
PRESETS = [
    ('preset2', {'upscale': '2'}),
    ('preset4', {'upscale': '3'}),
    ('preset6', {'upscale': '4'}),
]
PRESET_ALL = {'pgxp': '1', 'crackfix': '1', 'perspective': '1', 'interp': '1'}

# option -> (what it needs, as a text key): greyed out until it holds
def _up2(v):
    return v('upscale') not in ('', '1')
REQUIRES = {
    'pgxp': (_up2, 'need_up2'),
    'crackfix': (lambda v: _up2(v) and v('pgxp') == '1', 'need_pgxp'),
    'perspective': (lambda v: _up2(v) and v('pgxp') == '1', 'need_pgxp'),
    'interp': (lambda v: _up2(v) and v('pgxp') == '1', 'need_pgxp'),
    'drawthreads': (_up2, 'need_up2'),
    'texturepack': (_up2, 'need_up2'),
    'dumptextures': (_up2, 'need_up2'),
}

# option -> (title, explanation)
TEXT = {
    'en': {
        'upscale': ('Internal resolution', 'Draws the 3D 2, 3 or 4 times larger than the original 320x240, '
                    'for smoother edges (1 = original). The processor draws the picture, so each step costs a '
                    'lot more: 2 for 2 cores, 3 for 4 cores, 4 for 6 or more.'),
        'widescreen': ('Widescreen', 'Shows more of the farm and town at the sides. Title, menus and videos stay '
                       '4:3. With resolution 2 or more, people and the HUD keep their shape.'),
        'pgxp': ('Precise 3D (PGXP)', 'No more wobbling polygons, the PlayStation\'s trademark shimmer.'),
        'crackfix': ('Close cracks between models', 'Fills the thin gaps precise 3D opens where models meet '
                     '(ground tiles, door frames, fences on the ground), as the PlayStation shows them. Costs '
                     'about 1 ms a frame at resolution 4.'),
        'perspective': ('Perspective-correct textures', 'No warping on large floors and walls. Costs some '
                        'speed.'),
        'interp': ('60 fps camera', 'The camera and scenery move 60 times a second (the game itself still runs '
                   'at 30). Drawn on spare processor cores; F9 switches it while playing.'),
        'texturepack': ('Texture pack', 'Draws people, animals, field objects, text and the HUD with the images in '
                        'the textures folder (not the 3D world). See docs/GRAPHICS.md.'),
        'dumptextures': ('Dump textures', 'Writes each sprite texture the game shows to textures/dump, '
                         'to draw a texture pack from.'),
        'drawthreads': ('Drawing threads', 'Processor threads that draw the 3D together, each a share of the '
                        'picture (the picture is the same). auto = by processor; 0 = the game\'s thread alone.'),
        'scale': ('Window size', 'Multiple of 320x240, when not in fullscreen.'),
        'fullscreen': ('Start in fullscreen', 'F11 or Alt+Enter switches at any time.'),
        'scaling': ('Scaling to the screen', 'sharp-bilinear: sharp, even pixels (recommended). sharp: plain '
                    'nearest pixel. smooth: soft.'),
        'aspect43': ('4:3 picture', 'Like a CRT TV. Off: square pixels.'),
        'disc': ('Game disc', 'Your copy of the game: a .cue/.bin image or a .pak data pack. Empty: the disc '
                 'folder or HarvestMoonBTN.pak next to the game.'),
        'savedir': ('Memory card folder', 'Where the memory cards (card1.mcd ... card1_4.mcd) are kept. Empty: '
                    'the saves folder.'),
        'autoslot': ('Memory card slot 1 directly', 'Saving and loading skip the slot question.'),
        'fastcard': ('Fast memory card', 'No waiting while the card is searched, read or written.'),
        'quickui': ('Quick menus', 'Windows open at once, shorter fades.'),
        'fastload': ('Fast loading', 'Full speed on "Now Loading" and through doors.'),
        'fastcd': ('Disc speed', 'Shorter loading pauses: 0 = original, 1 = 4x, 8 = 8x, 16 = 16x. The music '
                   'is not affected.'),
        'textspeed': ('Text speed', 'How fast dialogue is typed: 1 = original, up to 16.'),
        'skipscenes': ('Fast-forward cutscenes', 'Hold START during a cutscene.'),
        'fastforward': ('Fast-forward with Tab', 'Hold Tab to run the game at full speed.'),
        'overclock': ('Processor speed', 'In percent: 100 = the original PlayStation (slows down in busy '
                      'scenes), 200 = no slowdowns.'),
        'emptybag': ('Empty rucksack: R1 / R2 do nothing', 'So running doesn\'t stop when you press them by '
                     'accident.'),
        'record': ('Record sessions', 'Keeps your button presses in the recordings folder, for bug reports.'),
        'stick_as_dpad': ('Left stick moves too', 'The left stick works as the D-pad.'),
        'coop_address': ('Host address', 'To join: the address your friend gave you (e.g. 26.12.34.56 or '
                         '26.12.34.56:7845).'),
        'coop_port': ('Port', 'The same on both PCs. Empty: 7845.'),
        'p2view_upscale': ('Farmer 2\'s window resolution', 'auto = the same as the game\'s window. 1 is the '
                           'lightest (original 320x240), for a slower PC, but at 30 pictures a second; above the '
                           'game\'s does nothing.'),
        'coop_delay': ('Controls delay', 'In frames (1 = 17 ms). Higher if the game stutters on a slow '
                       'connection; the host\'s counts.'),
        'cheat_money': ('Money', 'Your money when you reach the farm, once each time the game opens; '
                        'then it changes as usual. Changed here with F5 in the game: at once. 0 = off; up to 9999999.'),
        'cheat_stamina': ('Endless stamina', 'The farmers never get tired and never faint.'),
        'cheat_clock': ('Stop the clock', 'Time stands still (shops and nightfall too). Sleeping still starts the next day.'),
        'cheat_bag': ('Big rucksack', 'The biggest rucksack (8 places) from the start, for both farmers.'),
        'cheat_items': ('Materials at most', 'Lumber, fodder, fish food and chicken feed always at 999.'),
        'cheat_tools': ('Tools at most', 'Sickle, hoe, axe, hammer and watering can at their highest level, for '
                        'both farmers.'),
        'coop_p2': ('Two farmers (experimental)', 'The friend gets a farmer of their own, who walks with a pose '
                    'of their own (stops at walls, fences and water; no tools yet). The host\'s choice counts.'),
    },
    'pt': {
        'upscale': ('Resolução interna', 'Desenha o 3D 2, 3 ou 4 vezes maior que o original 320x240, com bordas '
                    'mais lisas (1 = original). Quem desenha é o processador, então cada nível pesa bem mais: '
                    '2 para 2 núcleos, 3 para 4 núcleos, 4 para 6 ou mais.'),
        'widescreen': ('Tela larga', 'Mostra mais da fazenda e da cidade nas laterais. Título, menus e vídeos '
                       'continuam 4:3. Com resolução 2 ou mais, pessoas e o HUD mantêm a forma.'),
        'pgxp': ('3D preciso (PGXP)', 'Acaba com os polígonos tremendo, o tremido típico do PlayStation.'),
        'crackfix': ('Fechar frestas entre modelos', 'Preenche as frestas finas que o 3D preciso abre onde os '
                     'modelos se encontram (pisos, batentes de portas, cercas no chão), como o PlayStation mostra. '
                     'Custa cerca de 1 ms por quadro na resolução 4.'),
        'perspective': ('Texturas com perspectiva correta', 'Sem distorção em chãos e paredes grandes. Pesa um '
                        'pouco.'),
        'interp': ('Câmera a 60 fps', 'A câmera e o cenário se movem 60 vezes por segundo (o jogo continua a '
                   '30). Usa núcleos livres do processador; F9 liga e desliga durante o jogo.'),
        'texturepack': ('Pacote de texturas', 'Desenha pessoas, animais, objetos do campo, texto e o HUD com as '
                        'imagens da pasta textures (não o mundo 3D). Veja docs/GRAPHICS.md.'),
        'dumptextures': ('Exportar texturas', 'Grava cada textura de sprite que o jogo mostra em textures/dump, '
                         'para criar um pacote de texturas.'),
        'drawthreads': ('Threads de desenho', 'Threads do processador que desenham o 3D juntas, cada uma uma '
                        'parte da imagem (a imagem é a mesma). auto = pelo processador; 0 = só a do jogo.'),
        'scale': ('Tamanho da janela', 'Múltiplo de 320x240, fora da tela cheia.'),
        'fullscreen': ('Começar em tela cheia', 'F11 ou Alt+Enter alterna a qualquer momento.'),
        'scaling': ('Ajuste à tela', 'sharp-bilinear: pixels nítidos e iguais (recomendado). sharp: pixel mais '
                    'próximo. smooth: suave.'),
        'aspect43': ('Imagem 4:3', 'Como numa TV de tubo. Desligado: pixels quadrados.'),
        'disc': ('Disco do jogo', 'Sua cópia do jogo: imagem .cue/.bin ou pacote .pak. Vazio: a pasta disc ou '
                 'o HarvestMoonBTN.pak ao lado do jogo.'),
        'savedir': ('Pasta dos memory cards', 'Onde ficam os memory cards (card1.mcd ... card1_4.mcd). Vazio: a '
                    'pasta saves.'),
        'autoslot': ('Usar o slot 1 direto', 'Salvar e carregar sem perguntar o slot do memory card.'),
        'fastcard': ('Memory card rápido', 'Sem espera enquanto o card é procurado, lido ou gravado.'),
        'quickui': ('Menus rápidos', 'Janelas abrem na hora, transições mais curtas.'),
        'fastload': ('Carregamento rápido', 'Velocidade máxima no "Now Loading" e ao passar por portas.'),
        'fastcd': ('Velocidade do disco', 'Pausas de carregamento menores: 0 = original, 1 = 4x, 8 = 8x, '
                   '16 = 16x. A música não é afetada.'),
        'textspeed': ('Velocidade do texto', 'Quão rápido o diálogo aparece: 1 = original, até 16.'),
        'skipscenes': ('Acelerar cenas', 'Segure START durante uma cena.'),
        'fastforward': ('Acelerar com Tab', 'Segure Tab para o jogo rodar em velocidade máxima.'),
        'overclock': ('Velocidade do processador', 'Em porcentagem: 100 = o PlayStation original (fica lento em '
                      'cenas cheias), 200 = sem lentidão.'),
        'emptybag': ('Mochila vazia: R1 / R2 não fazem nada', 'Para não parar de correr ao apertar sem querer.'),
        'record': ('Gravar sessões', 'Guarda os botões apertados na pasta recordings, para relatar bugs.'),
        'stick_as_dpad': ('Analógico esquerdo também anda', 'O analógico esquerdo funciona como o direcional.'),
        'coop_address': ('Endereço do anfitrião', 'Para entrar: o endereço que seu amigo passou (ex.: '
                         '26.12.34.56 ou 26.12.34.56:7845).'),
        'coop_port': ('Porta', 'A mesma nos dois PCs. Vazio: 7845.'),
        'p2view_upscale': ('Resolução da janela do fazendeiro 2', 'auto = a mesma da janela do jogo. 1 é a mais '
                           'leve (original 320x240), para PC mais fraco, mas a 30 imagens por segundo; acima da do '
                           'jogo não muda nada.'),
        'coop_delay': ('Atraso dos controles', 'Em quadros (1 = 17 ms). Aumente se o jogo engasgar com internet '
                       'lenta; vale o do anfitrião.'),
        'cheat_money': ('Dinheiro', 'O dinheiro da fazenda ao entrar nela, uma vez a cada vez que o jogo abre; '
                        'depois ele muda normalmente. Mudado aqui pelo F5 no jogo: na hora. 0 = desligado; até 9999999.'),
        'cheat_stamina': ('Estamina infinita', 'Os fazendeiros nunca cansam e nunca desmaiam.'),
        'cheat_clock': ('Parar o relógio', 'O tempo não passa (lojas e noite também). Dormir ainda começa o dia seguinte.'),
        'cheat_bag': ('Mochila grande', 'A maior mochila (8 lugares) desde o começo, para os dois fazendeiros.'),
        'cheat_items': ('Materiais no máximo', 'Madeira, feno, comida de peixe e ração de galinha sempre em 999.'),
        'cheat_tools': ('Ferramentas no máximo', 'Foice, enxada, machado, marreta e regador no nível mais alto, para '
                        'os dois fazendeiros.'),
        'coop_p2': ('Dois fazendeiros (experimental)', 'O amigo ganha um fazendeiro próprio, que anda com pose '
                    'própria (para em paredes, cercas e água; ainda sem ferramentas). Vale a escolha do anfitrião.'),
    },
    'es': {
        'upscale': ('Resolución interna', 'Dibuja el 3D 2, 3 o 4 veces más grande que el original 320x240, con '
                    'bordes más suaves (1 = original). Dibuja el procesador, así que cada nivel cuesta mucho más: '
                    '2 para 2 núcleos, 3 para 4 núcleos, 4 para 6 o más.'),
        'widescreen': ('Pantalla panorámica', 'Muestra más de la granja y del pueblo a los lados. Título, menús y '
                       'vídeos siguen en 4:3. Con resolución 2 o más, las personas y el HUD mantienen su forma.'),
        'pgxp': ('3D preciso (PGXP)', 'Se acabaron los polígonos temblorosos, el temblor típico de PlayStation.'),
        'crackfix': ('Cerrar grietas entre modelos', 'Rellena las grietas finas que el 3D preciso abre donde los '
                     'modelos se juntan (suelo, marcos de puertas, cercas en el suelo), como las muestra '
                     'PlayStation. Cuesta cerca de 1 ms por cuadro en resolución 4.'),
        'perspective': ('Texturas con perspectiva correcta', 'Sin deformación en suelos y paredes grandes. Cuesta '
                        'algo de velocidad.'),
        'interp': ('Cámara a 60 fps', 'La cámara y el escenario se mueven 60 veces por segundo (el juego sigue a '
                   '30). Usa núcleos libres del procesador; F9 lo alterna durante el juego.'),
        'texturepack': ('Paquete de texturas', 'Dibuja personas, animales, objetos del campo, texto y el HUD con las '
                        'imágenes de la carpeta textures (no el mundo 3D). Ver docs/GRAPHICS.md.'),
        'dumptextures': ('Exportar texturas', 'Guarda cada textura de sprite que el juego muestra en textures/dump, '
                         'para crear un paquete de texturas.'),
        'drawthreads': ('Hilos de dibujo', 'Hilos del procesador que dibujan el 3D juntos, cada uno una parte de '
                        'la imagen (la imagen es la misma). auto = según el procesador; 0 = solo el del juego.'),
        'scale': ('Tamaño de la ventana', 'Múltiplo de 320x240, fuera de pantalla completa.'),
        'fullscreen': ('Empezar en pantalla completa', 'F11 o Alt+Enter la alterna en cualquier momento.'),
        'scaling': ('Ajuste a la pantalla', 'sharp-bilinear: píxeles nítidos y parejos (recomendado). sharp: '
                    'píxel más cercano. smooth: suave.'),
        'aspect43': ('Imagen 4:3', 'Como en un televisor de tubo. Apagado: píxeles cuadrados.'),
        'disc': ('Disco del juego', 'Tu copia del juego: imagen .cue/.bin o paquete .pak. Vacío: la carpeta disc '
                 'o el HarvestMoonBTN.pak junto al juego.'),
        'savedir': ('Carpeta de memory cards', 'Donde se guardan las memory cards (card1.mcd ... card1_4.mcd). '
                    'Vacío: la carpeta saves.'),
        'autoslot': ('Usar directamente la ranura 1', 'Guardar y cargar sin preguntar la ranura de la memory '
                     'card.'),
        'fastcard': ('Memory card rápida', 'Sin esperas mientras la tarjeta se busca, se lee o se escribe.'),
        'quickui': ('Menús rápidos', 'Las ventanas se abren al instante, transiciones más cortas.'),
        'fastload': ('Carga rápida', 'Velocidad máxima en "Now Loading" y al cruzar puertas.'),
        'fastcd': ('Velocidad del disco', 'Pausas de carga más cortas: 0 = original, 1 = 4x, 8 = 8x, 16 = 16x. '
                   'La música no se ve afectada.'),
        'textspeed': ('Velocidad del texto', 'Qué tan rápido aparece el diálogo: 1 = original, hasta 16.'),
        'skipscenes': ('Acelerar escenas', 'Mantén START durante una escena.'),
        'fastforward': ('Acelerar con Tab', 'Mantén Tab para que el juego corra a velocidad máxima.'),
        'overclock': ('Velocidad del procesador', 'En porcentaje: 100 = la PlayStation original (se ralentiza en '
                      'escenas cargadas), 200 = sin ralentizaciones.'),
        'emptybag': ('Mochila vacía: R1 / R2 no hacen nada', 'Para no dejar de correr si los pulsas sin querer.'),
        'record': ('Grabar sesiones', 'Guarda los botones pulsados en la carpeta recordings, para reportar '
                   'errores.'),
        'stick_as_dpad': ('El stick izquierdo también mueve', 'El stick izquierdo funciona como la cruceta.'),
        'coop_address': ('Dirección del anfitrión', 'Para entrar: la dirección que te pasó tu amigo (por ejemplo '
                         '26.12.34.56 o 26.12.34.56:7845).'),
        'coop_port': ('Puerto', 'El mismo en los dos PCs. Vacío: 7845.'),
        'p2view_upscale': ('Resolución de la ventana del granjero 2', 'auto = la misma que la ventana del juego. 1 '
                           'es la más ligera (original 320x240), para un PC más lento, pero a 30 imágenes por segundo; '
                           'más que la del juego no cambia nada.'),
        'coop_delay': ('Retraso de los controles', 'En fotogramas (1 = 17 ms). Más alto si el juego se traba '
                       'con internet lenta; vale el del anfitrión.'),
        'cheat_money': ('Dinero', 'El dinero de la granja al llegar a ella, una vez cada vez que se abre el '
                        'juego; después cambia como siempre. Cambiado aquí con F5 en el juego: al momento. 0 = desactivado; hasta 9999999.'),
        'cheat_stamina': ('Energía infinita', 'Los granjeros nunca se cansan ni se desmayan.'),
        'cheat_clock': ('Parar el reloj', 'El tiempo no pasa (tiendas y noche también). Dormir sigue empezando el día siguiente.'),
        'cheat_bag': ('Mochila grande', 'La mochila más grande (8 huecos) desde el principio, para los dos granjeros.'),
        'cheat_items': ('Materiales al máximo', 'Madera, forraje, comida de peces y pienso de gallina siempre en 999.'),
        'cheat_tools': ('Herramientas al máximo', 'Hoz, azada, hacha, martillo y regadera en su nivel más alto, para '
                        'los dos granjeros.'),
        'coop_p2': ('Dos granjeros (experimental)', 'Tu amigo tiene su propio granjero, que camina con pose '
                    'propia (se detiene en paredes, vallas y agua; aún sin herramientas). Vale lo que elija el anfitrión.'),
    },
}
BUTTON_NAMES = {
    'en': {'up': 'Up', 'down': 'Down', 'left': 'Left', 'right': 'Right', 'cross': 'Cross (X)',
           'circle': 'Circle (O)', 'square': 'Square', 'triangle': 'Triangle', 'l1': 'L1', 'r1': 'R1',
           'l2': 'L2', 'r2': 'R2', 'start': 'START', 'select': 'SELECT'},
    'pt': {'up': 'Cima', 'down': 'Baixo', 'left': 'Esquerda', 'right': 'Direita', 'cross': 'Xis (X)',
           'circle': 'Círculo (O)', 'square': 'Quadrado', 'triangle': 'Triângulo', 'l1': 'L1', 'r1': 'R1',
           'l2': 'L2', 'r2': 'R2', 'start': 'START', 'select': 'SELECT'},
    'es': {'up': 'Arriba', 'down': 'Abajo', 'left': 'Izquierda', 'right': 'Derecha', 'cross': 'Equis (X)',
           'circle': 'Círculo (O)', 'square': 'Cuadrado', 'triangle': 'Triángulo', 'l1': 'L1', 'r1': 'R1',
           'l2': 'L2', 'r2': 'R2', 'start': 'START', 'select': 'SELECT'},
}
UI = {
    'en': {
        'subtitle': 'PC port settings', 'start': 'Start', 'video': 'Video', 'game': 'Game',
        'keyboard': 'Keyboard', 'controller': 'Controller',
        'welcome': 'Welcome to the farm!', 'welcome_sub': 'Choose the setup for your PC, then press Play. '
        'Everything can be fine-tuned on the other pages.',
        'data_ok': 'Game found: %s', 'data_missing': 'Game disc not found. Put the .cue/.bin in the disc folder '
        'or choose it on the Game page.',
        'pc': 'This PC: %d cores, %d processor threads. No graphics card needed: the processor draws the picture.',
        'quick': 'Quick setup', 'quick_sub': 'All effects stay on (precise 3D, perspective, 60 fps camera); only '
        'the resolution changes.',
        'preset2': 'PC with 2 cores', 'preset4': 'PC with 4 cores', 'preset6': 'PC with 6 or more cores',
        'preset2_d': 'Resolution 2 (640x480). Smooth on simple or older processors.',
        'preset4_d': 'Resolution 3 (960x720). Sharp, and smooth on most PCs.',
        'preset6_d': 'Resolution 4 (1280x960). The sharpest; needs a strong processor.',
        'use': 'Use this', 'in_use': 'In use', 'recommended': 'Recommended for this PC',
        'keys': 'Handy keys', 'keys_list': [
            ('F1 - F4', 'Memory card 1 - 4 in slot 1 (shown in the window title)'),
            ('L1 / R1  (Q / W)', 'On a diary list: previous / next memory card'),
            ('Triangle (hold)', 'On a diary list: hold 2 seconds to delete that diary'),
            ('START (hold)', 'Fast-forward a cutscene'),
            ('Tab (hold)', 'Fast-forward the game'),
            ('F5', 'Cheats, while playing'),
            ('F9', '60 fps camera on / off'),
            ('F11  /  Alt+Enter', 'Fullscreen'),
            ('F12', 'Screenshot (screenshots folder)')],
        'applied': '%s chosen. Save to keep it.',
        'video_sub': 'The picture is drawn by the processor: resolution is what costs the most.',
        'game_sub': 'Game files, memory cards and shortcuts that save time.',
        'keyboard_sub': 'Key names, comma-separated (e.g. X,Space,K). Empty: the default.',
        'controller_sub': 'Controller buttons: a b x y back start leftshoulder rightshoulder lefttrigger '
        'righttrigger dpup dpdown dpleft dpright leftstick rightstick. Empty: the default.',
        'need_up2': 'Needs resolution 2 or more', 'need_pgxp': 'Needs Precise 3D (PGXP)',
        'on': 'On', 'off': 'Off', 'browse': 'Browse...', 'default': 'default: %s',
        'close': 'Close', 'save': 'Save', 'play': 'Save and play',
        'unsaved': 'Unsaved changes', 'saved': 'Saved', 'ask_save': 'Save your changes?',
        'missing': '%s is missing.',
        'coop': 'Online co-op',
        'coop_sub': 'Play with a friend over the internet: each one runs the game on their own PC and both '
        'control the same farmer. Your normal game is not affected.',
        'host': 'Host', 'host_d': 'Your farm: your memory cards and settings. Your friend joins you. Saving '
        'in co-op writes to your memory cards.',
        'join': 'Join', 'join_d': 'Your friend\'s farm. Fill in their address below. Your own saves are not '
        'touched (co-op saves go to saves\\netplay).',
        'host_btn': 'Host and play', 'join_btn': 'Join and play',
        'cheats': 'Cheats', 'cheats_sub': 'Changes to the game itself. What you save with them stays in the '
        'save. In co-op, the host\'s cheats count for both.',
        'p2': 'Local co-op', 'p2_sub': 'Two farmers on this PC, no internet: you play the game\'s farmer and a '
        'second player gets a farmer of their own, with a window of their own (experimental).',
        'local': 'Two farmers on this PC', 'local_d': 'Farmer 1 plays with the keyboard or the first controller; '
        'farmer 2 with the second controller or the numeric keypad.',
        'local_btn': 'Play with two farmers',
        'p2_keys': 'Farmer 2\'s controls', 'p2_head': ('', 'Controller 2', 'Numeric keypad'), 'p2_list': [
            ('Walk', 'D-pad', '8  4  5  6'), ('Use the tool', 'Square', '7'),
            ('Look, talk, open, trade', 'Cross', '9'), ('Run', 'Circle', '1'), ('Change tools', 'R1', '3'),
            ('Put away / change the item', 'R2', '+'), ('Whistle', 'L1 / L2', '/   *'),
            ('Rucksack', 'Triangle', '0'), ('Menu', 'Start', 'Enter'), ('Map', 'Select', '.')],
        'p2_tips': 'Good to know', 'p2_tips_list': [
            ('\u21c4', 'Trading: stand in front of the other farmer, close, and press Cross (9). First the tools, '
                        'then the items, as many as the other\'s rucksack holds.'),
            ('!', 'There is one of each tool: farmer 2 starts with nothing and takes tools from the tool chest.'),
            ('!', 'Only what farmer 1 carries is saved: when saving, farmer 2\'s tools go back to the chest.')],
        'my_ip': 'Addresses of this PC (give one to your friend): %s',
        'build_id': "Game version: %s. Your friend's must be the same (it is also in the game's window title).",
        'need_addr': 'Fill in the host address first.',
        'build': 'Build from my disc', 'build_sub': 'Makes the PC game from your own disc image, here in this '
        'folder. The first time, it also downloads the compiler (about 65 MB, checked file by file).',
        'pak': 'Data pack', 'pak_d': "Also copies the game's data into HarvestMoonBTN.pak: the disc image is not "
        'needed afterwards.',
        'build_btn': 'Build the game', 'building': 'Building... (this takes a few minutes)',
        'built': 'Done: the game is ready.', 'build_failed': 'The build stopped: the last lines above say why.',
        'need_cue': 'Choose your disc image (.cue or .bin) first.',
        'update': 'Update to %s', 'update_note': 'Launcher %s is out.',
        'updating': 'Downloading the new launcher...', 'update_failed': 'The update failed: %s',
        'ask_stop': 'A build is running. Stop it and close?',
        'build_note': 'The build-files folder holds files from your disc: it is for this PC only. To give the '
        'game to a friend, give them this launcher; they build from their own disc.',
        'how': 'How to connect', 'how_list': [
            ('1', 'Both need this same game folder version (the same HarvestMoonBTN.exe).'),
            ('2', 'Easiest: both install Radmin VPN or ZeroTier and join the same network; the host gives the '
                  'address it shows there (26.x.x.x).'),
            ('3', 'Or the host opens TCP port %s in the router and gives the internet address.'),
            ('4', 'The host presses Host and play first, then the friend presses Join and play. If Windows asks '
                  'about the firewall, allow it.'),
            ('!', 'Tab and F1-F4 are off in co-op; the window title shows the connection.')],
    },
    'pt': {
        'subtitle': 'Configurações do port para PC', 'start': 'Início', 'video': 'Vídeo', 'game': 'Jogo',
        'keyboard': 'Teclado', 'controller': 'Controle',
        'welcome': 'Bem-vindo à fazenda!', 'welcome_sub': 'Escolha a configuração do seu PC e clique em '
        'Jogar. Tudo pode ser ajustado nas outras páginas.',
        'data_ok': 'Jogo encontrado: %s', 'data_missing': 'Disco do jogo não encontrado. Coloque o .cue/.bin na '
        'pasta disc ou escolha na página Jogo.',
        'pc': 'Este PC: %d núcleos, %d threads. Não precisa de placa de vídeo: o processador desenha a imagem.',
        'quick': 'Configuração rápida', 'quick_sub': 'Todos os efeitos continuam ligados (3D preciso, '
        'perspectiva, câmera a 60 fps); só a resolução muda.',
        'preset2': 'PC com 2 núcleos', 'preset4': 'PC com 4 núcleos', 'preset6': 'PC com 6 ou mais núcleos',
        'preset2_d': 'Resolução 2 (640x480). Liso em processadores simples ou antigos.',
        'preset4_d': 'Resolução 3 (960x720). Nítido e liso na maioria dos PCs.',
        'preset6_d': 'Resolução 4 (1280x960). O mais nítido; pede um processador forte.',
        'use': 'Usar este', 'in_use': 'Em uso', 'recommended': 'Recomendado para este PC',
        'keys': 'Teclas úteis', 'keys_list': [
            ('F1 - F4', 'Memory card 1 - 4 no slot 1 (aparece no título da janela)'),
            ('L1 / R1  (Q / W)', 'Na lista de diários: memory card anterior / seguinte'),
            ('Triângulo (segurar)', 'Na lista de diários: segure 2 segundos para apagar o diário'),
            ('START (segurar)', 'Acelera uma cena'),
            ('Tab (segurar)', 'Acelera o jogo'),
            ('F5', 'Cheats, durante o jogo'),
            ('F9', 'Liga / desliga a câmera a 60 fps'),
            ('F11  /  Alt+Enter', 'Tela cheia'),
            ('F12', 'Captura de tela (pasta screenshots)')],
        'applied': '%s escolhido. Salve para manter.',
        'video_sub': 'A imagem é desenhada pelo processador: a resolução é o que mais pesa.',
        'game_sub': 'Arquivos do jogo, memory cards e atalhos que poupam tempo.',
        'keyboard_sub': 'Nomes das teclas separados por vírgula (ex.: X,Space,K). Vazio: o padrão.',
        'controller_sub': 'Botões do controle: a b x y back start leftshoulder rightshoulder lefttrigger '
        'righttrigger dpup dpdown dpleft dpright leftstick rightstick. Vazio: o padrão.',
        'need_up2': 'Precisa de resolução 2 ou mais', 'need_pgxp': 'Precisa do 3D preciso (PGXP)',
        'on': 'Ligado', 'off': 'Desligado', 'browse': 'Procurar...', 'default': 'padrão: %s',
        'close': 'Fechar', 'save': 'Salvar', 'play': 'Salvar e jogar',
        'unsaved': 'Alterações não salvas', 'saved': 'Salvo', 'ask_save': 'Salvar as alterações?',
        'missing': '%s não foi encontrado.',
        'coop': 'Coop online',
        'coop_sub': 'Jogue com um amigo pela internet: cada um roda o jogo no próprio PC e os dois controlam o '
        'mesmo fazendeiro. O seu jogo normal não muda.',
        'host': 'Hospedar', 'host_d': 'A sua fazenda: seus memory cards e configurações. O amigo entra no seu '
        'jogo. Salvar no coop grava nos seus memory cards.',
        'join': 'Entrar', 'join_d': 'A fazenda do amigo. Preencha o endereço dele abaixo. Seus saves não são '
        'tocados (o que salvar no coop vai para saves\\netplay).',
        'host_btn': 'Hospedar e jogar', 'join_btn': 'Entrar e jogar',
        'cheats': 'Cheats', 'cheats_sub': 'Mudam o próprio jogo. O que você salvar com eles fica no save. No '
        'coop, valem os cheats do anfitrião para os dois.',
        'p2': 'Coop local', 'p2_sub': 'Dois fazendeiros neste PC, sem internet: você joga com o fazendeiro do '
        'jogo e outra pessoa ganha um fazendeiro próprio, com uma janela só dele (experimental).',
        'local': 'Dois fazendeiros neste PC', 'local_d': 'O fazendeiro 1 joga com o teclado ou o primeiro '
        'controle; o fazendeiro 2, com o segundo controle ou o teclado numérico.',
        'local_btn': 'Jogar com dois fazendeiros',
        'p2_keys': 'Controles do fazendeiro 2', 'p2_head': ('', 'Controle 2', 'Teclado numérico'), 'p2_list': [
            ('Andar', 'Direcional', '8  4  5  6'), ('Usar a ferramenta', 'Quadrado', '7'),
            ('Olhar, falar, abrir, trocar', 'X', '9'), ('Correr', 'Círculo', '1'),
            ('Trocar de ferramenta', 'R1', '3'), ('Guardar / trocar o item', 'R2', '+'),
            ('Assobiar', 'L1 / L2', '/   *'), ('Mochila', 'Triângulo', '0'), ('Menu', 'Start', 'Enter'),
            ('Mapa', 'Select', '.')],
        'p2_tips': 'Bom saber', 'p2_tips_list': [
            ('\u21c4', 'Troca: fique de frente para o outro fazendeiro, bem perto, e aperte X (9). Primeiro as '
                        'ferramentas, depois os itens, até onde cabe na mochila do outro.'),
            ('!', 'Há uma de cada ferramenta: o fazendeiro 2 começa sem nada e pega no baú de ferramentas.'),
            ('!', 'Só o que o fazendeiro 1 carrega é salvo: ao salvar, as ferramentas do 2 voltam para o baú.')],
        'my_ip': 'Endereços deste PC (passe um para o amigo): %s',
        'build_id': 'Versão do jogo: %s. A do amigo tem que ser a mesma (ela também fica no título da janela do jogo).',
        'need_addr': 'Preencha o endereço do anfitrião primeiro.',
        'build': 'Montar do meu disco', 'build_sub': 'Faz o jogo de PC a partir da imagem do seu próprio disco, '
        'aqui nesta pasta. Na primeira vez, também baixa o compilador (uns 65 MB, conferido arquivo por arquivo).',
        'pak': 'Pacote de dados', 'pak_d': 'Também copia os dados do jogo para HarvestMoonBTN.pak: depois a imagem '
        'do disco não é mais necessária.',
        'build_btn': 'Montar o jogo', 'building': 'Montando... (leva alguns minutos)',
        'built': 'Pronto: o jogo está montado.', 'build_failed': 'A montagem parou: as últimas linhas acima dizem por quê.',
        'need_cue': 'Escolha primeiro a imagem do disco (.cue ou .bin).',
        'update': 'Atualizar para %s', 'update_note': 'Saiu o launcher %s.',
        'updating': 'Baixando o novo launcher...', 'update_failed': 'A atualização falhou: %s',
        'ask_stop': 'Uma montagem está em andamento. Parar e fechar?',
        'build_note': 'A pasta build-files guarda arquivos do seu disco: é só para este PC. Para dar o jogo a um '
        'amigo, dê este launcher; ele monta com o próprio disco.',
        'how': 'Como conectar', 'how_list': [
            ('1', 'Os dois precisam desta mesma versão do jogo (o mesmo HarvestMoonBTN.exe).'),
            ('2', 'Mais fácil: os dois instalam o Radmin VPN ou o ZeroTier e entram na mesma rede; o anfitrião '
                  'passa o endereço que aparece lá (26.x.x.x).'),
            ('3', 'Ou o anfitrião abre a porta TCP %s no roteador e passa o endereço da internet.'),
            ('4', 'O anfitrião clica em Hospedar e jogar primeiro; depois o amigo clica em Entrar e jogar. Se o '
                  'Windows perguntar sobre o firewall, permita.'),
            ('!', 'No coop, Tab e F1-F4 ficam desligados; o título da janela mostra a conexão.')],
    },
    'es': {
        'subtitle': 'Ajustes del port para PC', 'start': 'Inicio', 'video': 'Vídeo', 'game': 'Juego',
        'keyboard': 'Teclado', 'controller': 'Mando',
        'welcome': '¡Bienvenido a la granja!', 'welcome_sub': 'Elige la configuración de tu PC y pulsa Jugar. '
        'Todo se puede ajustar en las otras páginas.',
        'data_ok': 'Juego encontrado: %s', 'data_missing': 'No se encontró el disco del juego. Pon el .cue/.bin '
        'en la carpeta disc o elígelo en la página Juego.',
        'pc': 'Este PC: %d núcleos, %d hilos. No hace falta tarjeta gráfica: el procesador dibuja la imagen.',
        'quick': 'Configuración rápida', 'quick_sub': 'Todos los efectos siguen activos (3D preciso, '
        'perspectiva, cámara a 60 fps); solo cambia la resolución.',
        'preset2': 'PC con 2 núcleos', 'preset4': 'PC con 4 núcleos', 'preset6': 'PC con 6 o más núcleos',
        'preset2_d': 'Resolución 2 (640x480). Fluido en procesadores sencillos o antiguos.',
        'preset4_d': 'Resolución 3 (960x720). Nítido y fluido en la mayoría de PCs.',
        'preset6_d': 'Resolución 4 (1280x960). La más nítida; necesita un procesador potente.',
        'use': 'Usar este', 'in_use': 'En uso', 'recommended': 'Recomendado para este PC',
        'keys': 'Teclas útiles', 'keys_list': [
            ('F1 - F4', 'Memory card 1 - 4 en la ranura 1 (se ve en el título de la ventana)'),
            ('L1 / R1  (Q / W)', 'En la lista de diarios: memory card anterior / siguiente'),
            ('Triángulo (mantener)', 'En la lista de diarios: mantén 2 segundos para borrar ese diario'),
            ('START (mantener)', 'Acelera una escena'),
            ('Tab (mantener)', 'Acelera el juego'),
            ('F5', 'Trucos, durante el juego'),
            ('F9', 'Activa / desactiva la cámara a 60 fps'),
            ('F11  /  Alt+Enter', 'Pantalla completa'),
            ('F12', 'Captura de pantalla (carpeta screenshots)')],
        'applied': '%s elegido. Guarda para conservarlo.',
        'video_sub': 'La imagen la dibuja el procesador: la resolución es lo que más cuesta.',
        'game_sub': 'Archivos del juego, memory cards y atajos que ahorran tiempo.',
        'keyboard_sub': 'Nombres de teclas separados por comas (p. ej. X,Space,K). Vacío: el predeterminado.',
        'controller_sub': 'Botones del mando: a b x y back start leftshoulder rightshoulder lefttrigger '
        'righttrigger dpup dpdown dpleft dpright leftstick rightstick. Vacío: el predeterminado.',
        'need_up2': 'Necesita resolución 2 o más', 'need_pgxp': 'Necesita el 3D preciso (PGXP)',
        'on': 'Activado', 'off': 'Desactivado', 'browse': 'Buscar...', 'default': 'predeterminado: %s',
        'close': 'Cerrar', 'save': 'Guardar', 'play': 'Guardar y jugar',
        'unsaved': 'Cambios sin guardar', 'saved': 'Guardado', 'ask_save': '¿Guardar los cambios?',
        'missing': 'No se encontró %s.',
        'coop': 'Coop en línea',
        'coop_sub': 'Juega con un amigo por internet: cada uno corre el juego en su PC y los dos controlan al '
        'mismo granjero. Tu juego normal no cambia.',
        'host': 'Ser anfitrión', 'host_d': 'Tu granja: tus memory cards y ajustes. Tu amigo entra en tu juego. '
        'Guardar en el coop escribe en tus memory cards.',
        'join': 'Entrar', 'join_d': 'La granja de tu amigo. Escribe su dirección abajo. Tus partidas no se tocan '
        '(lo que guardes en el coop va a saves\\netplay).',
        'host_btn': 'Ser anfitrión y jugar', 'join_btn': 'Entrar y jugar',
        'cheats': 'Trucos', 'cheats_sub': 'Cambian el juego en sí. Lo que guardes con ellos queda en la partida. '
        'En el coop valen los trucos del anfitrión para los dos.',
        'p2': 'Coop local', 'p2_sub': 'Dos granjeros en este PC, sin internet: tú juegas con el granjero del '
        'juego y otra persona tiene su propio granjero, con una ventana propia (experimental).',
        'local': 'Dos granjeros en este PC', 'local_d': 'El granjero 1 juega con el teclado o el primer mando; '
        'el granjero 2, con el segundo mando o el teclado numérico.',
        'local_btn': 'Jugar con dos granjeros',
        'p2_keys': 'Controles del granjero 2', 'p2_head': ('', 'Mando 2', 'Teclado numérico'), 'p2_list': [
            ('Caminar', 'Cruceta', '8  4  5  6'), ('Usar la herramienta', 'Cuadrado', '7'),
            ('Mirar, hablar, abrir, intercambiar', 'X', '9'), ('Correr', 'Círculo', '1'),
            ('Cambiar de herramienta', 'R1', '3'), ('Guardar / cambiar el objeto', 'R2', '+'),
            ('Silbar', 'L1 / L2', '/   *'), ('Mochila', 'Triángulo', '0'), ('Menú', 'Start', 'Enter'),
            ('Mapa', 'Select', '.')],
        'p2_tips': 'Bueno saber', 'p2_tips_list': [
            ('\u21c4', 'Intercambio: ponte delante del otro granjero, muy cerca, y pulsa X (9). Primero las '
                        'herramientas, después los objetos, hasta lo que cabe en la mochila del otro.'),
            ('!', 'Hay una de cada herramienta: el granjero 2 empieza sin nada y las toma del baúl.'),
            ('!', 'Solo se guarda lo que lleva el granjero 1: al guardar, las herramientas del 2 vuelven al baúl.')],
        'my_ip': 'Direcciones de este PC (pásale una a tu amigo): %s',
        'build_id': 'Versión del juego: %s. La de tu amigo tiene que ser la misma (también está en el título de la ventana del juego).',
        'need_addr': 'Escribe primero la dirección del anfitrión.',
        'build': 'Crear desde mi disco', 'build_sub': 'Hace el juego de PC a partir de la imagen de tu propio '
        'disco, aquí en esta carpeta. La primera vez también descarga el compilador (unos 65 MB, comprobado '
        'archivo por archivo).',
        'pak': 'Paquete de datos', 'pak_d': 'También copia los datos del juego a HarvestMoonBTN.pak: después la '
        'imagen del disco ya no hace falta.',
        'build_btn': 'Crear el juego', 'building': 'Creando... (tarda unos minutos)',
        'built': 'Listo: el juego está creado.', 'build_failed': 'La creación se detuvo: las últimas líneas de '
        'arriba dicen por qué.',
        'need_cue': 'Elige primero la imagen del disco (.cue o .bin).',
        'update': 'Actualizar a %s', 'update_note': 'Salió el launcher %s.',
        'updating': 'Descargando el nuevo launcher...', 'update_failed': 'La actualización falló: %s',
        'ask_stop': 'Hay una creación en curso. ¿Detenerla y cerrar?',
        'build_note': 'La carpeta build-files guarda archivos de tu disco: es solo para este PC. Para darle el '
        'juego a un amigo, dale este launcher; él lo crea con su propio disco.',
        'how': 'Cómo conectarse', 'how_list': [
            ('1', 'Los dos necesitan esta misma versión del juego (el mismo HarvestMoonBTN.exe).'),
            ('2', 'Lo más fácil: los dos instalan Radmin VPN o ZeroTier y entran en la misma red; el anfitrión '
                  'pasa la dirección que aparece ahí (26.x.x.x).'),
            ('3', 'O el anfitrión abre el puerto TCP %s en el router y pasa la dirección de internet.'),
            ('4', 'El anfitrión pulsa Ser anfitrión y jugar primero; luego el amigo pulsa Entrar y jugar. Si '
                  'Windows pregunta por el firewall, permítelo.'),
            ('!', 'En el coop, Tab y F1-F4 están desactivados; el título de la ventana muestra la conexión.')],
    },
}

LINE = re.compile(r'^\s*([#;]?)\s*([A-Za-z][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$')


def read_ini(text):
    """key -> value; active lines win over commented examples ('#key = value')"""
    vals, active = {}, set()
    for line in text.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        hashed, k, v = m.groups()
        if not hashed:
            vals[k] = v
            active.add(k)
        elif k not in active:
            vals[k] = v
    return vals, active


def set_ini(text, section, key, value):
    """set (or with value '' comment out) one option, keeping everything else as it is"""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = LINE.match(line)
        if m and m.group(2) == key and not m.group(1):
            lines[i] = '%s = %s' % (key, value) if value else '#' + line.lstrip()
            return '\n'.join(lines) + '\n'
    if not value:
        return text
    for i, line in enumerate(lines):          # above the commented example, if there is one
        m = LINE.match(line)
        if m and m.group(2) == key:
            lines.insert(i, '%s = %s' % (key, value))
            return '\n'.join(lines) + '\n'
    head = '[%s]' % section
    if head not in (l.strip() for l in lines):
        lines += ['', head]
    i = [l.strip() for l in lines].index(head)
    lines.insert(i + 1, '%s = %s' % (key, value))
    return '\n'.join(lines) + '\n'


def selftest():
    t = '[video]\n# 1 = smooth\nsmooth = 0\n[keyboard]\n#key_up = Up\n'
    v, act = read_ini(t)
    assert v == {'smooth': '0', 'key_up': 'Up'} and act == {'smooth'}, v
    assert set_ini(t, 'video', 'smooth', '1') == t.replace('smooth = 0', 'smooth = 1')
    assert set_ini(t, 'keyboard', 'key_up', 'W') == t.replace('#key_up', 'key_up = W\n#key_up')
    assert set_ini(t, 'video', 'smooth', '') == t.replace('smooth = 0', '#smooth = 0')
    assert set_ini(t, 'general', 'disc', 'a.cue').endswith('[general]\ndisc = a.cue\n')
    assert read_ini(set_ini(t, 'keyboard', 'key_up', 'W'))[0]['key_up'] == 'W'
    assert cpu_cores()[0] >= 1 and language() in UI and preset_for(2) == 0 and preset_for(8) == 2
    assert all(set(TEXT[l]) == set(TEXT['en']) and set(UI[l]) == set(UI['en']) for l in UI)
    assert mix('#000000', '#ffffff', 0.5) == '#808080'
    assert vtuple('v1.10.0') > vtuple('1.9.3') > vtuple('1.9') and vtuple('v2') == (2,)
    import tempfile
    cur = os.path.join(tempfile.gettempdir(), 'hmbtn_selftest.cur')
    write_cursor(cur, [[(255, 255, 255), None]] * 29, (1, 0))
    assert os.path.getsize(cur) == 22 + 40 + 32 * 32 * 4 + 32 * 4
    os.remove(cur)
    print('selftest ok')


def cpu_cores():
    """(physical cores, logical processors)"""
    threads = os.cpu_count() or 1
    try:
        if os.name == 'nt':
            import ctypes
            from ctypes import wintypes
            k32 = ctypes.windll.kernel32
            n = wintypes.DWORD(0)
            k32.GetLogicalProcessorInformationEx(0, None, ctypes.byref(n))     # 0: RelationProcessorCore
            buf = ctypes.create_string_buffer(n.value)
            if k32.GetLogicalProcessorInformationEx(0, buf, ctypes.byref(n)):
                cores = off = 0
                while off < n.value:
                    rel, size = struct.unpack_from('<II', buf.raw, off)
                    cores += rel == 0
                    off += size
                if cores:
                    return cores, threads
        else:
            ids = set()
            phys = None
            for line in open('/proc/cpuinfo'):
                k, _, v = line.partition(':')
                if k.strip() == 'physical id':
                    phys = v.strip()
                elif k.strip() == 'core id':
                    ids.add((phys, v.strip()))
            if ids:
                return len(ids), threads
    except Exception:
        pass
    return max(1, threads // 2) if threads > 2 else threads, threads


def preset_for(cores):
    return 0 if cores <= 2 else 1 if cores <= 5 else 2


def language():
    try:
        if os.name == 'nt':
            import ctypes
            return {0x16: 'pt', 0x0A: 'es'}.get(ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF, 'en')
        import locale
        lang = (locale.getlocale()[0] or os.environ.get('LANG', '')).lower()[:2]
        return lang if lang in ('pt', 'es') else 'en'
    except Exception:
        return 'en'


def game_data(disc):
    """the game data the game will use (as it looks for it), or None"""
    if disc:
        p = disc if os.path.isabs(disc) else os.path.join(HERE, disc)
        if os.path.exists(p):
            return p
    pak = os.path.join(HERE, 'HarvestMoonBTN.pak')
    if os.path.exists(pak):
        return pak
    d = os.path.join(HERE, 'disc')
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            if f.lower().endswith(('.cue', '.bin')):
                return os.path.join(d, f)
    return None


# looks: harvestmoonbtn.com.br, which dresses like the game's menus. Colours from its stylesheet, pictures
# from its assets (launcher-assets next to this file; without them the launcher still works, plainer).
def local_ips():
    """this PC's IPv4 addresses a friend could reach (LAN, Radmin VPN, ZeroTier...), main one first"""
    import socket
    ips = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('8.8.8.8', 80))                    # sends nothing: only picks the outgoing interface
        ips.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        ips += socket.gethostbyname_ex(socket.gethostname())[2]
    except OSError:
        pass
    return [ip for i, ip in enumerate(ips) if ip not in ips[:i] and not ip.startswith('127.')]


def vtuple(v):
    """'v1.2.3' -> (1, 2, 3)"""
    return tuple(int(x) for x in re.findall(r'\d+', v)[:3])


def latest_release():
    """(version, download address, sha256 or None) of a newer launcher on GitHub, or None"""
    import json, urllib.request
    try:
        req = urllib.request.Request(RELEASES, headers={'User-Agent': 'HarvestMoonBTN-Launcher',
                                                        'Accept': 'application/vnd.github+json'})
        with urllib.request.urlopen(req, timeout=10) as r:
            rel = json.load(r)
        asset = next(a for a in rel.get('assets', []) if a['name'] == LAUNCHER_EXE)
        if vtuple(rel['tag_name']) <= vtuple(VERSION):
            return None
    except Exception:                               # offline, no release yet...: no news
        return None
    digest = asset.get('digest') or ''
    return rel['tag_name'].lstrip('v'), asset['browser_download_url'], digest[7:] if digest.startswith('sha256:') else None


def self_update(url, sha):
    """put the new Launcher.exe in place of this one (Windows lets a running exe be renamed, not overwritten)"""
    import hashlib, urllib.request
    me = sys.executable
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'HarvestMoonBTN-Launcher'}),
                                timeout=120) as r:
        data = r.read()
    if sha and hashlib.sha256(data).hexdigest() != sha:
        raise OSError('the download is damaged (checksum)')
    with open(me + '.new', 'wb') as f:
        f.write(data)
    if os.path.exists(me + '.old'):
        os.remove(me + '.old')
    os.replace(me, me + '.old')
    os.replace(me + '.new', me)


def build_id():
    """the co-op version the game was built with (runtime/netplay.c keeps it as text in the executable)"""
    try:
        with open(EXE, 'rb') as f:
            m = re.search(rb'HMBTN-BUILD 0x([0-9a-f]{8})', f.read())
        return m.group(1).decode() if m else None
    except OSError:
        return None


ASSETS = os.path.join(HERE, 'launcher-assets')
if not os.path.isdir(ASSETS) and SRC:      # a Launcher.exe on its own: the pictures it carries
    ASSETS = os.path.join(SRC, 'dist-template', 'launcher-assets')
BG, CARD, LINE_C = '#ffffff', '#ffffff', '#e6d7b8'
# .game-box: a pale-to-golden gradient in a 4px frame, light above and left, brown right and below.
# WOOD marks the widgets that take the gradient's colour at their height.
WOOD, WOOD_TOP, WOOD_BOT, WOOD_HI = '#f4d6a3', '#f2debb', '#f6cd8b', '#ecc07c'
BOX_LIGHT, BOX_DARK = '#fcebd0', '#b38c47'
SITE_BG = '#cfdcf2'                                 # under the tiled background, until it is drawn
TEXT_C, MUTED, WARN, OK = '#212529', '#0000ff', '#b5651d', '#2e7d32'
RED, BLUE, NAV_C, SWITCH = '#ff0000', '#0000ff', '#343a40', '#3b4b95'


def mix(a, b, t):
    """the colour a fraction t of the way from a to b"""
    a, b = [int(a[i:i + 2], 16) for i in (1, 3, 5)], [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return '#%02x%02x%02x' % tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def write_cursor(path, rows, hotspot):
    """a Windows .cur file from rows of (r, g, b) or None (see-through), at most 64x64"""
    n = 32 if max(len(rows), max(len(r) for r in rows)) <= 32 else 64
    rows = [list(r) + [None] * (n - len(r)) for r in rows] + [[None] * n] * (n - len(rows))
    pix, mask = b'', b''
    for row in reversed(rows):                          # bottom-up, as bitmaps are
        for c in row:
            pix += bytes([c[2], c[1], c[0], 255]) if c else b'\0\0\0\0'
        mask += int(''.join('1' if c is None else '0' for c in row), 2).to_bytes(n // 8, 'big')
    img = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 32, 0, len(pix) + len(mask), 0, 0, 0, 0) + pix + mask
    with open(path, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 2, 1) + struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, hotspot[0],
                                                           hotspot[1], len(img), 22) + img)


def main():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox, font as tkfont

    text = open(INI, encoding='utf-8').read() if os.path.exists(INI) else ''
    if not text and SRC:                    # a new folder: the first save writes the commented hmbtn.ini
        text = open(os.path.join(SRC, 'dist-template', 'hmbtn.ini'), encoding='utf-8').read()
    loaded, active = read_ini(text)
    cores, threads = cpu_cores()
    # --page NAME opens that page; --live (F5 in the game): the game is running, every change is saved
    # at once and the game takes it up (the cheats), and there is no Play
    live = '--live' in sys.argv
    # --after-update: a new Launcher.exe just replaced the old one, and rebuilds the game with its sources
    rebuild = '--after-update' in sys.argv and bool(SRC) and os.path.exists(EXE)
    first = sys.argv[sys.argv.index('--page') + 1] if '--page' in sys.argv[:-1] else \
        'build' if SRC and (rebuild or not os.path.exists(EXE)) else 'start'
    if getattr(sys, 'frozen', False):
        try:
            os.remove(sys.executable + '.old')      # left by the last update
        except OSError:
            pass
    lang = loaded.get('language') if 'language' in active else None     # the flag chosen last time
    state = {'lang': lang if lang in UI else language(), 'page': first, 'dirty': False, 'note': ''}

    if os.name == 'nt':             # sharp text on scaled screens (Tk then sizes fonts for the DPI)
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
        try:                        # its own taskbar button, with the port's icon instead of Python's
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('HarvestMoonBTN.Launcher')
        except Exception:
            pass
    root = tk.Tk()
    if live:                                        # over the game's window
        root.attributes('-topmost', True)
    k = max(1.0, root.winfo_fpixels('1i') / 96.0)     # pixel sizes below are for 96 DPI

    def px(n):
        return int(n * k)
    zoom = 2 if k >= 1.5 else 1                       # the site's little pictures, doubled on scaled screens

    def picture(name, big=True, **kw):
        """a picture from launcher-assets (doubled with zoom if big), or None"""
        try:
            im = tk.PhotoImage(file=os.path.join(ASSETS, name), **kw)
        except Exception:
            return None
        return im.zoom(zoom) if big and zoom > 1 else im
    P = {'pointer': picture('game-pointer.png'), 'heart': picture('heart-level.png'),
         'click': picture('game-click-icon.png'), 'tile': picture('background.png', False),
         'header': picture('header-%d.png' % min((420, 520, 640), key=lambda w: abs(w - px(420))), False),
         'flag_en': picture('flag-en.png'), 'flag_es': picture('flag-es.png'), 'flag_pt': picture('flag-pt.png'),
         'dialog': [picture('dialog-end.gif', format='gif -index %d' % i) for i in (0, 1)]}

    cursor = 'hand2'               # all over the window: the game's glove, if Windows takes it
    if os.name == 'nt' and P['pointer']:
        try:
            import tempfile
            im = P['pointer']
            rows = [[None if im.transparency_get(x, y) else im.get(x, y) for x in range(im.width())]
                    for y in range(im.height())]
            tip = next((max(x for x, c in enumerate(r) if c), y) for y, r in enumerate(rows) if any(r))
            cur = os.path.join(tempfile.gettempdir(), 'hmbtn_pointer.cur')
            write_cursor(cur, rows, tip)
            root.configure(cursor='@' + cur.replace('\\', '/').replace(' ', '\\ '))
            cursor = root.cget('cursor')
        except Exception:
            pass
    root.configure(cursor=cursor)  # inherited by every widget; text fields keep their I-beam
    root.option_add('*Toplevel.cursor', cursor)
    root.option_add('*TCombobox*Listbox.cursor', cursor)
    root.title('Harvest Moon: Back to Nature')
    try:                           # the port's icon, the same one as in HarvestMoonBTN.exe
        if os.name == 'nt':
            root.iconbitmap(default=os.path.join(ASSETS, 'icon.ico'))
        else:
            root.iconphoto(True, tk.PhotoImage(file=os.path.join(ASSETS, 'icon.png')))
    except Exception:
        pass
    root.geometry('%dx%d' % (px(940), px(700)))
    root.minsize(px(780), px(560))
    root.configure(bg=BG)
    fam = 'Segoe UI' if os.name == 'nt' else 'TkDefaultFont'
    # the site's sizes: 20px titles, 16px menu, 14px text
    F = {'h1': (fam, 15), 'h2': (fam, 14, 'bold'), 'b': (fam, 10, 'bold'), 'n': (fam, 10),
         's': (fam, 10), 'nav': (fam, 12), 'navb': (fam, 12, 'bold')}

    st = ttk.Style(root)
    st.theme_use('clam')
    st.configure('TCombobox', fieldbackground='white', background=WOOD_TOP, arrowcolor=SWITCH, padding=4,
                 bordercolor=BOX_DARK)
    st.map('TCombobox', fieldbackground=[('readonly', 'white'), ('disabled', '#eee9dc')])
    st.configure('TEntry', fieldbackground='white', padding=4, bordercolor=BOX_DARK)
    st.configure('TSpinbox', fieldbackground='white', arrowcolor=SWITCH, padding=4, bordercolor=BOX_DARK)
    st.configure('Vertical.TScrollbar', background=WOOD_TOP, troughcolor=BG, bordercolor=BOX_DARK,
                 arrowcolor=BOX_DARK)
    root.option_add('*TCombobox*Listbox.font', F['n'])

    def T(k):
        return UI[state['lang']][k]

    # one variable per option, kept across rebuilds (language switch)
    vars_ = {}
    for page, section, key, kind in OPTIONS:
        # a path from a commented example ("#disc = C:\Games\...") is not a real setting
        start = loaded.get(key, '') if (key in active or kind not in ('file', 'dir')) else ''
        if kind.startswith(('choice:', 'bool:')) and not start:
            start = kind.split(':')[1].split('|')[0]     # the game's default
        vars_[key] = tk.StringVar(value=start)

    def val(k):
        return vars_[k].get().strip()

    widgets = {}                                 # key -> (set_enabled, note label)

    def refresh():
        for key, (cond, why) in REQUIRES.items():
            if key in widgets:
                ok = cond(val)
                widgets[key][0](ok)
                widgets[key][1].configure(text='' if ok else T(why))
        status.configure(text=state['note'] or (T('unsaved') if state['dirty'] else ''),
                         fg=WARN if state['dirty'] else OK)

    def changed(*_):
        state['dirty'] = True
        state['note'] = ''
        if live:
            save()
        refresh()

    # ---- widgets ----
    def raised(parent, bg=WOOD, d=4):
        """the site's .game-box: a 4px frame, light above and left, brown right and below; a WOOD one also
        gets its gradient. Returns (outer, inner)."""
        outer = tk.Frame(parent, bg=BOX_DARK)
        rim = tk.Frame(outer, bg=BOX_LIGHT)
        rim.pack(fill='both', expand=True, padx=(0, px(d)), pady=(0, px(d)))
        inner = tk.Frame(rim, bg=bg)
        inner.pack(fill='both', expand=True, padx=(px(d), 0), pady=(px(d), 0))
        if bg == WOOD:
            inner.grad = tk.Label(inner, bd=0, bg=WOOD, anchor='nw')
            inner.bind('<Configure>', lambda e: inner.after_idle(shade, inner), add='+')
            windows.append(inner)
        return outer, inner
    windows = []

    def shade_all():
        windows[:] = [w for w in windows if w.winfo_exists()]
        for w in windows:
            shade(w)

    def shade(win):
        """paint the gradient behind a window, and give each WOOD widget in it the colour at its height
        (Tk widgets can't be see-through)"""
        if not win.winfo_exists():
            return
        w, h = max(1, win.winfo_width()), max(1, win.winfo_height())
        span = max(h, px(160))           # a small box gets only the top of the gradient (labels can't blend)
        if getattr(win, 'size', None) != (w, h):
            win.size = (w, h)
            img = tk.PhotoImage(width=1, height=h)
            img.put(' '.join('{%s}' % mix(WOOD_TOP, WOOD_BOT, y / span) for y in range(h)))
            win.img = img.zoom(w, 1)
            win.grad.configure(image=win.img)
            # place() counts from inside the padding; the gradient goes under the padding too
            win.grad.place(x=-int(str(win.cget('padx'))), y=-int(str(win.cget('pady'))), width=w, height=h)
            win.grad.lower()

        def walk(parent, top):
            for c in parent.winfo_children():
                if c is win.grad or not c.winfo_exists():
                    continue
                y = top + c.winfo_y()
                if getattr(c, 'tint', None) or (c.winfo_class() in ('Label', 'Frame', 'Canvas')
                                                and str(c.cget('bg')) == WOOD):
                    c.tint = mix(WOOD_TOP, WOOD_BOT, min(1, max(0, (y + c.winfo_height() / 2) / span)))
                    c.configure(bg=c.tint)
                walk(c, y)
        walk(win, 0)

    def button(parent, label, cmd, accent=False, small=False):
        """a .game-box button; the main one bold, with the game's 'press' arrow"""
        f, inner = raised(parent, WOOD_TOP, 3)
        b = tk.Label(inner, text=label, bg=WOOD_TOP, fg=TEXT_C, cursor=cursor, font=F['b'] if accent else F['n'],
                     padx=10 if small else 16, pady=4 if small else 7)
        if accent and P['click']:
            b.configure(image=P['click'], compound='right')
        b.pack()
        b.bind('<Button-1>', lambda e: cmd())
        b.bind('<Enter>', lambda e: b.configure(bg=WOOD_HI))
        b.bind('<Leave>', lambda e: b.configure(bg=WOOD_TOP))
        return f

    def plaque(parent, label, font, bg=BG):
        """the site's .box-title: a title on a .game-box"""
        fnt = tkfont.Font(root, font=font)
        w, h, d = fnt.measure(label) + px(80), fnt.metrics('linespace') + px(20), px(4)
        c = tk.Canvas(parent, width=w, height=h, bg=bg, highlightthickness=0)
        for y in range(h):
            c.create_line(0, y, w, y, fill=mix(WOOD_TOP, WOOD_BOT, y / max(1, h - 1)))
        c.create_rectangle(0, 0, w, d, width=0, fill=BOX_LIGHT)
        c.create_rectangle(0, 0, d, h, width=0, fill=BOX_LIGHT)
        c.create_polygon(w, 0, w, h, 0, h, d, h - d, w - d, h - d, w - d, d, width=0, fill=BOX_DARK)
        c.create_text(w / 2, h / 2, text=label, font=font, fill=TEXT_C)
        return c

    def toggle(parent, var):
        """an on/off switch for a '1'/'0' variable"""
        f = tk.Frame(parent, bg=CARD)
        c = tk.Canvas(f, width=px(46), height=px(24), bg=CARD, highlightthickness=0, cursor=cursor)
        c.pack(side='left')
        lab = tk.Label(f, bg=CARD, font=F['s'], fg=TEXT_C, width=11, anchor='w')
        lab.pack(side='left', padx=(6, 0))
        en = [True]

        def draw(*_):
            on = var.get() == '1'
            c.delete('all')
            col = (SWITCH if on else '#c9bfa6') if en[0] else '#e2dccb'
            c.create_oval(px(1), px(1), px(23), px(23), fill=col, outline=col)
            c.create_oval(px(23), px(1), px(45), px(23), fill=col, outline=col)
            c.create_rectangle(px(12), px(1), px(34), px(23), fill=col, outline=col)
            x = px(24) if on else px(3)
            c.create_oval(x, px(3), x + px(18), px(21), fill='white', outline='white')
            lab.configure(text=T('on') if on else T('off'))

        def click(e):
            if en[0]:
                var.set('0' if var.get() == '1' else '1')
        c.bind('<Button-1>', click)
        tid = var.trace_add('write', draw)
        c.bind('<Destroy>', lambda e: var.trace_remove('write', tid))   # rebuilt pages (language) get new ones
        draw()

        def enable(ok):
            en[0] = ok
            c.configure(cursor=cursor)
            draw()
        return f, enable

    def flag(parent, lang, bg):
        """the flag pictures (drawn plainly if one is missing)"""
        im = P.get('flag_' + lang)
        if im:
            return tk.Label(parent, image=im, bg=bg, bd=0, cursor=cursor)
        w, h = 32 * zoom, 32 * zoom
        c = tk.Canvas(parent, width=w, height=h, highlightthickness=0, cursor=cursor, bg=bg)
        y0, y1 = 4 * zoom, 29 * zoom                      # the site's flags fill rows 4 to 28
        if lang == 'pt':                                  # Brazil: green, yellow diamond, blue globe
            c.create_rectangle(0, y0, w, y1, width=0, fill='#009c3b')
            c.create_polygon(w / 2, y0 + 2 * zoom, w - 3 * zoom, (y0 + y1) / 2, w / 2, y1 - 2 * zoom, 3 * zoom,
                             (y0 + y1) / 2, fill='#ffdf00')
            r = (y1 - y0) * 0.24
            c.create_oval(w / 2 - r, (y0 + y1) / 2 - r, w / 2 + r, (y0 + y1) / 2 + r, width=0, fill='#002776')
            c.create_rectangle(0, y0, w, y0 + 3 * zoom, width=0, fill='#40b46c')      # the site's shine on top
        else:                                             # a picture missing: its colours, plainly
            c.create_rectangle(0, y0, w, y1, width=0, fill='#c60b1e' if lang == 'es' else '#012169')
        return c

    def draw_header(c, w, h):
        """the site's banner, centred over its background"""
        c.delete('art')
        tile(c, w, h)
        if P['header']:
            c.create_image(w // 2, 0, image=P['header'], anchor='n', tags='art')
        c.coords('langs', w - px(14), px(12))

    def tile(c, w, h):
        """the site's background picture, repeated"""
        c.delete('tile')
        im = P['tile']
        if im:
            for y in range(0, h, im.height()):
                for x in range(0, w, im.width()):
                    c.create_image(x, y, image=im, anchor='nw', tags=('tile', 'art'))
        c.tag_lower('tile')

    def scroll_page(parent):
        """a page that scrolls; returns its inner frame"""
        outer = tk.Frame(parent, bg=BG)
        cv = tk.Canvas(outer, bg=BG, highlightthickness=0)
        sb = ttk.Scrollbar(outer, orient='vertical', command=cv.yview)
        inner = tk.Frame(cv, bg=BG)
        win = cv.create_window(0, 0, window=inner, anchor='nw')
        cv.configure(yscrollcommand=sb.set)
        inner.bind('<Configure>', lambda e: cv.configure(scrollregion=cv.bbox('all')))
        cv.bind('<Configure>', lambda e: cv.itemconfigure(win, width=e.width))
        cv.pack(side='left', fill='both', expand=True)
        sb.pack(side='right', fill='y')

        def wheel(e):
            if inner.winfo_height() > cv.winfo_height():
                cv.yview_scroll(int(-e.delta / 120) or (-1 if e.delta > 0 else 1), 'units')
        cv.bind('<Enter>', lambda e: root.bind_all('<MouseWheel>', wheel))
        cv.bind('<Leave>', lambda e: root.unbind_all('<MouseWheel>'))
        return outer, inner

    def card(parent, pad=16, wood=False):
        """a white box, or (wood) a wooden note like the site's"""
        if wood:
            outer, f = raised(parent, WOOD)
            f.configure(padx=pad, pady=pad - 4)
        else:
            outer = f = tk.Frame(parent, bg=CARD, highlightthickness=1, highlightbackground=LINE_C, padx=pad,
                                 pady=pad - 4)
        outer.pack(fill='x', padx=24, pady=(0, 12))
        return f

    hearts = None
    if P['heart']:                                       # .heart-level.lv3: the heart repeated three times
        hearts = tk.PhotoImage(width=P['heart'].width() * 3, height=P['heart'].height())
        hearts.tk.call(hearts, 'copy', P['heart'], '-to', 0, 0, hearts.width(), hearts.height())

    def section(parent, title, sub=''):
        tk.Label(parent, text=title, font=F['h2'], bg=BG, fg=RED, anchor='w').pack(fill='x', padx=24,
                                                                                  pady=(10, 0))
        if sub:
            tk.Label(parent, text=sub, font=F['s'], bg=BG, fg=MUTED, anchor='w', justify='left',
                     wraplength=px(700)).pack(fill='x', padx=24, pady=(0, 8))

    def page_head(parent, title, sub):
        plaque(parent, title, F['h1']).pack(pady=(18, 0))
        tk.Label(parent, text=sub, font=F['n'], bg=BG, fg=BLUE, justify='center',
                 wraplength=px(640)).pack(fill='x', padx=24, pady=(10, 14))

    def option_row(parent, key, kind, title, desc, first):
        if not first:
            tk.Frame(parent, bg=LINE_C, height=1).pack(fill='x', pady=8)
        row = tk.Frame(parent, bg=CARD)
        row.pack(fill='x')
        row.columnconfigure(0, weight=1)
        txt = tk.Frame(row, bg=CARD)
        txt.grid(row=0, column=0, sticky='w')
        tk.Label(txt, text=title, font=F['b'], bg=CARD, fg=BLUE, anchor='w').pack(fill='x')
        if desc:
            d = tk.Label(txt, text=desc, font=F['s'], bg=CARD, fg=MUTED, anchor='w', justify='left',
                         wraplength=px(430))
            d.pack(fill='x')
            # lines break where the control beside it begins
            row.bind('<Configure>', lambda e, d=d: d.configure(
                wraplength=max(px(200), e.width - ctl.winfo_reqwidth() - px(28))))
        note = tk.Label(txt, text='', font=F['s'], bg=CARD, fg=WARN, anchor='w')
        note.pack(fill='x')
        ctl = tk.Frame(row, bg=CARD)
        ctl.grid(row=0, column=1, sticky='e', padx=(16, 0))
        var = vars_[key]
        if kind.startswith('bool'):
            w, enable = toggle(ctl, var)
            w.pack()
        elif kind.startswith(('choice:', 'edit:')):
            w = ttk.Combobox(ctl, values=kind.split(':', 1)[1].split('|'), width=16, font=F['n'],
                             textvariable=var, state='readonly' if kind.startswith('choice:') else 'normal')
            w.pack()
            enable = lambda ok, w=w, ro=kind.startswith('choice:'): w.configure(
                state=('readonly' if ro else 'normal') if ok else 'disabled')
        elif kind.startswith('spin'):
            w = ttk.Spinbox(ctl, from_=1, to=int(kind[4:]), width=6, font=F['n'], textvariable=var)
            w.pack()
            enable = lambda ok, w=w: w.configure(state='normal' if ok else 'disabled')
        else:
            w = ttk.Entry(ctl, textvariable=var, width=34 if kind in ('file', 'dir') else 22, font=F['n'])
            w.pack(side='left')
            enable = lambda ok, w=w: w.configure(state='normal' if ok else 'disabled')
            if kind in ('file', 'dir'):
                def browse(kind=kind):
                    p = (filedialog.askopenfilename(filetypes=[('Game data', '*.cue *.bin *.pak'), ('*', '*')])
                         if kind == 'file' else filedialog.askdirectory())
                    if p:
                        var.set(os.path.normpath(p))
                button(ctl, T('browse'), browse, small=True).pack(side='left', padx=(6, 0))
        widgets[key] = (enable, note)

    def options_page(parent, page):
        rows = [(key, kind) for pg, sec, key, kind in OPTIONS if pg == page]
        c = card(parent)
        for i, (key, kind) in enumerate(rows):
            if page in ('keyboard', 'controller') and key[:4] in ('key_', 'pad_'):
                b = key[4:]
                dflt = loaded.get(key, '') if key not in active else ''
                option_row(c, key, kind, BUTTON_NAMES[state['lang']][b],
                           T('default') % dflt if dflt else '', i == 0)
            else:
                title, desc = TEXT[state['lang']][key]
                option_row(c, key, kind, title, desc, i == 0)

    def start_page(parent):
        page_head(parent, T('welcome'), T('welcome_sub'))
        c = card(parent, wood=True)
        data = game_data(val('disc'))
        if data and os.path.abspath(data).startswith(os.path.abspath(HERE)):
            data = os.path.relpath(data, HERE)
        first = tk.Label(c, text=('\u2714  ' + T('data_ok') % data) if data else ('\u26a0  ' + T('data_missing')),
                         font=F['n'], bg=WOOD, fg=OK if data else WARN, anchor='w', justify='left')
        first.pack(fill='x')
        last = tk.Frame(c, bg=WOOD)
        last.pack(fill='x', pady=(6, 0))
        pc = tk.Label(last, text='\U0001f5a5  ' + T('pc') % (cores, threads), font=F['n'], bg=WOOD, fg=TEXT_C,
                      anchor='w', justify='left')
        pc.pack(side='left')
        # lines break at the box's width, leaving room for the arrow
        c.bind('<Configure>', lambda e: [first.configure(wraplength=max(px(200), e.width - px(36))),
                                         pc.configure(wraplength=max(px(200), e.width - px(60)))])
        if all(P['dialog']):                              # the game's blinking "more" arrow, as on the site
            arrow = tk.Label(last, image=P['dialog'][0], bg=WOOD, bd=0)
            arrow.pack(side='right', anchor='s')

            def blink(i=0):
                if arrow.winfo_exists():
                    arrow.configure(image=P['dialog'][i])
                    arrow.after(450, blink, 1 - i)
            blink()

        section(parent, T('quick'), T('quick_sub'))
        row = tk.Frame(parent, bg=BG)
        row.pack(fill='x', padx=24)
        rec = preset_for(cores)
        for i, (name, vals) in enumerate(PRESETS):
            row.columnconfigure(i, weight=1, uniform='p')
            in_use = all(val(k) == v for k, v in {**PRESET_ALL, **vals}.items())
            outer, f = raised(row, WOOD)
            outer.grid(row=0, column=i, sticky='nsew', padx=(0 if i == 0 else 8, 0))
            f.configure(padx=14, pady=12)
            if i == rec and hearts:                       # the recommended one: three of the game's hearts
                tk.Label(f, image=hearts, bg=WOOD, bd=0, anchor='w').pack(fill='x')
            r = tk.Label(f, text=T('recommended') if i == rec else ' ', font=F['s'], bg=WOOD, fg=RED, anchor='w',
                         justify='left')
            r.pack(fill='x')
            labs = [tk.Label(f, text=T(name), font=F['h2'], bg=WOOD, fg=BLUE, anchor='w', justify='left'),
                    tk.Label(f, text=T(name + '_d'), font=F['s'], bg=WOOD, fg=TEXT_C, anchor='w', justify='left')]
            labs[0].pack(fill='x')
            labs[1].pack(fill='x', pady=(4, 10))
            # lines break at the card's width, whatever the window's
            f.bind('<Configure>', lambda e, labs=labs + [r]: [l.configure(wraplength=max(px(80), e.width - px(34)))
                                                               for l in labs])
            if in_use:
                tk.Label(f, text='\u2714 ' + T('in_use'), font=F['b'], bg=WOOD, fg=OK, anchor='w').pack(fill='x')
            else:
                def use(name=name, vals=vals):
                    for k, v in {**PRESET_ALL, **vals}.items():
                        vars_[k].set(v)
                    state['note'] = T('applied') % T(name)
                    show('start')
                    refresh()
                button(f, T('use'), use, accent=i == rec, small=True).pack(anchor='w')

        tk.Frame(parent, bg=BG, height=8).pack()
        section(parent, T('keys'))
        tk.Frame(parent, bg=BG, height=6).pack()
        c = card(parent)
        for i, (key, what) in enumerate(T('keys_list')):
            tk.Label(c, text=key, font=F['b'], bg=CARD, fg=BLUE, anchor='w').grid(row=i, column=0, sticky='w',
                                                                                  pady=2)
            tk.Label(c, text=what, font=F['n'], bg=CARD, fg=BLUE, anchor='w').grid(row=i, column=1, sticky='w',
                                                                                    padx=(18, 0), pady=2)

    def coop_page(parent):
        page_head(parent, T('coop'), T('coop_sub'))
        row = tk.Frame(parent, bg=BG)
        row.pack(fill='x', padx=24, pady=(0, 12))
        for i, (name, cmd) in enumerate((('host', coop_host), ('join', coop_join))):
            outer, f = raised(row, WOOD)
            row.columnconfigure(i, weight=1, uniform='c')
            outer.grid(row=0, column=i, sticky='nsew', padx=(0 if i == 0 else 8, 0))
            f.configure(padx=14, pady=12)
            labs = [tk.Label(f, text=T(name), font=F['h2'], bg=WOOD, fg=BLUE, anchor='w', justify='left'),
                    tk.Label(f, text=T(name + '_d'), font=F['s'], bg=WOOD, fg=TEXT_C, anchor='w', justify='left')]
            labs[0].pack(fill='x')
            labs[1].pack(fill='x', pady=(4, 10))
            f.bind('<Configure>', lambda e, labs=labs: [l.configure(wraplength=max(px(80), e.width - px(34)))
                                                        for l in labs])
            button(f, T(name + '_btn'), cmd, accent=True, small=True).pack(anchor='w')
        ips = local_ips()
        if ips:
            tk.Label(parent, text=T('my_ip') % '   '.join(ips), font=F['b'], bg=BG, fg=BLUE, anchor='w',
                     justify='left', wraplength=px(640)).pack(fill='x', padx=24, pady=(0, 10))
        bid = build_id()
        if bid:
            tk.Label(parent, text=T('build_id') % bid, font=F['b'], bg=BG, fg=BLUE, anchor='w',
                     justify='left', wraplength=px(640)).pack(fill='x', padx=24, pady=(0, 10))
        options_page(parent, 'coop')
        section(parent, T('how'))
        tk.Frame(parent, bg=BG, height=6).pack()
        c = card(parent)
        c.columnconfigure(1, weight=1)
        for i, (n, what) in enumerate(T('how_list')):
            tk.Label(c, text=n, font=F['b'], bg=CARD, fg=RED, anchor='nw').grid(row=i, column=0, sticky='nw', pady=2)
            w = tk.Label(c, text=what.replace('%s', val('coop_port') or COOP_PORT), font=F['n'], bg=CARD,
                         fg=BLUE, anchor='w', justify='left', wraplength=px(560))
            w.grid(row=i, column=1, sticky='w', padx=(14, 0), pady=2)

    def p2_page(parent):
        page_head(parent, T('p2'), T('p2_sub'))
        c = card(parent, wood=True)
        labs = [tk.Label(c, text=T('local'), font=F['h2'], bg=WOOD, fg=BLUE, anchor='w', justify='left'),
                tk.Label(c, text=T('local_d'), font=F['s'], bg=WOOD, fg=TEXT_C, anchor='w', justify='left')]
        labs[0].pack(fill='x')
        labs[1].pack(fill='x', pady=(4, 10))
        c.bind('<Configure>', lambda e: [l.configure(wraplength=max(px(80), e.width - px(34))) for l in labs])
        button(c, T('local_btn'), lambda: play(['--p2']), accent=True, small=True).pack(anchor='w')
        tk.Frame(parent, bg=BG, height=12).pack()
        options_page(parent, 'p2')
        section(parent, T('p2_keys'))
        tk.Frame(parent, bg=BG, height=6).pack()
        c = card(parent)
        for k, h in enumerate(T('p2_head')):
            tk.Label(c, text=h, font=F['s'], bg=CARD, fg=MUTED, anchor='w').grid(row=0, column=k, sticky='w',
                                                                                 padx=(0 if k == 0 else 18, 0))
        for i, row in enumerate(T('p2_list')):
            for k, what in enumerate(row):
                tk.Label(c, text=what, font=F['n'] if k == 0 else F['b'], bg=CARD, fg=BLUE, anchor='w').grid(
                    row=i + 1, column=k, sticky='w', padx=(0 if k == 0 else 18, 0), pady=2)
        section(parent, T('p2_tips'))
        tk.Frame(parent, bg=BG, height=6).pack()
        c = card(parent)
        c.columnconfigure(1, weight=1)
        for i, (n, what) in enumerate(T('p2_tips_list')):
            tk.Label(c, text=n, font=F['b'], bg=CARD, fg=RED, anchor='nw').grid(row=i, column=0, sticky='nw', pady=2)
            tk.Label(c, text=what, font=F['n'], bg=CARD, fg=BLUE, anchor='w', justify='left',
                     wraplength=px(560)).grid(row=i, column=1, sticky='w', padx=(14, 0), pady=2)

    job = {'proc': None, 'log': [], 'ok': None, 'view': None}    # the build, kept across rebuilt pages

    def build_page(parent):
        page_head(parent, T('build'), T('build_sub'))
        c = card(parent)
        option_row(c, 'disc', 'file', *TEXT[state['lang']]['disc'], True)
        option_row(c, 'build_pak', 'bool', T('pak'), T('pak_d'), False)
        row = tk.Frame(parent, bg=BG)
        row.pack(fill='x', padx=24, pady=(0, 10))
        button(row, T('build_btn'), start_build, accent=True).pack(side='left')
        note = tk.Label(row, font=F['b'], bg=BG, anchor='w', justify='left')
        note.pack(side='left', padx=14)
        c = card(parent, pad=8)
        log = tk.Text(c, height=14, font=('Consolas' if os.name == 'nt' else 'TkFixedFont', 9), bg=CARD,
                      fg=TEXT_C, bd=0, wrap='word', state='disabled')
        log.pack(fill='both', expand=True)
        tk.Label(parent, text=T('build_note'), font=F['s'], bg=BG, fg=MUTED, anchor='w', justify='left',
                 wraplength=px(640)).pack(fill='x', padx=24)
        job['view'] = (log, note)
        show_log()

    def show_log():
        if not (job['view'] and job['view'][0].winfo_exists()):
            return
        log, note = job['view']
        log.configure(state='normal')
        log.delete('1.0', 'end')
        log.insert('end', ''.join(job['log'][-400:]))
        log.see('end')
        log.configure(state='disabled')
        note.configure(text=T('building') if job['proc'] else '' if job['ok'] is None else
                       '✔ ' + T('built') if job['ok'] else '⚠ ' + T('build_failed'),
                       fg=WARN if job['ok'] is False else OK)

    def start_build():
        import queue, shutil, threading
        if job['proc']:
            return
        disc = game_data(val('disc'))
        if not disc or not disc.lower().endswith(('.cue', '.bin')):
            disc = None                     # a rebuild can do with the files a build took from the disc
            if val('build_pak') == '1' or not all(os.path.exists(os.path.join(WORK, 'extract', n))
                                                  for n in GAME_FILES):
                messagebox.showwarning('Harvest Moon', T('need_cue'))
                return
        save()
        if SRC != WORK:                     # the scripts this exe carries, beside the build's own files
            os.makedirs(WORK, exist_ok=True)
            for n in ('build.py', 'tools', 'runtime', 'dist-template'):
                s, d = os.path.join(SRC, n), os.path.join(WORK, n)
                shutil.copytree(s, d, dirs_exist_ok=True) if os.path.isdir(s) else shutil.copy2(s, d)
        cmds = [[sys.executable, os.path.join(WORK, 'tools', 'fetch_deps.py'), '--min']] if os.name == 'nt' else []
        cmds.append([sys.executable, os.path.join(WORK, 'build.py'), '--into', HERE] +
                    (['--disc', disc] if disc else []) + (['--pak'] if val('build_pak') == '1' else []))
        env = dict(os.environ, PYINSTALLER_RESET_ENVIRONMENT='1', PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8')
        lines = queue.Queue()
        job.update(log=[], ok=None, proc=True)

        def work():
            for cmd in cmds:
                try:
                    p = subprocess.Popen(cmd, cwd=WORK, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, encoding='utf-8', errors='replace',
                                         creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                except OSError as e:
                    lines.put('%s\n' % e)
                    return lines.put(False)
                job['proc'] = p
                for line in p.stdout:
                    lines.put(line)
                if p.wait():
                    return lines.put(False)
            lines.put(True)
        threading.Thread(target=work, daemon=True).start()

        def poll():
            done = None
            while not lines.empty():
                x = lines.get()
                if isinstance(x, bool):
                    done = x
                else:
                    job['log'].append(x)
            if done is None:
                show_log()
                root.after(150, poll)
                return
            job.update(proc=None, ok=done)
            if done:                        # the game is there now: the pages that show it
                build()
            show_log()
        poll()

    # ---- window ----
    body = tk.Frame(root, bg=BG)
    body.pack(fill='both', expand=True)
    foot_o, foot = raised(root, WOOD)
    foot_o.pack(fill='x', side='bottom')
    status = tk.Label(foot, text='', font=F['b'], bg=WOOD, anchor='w')

    def save():
        nonlocal text
        for page, section, key, kind in OPTIONS:
            v = val(key)
            if kind.startswith('spin') and v.isdigit():     # typed numbers too: keep them in range
                v = str(min(max(int(v), 1), int(kind[4:])))
                vars_[key].set(v)
            was = loaded.get(key, '') if (key in active or kind not in ('file', 'dir')) else ''
            if not was and kind.startswith(('choice:', 'bool:')):
                was = kind.split(':')[1].split('|')[0]      # the game's default: nothing to write
            if v != was:
                text = set_ini(text, section, key, v)
        with open(INI, 'w', encoding='utf-8') as f:
            f.write(text)
        loaded.update(read_ini(text)[0])
        active.update(read_ini(text)[1])
        state['dirty'] = False
        state['note'] = '\u2714 ' + T('saved')
        refresh()

    def set_lang(l):
        """switch the language and keep it for next time, at once (other unsaved changes stay unsaved)"""
        nonlocal text
        state['lang'] = l
        text = set_ini(text, 'launcher', 'language', l)
        with open(INI, 'w', encoding='utf-8') as f:
            f.write(text)
        build()

    def play(args=()):
        save()
        if not os.path.exists(EXE):
            if SRC:
                show('build')
            else:
                messagebox.showerror('Harvest Moon', T('missing') % EXE)
            return
        # the game opens this launcher again with F5: it must not inherit this one's PyInstaller
        # variables (its files are gone by then: "Failed to load Python DLL")
        env = dict(os.environ, PYINSTALLER_RESET_ENVIRONMENT='1')
        subprocess.Popen([EXE, *args], cwd=HERE, env=env)
        root.destroy()

    def coop_port():
        p = val('coop_port')
        return p if p.isdigit() else COOP_PORT

    def coop_host():
        play(['--host', coop_port(), '--netdelay', val('coop_delay') or '3'] + (['--p2'] if val('coop_p2') == '1' else []))

    def coop_join():
        a = val('coop_address')
        if not a:
            messagebox.showwarning('Harvest Moon', T('need_addr'))
            show('coop')
            return
        if a.count(':') != 1 and not a.startswith('['):   # no port given (IPv6 needs [addr]:port)
            a = ('[%s]' % a if ':' in a else a) + ':' + coop_port()
        play(['--join', a])

    def update():
        if job['proc']:
            return
        state['note'] = T('updating')
        refresh()
        root.update()
        try:
            self_update(*state['update'][1:])
        except Exception as e:
            state['note'] = ''
            refresh()
            messagebox.showerror('Harvest Moon', T('update_failed') % e)
            return
        if state['dirty']:
            save()
        subprocess.Popen([sys.executable, '--after-update'], cwd=HERE,
                         env=dict(os.environ, PYINSTALLER_RESET_ENVIRONMENT='1'))
        root.destroy()

    def check_updates():
        """ask GitHub in the background; a newer launcher gets an Update button"""
        import threading
        found = []
        threading.Thread(target=lambda: found.append(latest_release()), daemon=True).start()

        def poll():
            if not found:
                return root.after(300, poll)
            if found[0]:
                state['update'] = found[0]
                state['note'] = T('update_note') % found[0][0]
                build()
        poll()

    def close():
        if job['proc']:
            if not messagebox.askyesno('Harvest Moon', T('ask_stop')):
                return
            if job['proc'] is not True:
                job['proc'].kill()
        if state['dirty']:
            a = messagebox.askyesnocancel('Harvest Moon', T('ask_save'))
            if a is None:
                return
            if a:
                save()
        root.destroy()
    root.protocol('WM_DELETE_WINDOW', close)

    def build():
        for w in body.winfo_children():
            w.destroy()
        for w in foot.winfo_children():
            if w is status:
                w.pack_forget()
            elif w is not foot.grad:
                w.destroy()
        widgets.clear()
        # header: the site's banner, the flags on a little .game-box
        hh = P['header'].height() if P['header'] else px(110)
        head = tk.Canvas(body, height=hh, highlightthickness=0, bg=SITE_BG)
        head.pack(fill='x')
        langs_o, langs = raised(head, WOOD, 3)
        langs.configure(padx=4, pady=2)
        head.create_window(0, 0, window=langs_o, anchor='ne', tags='langs')
        for l in ('en', 'es', 'pt'):
            on = l == state['lang']
            # the language in use stands on a darker square, the others brighten under the mouse
            b = tk.Frame(langs, bg=WOOD_HI if on else WOOD, padx=2)
            b.pack(side='left', padx=1)
            fl = flag(b, l, WOOD_HI if on else WOOD)
            fl.pack()
            for w in (b, fl):
                w.bind('<Button-1>', lambda e, l=l: set_lang(l))
                if not on:
                    w.bind('<Enter>', lambda e, b=b, fl=fl: [x.configure(bg='white') for x in (b, fl)])
                    w.bind('<Leave>', lambda e, b=b, fl=fl: [x.configure(bg=getattr(x, 'tint', WOOD))
                                                             for x in (b, fl)])
            if on:                                        # keeps its colour, not the gradient's
                b.tint = fl.tint = None
        head.bind('<Configure>', lambda e: draw_header(head, e.width, e.height))
        # the site's background, with the menu box and the white page on it
        main_ = tk.Canvas(body, highlightthickness=0, bg=SITE_BG)
        main_.pack(fill='both', expand=True)
        main_.bind('<Configure>', lambda e: tile(main_, e.width, e.height))
        m, sw = px(15), px(210)
        side_o, side = raised(main_, WOOD)
        side_o.place(x=m, y=m + px(8), width=sw)
        side.configure(padx=px(4), pady=px(4))
        holder = tk.Frame(main_, bg=BG)
        holder.place(x=2 * m + sw, y=m + px(8), relwidth=1, width=-(3 * m + sw), relheight=1,
                     height=-(2 * m + px(8)))
        pages, navs = {}, {}
        names = ('start', 'video', 'game', 'keyboard', 'controller', 'p2', 'coop', 'cheats')
        for name in names + (('build',) if SRC else ()):
            # a nav-link; the mouse pointer is already the game's glove
            n = tk.Label(side, text=T(name), font=F['nav'], bg=WOOD, fg=NAV_C, anchor='w', padx=px(10),
                         pady=px(6), cursor=cursor)
            n.pack(fill='x')
            n.bind('<Button-1>', lambda e, name=name: show(name))
            navs[name] = n
            outer, inner = scroll_page(holder)
            pages[name] = outer
            if name == 'start':
                start_page(inner)
            elif name == 'coop':
                coop_page(inner)
            elif name == 'p2':
                p2_page(inner)
            elif name == 'build':
                build_page(inner)
            else:
                page_head(inner, T(name), T(name + '_sub'))
                options_page(inner, name)
            tk.Frame(inner, bg=BG, height=12).pack()
        state['pages'], state['navs'] = pages, navs
        # footer
        tk.Label(foot, text='v' + VERSION, font=F['s'], bg=WOOD, fg=MUTED).pack(side='left', padx=(16, 0))
        if not live:
            button(foot, T('play'), play, accent=True).pack(side='right', padx=(6, 16), pady=10)
            button(foot, T('save'), save).pack(side='right', padx=6, pady=10)
        button(foot, T('close'), close).pack(side='right', padx=6, pady=10)
        if state.get('update') and not live:
            button(foot, T('update') % state['update'][0], update, accent=True).pack(side='right', padx=6, pady=10)
        status.pack(side='left', padx=16)           # last: on a narrow window the text gives way, not a button
        show(state['page'])
        refresh()

    def show(name):
        if name == 'start' and 'pages' in state:   # the presets show what is in use now
            old = state['pages'].pop('start')
            outer, inner = scroll_page(old.master)
            start_page(inner)
            state['pages']['start'] = outer
            old.destroy()
        state['page'] = name
        for n, p in state['pages'].items():
            p.pack_forget()
            state['navs'][n].configure(font=F['navb'] if n == name else F['nav'],
                                          fg=TEXT_C if n == name else NAV_C)
        state['pages'][name].pack(fill='both', expand=True)
        root.after(50, shade_all)

    for v in vars_.values():
        v.trace_add('write', changed)
    vars_['build_pak'] = tk.StringVar(value='0')    # the Build page's, not a setting
    build()
    if rebuild:
        start_build()
    elif getattr(sys, 'frozen', False) and not live:   # only an exe can replace itself
        check_updates()
    root.mainloop()


if __name__ == '__main__':
    if '--version' in sys.argv:
        print(VERSION)
    else:
        selftest() if '--selftest' in sys.argv else main()
