# Guide Developpeur Embarque (Debutant C)

Ce document explique le firmware embarque de ce projet de zero, pour une personne qui code mais ne connait pas encore bien le C ni l'embarque.

Objectif:
- comprendre l'architecture globale
- comprendre le role de chaque fichier embarque
- comprendre le protocole UWB DS-TWR
- savoir compiler, flasher, verifier et depanner
- savoir modifier le code sans casser le systeme

---

## 1) Vue d'ensemble en 60 secondes

Ce projet fait du ranging UWB entre deux cartes:
- initiator: declenche la mesure
- responder: recoit, repond, calcule la distance

Le protocole utilise est DS-TWR (Double-Sided Two-Way Ranging) avec 3 messages:
- POLL (initiator -> responder)
- RESPONSE (responder -> initiator)
- FINAL (initiator -> responder)

Le responder calcule la distance et l'envoie en CSV sur UART:
- format: ms,sample,dist,iax,iay,iaz,rax,ray,raz
- destination: app Android OTG (USB serie)

---

## 2) Structure reelle du firmware

Dossier principal du firmware:
- src/main.c
- src/common/ranging.h
- src/common/ranging.c
- src/initiator/main_initiator.c
- src/responder/main_responder.c
- src/accel/accel.h
- src/accel/accel.c
- src/uart/uart_log.h
- src/uart/uart_log.c
- src/platform/deca_spi_dwm3001cdk.c
- src/platform/port_dwm3001cdk.c
- src/board/custom_board.h
- CMakeLists.txt
- CMakePresets.json

Le SDK Qorvo est vendorise dans vendor/sdk.
Tu utilises donc le code SDK sans le modifier directement.

---

## 3) Mini base C pour lire ce projet

Si tu viens d'un langage haut niveau, retiens surtout ces points:

1. Le point d'entree est main()
- ici: src/main.c
- ce fichier initialise la carte puis appelle le role (initiator ou responder)

2. Les .h sont des contrats, les .c sont l'implementation
- .h: declarations (types, prototypes, constantes)
- .c: code executable

3. Les macros #define sont des constantes compile-time
- exemple: RNG_DELAY_MS, TX_ANT_DLY
- elles controlent timing et comportement

4. Les pointeurs existent partout
- un tableau de bytes uint8_t[] est souvent manipule via pointeur
- les messages UWB sont des buffers d'octets

5. Beaucoup de code embarque est en boucle infinie
- while (1) est normal
- on fait un cycle de mesure en continu

6. Registre materiel = champ memoire mappe
- exemple: NRF_UARTE0->TASKS_STARTTX = 1
- tu pilotes le peripherique en ecrivant/lisant des registres

---

## 4) Build system: qui compile quoi

### 4.1 CMakePresets

Le fichier CMakePresets.json propose 4 builds:
- initiator_debug
- initiator_release
- responder_debug
- responder_release

La variable cle est UWB_ROLE:
- initiator ou responder

### 4.2 CMakeLists principal

CMakeLists.txt fait 3 choses majeures:

1. branche le SDK vendorise
- QORVO_SDK_PATH = vendor/sdk
- toolchain ARM fournie par le SDK

2. construit les libs SDK necessaires
- driver UWB
- libs platform (rtt, qio, qosal, chelpers)
- startup nRF52833

3. ajoute ton code projet selon le role
- initiator: src/initiator/main_initiator.c + accel
- responder: src/responder/main_responder.c + accel + uart

Important:
- main.c est toujours compile
- UWB_ROLE choisit la fonction de ranging appelee

---

## 5) Boot sequence exacte

### 5.1 src/main.c

Au demarrage:
1. qio_init() pour RTT/printf debug
2. bsp_board_init() pour LEDs/boutons
3. gpio_init() pour GPIOTE
4. nrf52840_dk_spi_init() pour interface DW3000
5. dw_irq_init() pour interruptions DW3000
6. nrf_delay_ms(2)
7. appel du role:
   - ds_twr_initiator_custom() si build initiator
   - ds_twr_responder_custom() si build responder

Le choix est compile-time via:
- UWB_ROLE_INITIATOR
- UWB_ROLE_RESPONDER

---

## 6) Protocole radio DS-TWR de ce projet

### 6.1 Les 6 timestamps

Notation:
- t1: poll_tx (initiator envoie POLL)
- t2: poll_rx (responder recoit POLL)
- t3: resp_tx (responder envoie RESPONSE)
- t4: resp_rx (initiator recoit RESPONSE)
- t5: final_tx (initiator envoie FINAL)
- t6: final_rx (responder recoit FINAL)

