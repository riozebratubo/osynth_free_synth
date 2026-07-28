#include "translator.h"

#include <QDebug>

Translator& Translator::instance() {
  static Translator myInstance;
  return myInstance;
}

void Translator::setActiveLanguage(const QString& language) {
  const QString lang = language.toLower();
  if (languagesMap.contains(lang)) {
    currentTranslations = translations[languagesMap[lang]];
    // qDebug() << ">>> set current language success!";
    // qDebug() << ">>> current translations: " << currentTranslations;
    qDebug() << "App | Current language: " << lang;
  }
}

QString Translator::t(const QString& str) {
  //
  return currentTranslations.value(str, str);
}

QString Translator::ts(const QString& str,
                       const QString& param1,
                       const QString& param2,
                       const QString& param3) {
  // Substitute only the markers the string actually contains — the multi-arg
  // .arg(p1, p2, p3) logs a "QString::arg: argument(s) missing" warning for
  // every string with fewer than 3 placeholders.
  QString s = currentTranslations.value(str, str);
  if (s.contains(QLatin1String("%1"))) s = s.arg(param1);
  if (s.contains(QLatin1String("%2"))) s = s.arg(param2);
  if (s.contains(QLatin1String("%3"))) s = s.arg(param3);
  return s;
}

Translator::Translator()
  : currentTranslations{dummyTranslations} {
  languagesMap = {{"pt_br", 0}};

  translations.append(QHash<QString, QString>{});
  auto& pt = translations[0];  // Brazilian Portuguese (pt_BR)

  // Only strings osyntho actually shows are listed. Universal audio terms and
  // short nav labels that read the same in pt_BR (Bluetooth, Mixer, Reverb,
  // Chorus, Delay, Tempo, Master, Preset, Wavetable, LFO, Envelope, FM, Osc,
  // Flt, Mod, FX, Arp, Pre, Lib, …) are intentionally omitted so t() falls
  // back to the source string.

  // ── Toolbar menu (Toolbar.qml) ─────────────────────────────────────────
  pt["Save data..."] = "Salvar dados...";
  pt["Restore data..."] = "Restaurar dados...";
  pt["Show keyboard"] = "Mostrar teclado";
  pt["Select device..."] = "Selecionar dispositivo...";
  pt["Update firmware..."] = "Atualizar firmware...";
  pt["Settings"] = "Configurações";

  // ── Toolbar subtitle / navigation dock (Toolbar.qml, Main.qml) ─────────
  pt["Not connected"] = "Desconectado";
  pt["connecting…"] = "conectando…";
  pt["Home"] = "Início";

  // ── Backup file dialog filters (Main.qml) ──────────────────────────────
  pt["Database (*.db)"] = "Banco de dados (*.db)";
  pt["All files (*)"] = "Todos os arquivos (*)";

  // ── Toasts / messages (Main.qml, synthcontroller.cpp) ──────────────────
  pt["Firmware update is not available yet for osynth."] =
      "A atualização de firmware ainda não está disponível para o osynth.";
  pt["Backup restored."] = "Backup restaurado.";
  pt["Nothing to save yet — no parameters have been read."] =
      "Nada para salvar ainda — nenhum parâmetro foi lido.";
  pt["Could not save the patch."] = "Não foi possível salvar o patch.";

  // ── Settings ▸ General (SettingsScreen.qml) ────────────────────────────
  pt["Osyntho — Settings"] = "Osyntho — Configurações";
  pt["Version:"] = "Versão:";
  pt["General"] = "Geral";
  pt["Keyboard"] = "Teclado";
  pt["App theme"] = "Tema do app";
  pt["Use dark theme"] = "Usar tema escuro";
  pt["App font size"] = "Tamanho da fonte";
  pt["Sample text"] = "Texto de exemplo";
  pt["Panel layout"] = "Disposição dos painéis";
  pt["Tiled"] = "Lado a lado";
  pt["One per line"] = "Um por linha";
  pt["How the parameter panels (oscillator, filter, FX…) are arranged: packed left to right "
     "at the width each one needs, or each one alone on a full-width line."] =
      "Como os painéis de parâmetros (oscilador, filtro, FX…) são dispostos: agrupados da "
      "esquerda para a direita, cada um com a largura de que precisa, ou cada um sozinho em "
      "uma linha inteira.";
  pt["Startup screen"] = "Tela inicial";
  pt["Last used"] = "Última usada";
  pt["Which page the app opens on. \"Last used\" reopens whichever page you were on when "
     "you closed it."] =
      "Em qual página o app abre. \"Última usada\" reabre a página em que você estava ao "
      "fechá-lo.";
  // Full page names for the picker above (UI.screens). "Home", "Sequencer",
  // "Arpeggiator" and "Patch library" are already listed elsewhere; "Looper"
  // and "Presets" read the same in pt_BR and are left to fall back.
  pt["Oscillator"] = "Oscilador";
  pt["Filter & envelopes"] = "Filtro e envelopes";
  pt["Modulation"] = "Modulação";
  pt["Effects"] = "Efeitos";
  pt["Drums"] = "Bateria";
  pt["App language (restart to apply)"] = "Idioma (reinicie o app para aplicar)";
  pt["English"] = "Inglês";
  pt["Portuguese"] = "Português";
  pt["Display"] = "Tela";
  pt["Fullscreen (immersive)"] = "Tela cheia (imersivo)";
  pt["Hides the status and navigation bars. Recommended: it also stops some phones "
     "(e.g. Xiaomi) from stealing multi-finger touches for system gestures."] =
      "Oculta as barras de status e de navegação. Recomendado: também impede que alguns "
      "celulares (ex.: Xiaomi) capturem toques com vários dedos para gestos do sistema.";

  // ── Settings ▸ Keyboard (SettingsScreen.qml) ───────────────────────────
  pt["On-screen keyboard"] = "Teclado na tela";
  pt["Base octave"] = "Oitava base";
  pt["Velocity"] = "Velocidade";
  pt["Latch notes (hold)"] = "Sustentar notas (segurar)";
  pt["Show note names on keys"] = "Mostrar nomes das notas nas teclas";
  pt["Computer keys play the synth"] = "Teclas do computador tocam o sintetizador";
  pt["Top rows play drum pads"] = "Linhas de cima tocam os pads de bateria";
  pt["Resize control"] = "Controle de redimensionamento";
  pt["Divider (drag handle)"] = "Divisória (alça de arraste)";
  pt["Slider"] = "Controle deslizante";
  pt["How to resize the on-screen keyboard: drag its top edge, or use a slider in its "
     "toolbar."] =
      "Como redimensionar o teclado na tela: arraste a borda superior ou use um controle "
      "deslizante na barra de ferramentas.";
  pt["Divider thickness (px)"] = "Espessura da divisória (px)";

  // ── Settings ▸ Bluetooth (SettingsScreen.qml) ──────────────────────────
  pt["Bluetooth changes take effect on the next scan; a restart is safest."] =
      "As alterações de Bluetooth têm efeito na próxima busca; reiniciar é mais seguro.";
  pt["Enable Bluetooth"] = "Habilitar Bluetooth";
  pt["Specific device"] = "Dispositivo específico";
  pt["Lock to a specific device"] = "Travar em um dispositivo específico";
  pt["No device saved"] = "Nenhum dispositivo salvo";
  pt["Clear"] = "Limpar";
  pt["Scan time (s)"] = "Tempo de busca (s)";
  pt["Device name prefix"] = "Prefixo do nome do dispositivo";

  // ── Bluetooth device selector (BluetoothDeviceSelectorScreen.qml) ──────
  pt["Select Bluetooth Device"] = "Selecionar dispositivo Bluetooth";
  pt["Saved device"] = "Dispositivo salvo";  // also reused for "Saved device: …"
  pt["None"] = "Nenhum";
  pt["Clear selection"] = "Limpar seleção";
  pt["Scanning for devices..."] = "Procurando dispositivos...";
  pt["Not scanning. Scan will start automatically."] =
      "Não está procurando. A busca iniciará automaticamente.";
  pt["Enable Bluetooth to discover devices."] =
      "Habilite o Bluetooth para descobrir dispositivos.";
  pt["Discovered devices"] = "Dispositivos encontrados";
  pt["(unknown)"] = "(desconhecido)";
  pt["Select"] = "Selecionar";
  pt["No devices discovered yet"] = "Nenhum dispositivo encontrado ainda";

  // ── Home (HomeScreen.qml) ──────────────────────────────────────────────
  pt["%1 engine"] = "Motor %1";
  pt["Discovering…"] = "Descobrindo…";
  pt["Subtractive"] = "Subtrativo";  // engine name; FM / Wavetable read the same
  pt["Additive"] = "Aditivo";
  pt["Voice"] = "Voz";
  pt["factory"] = "fábrica";  // also reused for the "(factory)" preset tag

  // ── Synth screens: discovery overlay (Tone/Filter/Mod/Fx/ArpSeq) ───────
  pt["Discovering parameters…"] = "Descobrindo parâmetros…";

  // ── Param group titles (ParamGroup.qml, translated centrally) ──────────
  pt["Oscillator 1"] = "Oscilador 1";
  pt["Oscillator 2"] = "Oscilador 2";
  pt["Noise"] = "Ruído";
  pt["Partials / Spectrum"] = "Parciais / Espectro";
  pt["Operator A"] = "Operador A";
  pt["Operator B"] = "Operador B";
  pt["FM velocity"] = "Velocidade FM";
  pt["Table motion"] = "Movimento da tabela";
  pt["Brightness env"] = "Env. de brilho";
  pt["Filter"] = "Filtro";
  pt["Vibrato LFO"] = "LFO de vibrato";
  pt["Granular delay"] = "Delay granular";
  pt["Arpeggiator"] = "Arpejador";
  pt["Sequencer"] = "Sequenciador";

  // ── Modulation matrix (ModScreen.qml, ModMatrixSlot.qml) ───────────────
  pt["Mod matrix"] = "Matriz de modulação";
  pt["— none —"] = "— nenhum —";
  pt["dest"] = "destino";

  // ── Arp / sequencer transport (ArpSeqScreen.qml) ───────────────────────
  pt["Stop"] = "Parar";
  pt["Play"] = "Tocar";
  pt["Rec"] = "Gravar";

  // ── Looper (LooperScreen.qml; Stop/Play/Rec shared with the arp above,
  //    "Slot" reads the same in pt_BR) ────────────────────────────────────
  pt["Tracks"] = "Faixas";
  pt["Clear track"] = "Limpar faixa";
  pt["Clear all"] = "Limpar tudo";
  pt["Save set"] = "Salvar conjunto";
  pt["Load set"] = "Carregar conjunto";
  pt["Flash backend needs the loop stopped; loading replaces the current set."] =
      "O backend flash precisa do loop parado; carregar substitui o conjunto atual.";
  pt["Clear all tracks?"] = "Limpar todas as faixas?";
  pt["All recorded tracks are discarded and the loop length is reset."] =
      "Todas as faixas gravadas são descartadas e a duração do loop é zerada.";
  pt["Looper not available on this synth (needs PSRAM)."] =
      "Looper não disponível neste synth (precisa de PSRAM).";
  pt["loop %1 s"] = "loop de %1 s";
  pt["no loop — rec records the first track and sets the length (max %1 s)"] =
      "sem loop — rec grava a primeira faixa e define a duração (máx. %1 s)";
  pt["punch at loop start — %1 s"] = "punch no início do loop — %1 s";
  pt["● rec %1"] = "● grav %1";
  pt["Record mono"] = "Gravar em mono";
  pt["4 tracks"] = "4 faixas";
  pt["applies to the next loop (after clear all)"] =
      "vale para o próximo loop (após limpar tudo)";
  pt["max loop %1 s"] = "loop de no máx. %1 s";

  // ── Presets (PresetsScreen.qml) ────────────────────────────────────────
  pt["Refresh"] = "Atualizar";
  pt["(unnamed)"] = "(sem nome)";  // shared with the patch library
  pt["Load"] = "Carregar";
  pt["No presets"] = "Nenhum preset";
  pt["Save to slot"] = "Salvar no slot";
  pt["Preset name"] = "Nome do preset";
  pt["Save"] = "Salvar";

  // ── Patch interchange (Main.qml, PresetsScreen.qml, PatchLibraryScreen.qml,
  //    synthcontroller.cpp) ────────────────────────────────────────────────
  pt["Import…"] = "Importar…";
  pt["Export all…"] = "Exportar tudo…";
  pt["Export this patch to a JSON file"] = "Exportar este patch para um arquivo JSON";
  pt["Export this preset to a JSON file. It is loaded first — the only way to read a slot."] =
      "Exportar este preset para um arquivo JSON. Ele é carregado antes — é a única forma de "
      "ler um slot.";
  pt["Patch files (*.json)"] = "Arquivos de patch (*.json)";
  pt["Exported."] = "Exportado.";
  pt["Nothing to export."] = "Nada para exportar.";
  pt["Could not write the file."] = "Não foi possível gravar o arquivo.";
  pt["Could not read the file."] = "Não foi possível ler o arquivo.";
  pt["Connect to the synth first."] = "Conecte-se ao sintetizador primeiro.";
  pt["Loading preset %1 to read it…"] = "Carregando o preset %1 para lê-lo…";
  pt["That file is not valid JSON."] = "Esse arquivo não é um JSON válido.";
  pt["That file holds no patch."] = "Esse arquivo não contém nenhum patch.";
  pt["That patch file is version %1; this app reads up to %2."] =
      "Esse arquivo de patch é da versão %1; este app lê até a versão %2.";
  pt["That file holds %1 patches; imported the first."] =
      "Esse arquivo contém %1 patches; o primeiro foi importado.";
  pt["No parameter in that file matches this synth."] =
      "Nenhum parâmetro desse arquivo corresponde a este sintetizador.";
  pt["Applied %1 parameters."] = "%1 parâmetros aplicados.";
  pt["Applied %1 parameters; %2 in the file do not apply to this synth."] =
      "%1 parâmetros aplicados; %2 do arquivo não se aplicam a este sintetizador.";

  // ── Patch library (PatchLibraryScreen.qml) ─────────────────────────────
  pt["Patch library"] = "Biblioteca de patches";
  pt["Save current…"] = "Salvar atual…";
  pt["Rename"] = "Renomear";
  pt["Delete"] = "Excluir";
  pt["No saved patches yet"] = "Nenhum patch salvo ainda";
  pt["Save patch"] = "Salvar patch";
  pt["Rename patch"] = "Renomear patch";
  pt["Patch name"] = "Nome do patch";

  // ── On-screen keyboard (Keyboard.qml) ──────────────────────────────────
  pt["Octave"] = "Oitava";
  pt["hold"] = "segurar";
  pt["size"] = "tamanho";
  pt["Computer keys play the synth. Click to release them to the app."] =
      "As teclas do computador tocam o sintetizador. Clique para liberá-las para o "
      "aplicativo.";
  pt["Computer keys are off. Click to play the synth from them."] =
      "As teclas do computador estão desligadas. Clique para tocar o sintetizador com "
      "elas.";
  pt["Q…I and 1…8 fire the drum pads. Click to play a second octave instead."] =
      "Q…I e 1…8 tocam os pads de bateria. Clique para tocarem uma segunda oitava.";
  pt["Q…I and 1…8 play a second octave. Click to fire the drum pads instead."] =
      "Q…I e 1…8 tocam uma segunda oitava. Clique para tocarem os pads de bateria.";
}
