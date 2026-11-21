# Everything Search for Explorer

> **⚠️ PROGETTO IN CORSO — NON È UNA VERSIONE FINITA.**
> Questo repository è uno stato di cantiere pubblicato per tenere traccia delle
> prove e raccogliere feedback. La mod manipola il visual tree XAML di
> `explorer.exe` tramite **XAML Diagnostics**, un'API non supportata: un
> aggiornamento di Windows può romperla in qualsiasi momento. Nessuna garanzia,
> nessun supporto, nessun auto-update. Usalo a tuo rischio, preferibilmente in una
> macchina di prova.
>
> Questo progetto **non è affiliato né supportato da voidtools**. "Everything" è
> un marchio di voidtools; qui viene usato solo come motore di ricerca esterno.
>
> [English](README.md)

Ricerca istantanea con [Everything](https://www.voidtools.com/) **dentro** le
finestre di Esplora file: la casella di ricerca nativa viene sostituita nel
visual tree XAML di Explorer, senza overlay e senza finestre aggiuntive. I
risultati sono una vera cartella servita da una namespace extension COM, quindi
icone, menu contestuali, drag & drop e proprietà restano quelli nativi.

## Componenti

| Componente | Dove gira | Ruolo |
|---|---|---|
| `mod/everything-explorer-search.wh.cpp` | mod Windhawk in `explorer.exe` | sostituisce la casella XAML, scrive la request, naviga la vista |
| `nse/EverythingSearchNse.dll` | in-proc in `explorer.exe` | `IShellFolder` che enumera i risultati come file reali |
| Everything | processo separato dell'utente | indice e motore di ricerca (prerequisito) |

L'NSE **non linka né carica `Everything64.dll`**: parla il protocollo IPC
documentato (`QUERY2` via `WM_COPYDATA`) da un thread dedicato con timeout, così
un Everything lento o assente non blocca Explorer.

```
Ctrl+F / casella XAML → Invio
        │
        ├─ mod: scrive request-<HWND>.txt  (%LOCALAPPDATA%\EverythingExplorerSearch)
        └─ mod: BrowseObject / Refresh su shell:::{7B39...4F60}
                        │
                 NSE in explorer.exe ──WM_COPYDATA/QUERY2──▶ Everything.exe
                        │
                 risultati = PIDL di file reali nella vista Explorer
```

## Come si usa

1. In una finestra di Explorer premi `Ctrl+F` (o `Ctrl+E` / `F3`) per dare il
   focus alla casella sostituita.
2. Digita la query e premi **Invio**: la vista passa alla cartella
   `Everything Search` con nome, tipo, dimensione e data (cartelle incluse).
3. Usa il pulsante **Opzioni di ricerca** nella barra dei comandi per filtrare.
4. **Indietro** torna alla cartella di partenza.

La mod mantiene uno stato separato per scheda: testo della casella, caret e
risultati di una scheda non vengono adottati da un'altra.

### Opzioni di ricerca

Tutte le voci scrivono modificatori Everything nel file di request e vengono
applicate alla query successiva; non compaiono nella casella di testo.

| Voce | Effetto |
|---|---|
| Tutte le sottocartelle | `path:"cartella"` (ricorsivo) |
| Cartella corrente | `parent:"cartella"` (solo figli diretti) |
| Data ultima modifica | `date-modified:today` / `7days` / `30days` |
| Tipologia | `doc:` / `image:` / `video:` / `audio:` |
| Dimensioni | `size:0` / `size:0..10kb` / `size:10kb..100kb` |
| File di sistema | `attributes:s` |
| Cartelle compresse (ZIP) | `zip:` |
| Contenuto file | `content:<query>` (può essere molto lento) |

`Cartella corrente` è disponibile solo quando esiste una cartella reale a cui
limitare la ricerca: dalle cartelle virtuali come "Questo PC" resta disabilitata.

## Requisiti

- Windows 11 x64 (unico ambiente provato), Explorer a processi separati:

  ```powershell
  Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' SeparateProcess 1 -Type DWord
  ```

  Serve perché XAML Diagnostics è disponibile solo nei processi `/factory`.
- Windhawk con la mod `local@everything-explorer-search` attiva.
- Everything 1.4.1+ in esecuzione **senza elevazione** (altrimenti `explorer.exe`
  non può inviargli `WM_COPYDATA`; usare il servizio Everything).
- La DLL della namespace extension installata (vedi sotto).

## Setup iniziale (da clone pulito)

1. **Strumenti** (una volta sola):

   ```powershell
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   winget install Windhawk.Windhawk
   winget install voidtools.Everything
   ```

   Servono anche il **Windows SDK** (per `cppwinrt.exe`) e i WinMD di WinUI nella
   cache NuGet. Se non ci sono già, popolali con un progetto temporaneo:

   ```powershell
   mkdir "$env:TEMP\esx-winmd"; cd "$env:TEMP\esx-winmd"
   dotnet new classlib -f net8.0
   dotnet add package Microsoft.WindowsAppSDK
   dotnet add package Microsoft.Web.WebView2
   dotnet add package Microsoft.Windows.SDK.NET.Ref
   dotnet restore
   ```

   `mod\regenerate-winrt.ps1` trova automaticamente i percorsi e genera gli
   header. Se manca qualche WinMD, lo script indica quale pacchetto installare.

2. **Configura Explorer** con `SeparateProcess=1` (comando in *Requisiti*), poi
   riavvia Explorer.

3. **Compila la NSE** (CMake + Ninja, già nei Build Tools):

   ```powershell
   .\build.ps1
   ```

   Produce `dist\bin\EverythingSearchNse.dll`.

4. **Registra la NSE**: `.\install.ps1 -RestartExplorer`.

5. **Compila la mod Windhawk**: `.\mod\build-mod.ps1`. Alla prima esecuzione
   genera da solo le intestazioni C++/WinRT e produce
   `dist\windhawk\local@everything-explorer-search_0.5.0_<build>.dll`.

6. **Installa la mod**: `.\install-mod.ps1` (chiede admin) oppure carica la DLL
   dal pannello di Windhawk.

7. **Verifica rapida**: `.\tools\esx.ps1 -Query "*.ps1" -Global` apre la
   cartella risultati senza passare dalla mod. Il probe end-to-end
   `.\tests\run-mingw.ps1` richiede MinGW in
   `$env:TEMP\mingw\mingw64\bin\g++.exe` (percorso passabile con `-Gcc`).

## Installazione

```powershell
.\build.ps1                    # NSE: compila dist\bin\EverythingSearchNse.dll
.\install.ps1                  # NSE: copia la DLL in %LOCALAPPDATA% e registra il CLSID HKCU
.\mod\build-mod.ps1            # mod: compila il .wh.cpp in dist\windhawk
.\install-mod.ps1              # mod: registra in HKLM, riavvia Windhawk (admin)
```

`install-mod.ps1` usa `mod\build-mod.ps1`, che produce
`dist\windhawk\local@everything-explorer-search_0.5.0_<build>.dll`. La cartella
della DLL NSE è versionata perché una DLL già caricata da Explorer non si può
sovrascrivere. Per rimuovere tutto: `.\uninstall-mod.ps1` e `.\uninstall.ps1`.

Dettagli di architettura e protocollo: `docs/DIRECT-XAML-REPLACEMENT.md` e
`docs/DESIGN.md` (quest'ultimo è in parte storico: per lo stato attuale prevale
il README e il codice).

## Build e test

```powershell
.\mod\build-mod.ps1                          # mod Windhawk (MSVC x64)
.\tests\run-mingw.ps1 -Folder C:\Windows -Query '*.exe' -Max 20   # NSE senza MSVC + probe end-to-end
```

Le intestazioni C++/WinRT **non sono nel repo**: `mod\regenerate-winrt.ps1` le
ricrea da Windows SDK e NuGet quando mancano (circa 30 MB, 254 file). Servono i
pacchetti `Microsoft.WindowsAppSDK.*`, `Microsoft.Web.WebView2` e
`Microsoft.Windows.SDK.NET.Ref` nella cache NuGet; per forzare la rigenerazione:

```powershell
.\mod\regenerate-winrt.ps1 -Force
```

Il log della mod va in `%TEMP%\everything-explorer-search.log`.

```powershell
.\tools\esx.ps1 -Query "fattura" -Folder "C:\Users\<utente>\Documenti"
```

`esx.ps1` scrive la request e apre la cartella virtuale senza passare dalla mod:
utile per provare la sola NSE.

## Limiti noti / problemi aperti

Questa è la parte importante: il progetto **non è pronto**.

- **Cartella corrente non è ancora affidabile**: se la seconda ricerca parte
  dentro la vista risultati, la cartella di origine può andare persa e il
  modificatore diventa `parent:""` (nessun risultato). Vedi il commento nel
  codice in `ApplySearchOptions` / handler `KeyDown`.
- Dopo una seconda ricerca con molti risultati, la nuova lista può comparire
  solo al termine della ri-enumerazione completa.
- La barra "Organizza" legacy può ricomparire in cartelle normali della stessa
  finestra dopo una ricerca.
- Il titolo della finestra/scheda resta `Everything Search` invece di
  `<query> - Risultati ricerca in ...` (Explorer non chiama `GetDisplayNameOf`
  sulla root).
- L'icona desktop `Everything Search` può ricomparire a intermittenza: sembra un
  artefatto della cache delle icone, non una vera cartella namespace.
- Serve `SeparateProcess=1`; con finestre non separate la barra nativa non viene
  toccata. XAML Diagnostics può entrare in conflitto con altre mod o con
  aggiornamenti di Windows.
- Solo x64 / Windows 11. Le cartelle virtuali non hanno percorso filesystem e
  vengono quindi cercate globalmente.
- Vista piatta e read-only: `BindToObject` non è implementato, quindi le cartelle
  nei risultati non navigano dentro la lista.
- Ogni ricerca ri-enumera e perde la selezione; non c'è aggiornamento live mentre
  si digita (serve Invio). Tetto di sicurezza ~393.000 risultati grezzi
  (2048 × 192 batch).
- Il risultato dipende dall'indice di Everything: volumi non indicizzati e
  condivisioni di rete senza folder indexing/ETP non compaiono.
- Nessun auto-update, nessuna firma digitale, nessun supporto.

## Struttura

```
shared/protocol.h        formato request (mod ↔ NSE), flag e filtri
shared/everything_ipc.h  definizioni minime del protocollo QUERY2
nse/                     namespace extension + client IPC
mod/                     mod Windhawk: sostituzione XAML della barra nativa
mod/regenerate-winrt.ps1 genera le intestazioni C++/WinRT (non committate)
tests/                   probe NSE, IPC grezzo, menu/vista dal vivo
tools/esx.ps1            trigger di test senza mod
docs/                    note di progetto e indagine sui crash
```

## Licenza

MIT — vedi `LICENSE`. Le intestazioni C++/WinRT generate e quelle di Windhawk
restano di rispettivi proprietari; i relativi avvisi sono nei file.
