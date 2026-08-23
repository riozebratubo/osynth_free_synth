#!/usr/bin/env python3
"""Add the Chord page's pt_BR strings to app_osyntho/src/translator.cpp.

Kept per the project's intermediary-artifacts policy. Idempotent.

Following the file's own convention: only strings the app actually shows, and
universal musical terms that read the same in pt_BR are left out so t() falls
back to the source string. "Voicing", "Free", "Chord", "Mono", "Poly" and the
chord-quality labels (which come from the firmware anyway) are omitted for
that reason.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
path = REPO / "app_osyntho/src/translator.cpp"

BLOCK = '''
  // \u2500\u2500 Chord mode (ChordScreen.qml, TrackSheet.qml, S41) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
  pt["Chord mode"] = "Modo acorde";
  pt["Keys play chords."] = "As teclas tocam acordes.";
  pt["Keys play single notes."] = "As teclas tocam notas simples.";
  pt["User set"] = "Conjunto pr\\u00f3prio";
  pt["One chord shape under every key, transposed."] =
      "Uma forma de acorde sob cada tecla, transposta.";
  pt["The key picks a scale degree and the chord is stacked in scale thirds, so the quality follows the degree \\u2014 there is no wrong chord to play."] =
      "A tecla escolhe um grau da escala e o acorde \\u00e9 empilhado em ter\\u00e7as da "
      "escala, ent\\u00e3o a qualidade segue o grau \\u2014 n\\u00e3o existe acorde errado para tocar.";
  pt["Twelve slots, one per key of the octave above the root."] =
      "Doze posi\\u00e7\\u00f5es, uma para cada tecla da oitava acima da t\\u00f4nica.";
  pt["C plays"] = "C toca";
  pt["Tap a chord to hear it. In 'degrees' the keys run one scale step apart from middle C, so no key is dead or doubled; in 'chromatic' they keep their own pitch and are snapped into the scale."] =
      "Toque um acorde para ouvi-lo. Em 'degrees' as teclas avan\\u00e7am um grau da "
      "escala a partir do d\\u00f3 central, ent\\u00e3o nenhuma tecla fica morta ou repetida; "
      "em 'chromatic' elas mant\\u00eam a pr\\u00f3pria altura e s\\u00e3o ajustadas \\u00e0 escala.";
  pt["User chord set"] = "Conjunto de acordes pr\\u00f3prio";
  pt["This firmware has no user chord set."] =
      "Este firmware n\\u00e3o tem conjunto de acordes pr\\u00f3prio.";
  pt["silent"] = "silencioso";
  pt["Try"] = "Testar";
  pt["Voice-leading picks the inversion nearest the chord you just played, so a progression moves by the shortest path \\u2014 it overrides the inversion setting while it is on."] =
      "A condu\\u00e7\\u00e3o de vozes escolhe a invers\\u00e3o mais pr\\u00f3xima do acorde que voc\\u00ea "
      "acabou de tocar, ent\\u00e3o a progress\\u00e3o se move pelo caminho mais curto \\u2014 "
      "enquanto est\\u00e1 ligada, ela ignora o ajuste de invers\\u00e3o.";
  pt["Milliseconds between the notes of a chord. At 0 they all start together."] =
      "Milissegundos entre as notas de um acorde. Em 0 todas come\\u00e7am juntas.";
  pt["Routing"] = "Roteamento";
  pt["Pre-arp: one key gives a running arpeggio of the chord. Post-arp: each note the arpeggiator plays comes out as a block chord. Mono releases the previous key's chord when a new key lands, which is what keeps a chord inside eight voices."] =
      "Pre-arp: uma tecla gera um arpejo cont\\u00ednuo do acorde. Post-arp: cada nota "
      "que o arpejador toca sai como um acorde em bloco. Mono solta o acorde da "
      "tecla anterior quando uma nova tecla chega, que \\u00e9 o que mant\\u00e9m um acorde "
      "dentro das oito vozes.";
  pt["Chord mode is a performance setting: it survives a power cycle, and loading a preset never changes it. Sequencer tracks are chorded one at a time \\u2014 the switch is on the track sheet of the Sequencer page."] =
      "O modo acorde \\u00e9 um ajuste de performance: sobrevive a um desligamento, e "
      "carregar um preset nunca o altera. As faixas do sequenciador recebem acordes "
      "uma a uma \\u2014 a chave fica na ficha da faixa, na p\\u00e1gina Sequenciador.";
  pt["Chord mode expands this track"] = "O modo acorde expande esta faixa";
'''


def main():
    src = path.read_text(encoding="utf-8")
    if '"Chord mode"' in src:
        print("already patched")
        return
    anchor = '  // \u2500\u2500 On-screen keyboard (Keyboard.qml) '
    if src.count(anchor) != 1:
        sys.exit("anchor appears %d times" % src.count(anchor))
    # decode the \uXXXX escapes we wrote above into real characters
    block = BLOCK.encode("ascii", "backslashreplace").decode("unicode_escape")
    idx = src.index(anchor)
    path.write_text(src[:idx] + block.lstrip("\n") + "\n" + src[idx:],
                    encoding="utf-8")
    print("patched")


main()
