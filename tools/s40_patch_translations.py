p = 'app_osyntho/src/translator.cpp'
s = open(p, encoding='utf-8').read()

old = """  pt["Save patch"] = "Salvar patch";
  pt["Rename patch"] = "Renomear patch";
  pt["Patch name"] = "Nome do patch";
"""
new = """  pt["Save patch"] = "Salvar patch";
  pt["Rename patch"] = "Renomear patch";
  pt["Patch name"] = "Nome do patch";
  pt["Load with the patch:"] = "Carregar junto com o patch:";
  pt["Synth volume"] = "Volume do sintetizador";
  pt["Headphone level"] = "Nível do fone";
  pt["Also set the master volume a patch was saved with. Off by default: that is the level you are monitoring at, not part of the sound."] =
      "Também ajustar o volume principal com que o patch foi salvo. Desligado por "
      "padrão: esse é o nível em que você está monitorando, não faz parte do som.";
  pt["Also set the analogue output level a patch was saved with. Off by default: it is set once by ear for what is plugged into the jack."] =
      "Também ajustar o nível de saída analógica com que o patch foi salvo. Desligado "
      "por padrão: ele é ajustado uma vez de ouvido para o que está ligado na saída.";

  // ── Working state / reset (HomeScreen.qml) ─────────────────────────────
  pt["Start from scratch"] = "Começar do zero";
  pt["The synth remembers how you left it and comes back that way. This puts it back to the sound it had out of the box."] =
      "O sintetizador lembra como você o deixou e volta assim. Isto o devolve ao som "
      "que ele tinha de fábrica.";
  pt["Reset…"] = "Redefinir…";
  pt["Reset the synth?"] = "Redefinir o sintetizador?";
  pt["Every sound setting goes back to its default, and the sequencer patterns and the modular patch are cleared. Your saved presets, the patch library, the looper and the volume and input settings are left alone."] =
      "Todos os ajustes de som voltam ao padrão, e os padrões do sequenciador e o patch "
      "modular são apagados. Seus presets salvos, a biblioteca de patches, o looper e "
      "os ajustes de volume e de entrada não são alterados.";

  // ── Navigation setting (SettingsScreen.qml) ────────────────────────────
  pt["Navigation"] = "Navegação";
  pt["Swipe to change screens"] = "Deslizar para trocar de tela";
  pt["Drag left or right anywhere on a page to move to the next one. Off by default: most pages are covered in knobs, grids and cables, and a drag meant for one of those changes the page instead. The bar at the top and the arrows in the toolbar always work."] =
      "Arraste para a esquerda ou direita em qualquer ponto da página para ir à "
      "seguinte. Desligado por padrão: a maioria das páginas é cheia de knobs, grades e "
      "cabos, e um arrasto destinado a um deles acaba trocando de página. A barra no "
      "topo e as setas da barra de ferramentas sempre funcionam.";
"""
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("ok")
