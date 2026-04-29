# UWB Ranging Minimal — DS-TWR sans FiRa

Système de mesure de distance UWB entre deux modules Qorvo DW3000,
basé sur le protocole Double-Sided Two-Way Ranging (DS-TWR).

## Guide développeur détaillé

Pour une explication pas à pas du code embarqué (niveau débutant C), lire:

- [README_DEV.md](README_DEV.md)

## Architecture du projet

```
uwb-ranging/
├── README.md                  ← ce fichier
├── README_DEV.md              ← guide dev détaillé (débutant C)
├── CMakeLists.txt             ← build principal (pointe vers le SDK)
├── CMakePresets.json           ← presets de build (debug/release)
│
├── src/
│   ├── initiator/
│   │   └── main_initiator.c   ← firmware Module A (envoie Poll, calcule distance)
│   │
│   ├── responder/
│   │   └── main_responder.c   ← firmware Module B (répond au Poll)
│   │
│   └── common/
│       ├── ranging.h           ← types et constantes partagés
│       └── ranging.c           ← fonctions utilitaires partagées
```

## Comment ça fonctionne

### Protocole DS-TWR (3 messages)

```
Module A (Initiator)              Module B (Responder)
       |                                  |
  t1   |-------- POLL ------------------>| t2
       |                                  |
  t4   |<------- RESPONSE ---------------| t3
       |                                  |
  t5   |-------- FINAL ----------------->| t6
       |                                  |
       |  (A envoie t1,t4,t5 dans FINAL)  |
       |                                  |
       |  B calcule la distance avec      |
       |  les 6 timestamps (t1..t6)       |
```

### Formule DS-TWR

```
Ra = t4 - t1    (round-trip A)
Rb = t6 - t3    (round-trip B)
Da = t5 - t4    (délai traitement A)
Db = t3 - t2    (délai traitement B)

ToF = (Ra × Rb - Da × Db) / (Ra + Rb + Da + Db)
Distance = ToF × vitesse_lumière
```

**Avantage DS-TWR** : les erreurs de clock s'annulent grâce au double
aller-retour → pas besoin de correction clockOffset comme en SS-TWR.

## Dépendances

Ce projet utilise le SDK Qorvo **DW3_QM33_SDK_1** comme dépendance externe.
Le chemin vers le SDK est configuré dans `CMakeLists.txt` via la variable
`QORVO_SDK_PATH`.

### Prérequis

- CMake ≥ 3.20
- Ninja
- ARM GNU Toolchain 12.2 (`arm-none-eabi-gcc`)
- SDK Qorvo DW3_QM33_SDK_1

## Build

```bash
# Compiler l'initiator (Module A)
cmake --preset=initiator_debug
cmake --build --preset initiator_debug

# Compiler le responder (Module B)
cmake --preset=responder_debug
cmake --build --preset responder_debug
```

## Flash

```bash
# Via nrfjprog (Nordic)
nrfjprog --program build/initiator_debug/uwb_initiator.hex --chiperase --verify
nrfjprog --reset

# Ou via J-Link
JLinkExe -device NRF52840_XXAA -if SWD -speed 4000 -autoconnect 1
> loadfile build/initiator_debug/uwb_initiator.hex
> r
> g
```

## Output UART (debug)

À 115200 baud, format CSV :

```
# sample,distance_m,poll_tx,resp_rx,final_tx,poll_rx,resp_tx,final_rx
1,2.34,0x1A2B3C,0x4D5E6F,0x7A8B9C,0xAB1234,0xCD5678,0xEF9ABC
2,2.31,...
```