Le responder connait:
- localement: t2, t3, t6
- via FINAL: t1, t4, t5

### 6.2 Formule

Ra = t4 - t1
Rb = t6 - t3
Da = t5 - t4
Db = t3 - t2

ToF = (Ra*Rb - Da*Db) / (Ra + Rb + Da + Db)
distance = ToF * SPEED_OF_LIGHT

Le code applique cette formule dans src/responder/main_responder.c.

### 6.3 Trames et offsets

Constantes dans src/common/ranging.h:
- FUNC_CODE_POLL = 0x21
- FUNC_CODE_RESPONSE = 0x10
- FUNC_CODE_FINAL = 0x23
- offsets accel dans POLL: 10/12/14
- offsets timestamps dans FINAL: 10/14/18

Ce sont juste des index de byte dans des tableaux uint8_t[].

---

## 7) Initiator: detail ligne de vie

Fichier: src/initiator/main_initiator.c

### 7.1 Initialisation

L'init fait:
1. reset/probe/init DW3000
2. dwt_configure() avec config SDK
3. configuration TX RF selon canal
4. antenna delays (TX_ANT_DLY / RX_ANT_DLY)
5. timeouts et delais RX/TX
6. LNA/PA + LEDs debug
7. init accelerometre LIS2DH12

### 7.2 Boucle de mesure

Cycle principal:
1. lire accelerometre (ou tenter recovery si KO)
2. encoder accel X/Y/Z dans tx_poll_msg
3. envoyer POLL
4. attendre RESPONSE
5. si RESPONSE valide:
   - lire poll_tx_ts (t1) et resp_rx_ts (t4)
   - programmer FINAL en delayed TX
   - calculer final_tx_ts (t5)
   - encoder t1/t4/t5 dans FINAL
   - envoyer FINAL
6. sleep RNG_DELAY_MS

Notes importantes:
- pas de gros logs par echantillon dans hot path
- si delayed TX rate: cycle skip, pas de blocage

---

## 8) Responder: detail ligne de vie

Fichier: src/responder/main_responder.c

### 8.1 Initialisation

L'init fait:
1. reset/probe/init DW3000
2. configuration UWB
3. antenna delays
4. LNA/PA + LEDs
5. init accelerometre local
6. init UART (uart_log_init)
7. start RTC2 pour timestamp ms local
8. emission header CSV

### 8.2 Boucle de mesure

Cycle principal:
1. activer RX pour attendre POLL
2. si POLL valide:
   - extraire accel initiator du POLL
   - sauvegarder t2 (poll_rx_ts)
   - programmer TX RESPONSE en delayed
   - activer attente du FINAL
3. si FINAL valide:
   - lire t3 et t6 localement
   - decoder t1, t4, t5 depuis FINAL
   - calculer distance DS-TWR
   - lire accel local
   - formatter CSV
   - envoyer via UART

Comportement erreur:
- timeout RX ou erreur radio: clear status et on repart
- delayed TX impossible: skip cycle

---

## 9) Accelerometre LIS2DH12

Fichiers:
- src/accel/accel.h
- src/accel/accel.c

Points cle:
- communication I2C via TWIM0 en acces registre direct
- test WHO_AM_I = 0x33
- essai adresses 0x19 puis 0x18
- config capteur en 100 Hz, mode HR, +/-2g
- lecture burst 6 bytes (X/Y/Z)
- conversion en mg (shift >>4)

Pourquoi c'est robuste:
- timeouts TWI pour eviter blocage
- retry init accelerometre cote initiator si capteur indisponible

---

## 10) UART: pourquoi ce module existe

Fichiers:
- src/uart/uart_log.h
- src/uart/uart_log.c

Role:
- sortir les lignes CSV vers Android via USB serial

Choix techniques importants:
- UARTE0 a 115200 bauds
- envoi par chunks
- timeout de TX pour ne jamais bloquer la boucle de ranging
- copie en buffer RAM avant TX (EasyDMA lit en RAM)

Cette partie est critique pour la stabilite du runtime.

---

## 11) Couche platform (adaptation DWM3001CDK)

Fichiers:
- src/platform/deca_spi_dwm3001cdk.c
- src/platform/port_dwm3001cdk.c
- src/board/custom_board.h

