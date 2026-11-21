# Design

## Obiettivo

Rendere la ricerca di file in Esplora file veloce quanto Everything, senza
rinunciare alla vista di Explorer (selezione multipla, menu contestuali nativi,
drag & drop, proprietÃ , "Apri con", tab, cronologia).

Non obiettivo: reimplementare un file manager nÃ© il motore di ricerca.

## PerchÃ© serve una namespace extension

Explorer non espone un punto di estensione per fornire risultati a una ricerca.
La vista dei risultati Ã¨ una *search folder* popolata da Windows Search; le
vecchie federated search connector (`.searchconnector-ms`) sono state rimosse
da Windows 8.

Un mod Windhawk puÃ² hookare codice, ma non esiste una funzione da hookare che
"restituisca risultati" a DefView. L'unico modo supportato per mostrare item
arbitrari in una vista Explorer standard Ã¨ implementare una cartella shell
(`IShellFolder`) e far navigare Explorer su di essa.

## Architettura

```
                 â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ explorer.exe â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
 search box      â”‚  Windhawk mod (PeekMessageW hook)                                       â”‚
   â”‚             â”‚      â”œâ”€ IShellWindows â”€â–¶ IShellBrowser â”€â–¶ location / active view         â”‚
   â”‚             â”‚      â”œâ”€ scrive %LOCALAPPDATA%\EverythingExplorerSearch\request.txt        â”‚
   â”‚             â”‚      â””â”€ BrowseObject(shell:::{GUID}) oppure IShellView::Refresh()         â”‚
   â”‚             â”‚                                                                          â”‚
   â”‚             â”‚  EverythingSearchNse.dll  (in-proc COM, STA)                            â”‚
   â”‚             â”‚      IShellFolder2 / IPersistFolder2 / IShellFolderViewCB                 â”‚
   â”‚             â”‚      â”‚                                                                   â”‚
   â”‚             â”‚      â””â”€ EnumObjects â”€â–¶ legge request.txt â”€â–¶ bridge::Query â”€â”              â”‚
   â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¼â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
                                                                             â”‚ WM_COPYDATA
                                                                             â”‚ QUERY2, thread
                                                                             â”‚ dedicato, timeout
                                                                             â–¼
                                                                      Everything.exe
```

Tre artefatti distribuiti:

| Artefatto | Chi lo installa | Note |
|---|---|---|
| mod `.wh.cpp` | Windhawk (dalla sua UI o dal sito) | solo sorgente, nessun binario |
| `EverythingSearchNse.dll` | la mod, su click esplicito | asset della release GitHub |
| Everything | l'utente | prerequisito, dell'utente |

### PerchÃ© l'IPC diretto e non `Everything64.dll`

L'SDK di voidtools non Ã¨ un client "sottile" riutilizzabile con sicurezza
dentro la shell: `src/Everything.c` usa uno stato globale protetto da
`CRITICAL_SECTION` e `Everything_QueryW(TRUE)` attende
`WaitForSingleObject(..., INFINITE)`. Caricarlo in `explorer.exe` significa che
un Everything occupato o elevato blocca il thread chiamante e, con esso, parte
della shell. Inoltre servirebbe scaricare l'SDK a ogni build.

Il NSE parla quindi il protocollo IPC documentato (`QUERY2` in `WM_COPYDATA`
sulla finestra `EVERYTHING_TASKBAR_NOTIFICATION`) direttamente, con
`shared/everything_ipc.h` come unica dipendenza (una ventina di righe, nessun
codice di terzi caricato nel processo di Explorer).

## Trasporto della query

### request.txt

`%LOCALAPPDATA%\EverythingExplorerSearch\request.txt`, UTF-8, `chiave=valore`
una per riga (split sulla **prima** `=`):

```
seq=123456789
max=0
flags=3
folder=C:\Users\<utente>\Documenti
query=fattura 2026
```

- `seq`: solo per rilevare modifiche; il NSE confronta anche `query` e `folder`.
- `flags`: bit 0 = scoped alla cartella.
- Scrittura atomica: file temporaneo + `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`.

Il file Ã¨ deliberatamente stupido: sopravvive a riavvii di Explorer e rende
banale il testing da PowerShell.

### IPC con Everything

Dal thread chiamante non si tocca mai `WM_COPYDATA`:

- un solo thread worker possiede una finestra `HWND_MESSAGE`;
- `PostMessage(worker, ...)` con un `Job` che porta `replyId`, pagina
  (`offset`), `max_results` e i flag;
