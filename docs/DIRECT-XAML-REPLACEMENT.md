# Direct XAML Replacement (mod ufficiale 0.5)

## Stato

Implementazione ufficiale in `mod/everything-explorer-search.wh.cpp` (ID Windhawk
`local@everything-explorer-search`). Sostituzione diretta del controllo di ricerca
nativo di Explorer, senza overlay Win32 e senza `DesktopWindowXamlSource`.
Verificata su finestre `/separate`, `Ctrl+N` e finestre aperte dalla taskbar:

- layout copiato dal controllo nativo (`Grid.Row/Column`, `RowSpan/ColumnSpan`,
  `Margin`, allineamenti, vincoli dimensionali);
- posizione corretta a destra della barra degli indirizzi e adattamento al resize;
- digitazione, `Ctrl+F`/`Ctrl+E`/`F3`, Backspace, Esc e Invio funzionanti;
- Invio produce `request.txt` e navigazione nella cartella NSE con scope corretto;
- focus mantenuto dopo la navigazione;
- zero dump nelle prove finali.

## Architettura

1. **Observer passivo** (`kObserverOnly = true`): `OnVisualTreeChange` rileva
   `FileExplorerSearchBox` senza toccare il visual tree. Il vecchio percorso
   `CreateSession`/`DesktopWindowXamlSource` resta disattivato.
2. **Canale asincrono**: una message-only window (`HWND_MESSAGE`) viene creata
   nel costruttore del watcher, sul thread owner di `IVisualTreeService3`.
   Il callback fa solo `PostMessage`: nessuna chiamata COM rientrante.
3. **Transazione nel WndProc**: `GetCollectionCount` → snapshot → `AddChild` →
   verifica → `RemoveChild` del nativo → verifica, tutto sul thread owner.
   L'handle della collection viene ottenuto via `Panel.Children()` +
   `RegisterInstance`, non da `ParentChildRelation::Parent`.
4. **Layout**: letto dal `FrameworkElement` nativo sul dispatcher XAML e copiato
   sulla nostra `TextBox` con i setter WinRT (non valori pixel fissi).
5. **Input**: la `TextBox` ha handler `GotFocus`/`LostFocus`/`KeyDown`. Il low-level
   hook inoltra solo le scorciatoie al canale, che le accoda al dispatcher XAML.
6. **Ciclo di vita**: `kAutoRollback = false`. Il ripristino è esplicito su
   `Wh_ModUninit`, errore di inizializzazione o chiusura della finestra:
   `GetIInspectableFromHandle` prima della rimozione, `RegisterInstance` su
   quell'oggetto per ottenere un handle fresco, `AddChild` all'indice originale.

## Stato attuale dei flag

- `kObserverOnly = true` — non attivare il vecchio overlay.
- `kReplaceNative = true` — rimozione del controllo nativo dopo l'inserimento.
- `kAutoRollback = false` — sostituzione persistente.
- `kCallbackProbeOnly = true` — probe in sola lettura.

## Latenza di sostituzione

La correlazione e lo swap non devono stare dietro al timer di log da 2 s: il
coordinator li esegue a ogni tick (~16 ms). Inoltre la correlazione usa il
thread del callback XAML: se esiste **una sola** finestra `CabinetWClass`
visibile con lo stesso `PID + thread`, il frame è inequivocabile e si può
sostituire senza attendere l'UIA `FindFirst(Descendants)` (che può bloccare il
coordinator per centinaia di ms). In caso di ambiguità si resta sul percorso UIA
e la barra nativa non viene toccata. Le finestre già mappate via XAML non
vengono più scansionate con UIA.

Misurazioni (apertura di una nuova finestra, `C:\Users\<utente>\Downloads`):

| Fase | Prima | Dopo |
| --- | --- | --- |
| elemento XAML nativo visibile (`OBSERVE add`) | +0,88 s | +0,85 s |
| correlazione + inizio C3 | +2,70 s | +0,86 s |
| `C3 RESULT PASS` | +2,70 s | +1,04 s |

I ~0,85 s iniziali sono la costruzione della UI da parte di Explorer: non sono
eliminabili senza un overlay, che è escluso per progetto.

## Uninitialize

`Wh_ModUninit` ferma e joina esplicitamente i thread creati dalla mod
(`executor`, `advise`) invece di contare sui distruttori: Windhawk attende fino
a 80 s che nessuno stack contenga indirizzi della DLL. Con i join espliciti la
coda "Uninitializing..." si svuota in pochi secondi. Inoltre
`CoGetInterfaceAndReleaseStream` rilascia già lo stream: il successivo
`m_serviceStream = nullptr` era un doppio `Release` e causava un accesso
violation in esecuzione sul thread `executor`; ora si usa `detach()`.

## Limiti noti

- `SeparateProcess = 1` resta necessario: solo i processi `/factory` espongono
  XAML Diagnostics; la finestra shell non viene mai agganciata.
- Le cartelle virtuali (per esempio "Questo PC") non hanno un percorso
  filesystem: la scope resta vuota e la ricerca è globale, come nella vecchia
  versione con overlay. Le cartelle reali restano scoped.

## Nota di build

Il build MSVC deve esportare `InternalWhModPtr`: Windhawk lo cerca nel DLL per
iniettare il contesto usato da `Wh_GetIntSetting`. Senza quell'export il mod
crasha in `windhawk.dll+0xFD7F` appena legge le impostazioni. Il vecchio build
clang lo esportava automaticamente (`--export-all-symbols`).
