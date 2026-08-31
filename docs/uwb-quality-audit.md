# Notice d'audit qualite UWB

Objectif : evaluer la stabilite, la repetabilite et les derives de la distance mesuree par le firmware UWB pure, a partir du flux CSV :

```text
ms,sample,dist,cppm,valid,dist_filt,dist_smooth,rx_fail
```

## 1. Ce que l'on cherche a mesurer

La precision relative repond a la question :

> A distance fixe, de combien la mesure UWB bouge-t-elle naturellement ?

Ce n'est pas encore l'exactitude absolue.

Exemple : si les boitiers sont reellement a 2.00 m et que le systeme mesure toujours 1.92 m +/- 0.03 m, alors :

- precision relative : bonne, car faible dispersion ;
- exactitude absolue : biais de -8 cm a calibrer.

## 2. Indicateurs principaux

### Ecart-type, sigma

C'est l'indicateur central.

Il mesure la dispersion autour de la moyenne :

```text
sigma faible = mesure stable
sigma eleve = mesure bruitee
```

A calculer sur `dist` brut, puis sur `dist_filt` et `dist_smooth`.

A distance fixe, on veut idealement :

- sigma brut faible ;
- sigma filtre encore plus faible ;
- pas de derive lente cachee.

### Moyenne

La moyenne indique le niveau mesure.

Elle sert surtout a voir le biais par rapport a une distance connue :

```text
biais = moyenne_mesuree - distance_reelle
```

### Mediane

La mediane est souvent plus fiable que la moyenne en presence de pics.

Si moyenne et mediane sont tres differentes, il y a probablement des outliers ou du multipath.

### Percentiles P5 / P50 / P95

Ils donnent l'enveloppe pratique de mesure.

Exemple :

```text
P5  = 1.96 m
P50 = 2.01 m
P95 = 2.07 m
```

Cela signifie que 90% des mesures sont entre 1.96 m et 2.07 m.

### IQR

L'IQR est l'ecart entre P75 et P25.

C'est une mesure robuste du bruit central, moins sensible aux pics que l'ecart-type.

### Peak-to-peak

```text
max(dist) - min(dist)
```

Utile pour voir le pire ecart observe, mais tres sensible aux outliers. A utiliser avec prudence.

### MAD

Median Absolute Deviation.

Tres bon indicateur robuste :

```text
MAD = median(|dist - median(dist)|)
```

Il permet d'evaluer la stabilite meme s'il y a quelques mesures aberrantes.

## 3. Indicateurs temporels

### Cadence effective

A partir de `ms` et `sample` :

```text
rate_hz = delta_sample / delta_time_s
```

On a mesure environ 924 Hz actuellement.

Il faut surveiller :

- cadence moyenne ;
- regularite ;
- pauses ou trous.

### Jitter inter-echantillons

On regarde l'ecart entre deux timestamps successifs :

```text
dt = ms[i] - ms[i-1]
```

A environ 924 Hz, on attend environ 1.08 ms par mesure.

Attention : `ms` est en millisecondes, donc peu precis pour mesurer finement chaque intervalle. Il reste utile pour reperer les pauses longues.

### Delta de distance

```text
delta_dist = dist[i] - dist[i-1]
```

Tres utile pour visualiser le bruit instantane.

A distance fixe, les deltas doivent etre centres autour de zero.

### Derive lente

On decoupe la capture en fenetres :

```text
1 s, 5 s, 10 s, 60 s
```

Puis on calcule moyenne et sigma par fenetre.

Si la moyenne glisse avec le temps, ce n'est pas seulement du bruit : cela peut venir de la temperature, du quartz, du placement, du multipath ou de l'environnement.

### Allan deviation

Indicateur avance pour distinguer :

- bruit blanc, qui se moyenne bien ;
- derive lente, qui ne disparait pas avec un simple filtrage.

Tres utile si on veut savoir quelle fenetre de filtrage est optimale.

## 4. Qualite des mesures

### Ratio valid

Depuis la colonne `valid` :

```text
valid_ratio = nombre_valid / nombre_total
```

Idealement proche de 100%.

Si `valid` chute, le gate rejette des mesures physiquement suspectes.

### rx_fail

La colonne `rx_fail` est un compteur cumulatif.

A surveiller :

```text
rx_fail_delta = rx_fail_fin - rx_fail_debut
```

Sur une capture stable, on veut un delta proche de zero.

