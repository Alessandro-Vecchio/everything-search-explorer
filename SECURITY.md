# Security policy

Questo progetto inietta codice in `explorer.exe` e registra una shell extension
nell'utente corrente. Va trattato come software sperimentale, non come un
componente di sistema.

- **Non eseguire binari non fidati.** I DLL rilasciati non sono firmati. Se
  scarichi una build da terze parti, verificala sempre.
- **Non usare in ambienti produzione** o su macchine con dati sensibili senza
  aver prima provato disinstallazione e ripristino (`uninstall-mod.ps1`,
  `uninstall.ps1`).
- **Segnalazioni**: apri una issue con versione di Windows, versione di
  Everything, versione della mod/DLL e passi per riprodurre. Non includere log
  con percorsi o nomi di file riservati: i log della mod finiscono in
  `%TEMP%\everything-explorer-search.log`.
- Le vulnerabilità note più gravi sono l'uso di XAML Diagnostics (API non
  supportata) e la dipendenza dal contratto interno della shell di Explorer:
  non sono "bug di sicurezza" risolvibili entro lo scopo del progetto.

Il progetto non è affiliato a voidtools e non riceve supporto da quest'ultimo.