Ce code adapte le SDK a la carte cible:
- mapping GPIO reels (SPI, IRQ, RESET, UART)
- implementation SPI unique (pas de 2e DW3000)
- gestion IRQ DW3000
- reset/wakeup

Si un jour la carte change, c'est ce bloc qu'il faut revisiter en premier.

---

## 12) Donnees serie: contrat avec Android

Header:
- # ms,sample,dist,iax,iay,iaz,rax,ray,raz

Colonnes:
- ms: temps local responder (RTC2)
- sample: compteur de mesures
- dist: distance en metres
- iax/iay/iaz: accel initiator (mg)
- rax/ray/raz: accel responder (mg)

Exemple:
- 1203,57,2.34,-12,1018,23,-5,1002,19

Si tu modifies l'ordre des colonnes, il faut aussi ajuster le parseur Android.

---

## 13) Commandes utiles (build, flash, verification)

### 13.1 Build

Initiator debug:
- cmake --preset=initiator_debug
- cmake --build --preset initiator_debug

Responder debug:
- cmake --preset=responder_debug
- cmake --build --preset responder_debug

### 13.2 Flash (exemple nrfjprog)

Initiator:
- nrfjprog --program build/initiator_debug/uwb_initiator.hex --chiperase --verify
- nrfjprog --reset

Responder:
- nrfjprog --program build/responder_debug/uwb_responder.hex --chiperase --verify
- nrfjprog --reset

Avec deux sondes, ajoute --snr <serial> pour cibler la bonne carte.

### 13.3 Verification rapide

1. la LED init clignote au boot
2. la sortie UART du responder contient le header CSV
3. les lignes CSV defilent en continu
4. dist varie quand on bouge les cartes

---

## 14) Guide debug pour debutant

### 14.1 Aucun output UART

Checklist:
1. verifier que le responder est bien flashe
2. verifier cable USB/port modem
3. verifier uart_log_init appele
4. verifier que la boucle recoit des POLL

### 14.2 Distance figee ou absurde

Checklist:
1. verifier antennes et orientation
2. verifier antenna delays TX_ANT_DLY/RX_ANT_DLY
3. verifier timeouts trop agressifs
4. verifier que POLL/FINAL sont bien valides

### 14.3 Ranging qui saute

Checklist:
1. pas de printf lourds dans hot path
2. UART non bloquante (timeouts actifs)
3. pas de traitement lent dans boucle
4. verifier alimentation stable des cartes

---

## 15) Comment modifier le code sans te perdre

Methode conseillee:

1. modifier une seule chose a la fois
2. rebuild role concerne
3. reflasher
4. verifier CSV sur 1 a 2 minutes
5. commit petit et clair

Points sensibles:
- timings dans src/common/ranging.h
- sequence TX/RX dans initiator/responder
- code UART dans src/uart/uart_log.c
- adaptation pins dans src/board/custom_board.h

---

## 16) Lexique rapide

- UWB: Ultra Wideband
- DS-TWR: Double-Sided Two-Way Ranging
- ToF: Time of Flight
- DW3000: puce radio UWB Qorvo
- nRF52833: microcontroleur ARM
- ISR: Interrupt Service Routine
- SPI: bus serie pour parler au DW3000
- I2C/TWIM: bus serie pour le capteur accel
- UART/UARTE: sortie serie vers PC/Android

---

## 17) Carte mentale finale

En une phrase:

Le firmware initiator envoie des trames UWB synchronisees, le responder complete la sequence DS-TWR pour calculer la distance, ajoute les donnees accel des deux cotes, puis publie le resultat en CSV via UART pour l'application Android.

Si tu maitrises ce flux bout en bout, tu maitrises deja l'essentiel du projet embarque.

---

## 18) Explorateur CSV local (Mac)

Tu as maintenant un outil interactif pour explorer les CSV exportes:

- Script: [tools/csv_explorer_app.py](tools/csv_explorer_app.py)
- Dependances: [tools/requirements-csv-explorer.txt](tools/requirements-csv-explorer.txt)

### Installation

Depuis la racine du repo:

- pip install -r tools/requirements-csv-explorer.txt

### Lancement

- streamlit run tools/csv_explorer_app.py

### Fonctions

- timeline distance (raw + filtree si disponible)
- stats (moyenne, ecart-type, duree)
- superposition distance sur:
   - accel initiator
   - accel responder
   - gyro telephone
- trace GPS coloree par distance
- downsample et rolling mean pour lisser/accelerer l'affichage