Un `rx_fail` eleve peut indiquer :

- responder absent ;
- mauvaise orientation antenne ;
- distance trop grande ;
- environnement RF difficile ;
- timing trop agressif ;
- alimentation instable.

### Outliers

On peut definir un outlier comme :

```text
|dist - median(dist)| > 3 * sigma
```

Ou plus robuste :

```text
|dist - median(dist)| > 6 * MAD
```

A mesurer :

- nombre d'outliers ;
- pourcentage ;
- duree des bursts consecutifs.

Un outlier isole est moins grave qu'une rafale de 20 mesures mauvaises.

## 5. Correlations importantes

### cppm

La colonne `cppm` represente l'ecart d'horloge estime.

A auditer :

- moyenne de `cppm` ;
- ecart-type de `cppm` ;
- correlation entre `cppm` et `dist`.

Si `dist` varie fortement avec `cppm`, la correction d'horloge ou le timing SS-TWR merite d'etre revu.

### dist vs dist_filt vs dist_smooth

Comparer :

- `dist` : mesure brute ;
- `dist_filt` : mediane/gate ;
- `dist_smooth` : lissage lent.

A verifier :

- `dist_filt` reduit les pics sans introduire trop de retard ;
- `dist_smooth` reduit le bruit mais ne doit pas masquer une vraie variation rapide ;
- en test de deplacement brusque, mesurer le temps de convergence.

## 6. Protocole de test recommande

### Test A : repetabilite a distance fixe

1. Placer les deux boitiers immobiles.
2. Distance connue : par exemple 0.5 m, 2 m, 5 m, 10 m.
3. Environnement degage, ligne de vue directe.
4. Capture de 60 secondes minimum.
5. Calculer :
   - moyenne ;
   - mediane ;
   - sigma ;
   - P5/P95 ;
   - MAD ;
   - valid ratio ;
   - rx_fail delta ;
   - cadence moyenne.

### Test B : stabilite longue

1. Distance fixe.
2. Capture de 5 a 10 minutes.
3. Calculer les metriques par fenetres de 10 s.
4. Observer la derive de la moyenne.

Objectif : voir si la distance glisse avec le temps.

### Test C : reponse a un deplacement

1. Demarrer a distance fixe.
2. Deplacer brutalement un boitier de 50 cm.
3. Observer :
   - temps de reaction de `dist` ;
   - temps de convergence de `dist_filt` ;
   - temps de convergence de `dist_smooth`.

Ce test mesure la latence introduite par le filtrage.

### Test D : robustesse environnementale

Repeter les captures :

- proche du sol ;
- a hauteur reelle d'utilisation ;
- avec obstacle partiel ;
- avec corps humain proche ;
- avec orientations antennes differentes.

Cela permet d'identifier les biais lies au multipath et a l'orientation.

## 7. Tableau de rapport type

Pour chaque capture :

```text
Nom test:
Distance reelle:
Duree:
Nombre echantillons:
Cadence moyenne:
valid ratio:
rx_fail delta:

dist brut:
  moyenne:
  mediane:
  ecart-type:
  P5:
  P95:
  min:
  max:
  MAD:

dist_filt:
  moyenne:
  ecart-type:
  P5/P95:

dist_smooth:
  moyenne:
  ecart-type:
  latence estimee si test step:

cppm:
  moyenne:
  ecart-type:
  correlation dist/cppm:

Commentaires:
  outliers:
  derive:
  environnement:
```

## 8. Interpretation rapide

Bon signe :

- sigma faible ;
- P5/P95 serres ;
- `valid` proche de 100% ;
- `rx_fail_delta` proche de 0 ;
- pas de derive visible par fenetres ;
- faible correlation entre `dist` et `cppm`.

Signe a investiguer :

- sigma eleve mais peu d'outliers : bruit continu ;
- beaucoup d'outliers : multipath ou timing ;
- derive lente : horloge, temperature, alimentation, environnement ;
- `rx_fail` qui augmente : timing trop serre, RF faible, responder instable ;
- moyenne stable mais fausse : besoin de calibration d'antenna delay.

## 9. Metriques prioritaires

Si on doit retenir seulement trois indicateurs :

```text
1. ecart-type de dist a distance fixe
2. P5/P95 de dist
3. rx_fail_delta + valid_ratio
```

Et pour un audit plus serieux :

```text
4. derive par fenetre de 10 s
5. correlation dist/cppm
6. outliers par MAD
```
