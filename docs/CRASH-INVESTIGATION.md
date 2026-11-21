# Indagine sul crash di Explorer

## Causa (accertata)

Il crash-loop era causato dalla mod Windhawk **`taskbar-fluent-media-player`
1.7.0**, non dal nostro spike e non dal metodo di riavvio.

L'utente ha fatto il bisect decisivo: disattivando tutte le mod e tenendo attiva
solo `taskbar-fluent-media-player`, il crash-loop compariva; disattivandola e
riattivando le altre, non compariva più.

## Firma

- `0xC0000005`, lettura da indirizzo 0 (parametri `0x0, 0x0`)
- sempre in `Windows.UI.Xaml.dll+0x2E4126` (offset `0x29D706` su una build di
  Windows precedente)
- nessun frame del nostro codice nello stack
- dump 13:37–13:40: `spike=absent`, quindi la stessa firma si presenta con la
  nostra mod non mappata

## Errori metodologici da non ripetere

1. Ho attribuito il crash allo spike perché i dump coincidevano con il suo
   riavvio. In realtà il riavvio ricaricava **tutte** le mod, incluso il colpevole.
2. Il presunto "controllo con mod disattivata" era invalido: le altre mod,
   `taskbar-fluent-media-player` compresa, erano ancora attive — infatti gli
   stessi dump sono ricomparsi (13:26:19, 13:26:32) con lo spike disattivato.
3. Il gate di 20 s sembrava risolvere il problema: falso positivo, la finestra
   di osservazione finiva prima del crash e il loop si fermava da solo dopo
   qualche riavvio.
4. Il riavvio forzato (`Stop-Process -Force`) era solo l'innesco che faceva
   ricaricare le mod, non la causa.

## Cosa non era la causa

- island XAML / `DesktopWindowXamlSource` creata nello spike;
- XAML Diagnostics e hiding dell'`AutoSuggestBox` nativo;
- apartment COM, message pump, hook tastiera `WH_KEYBOARD_LL`;
- import di `CoreMessaging.dll` o bootstrap WinAppSDK.

Tutte queste varianti sono state eseguite: il crash compariva e spariva in base
alla presenza di `taskbar-fluent-media-player`, non al codice della nostra mod.

## Stato attuale

- `taskbar-fluent-media-player`: **disattivata** (causa rimossa);
- spike XAML attivo con `kStartupSettleMs = 0`, retry 250 ms → sostituzione
  visibile in 1,1–1,6 s;
- dal riavvio delle 13:41:25 nessun dump nuovo;
- kill-switch di emergenza ancora presente in `Wh_ModInit`: conta gli avvii
  della shell (processi che partono senza taskbar) e disattiva la mod al
  secondo avvio in 120 s. **TEMPORANEO**: va rimosso quando la mod sarà quasi
  finita; fino ad allora può dare falsi positivi se si riavvia Explorer due
  volte in due minuti — in quel caso basta rimuovere
  `%LOCALAPPDATA%\EverythingExplorerSearch\spike-starts.txt`.

## Regole per i test futuri

1. Quando si cerca la causa di un crash, isolare **una mod alla volta**, non
   limitarsi a disattivare la propria.
2. Non considerare un dump come prova di colpevolezza solo perché il processo
   crashato ha la nostra DLL mappata: controllare sempre `spike=absent` nei dump
   di controllo.
3. Preferire finestre `/separate` o `CloseMainWindow` ai kill forzati durante
   lo sviluppo, per non riavviare inutilmente tutte le mod.

## Fase 1 — observer passivo (stato attuale)

`kObserverOnly = true` in `spike.cpp`: niente `HitTest`, niente modifiche al
visual tree, niente island, niente sostituzione. Il mod:

- apre una connessione Diagnostics per processo e registra gli eventi
  `OnVisualTreeChange` con `parent`, `child`, `ChildIndex`, handle, tipo, nome,
  PID e thread;
- deduplica con chiave `PID + watcher/connection + InstanceHandle`;
- inventaria le finestre già aperte via UIA (`UIA_DISCOVERED`), senza inventare
  handle XAML, parent o indici;
- rimuove le voci sugli eventi `remove` e quando la finestra UIA scompare.

Verificato: 10 finestre aperte rapidamente, 9 processi inventariati via UIA,
zero duplicati, zero mutazioni XAML, zero dump. Chiusura: eventi `remove` XAML e
UIA corretti, registry svuotato.

Limite noto: la copertura `XAML_MAPPED` resta parziale (1–4 finestre su 10)
perché `AdviseVisualTreeChange` non ripropa gli elementi creati prima della
registrazione. La copertura completa è garantita solo da `UIA_DISCOVERED`.
Non è stato usato `HitTest`: tre dump con `Microsoft.UI.Xaml+0x7D8087` avevano
mostrato che l'API non è sicura in questo scenario multi-finestra.

## Doppio Release dello stream COM (12:19, risolto)

Dump `explorer.exe.14756.dmp`: `0xC0000005` con RIP `0x52CD5E70`, cioè
esecuzione da un indirizzo heap, sul thread `executor` della nostra mod. Lo stack
mostrava `local@esx-xaml-spike.dll+0x1D997` (distruttore `com_ptr`) chiamato da
`+0x1786D` nel thread proc. Causa: `CoGetInterfaceAndReleaseStream` rilascia già
lo stream; il codice faceva poi `m_serviceStream = nullptr`, un secondo
`Release` su un oggetto già liberato → vtable nell'heap → AV in esecuzione.
Fix: `m_serviceStream.detach()` subito dopo la chiamata. Nessun dump nuovo dopo
la correzione (verificato con tre finestre e cicli disattiva/riattiva).

## Attesa di 80 s in "Uninitializing..." (risolta)

Windhawk 1.7.3 chiama `ThreadsCallStackWaitForRegions(regions, count, 200, 400)`
dopo `Wh_ModUninit`: finché un thread ha uno stack contenente la DLL della mod,
l'uninit resta visibile fino a 200 × 400 ms = 80 s. I thread creati dalla mod
(`executor`, `advise`) sopravvivevano perché XAML può mantenere reference ai
watcher anche dopo `Unadvise`. Fix: `StopThreads()` in `Detach()`/`Abandon()` e
`StopAllXamlDiagnostics()` in `Wh_ModUninit`, con join espliciti (2 s ciascuno).
Log: `WATCHER executor join ... wait=0`, `coordinator join wait=0`, coda svuotata
in meno di un secondo.
