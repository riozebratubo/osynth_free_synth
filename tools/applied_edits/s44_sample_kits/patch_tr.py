"""Add the S44 sample-kit pt_BR strings to app_osyntho/src/translator.cpp."""
import io

P = 'app_osyntho/src/translator.cpp'
s = io.open(P, encoding='utf-8').read()
if 'Kits de sample (S44)' in s:
    raise SystemExit('already patched')

anchor = '  pt["Sample kits"] = "Kits de sample";\n'
assert anchor in s

ELL = chr(0x2026)   # ...
DASH = chr(0x2014)  # em dash
AC = chr(0xE1)      # a-acute
IC = chr(0xED)      # i-acute
OT = chr(0xF3)      # o-acute
AT = chr(0xE3)      # a-tilde
CC = chr(0xE7)      # c-cedilla
UC = chr(0xFA)      # u-acute

rows = [
    ('Record', 'Gravar'),
    ('Recording', 'Gravando'),
    ('Armed', 'Armado'),
    ('Pick a pad', 'Escolha um pad'),
    ('Stop', 'Parar'),
    ('Undo', 'Desfazer'),
    ('Erase', 'Apagar'),
    ('Waiting for sound' + ELL, 'Aguardando som' + ELL),
    ('Saving' + ELL, 'Salvando' + ELL),
    ('Save kit', 'Salvar kit'),
    ('Rename kit', 'Renomear kit'),
    ('Rename this kit', 'Renomear este kit'),
    ('Kit name', 'Nome do kit'),
    ('Copy to' + ELL, 'Copiar para' + ELL),
    ('Copy pad to', 'Copiar pad para'),
    ('Play', 'Toque'),
    ('Play this pad backwards', 'Tocar este pad ao contr' + AC + 'rio'),
    ('Start', 'In' + IC + 'cio'),
    ('Choke', 'Choke'),
    ('empty', 'vazio'),
    ('pad', 'pad'),
    ('free', 'livres'),
    ('one-shot', 'one-shot'),
    ('gate', 'gate'),
    ('loop', 'loop'),
    ('Saving to SD card', 'Salvando no cart' + AT + 'o SD'),
    ('Saving to internal flash (small)',
     'Salvando na flash interna (pouco espa' + CC + 'o)'),
    ('No storage ' + DASH + ' kits are lost at power off',
     'Sem armazenamento ' + DASH + ' os kits se perdem ao desligar'),
    ('Put back whatever the last record, erase or copy replaced',
     'Desfaz o que a ' + UC + 'ltima grava' + CC + AT + 'o, exclus' + AT +
     'o ou c' + OT + 'pia substituiu'),
]

block = [
    '',
    '  // ---- Kits de sample (S44) ----',
    '  // The recorder and the per-pad editor. "Kit", "pad", "gate", "loop" and',
    '  // "one-shot" are listed with identical values on purpose rather than left',
    '  // to fall back: they are the words a Portuguese-speaking musician uses',
    '  // too, and having them in the table is what stops the next person from',
    '  // "fixing" them into something nobody says.',
]
for k, v in rows:
    block.append('  pt["%s"] = "%s";' % (k, v))
block.append('')

s = s.replace(anchor, anchor + '\n'.join(block) + '\n', 1)
io.open(P, 'w', encoding='utf-8').write(s)
print('added %d keys' % len(rows))
