# Contributing

Il progetto è uno **stato di cantiere**: contribuzioni sono benvenute, ma
l'obiettivo è tenere traccia delle prove, non fornire un prodotto finito.

Prima di aprire una PR:

1. Verifica che il comportamento sia riproducibile su Windows 11 x64 con
   `SeparateProcess=1` e Everything non elevato.
2. Indica sempre.Windows / Explorer / Everything / versioni mod e NSE usate.
3. Non inviare cambiamenti "di stile" su `mod/generated/`: è codice generato.
4. Se tocchi il contratto shell (`nse/`), prova anche `tests/run-mingw.ps1`.
5. Tieni le modifiche piccole e separa correzioni di bug da nuove feature.

Le aree più fragili e più utili da migliorare sono elencate in
`README.md` → *Limiti noti / problemi aperti*.

Non c'è un maintainer a tempo pieno: le PR possono restare aperte a lungo.
