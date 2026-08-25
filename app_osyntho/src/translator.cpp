#include "translator.h"

#include <QDebug>
#include <QtQml/qqmlengine.h>

Translator& Translator::instance() {
  static Translator myInstance;
  return myInstance;
}

void Translator::setActiveLanguage(const QString& language) {
  const QString lang = language.toLower();
  if (languagesMap.contains(lang)) {
    currentTranslations = translations[languagesMap[lang]];
    qDebug() << "App | Current language: " << lang;
    return;
  }
  // Any code with no table IS the source language (en): drop back to an empty
  // map so t() returns the source strings. Leaving the previous table in place
  // made pt_BR -> English a no-op — the app stayed in Portuguese, and only in
  // that direction, which is why it read as "restart to apply".
  currentTranslations.clear();
  qDebug() << "App | Current language: source strings (" << lang << ")";
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

// Starts empty, i.e. on the source strings, until setActiveLanguage() picks a
// table. main() calls it with the stored force_app_language before the engine
// loads any QML.
Translator::Translator() {
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
  pt["All notes off"] = "Silenciar todas as notas";
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
  pt["Backup saved."] = "Backup salvo.";
  pt["Could not write the backup file."] = "Não foi possível gravar o arquivo de backup.";
  // Why a restore did not happen (database.cpp). These used to be qDebug()
  // lines only, with "Backup restored." shown regardless of the outcome.
  pt["Could not restore the backup."] = "Não foi possível restaurar o backup.";
  pt["That file is not an Osyntho backup."] = "Esse arquivo não é um backup do Osyntho.";
  pt["That file no longer exists."] = "Esse arquivo não existe mais.";
  pt["The app database cannot be written to."] =
      "Não é possível gravar no banco de dados do aplicativo.";
  pt["Could not back up the current data before restoring."] =
      "Não foi possível fazer uma cópia dos dados atuais antes de restaurar.";
  pt["Could not copy the backup over the app database."] =
      "Não foi possível copiar o backup sobre o banco de dados do aplicativo.";
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
  // "Arpeggiator" and "Local presets" are already listed elsewhere; "Looper"
  // and "Presets" read the same in pt_BR and are left to fall back.
  pt["Oscillator"] = "Oscilador";
  pt["Filter & envelopes"] = "Filtro e envelopes";
  pt["Modulation"] = "Modulação";
  pt["Input"] = "Entrada";
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
  pt["Subtractive"] = "Subtrativo";  // engine name; FM / Wavetable / Modular / Granular read the same
  pt["Additive"] = "Aditivo";
  pt["Voice"] = "Voz";
  pt["factory"] = "fábrica";  // also reused for the "(factory)" preset tag

  // ── Synth screens: discovery overlay (Tone/Filter/Mod/Fx/ArpSeq) ───────
  pt["Discovering parameters…"] = "Descobrindo parâmetros…";

  // ── Hold-to-sample button (HoldSampleCard.qml) ─────────────────────────
  pt["Hold to sample"] = "Segure para gravar";
  pt["Recording…"] = "Gravando…";
  pt["Live"] = "Ao vivo";
  pt["Hold and speak; the vowel is held on release."] =
      "Segure e fale; a vogal fica retida ao soltar.";
  pt["Hold to record into the ring; frozen on release."] =
      "Segure para gravar no buffer; congela ao soltar.";
  pt["Hold to learn"] = "Segure para aprender";
  pt["Learning…"] = "Aprendendo…";
  pt["Learning"] = "Aprendendo";
  pt["Hold during a silent moment; the room is sampled."] =
      "Segure em um momento de silêncio; a sala é amostrada.";

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
  pt["Vocoder"] = "Vocoder";  // listed only because the panel title is looked up
  pt["Vocoder capture"] = "Captura do vocoder";
  pt["Hold to record"] = "Segure para gravar";
  pt["Hold and speak a phrase; each note then says it back."] =
      "Segure e fale uma frase; cada nota a repete.";
  pt["Buffer capture"] = "Captura do buffer";
  // Vocoder starting points (VocoderPresets.qml, S43). The button captions are
  // the character each one is going for, so they are translated; the names of
  // the effects themselves are not, per the note at the top of this file.
  pt["Vocoder starting points"] = "Pontos de partida do vocoder";
  pt["Robot"] = "Robô";
  pt["Talkbox"] = "Talkbox";
  pt["Choir"] = "Coral";
  pt["Whisper"] = "Sussurro";
  pt["Giant"] = "Gigante";
  pt["Alien"] = "Alienígena";
  pt["Speak"] = "Fala";
  pt["The classic. Hold a saw chord and speak evenly."] =
      "O clássico. Segure um acorde de serra e fale de forma constante.";
  pt["Narrower and more forward. Sawtooth or square."] =
      "Mais estreito e mais presente. Dente de serra ou quadrada.";
  pt["Slow and smeared. Hold a pad and sing vowels."] =
      "Lento e difuso. Segure um pad e cante vogais.";
  pt["Noise carrier — this one works with no note held."] =
      "Portadora de ruído — este funciona sem nenhuma nota presa.";
  pt["Formants down seven semitones. Play low."] =
      "Formantes sete semitons abaixo. Toque grave.";
  pt["Formants up, with noise mixed into the synth."] =
      "Formantes acima, com ruído misturado ao sintetizador.";
  pt["Tuned for words rather than character."] =
      "Ajustado para as palavras, não para o timbre.";
  pt["Sets the Vocoder knobs above and switches it on. Pick the mic under "
     "Input, and hold a note — a vocoder with no synth under it is silent."] =
      "Ajusta os controles do vocoder acima e o liga. Escolha o microfone em "
      "Input e segure uma nota — um vocoder sem sintetizador embaixo fica "
      "mudo.";
  pt["Limiter"] = "Limitador";
  pt["Mic NR"] = "Redução de ruído do mic";
  pt["Mic noise profile"] = "Perfil de ruído do mic";
  pt["Adaptive NR"] = "Redução adaptativa";
  pt["Noise reduction"] = "Redução de ruído";
  pt["Noise profile"] = "Perfil de ruído";
  pt["Grain cloud"] = "Nuvem de grãos";
  pt["Capture buffer"] = "Buffer de captura";
  pt["Formant env"] = "Env. de formante";
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
  pt["Looper parameters were not received from this synth."] =
      "Os parâmetros do looper não foram recebidos deste synth.";
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
  // Track download (S33). "Slot %1" reads the same in pt_BR.
  pt["Download"] = "Baixar";
  pt["Cancel"] = "Cancelar";
  pt["Track WAV"] = "WAV da faixa";
  pt["Mix WAV"] = "WAV da mixagem";
  pt["Live set"] = "Conjunto atual";
  pt["Track %1"] = "Faixa %1";
  pt["mono"] = "mono";
  pt["stereo"] = "estéreo";
  pt["downloading… %1%"] = "baixando… %1%";
  pt["nothing recorded there"] = "nada gravado aí";
  pt["%1 track(s), %2 s %3 — a download runs at BLE speed, so allow a while; "
     "the mix uses the track levels below"] =
      "%1 faixa(s), %2 s %3 — o download roda na velocidade do BLE, então "
      "leva um tempo; a mixagem usa os volumes das faixas abaixo";
  pt["Audio (*.wav)"] = "Áudio (*.wav)";
  // ...and what the download itself can answer (SynthController).
  pt["Nothing is recorded there."] = "Não há nada gravado aí.";
  pt["Nothing is recorded on that track."] = "Não há nada gravado nessa faixa.";
  pt["Every recorded track is at level 0 — the mix would be silent."] =
      "Todas as faixas gravadas estão no volume 0 — a mixagem sairia muda.";
  pt["The synth cannot read that right now — stop the recording (and the "
     "loop, on flash storage)."] =
      "O sintetizador não consegue ler isso agora — pare a gravação (e o "
      "loop, se o armazenamento for em flash).";
  pt["The link kept dropping data."] = "A conexão continuou perdendo dados.";
  pt["That loop is %1 s long — more than this app will download (%2 s)."] =
      "Esse loop tem %1 s — mais do que este aplicativo baixa (%2 s).";
  pt["This firmware cannot send loop tracks to the app."] =
      "Este firmware não envia faixas do looper para o aplicativo.";
  pt["The synth stopped answering."] = "O sintetizador parou de responder.";
  pt["The synth disconnected."] = "O sintetizador desconectou.";
  pt["The synth refused the download (status %1)."] =
      "O sintetizador recusou o download (status %1).";
  pt["The synth sent a track this app cannot decode."] =
      "O sintetizador enviou uma faixa que o aplicativo não sabe decodificar.";

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
  pt["That file is empty, or could not be read."] =
      "Esse arquivo está vazio ou não pôde ser lido.";
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
  // Importing into the library (PatchLibraryScreen.qml): stored, never played.
  pt["Import patches from a JSON file into this library, without playing them"] =
      "Importar patches de um arquivo JSON para esta biblioteca, sem tocá-los";
  pt["Imported patch"] = "Patch importado";
  pt["Nothing in that file could be added to the library."] =
      "Nada nesse arquivo pôde ser adicionado à biblioteca.";
  pt["Added \"%1\" to the library."] = "\"%1\" adicionado à biblioteca.";
  pt["Added %1 patches to the library."] = "%1 patches adicionados à biblioteca.";
  pt["Added %1 patches to the library; %2 skipped."] =
      "%1 patches adicionados à biblioteca; %2 ignorados.";

  // ── Input / output / USB (InputScreen.qml, HomeScreen.qml) ─────────────
  pt["Audio input"] = "Entrada de áudio";
  pt["Analogue output"] = "Saída analógica";
  pt["This synth has no audio input."] = "Este sintetizador não tem entrada de áudio.";
  pt["USB port"] = "Porta USB";

  // ── Patch library (PatchLibraryScreen.qml) ─────────────────────────────
  // The page title and its full name in the startup-screen picker; it used to
  // be "Patch library". The nav dock's short "Loc. Pre" reads the same in
  // pt_BR and is left to fall back.
  pt["Local presets"] = "Presets locais";
  pt["Save current…"] = "Salvar atual…";
  pt["Rename"] = "Renomear";
  pt["Delete"] = "Excluir";
  pt["No saved patches yet"] = "Nenhum patch salvo ainda";
  pt["Save patch"] = "Salvar patch";
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
  pt["Every sound setting goes back to its default, and the sequencer patterns and the modular patch are cleared. Your saved presets, your local presets, the looper and the volume and input settings are left alone."] =
      "Todos os ajustes de som voltam ao padrão, e os padrões do sequenciador e o patch "
      "modular são apagados. Seus presets salvos, seus presets locais, o looper e "
      "os ajustes de volume e de entrada não são alterados.";

  // ── Navigation setting (SettingsScreen.qml) ────────────────────────────
  pt["Navigation"] = "Navegação";
  pt["Swipe to change screens"] = "Deslizar para trocar de tela";
  pt["Drag left or right anywhere on a page to move to the next one. Off by default: most pages are covered in knobs, grids and cables, and a drag meant for one of those changes the page instead. The bar at the top and the arrows in the toolbar always work."] =
      "Arraste para a esquerda ou direita em qualquer ponto da página para ir à "
      "seguinte. Desligado por padrão: a maioria das páginas é cheia de knobs, grades e "
      "cabos, e um arrasto destinado a um deles acaba trocando de página. A barra no "
      "topo e as setas da barra de ferramentas sempre funcionam.";

  // ── Chord mode (ChordScreen.qml, TrackSheet.qml, S41) ────────────────
  pt["Chord mode"] = "Modo acorde";
  pt["Keys play chords."] = "As teclas tocam acordes.";
  pt["Keys play single notes."] = "As teclas tocam notas simples.";
  pt["User set"] = "Conjunto próprio";
  pt["One chord shape under every key, transposed."] =
      "Uma forma de acorde sob cada tecla, transposta.";
  pt["The key picks a scale degree and the chord is stacked in scale thirds, so the quality follows the degree — there is no wrong chord to play."] =
      "A tecla escolhe um grau da escala e o acorde é empilhado em terças da "
      "escala, então a qualidade segue o grau — não existe acorde errado para tocar.";
  pt["Twelve slots, one per key of the octave above the root."] =
      "Doze posições, uma para cada tecla da oitava acima da tônica.";
  pt["C plays"] = "C toca";
  pt["Tap a chord to hear it. In 'degrees' the keys run one scale step apart from middle C, so no key is dead or doubled; in 'chromatic' they keep their own pitch and are snapped into the scale."] =
      "Toque um acorde para ouvi-lo. Em 'degrees' as teclas avançam um grau da "
      "escala a partir do dó central, então nenhuma tecla fica morta ou repetida; "
      "em 'chromatic' elas mantêm a própria altura e são ajustadas à escala.";
  pt["User chord set"] = "Conjunto de acordes próprio";
  pt["This firmware has no user chord set."] =
      "Este firmware não tem conjunto de acordes próprio.";
  pt["silent"] = "silencioso";
  pt["Try"] = "Testar";
  pt["Voice-leading picks the inversion nearest the chord you just played, so a progression moves by the shortest path — it overrides the inversion setting while it is on."] =
      "A condução de vozes escolhe a inversão mais próxima do acorde que você "
      "acabou de tocar, então a progressão se move pelo caminho mais curto — "
      "enquanto está ligada, ela ignora o ajuste de inversão.";
  pt["Milliseconds between the notes of a chord. At 0 they all start together."] =
      "Milissegundos entre as notas de um acorde. Em 0 todas começam juntas.";
  pt["Routing"] = "Roteamento";
  pt["Pre-arp: one key gives a running arpeggio of the chord. Post-arp: each note the arpeggiator plays comes out as a block chord. Mono releases the previous key's chord when a new key lands, which is what keeps a chord inside eight voices."] =
      "Pre-arp: uma tecla gera um arpejo contínuo do acorde. Post-arp: cada nota "
      "que o arpejador toca sai como um acorde em bloco. Mono solta o acorde da "
      "tecla anterior quando uma nova tecla chega, que é o que mantém um acorde "
      "dentro das oito vozes.";
  pt["Chord mode is a performance setting: it survives a power cycle, and loading a preset never changes it. Sequencer tracks are chorded one at a time — the switch is on the track sheet of the Sequencer page."] =
      "O modo acorde é um ajuste de performance: sobrevive a um desligamento, e "
      "carregar um preset nunca o altera. As faixas do sequenciador recebem acordes "
      "uma a uma — a chave fica na ficha da faixa, na página Sequenciador.";
  pt["Chord mode expands this track"] = "O modo acorde expande esta faixa";
  pt["Every setting on this page takes effect on the chord you are already holding, not just the next one — the row above decides whether you hear only the notes that changed, or the whole chord played again. A change that moves no note is silent either way. Voice-leading picks the inversion nearest the chord you just played, so a progression moves by the shortest path — it overrides the inversion setting while it is on."] =
      "Cada ajuste desta página vale para o acorde que você já está segurando, não apenas "
      "para o próximo — a linha acima decide se você ouve só as notas que mudaram ou o "
      "acorde inteiro tocado de novo. Uma mudança que não move nenhuma nota é silenciosa "
      "nos dois casos. A condução de vozes escolhe a inversão mais próxima do acorde que "
      "você acabou de tocar, então a progressão se move pelo caminho mais curto — enquanto "
      "está ligada, ela ignora o ajuste de inversão.";
  // Pad-row labels and options. Passed as properties rather than literals,
  // so tools/s41_check_translations.py lists them explicitly.
  pt["Inversion"] = "Inversão";
  // "Root" here is the scale root, which is what it means on the track
  // sheet too; the inversion pad says "Root pos." precisely so the two
  // senses do not share one entry.
  pt["Root"] = "Tônica";
  pt["Scale"] = "Escala";
  pt["Notes in the chord"] = "Notas do acorde";
  pt["When a setting changes"] = "Quando um ajuste muda";
  pt["Play what changed"] = "Tocar o que mudou";
  pt["Play the whole chord"] = "Tocar o acorde inteiro";
  pt["Root pos."] = "Fundamental";
  pt["Single"] = "Nota única";
  pt["Triad"] = "Tríade";
  pt["1st"] = "1ª";
  pt["2nd"] = "2ª";
  pt["3rd"] = "3ª";
  pt["7th"] = "7ª";
  pt["9th"] = "9ª";
  pt["11th"] = "11ª";
  pt["13th"] = "13ª";

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

Translator* Translator::create(QQmlEngine*, QJSEngine*) {
  Translator* translator = &Translator::instance();
  QJSEngine::setObjectOwnership(translator, QJSEngine::CppOwnership);
  return translator;
}