- il worker invia con `SendMessageTimeoutW(..., SMTO_ABORTIFHUNG, 2000 ms)`;
- la risposta arriva in `WM_COPYDATA` e viene accettata **solo** se
  `dwData == job->replyId` (base `0x45530000` + contatore), cosÃ¬ una risposta
  in ritardo di una query abbandonata non viene scambiata per quella corrente;
- il chiamante attende l'evento di completamento con timeout 700 ms;
- il modulo resta pinnato (`g_cRefDll`) finchÃ© il worker Ã¨ vivo, perchÃ© COM non
  puÃ² scaricare una DLL che ha ancora una finestra e un thread attivi.

Layout e semantica del protocollo sono documentati in
`shared/everything_ipc.h`. Due dettagli verificati sul campo (Everything
1.5.0.1423) e facili da sbagliare:

- `Item2` Ã¨ `{DWORD flags; DWORD data_offset;}`, `data_offset` Ã¨ relativo
  all'inizio della `List2`;
- i campi sono impacchettati nell'ordine dei bit di `request_flags`, senza
  padding; le stringhe sono `DWORD` (numero di caratteri) + `(len + 1)` WCHAR;
- `PATH` Ã¨ la **cartella contenitrice**, non il percorso completo: va unito a
  `NAME` (gestendo la root `C:\` senza aggiungere un separatore).

### Paging

Everything tronca la lista *prima* che il NSE possa scartare le cartelle o i
drive, quindi una cartella con molte sottocartelle restituirebbe zero file
(verificato: `C:\Windows *.exe` restituiva 0 risultati). Il bridge pagina:
`kBatchSize = 2048` risultati grezzi per giro, fino a `kMaxBatches = 48`
(98.304 grezzi), fermandosi quando il totale richiesto Ã¨ raggiunto.

## Formato dei PIDL

Un PIDL Ã¨ una sequenza di `SHITEMID` terminata da un elemento di lunghezza 0.
Ogni `SHITEMID` inizia con `USHORT cb` (lunghezza totale dell'elemento, campo
compreso).

Root della cartella (8 byte, deterministico):

```
06 00 | 'E' 00 'S' 00 | 00 00
 cb=6      magic        terminatore
```

Item figlio (relativo alla root, come richiede il contratto shell):

```
[cb] | UTF-16 path assoluto + NUL
```

`DefView` combina la root con il PIDL relativo per costruire il PIDL assoluto;
per questo `EnumObjects` **non** deve restituire la root nel PIDL del figlio.

`GetPidlPath` accetta entrambe le forme (relativa e, per difesa, root+item) e
valida il risultato con `PathIsAbsoluteW` per non interpretare come path PIDL
estranei.

## Delega al shell ("proxy folder")

Ogni item Ã¨ un file reale. Il NSE non reimplementa nulla:

| Metodo | Implementazione |
|---|---|
| `EnumObjects` | elenca i path in cache, crea PIDL relativi |
| `GetDisplayNameOf` | `FORPARSING`/`FORADDRESSBAR` â†’ path reale, altrimenti nome file |
| `GetAttributesOf` | `SHParseDisplayName` per gli attributi richiesti, con cache per item per evitare chiamate ripetute durante selezione e drag |
| `GetUIObjectOf` | `IContextMenu*` â†’ oggetto nativo da `IShellItemArray::BindToHandler(BHID_SFUIObject)`; `IDataObject` â†’ `BHID_DataObject`; altro â†’ `BHID_SFUIObject` |
| `GetDetailsOf` | 4 colonne (Name, Size, Type, Date modified) dalla cache |
| `CompareIDs` | nome (`StrCmpLogicalW`), size, data |
| `BindToObject` | `E_NOTIMPL` (vista piatta read-only) |
| `SetNameOf` | `E_NOTIMPL` |
| `CreateViewObject` | `SHCreateShellFolderView` con callback per conteggio e modalitÃ  della vista |

`GetUIObjectOf` Ã¨ il punto che fa funzionare tutto: menu contestuale, "Apri
con", proprietÃ , copia, drag & drop e icone arrivano dal vero item filesystem.

## Installer e pannello della mod

La mod non puÃ² portare con sÃ© un binario (viene compilata da sorgente dentro
Windhawk), quindi fa da installatore:

- `Ctrl+Shift+F` apre il pannello; `Ctrl+F` apre il pannello solo se il
  componente non Ã¨ utilizzabile o Everything non Ã¨ in esecuzione (una ricerca
  su una cartella inesistente sarebbe solo un errore);
- il pannello mostra: stato di Everything (assente / irraggiungibile per UIPI /
  versione), stato del componente (non installato / DLL mancante / versione
  attesa);
- **Install** prende il `EverythingSearchNse*.dll` piÃ¹ recente in Download,
  **Choose file** apre `IFileOpenDialog`;
- la copia Ã¨ validata con `LoadLibraryEx(LOAD_LIBRARY_AS_DATAFILE)` prima di
  registrare il CLSID: un file sbagliato non finisce nel registro;
- la registrazione Ã¨ `HKCU\Software\Classes\CLSID\{GUID}` +
  `HKCU\Software\EverythingExplorerSearch` (`Version`, `Path`), con
  `ThreadingModel=Apartment` e `ShellFolder\Attributes = SFGAO_FOLDER`;
- **Uninstall** cancella le chiavi e il file (se Ã¨ in uso, `MOVEFILE_DELAY_UNTIL_REBOOT`);
- **Download** apre soltanto la pagina delle release: nessun download
  automatico, per scelta.

La DLL vive in `%LOCALAPPDATA%\EverythingExplorerSearch\nse\<versione>\`: una
DLL giÃ  mappata da Explorer non si puÃ² sovrascrivere, quindi ogni versione ha la
sua cartella e l'aggiornamento Ã¨ una copia nuova + rewrite della chiave.

### Build e install da sorgente

La mod si compila anche fuori dall'editor: `mod\build-mod.ps1` usa il clang in
`C:\Program Files\Windhawk\Compiler` con la stessa riga di comando dell'editor
(`windhawk.lib` dell'engine attivo, ricavato da `windhawk.ini`,
`-include windhawk_api.h`, `-DWH_MOD*`) e legge `@version`/`@compilerOptions`
dal sorgente, cosÃ¬ la build non puÃ² divergere da quella dell'UI.

`install-mod.ps1` chiude il cerchio per lo sviluppo: compila (o riusa
`-SkipBuild`), copia la DLL in `Engine\Mods\64`, allinea il sorgente in
`ModsSource`, scrive
`HKLM\SOFTWARE\Windhawk\Engine\Mods\local@everything-explorer-search` con
`Include`/`Exclude` presi dai metadati del `.wh.cpp`, `Architecture=x86-64`,
`Disabled=0` e i due setting predefiniti (`maxResults=0`,
`scopeToFolder=1`): senza chiave `Settings` l'engine leggerebbe 0 e la ricerca
non restituirebbe nulla. Poi riavvia l'engine; Explorer non viene terminato,
perchÃ© la mod viene iniettata alla finestra successiva.

`uninstall-mod.ps1` mette prima `Disabled=1` (una DLL ancora mappata non deve
essere ricaricata), poi rimuove chiave, binari e sorgente. Il componente shell
resta installato: per quello c'Ã¨ `uninstall.ps1`.

## Blocco della ricerca nativa

Explorer avvia la search-as-you-type appena si digita nella casella e naviga
verso la search folder di Windows Search (`{04731B67-D933-450A-90E6-4ACD2E9408FE}`).
Per restare nella vista cartella fino a Invio la mod blocca quella navigazione
su tre punti, senza toccare il testo della casella:

1. `SHParseDisplayName` â€” un nome `search-ms:`/`search:` viene risolto come
   cartella di partenza (`g_searchOrigin`), cosÃ¬ la vista non cambia;
2. `ShellExecuteExW` / `ShellExecuteW` â€” un moniker `search-ms:` viene
   dichiarato eseguito senza aprire nulla;
3. `IShellBrowser::BrowseObject` (slot 11 della vtable, ottenuta da
   `IShellWindows`) â€” un PIDL che punta alla search folder di Windows viene
   dichiarato navigato senza navigare. I PIDL della nostra cartella sono
   esclusi esplicitamente.

Le proprie chiamate della mod sono esentate con `tls_internalNavigation`,
altrimenti il blocco si applicherebbe anche alla navigazione verso la cartella
virtuale. Le build `-Trace` loggano in `%TEMP%\esx-mod.log` quale hook Ã¨ stato
colpito: se una build futura di Windows usa un quarto ingresso, il log lo
mostra e basta aggiungerlo.

## Flusso di refresh

1. Il mod scrive `request.txt`.
2. Se la finestra Ã¨ giÃ  sulla cartella virtuale â†’ `IShellView::Refresh()`.
   Altrimenti â†’ `IShellBrowser::BrowseObject(pidl, SBSP_SAMEBROWSER)`.
   Il PIDL viene da `SHParseDisplayName("shell:::{GUID}")` e, se il parsing
   fallisce, da `CoCreateInstance` + `IPersistFolder2::GetCurFolder` sulla DLL.
3. `Refresh` fa ri-enumerare DefView.
4. `EnumObjects` legge `request.txt`; se `(seq, query, folder, max)` non Ã¨
   cambiato usa la cache, altrimenti interroga Everything.
5. `MessageSFVCB` gestisce conteggio e modalitÃ  della vista e lascia a DefView
   il comportamento predefinito per il resto (incluso il re-enumero).

## Threading e lifetime

- Il NSE Ã¨ in-proc, apartment-threaded (`ThreadingModel=Apartment`).
- `IShellFolder` puÃ² essere chiamato da piÃ¹ thread: la cache Ã¨ protetta da
  `std::mutex`; l'IPC Ã¨ serializzato dal worker.
- Il thread chiamante non attende mai piÃ¹ di ~700 ms per pagina; il worker non
  attende mai piÃ¹ di 2 s Everything.
- `g_cRefDll` conta oggetti, `LockServer` **e** la vita del worker.
- Nessuna eccezione C++ deve attraversare il confine COM: le API chiamate non
  lanciano; le allocazioni usano `new (std::nothrow)`.

## Rischi noti

1. **Contratto shell**: il caso piÃ¹ delicato Ã¨ la combinazione
   root+item da parte di DefView. Se qualche build di Windows passa PIDL in
   forme inattese, `GetPidlPath` va esteso (log + test).
2. **Tab di Windows 11**: `IShellWindows` puÃ² restituire piÃ¹ browser per la
   stessa finestra; il matching usa visibilitÃ  + focus, non Ã¨ deterministico al
   100%.
3. **File inesistenti**: Everything puÃ² restituire path cancellati. Il NSE li
   mostra comunque (nessuno `stat` per voce, per non rallentare); Explorer li
   segnala come mancanti.
4. **Crash del NSE = crash di Explorer**: niente `try/catch` attorno a codice
   sconosciuto, nessun hook dentro il NSE, nessuna chiamata a `Everything64.dll`.
5. **UIPI**: se Everything gira elevato, un processo non elevato non puÃ²
   inviargli `WM_COPYDATA`. Il pannello lo rileva (finestra presente ma nessuna
   risposta a `WM_USER`) e chiede di disattivare l'elevazione.
6. **Versione del protocollo**: `QUERY2` esiste da Everything 1.4.1. Versioni
   piÃ¹ vecchie non sono supportate.

## Stato dei test

Verificato con `tests/run-mingw.ps1` (MinGW, fuori processo, nessun Explorer
coinvolto):

- `*.cpp` nel repository â†’ file reali con size e date corrette;
- `C:\Windows` + `*.exe` â†’ 50 file (prima del paging: 0);
- `IContextMenu`/`IContextMenu3` e `IDataObject` su un risultato â†’ `S_OK`;
- `CF_HDROP` e `Shell ID List Array` â†’ `S_OK`, con il path giusto nel `DROPFILES`;
- `GetDetailsEx` â†’ `PKEY_ItemTypeText` = "File di origine C++", `PKEY_Size` = byte;
- la mod compila con `-Wall -Wextra` e la sua versione coincide con `nse/version.h`.

Verificato dal vivo in `explorer.exe` (Windows 11 x64, Everything 1.5.0.1423):

- `install.ps1` â†’ DLL caricata da
  `%LOCALAPPDATA%\EverythingExplorerSearch\nse\<versione>\EverythingSearchNse.dll`;
- `shell::{GUID}` apre una finestra la cui vista contiene i file reali del
  repository (letti con `Shell.Application`), senza crash nÃ© eventi WER;
- `IFolderView2::GetViewModeAndIconSize` â†’ `8` (`FVM_CONTENT`);
- `IFolderView2::InvokeVerbOnSelection(NULL)` (equivalente del doppio clic) â†’
  il file viene aperto dal programma associato;
- `IShellBrowser::BrowseObject` su una finestra esistente naviga **in place**
  (nessuna seconda finestra), anche partendo da "Questo PC";
- il titolo osservato Ã¨ il nome semplice **"Everything Search"**.

Bug trovati e corretti in questo test:

1. **`GetDetailsOf` fuori range**: rispondeva `S_FALSE` (successo) per le
   intestazioni di colonna oltre le 4 disponibili, quindi DefView continuava a
   incrementare `iColumn` all'infinito (osservate 275.000 chiamate in pochi
   secondi). Il contratto Ã¨ `E_INVALIDARG`; vale sia per i titoli delle colonne
   sia per le righe.
2. **`explorer.exe shell:::{GUID}`** non apre la cartella e ripiega su
   "Documenti": serve `SHParseDisplayName` + `ShellExecuteEx` con
   `SEE_MASK_INVOKEIDLIST|SEE_MASK_IDLIST`, oppure `BrowseObject` dall'interno.
3. `LocationURL` Ã¨ vuoto per una namespace extension: per capire se la finestra
   mostra giÃ  la cartella, `NavigateWindow` confronta
   `IPersist::GetClassID` della vista attiva con il nostro CLSID e solo allora
   usa `IShellView::Refresh()`.
4. **Menu contestuale**: costruirlo con `SHCreateDefaultContextMenu` +
   `DEFCONTEXTMENU(psf=questo)`, o con un wrapper che restituiva una sola voce
   "Open" per `CMF_DEFAULTONLY`, faceva crashare `SHELL32.dll` al clic destro.
   La shell reale, misurata con `tests/context_menu_probe.cpp`, **ignora**
   `CMF_DEFAULTONLY` e restituisce sempre il menu completo: il contratto giusto
   Ã¨ delegare a `IShellItemArray::BindToHandler(BHID_SFUIObject)`, senza wrapper.
5. **Data object vuoto**: `SHCreateDataObject(nullptr, ...)` non esponeva nessun
   formato; ora si usa `SHCreateShellItemArrayFromIDLists` +
   `BHID_DataObject`.
6. **PIDL iniziale**: `SHParseDisplayName("shell:::{GUID}")` puÃ² fallire con
   `0x80070003` subito dopo il riavvio di Explorer mentre `CoCreateInstance`
   riesce; la mod ripiega su `IPersistFolder2::GetCurFolder` della DLL.

## Roadmap

- **v1.1** â€“ input senza popup: `ParseDisplayName` accetta
  `::{GUID}\<query>` cosÃ¬ la barra degli indirizzi diventa il campo di ricerca.
- **v1.2** â€“ aggiornamento live con debounce (Refresh mirato, selezione
  preservata), `IncludeFolders=on`.
- **v1.3** (fatto) â€“ `MapColumnToSCID` + `GetDetailsEx` per la vista Contenuto,
  `SFVM_DEFVIEWMODE = FVM_CONTENT`.
- **v1.4** â€“ localizzazione delle intestazioni (`GetDetailsOf` in inglese),
  anteprime (thumbnail) per immagini/video.
- **v2** â€“ `BindToObject` per navigare nelle cartelle risultato; `IShellFolder2`
  completo; virtualizzazione oltre 5.000 risultati.
- **Distribuzione** â€“ release GitHub con DLL versionata + SHA256, firma del
  binario, draft della mod su windhawk.net.

## Checklist di test

1. `tests/run-mingw.ps1`: la catena NSE â†’ IPC â†’ Everything funziona? âœ…
2. `esx.ps1`: la cartella virtuale si apre e mostra i file? âœ…
3. Menu contestuale su un risultato: "Apri", "Apri percorso file", proprietÃ . âœ…
   (probe; da riconfermare a mano dopo il fix del crash)
4. Drag & drop di un risultato su un'altra app. â³ (formati verificati, da
   provare col mouse)
5. Doppio clic su un file (default verb) e su una cartella (atteso: nessun
   effetto in v1). âœ… file; cartelle non incluse in v1.
6. Cartella con accenti/spazi; query con accenti; query vuota.
7. Everything chiuso â†’ messaggio di errore, nessun crash di Explorer.
8. Due finestre, una scoped su `C:\Windows`, una su `D:\`: nessuna interferenza.
9. `Ctrl+F` con tab multiple, con una finestra non attiva, con un dialog aperto.
10. Disinstallazione a caldo: `DllCanUnloadNow` e rimozione chiavi.
