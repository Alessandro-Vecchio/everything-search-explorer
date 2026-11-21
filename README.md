# Everything Search for Explorer

Ricerca istantanea con [Everything](https://www.voidtools.com/) **dentro** le
finestre di Esplora file: si usa la casella di ricerca nativa di Explorer come
input, ma i risultati sono una vera cartella servita da una namespace extension
COM.

Due componenti:

- una **mod Windhawk** che legge il testo della casella di ricerca nativa, lo
  passa a Everything e installa/aggiorna l'altro pezzo;
- una **shell extension** (`EverythingSearchNse.dll`) che mostra i risultati
  come una cartella di Explorer.

Everything resta obbligatorio: Ã¨ il motore di ricerca, non viene reimplementato.

Stato attuale: **funziona dal vivo in Explorer** su Windows 11 x64. La mod e la
DLL sono complete; la catena Ã¨ verificata sia fuori processo
(`tests/run-mingw.ps1`) sia dentro `explorer.exe`: cartella virtuale in vista
**Contenuto** con tipo/dimensione/data, doppio clic che apre il file, drag & drop
con `CF_HDROP` valido. L'ultima versione verificata Ã¨ quella in
`nse/version.h`. Il build ufficiale MSVC non Ã¨ mai stato eseguito (su questa
macchina c'Ã¨ solo MinGW). Vedi *Limiti noti*.

## Come funziona

```
Explorer (Ctrl+F / casella di ricerca) - digiti e premi Invio
    |- mod Windhawk -- ISearchBoxInfo::GetText (in-proc)
                   `-- scrive request.txt --> naviga a shell:::{GUID}
                                       |
                           NSE in explorer.exe
                                       |  WM_COPYDATA (QUERY2)
                                       |  su un thread dedicato, con timeout
                                   Everything.exe
                                       |
                              risultati (file + cartelle)
                                       |
```

| Componente | Dove gira | Ruolo |
|---|---|---|
| `nse/` | in-proc in `explorer.exe` | `IShellFolder` che enumera i risultati come PIDL di file reali e interroga Everything |
| `mod/` | mod Windhawk | legge la search box nativa, naviga/aggiorna la vista, installa la DLL |
| Everything | processo separato | indice e motore di ricerca (prerequisito, dell'utente) |

Il vantaggio della namespace extension "proxy" Ã¨ che ogni voce Ã¨ un percorso
reale: icone, menu contestuali, drag & drop, proprietÃ  e "Apri con" sono quelli
nativi di Explorer, non reimplementati. La vista predefinita Ã¨ **Contenuto**
(`FVM_CONTENT`) e le colonne leggono le proprietÃ  del file vero
(`PKEY_ItemTypeText`, `PKEY_Size`, `PKEY_DateModified`).

L'estensione **non linka nÃ© carica `Everything64.dll`**: parla direttamente con
Everything via IPC documentato (`QUERY2` sopra `WM_COPYDATA`) da un thread
dedicato, con timeout espliciti. CosÃ¬ Everything lento o assente non blocca
Explorer, e non serve scaricare l'SDK per compilare.

## Come si usa

1. In una finestra di Explorer premi `Ctrl+F` (o clicca la casella di ricerca),
   digita la query e premi **Invio**.
2. La finestra passa alla cartella `Everything Search`, che mostra i risultati
   di Everything con nome, tipo, dimensione e data, **cartelle incluse**, senza
   limite di 500.
3. Tasto destro sullo sfondo dei risultati:
   - **Search optionsâ€¦** apre un piccolo pannello con ambito (cartella corrente
     / ovunque), inclusione delle cartelle e numero massimo di risultati (`0` =
     tutti); le modifiche si applicano subito alla vista;
   - **Close search** torna alla cartella da cui era partita la ricerca.

Questi due comandi sostituiscono *Opzioni di ricerca* e *Chiudi ricerca* della
ricerca nativa: la barra dei comandi della search box appartiene alla vista di
Windows Search e una namespace extension non puÃ² albergarla.

 La casella di ricerca nativa **non viene usata**: scriverci tramite UIA o tasti
 sintetici fa crashare `Windows.UI.Xaml.dll`, e il testo viene cancellato quando
 cambia la vista. La mod principale mantiene il proprio campo Win32 e il motore
 Everything; quando Ã¨ attivo lo spike XAML, un `TextBox` separato sostituisce
 visivamente il controllo nativo, che viene reso invisibile e non interattivo.
 Invio passa la query al campo Win32 della mod principale, senza inviare testo o
 tasti alla casella XAML originale.

 Lo spike in `xaml-spike/` individua l'`AutoSuggestBox` nativo tramite XAML
 Diagnostics e applica le modifiche sul dispatcher dell'elemento tramite
 `IXamlDiagnosticsTestHooks`. Crea poi un `DesktopWindowXamlSource` sullo stesso
 dispatcher XAML di Explorer. Se non riesce a disattivare il controllo nativo o a
 creare il sostituto, ripristina la casella originale. Il test dal vivo ha
 verificato la sostituzione visiva; il passaggio della query a Everything resta
 da verificare dal vivo. XAML Diagnostics non Ã¨ un'API supportata e puÃ² entrare
 in conflitto con altre mod o con aggiornamenti di Windows.

 Con il solo mod principale, `Ctrl+F` dÃ  il focus al suo campo Win32; Invio scrive
 la query in `request.txt` e naviga alla cartella risultati. Con lo spike attivo,
 Invio nel `TextBox` XAML usa lo stesso percorso di ricerca e Escape svuota il
 campo. Testo, caret, selezione e clipboard del campo Win32 restano gestiti da
 Windows.

Mentre si digita, la ricerca di Windows viene **bloccata**: la mod intercetta
le tre strade che Explorer usa per aprire la vista Windows Search â€”
`SHParseDisplayName("search-ms:...")` (riportando alla cartella di partenza),
`ShellExecuteExW`/`ShellExecuteW` con un moniker `search-ms:` e
`IShellBrowser::BrowseObject` verso la search folder `{04731B67-...}`. La vista
resta quindi sulla cartella fino a Invio. I navigatori della mod sono
esentati con un flag `thread_local`, quindi la cartella virtuale si apre
normalmente. Le build con `-Trace` registrano in `%TEMP%\esx-mod.log` quale
hook Ã¨ stato colpito, utile se una build futura di Windows apre la ricerca in
un modo nuovo.

La prima ricerca Ã¨ automaticamente limitata alla cartella che stavate
guardando; le ricerche successive dalla cartella risultati riutilizzano lo
stesso ambito. `maxResults=0` (predefinito) restituisce tutti i match fino al
tetto di sicurezza del componente (98.000); un valore positivo impone un limite
esplicito.

## Build

Build ufficiale (MSVC x64) + CMake/Ninja:

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
.\build.ps1                       # compila in dist\bin
.\package.ps1                     # dist\EverythingSearchNse-v<versione>.dll + SHA256
```

Build della mod Windhawk, con il clang incluso in Windhawk (nessun SDK da
installare):

```powershell
.\mod\build-mod.ps1               # dist\windhawk\local@everything-explorer-search_<ver>_<build>.dll
.\mod\build-mod.ps1 -Trace        # build di debug: Explorer scrive %TEMP%\esx-mod.log
```

Build e registrazione del prototipo XAML in Windhawk (richiede UAC e riavvia
Windhawk; per sicurezza resta disabilitato):

```powershell
.\xaml-spike\install-spike.ps1
```

L'attivazione esplicita con `-Enable` Ã¨ riservata a test dal vivo: il prototipo
usa XAML Diagnostics non supportato e una build precedente ha causato crash di
Explorer su questa macchina. Il test corrente ha confermato il rendering del
sostituto senza crash; mantenere lo spike disabilitato fuori dai test finchÃ© non
sono verificati anche Invio, ridimensionamento e chiusura della finestra.

Build di verifica senza MSVC (MinGW), con probe end-to-end:

```powershell
.\tests\run-mingw.ps1 -Folder C:\Windows -Query *.exe -Max 20
```

Il probe carica la DLL fuori processo, enumera la cartella virtuale e stampa
quello che Explorer vedrebbe; controlla anche la compilazione della mod con uno
stub dell'API Windhawk. Gli altri probe servono per i test dal vivo:

| Probe | Cosa verifica |
|---|---|
| `nse_probe.exe` | enumerazione, `GetDetailsOf`/`GetDetailsEx`, menu contestuale, `IDataObject` (`CF_HDROP`, Shell ID List) |
| `context_menu_probe.exe` | confronto del contratto del menu con quello della shell su un file vero |
| `navigate_probe.exe` | `BrowseObject` verso la cartella virtuale: stessa finestra o finestra nuova |
| `view_probe.exe` | vista reale: modalitÃ  Contenuto e azione predefinita (doppio clic) su un risultato |

## Installazione

### Dalla mod (percorso normale)

1. Installa Everything 1.4.1+ e lascialo in esecuzione, **senza** "Run as
   administrator" (altrimenti Explorer non puÃ² parlargli: usa il servizio
   Everything e disattiva l'elevazione).
2. Installa la mod da Windhawk e attivala.
3. Premi `Ctrl+Shift+F`: si apre il pannello di setup con lo stato di
   Everything e della shell extension.
4. Scarica la DLL dalla pagina delle release, poi premi **Install** (usa
   l'ultimo `EverythingSearchNse*.dll` in Download) oppure **Choose file**.

La mod copia la DLL in
`%LOCALAPPDATA%\EverythingExplorerSearch\nse\<versione>\` e scrive le chiavi
`HKCU` (`CLSID\{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}` + stato). Nessun admin,
nessun riavvio. La cartella Ã¨ versionata perchÃ© una DLL giÃ  caricata da Explorer
non si puÃ² sovrascrivere: un aggiornamento finisce in una cartella nuova.

`Ctrl+Shift+F` apre il pannello di setup in qualsiasi momento.

### A mano (sviluppo)

```powershell
.\install.ps1 -RestartExplorer    # usa dist\bin, HKCU
.\uninstall.ps1
```

### Mod da sorgente (sviluppo)

```powershell
.\install-mod.ps1     # compila, registra, riavvia Windhawk (chiede admin)
.\uninstall-mod.ps1   # rimuove mod, DLL e sorgente (chiede admin)
```

Stessa idea di `install.ps1`, ma per la mod: qui serve l'elevazione perchÃ©
Windhawk tiene mod e sorgenti in `C:\ProgramData\Windhawk` e la registrazione in
`HKLM`. Lo script compila il `.wh.cpp` con `mod\build-mod.ps1` (stesso clang e
stessa riga di comando dell'editor di Windhawk), copia la DLL in
`Engine\Mods\64`, allinea il sorgente in `ModsSource`, riscrive la chiave della
mod con `Disabled=0` e i valori predefiniti (`maxResults`, `scopeToFolder`),
poi riavvia Windhawk. Explorer non viene terminato: la build nuova entra in una
finestra successiva. `-SkipBuild` riusa l'ultima DLL in `dist\windhawk`,
`-NoRestart` lascia Windhawk spento.

### Prima di pubblicare

Sostituisci `ESX_RELEASES_URL` in `mod/everything-explorer-search.wh.cpp` (ora
contiene `CHANGE-ME`) con la pagina release del tuo repository: il pulsante
**Download** apre quel link, la mod non scarica nulla da sola.

## Prova senza il mod

```powershell
.\tools\esx.ps1 -Query "fattura" -Folder "C:\Users\<utente>\Documenti"
```

Scrive `request.txt` e apre una nuova finestra su
`shell:::{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}`.

## Limiti noti (v1)

- Le cartelle compaiono nei risultati, ma la vista Ã¨ piatta e read-only:
  `BindToObject` non Ã¨ implementato, quindi le cartelle si aprono con il loro
  verbo predefinito (non navigano dentro la lista piatta).
- Ogni ricerca ri-enumera la vista e perde la selezione. La ricerca non si
  aggiorna mentre si digita: serve premere Invio.
- Il tetto di sicurezza Ã¨ ~98.000 risultati: oltre non ha senso enumerare in una
  vista Explorer. Per liste molto grandi la vista puÃ² comunque diventare lenta.
- **Solo x64** (Explorer su Windows 11 Ã¨ x64) e **solo Windows 11 testato**.
- La ricerca Ã¨ **scoped alla cartella corrente** per impostazione predefinita
  (modificabile dalla mod o con `esx.ps1 -Global`).
- Con le **tab** di Windows 11, l'individuazione della vista attiva Ã¨
  euristica (visibilitÃ  + focus).
- L'intestazione delle colonne Ã¨ in inglese (localizzazione da fare).
- Il risultato dipende dall'indice di Everything: file su volumi non indicizzati
  (o di rete senza folder indexing/ETP) non compaiono.
- **Aggiornamenti manuali**: nessun auto-update, per scelta. Se Everything non
  risponde entro i timeout la vista mostra un errore, non si blocca.
- Se Everything gira elevato, `explorer.exe` (non elevato) non puÃ² inviargli
  `WM_COPYDATA`: il pannello lo segnala e chiede di togliere l'elevazione.

## Struttura

```
shared/protocol.h        formato request.txt (mod â†” NSE)
shared/everything_ipc.h  definizioni minime del protocollo IPC QUERY2
nse/                     namespace extension + client IPC
mod/                     mod Windhawk (.wh.cpp, si compila in Windhawk)
mod/build-mod.ps1        compila la mod con il toolchain di Windhawk
install-mod.ps1          installa la mod in HKLM (chiede admin)
uninstall-mod.ps1        rimuove la mod
tests/                   probe NSE, IPC grezzo, menu/vista dal vivo, check mod
tools/esx.ps1            trigger di test senza mod
docs/DESIGN.md           dettagli di protocollo, PIDL, refresh, roadmap
```

## Prima di andare avanti

La parte fragile non Ã¨ Everything, Ã¨ il contratto shell. Prima di aggiungere
feature, valida con `ShellExView`/`Process Monitor` che Explorer non stia
chiamando `BindToObject`, `ParseDisplayName` o
`GetDisplayNameOf(SHGDN_FORPARSING)` in modi inattesi: Ã¨ lÃ¬ che si nascondono i
crash.
