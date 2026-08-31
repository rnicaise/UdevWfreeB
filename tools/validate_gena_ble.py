#!/usr/bin/env python3
"""Suite de validation automatique du firmware initiator GenA via BLE (NUS).

Le firmware suit le modèle Vario : toute écriture de règle (ARM,RULE / DISARM)
déclenche une sauvegarde puis un REDÉMARRAGE du boîtier (déconnexion BLE).
La suite travaille donc en sessions successives avec reconnexion.

Usage:
    .venv/bin/python tools/validate_gena_ble.py [--name UWB] [--fire-now]

Sans --fire-now, aucun ordre de tir n'est envoyé. Avec --fire-now, envoie
PYRO,FIRE_NOW (sans danger sans responder : le pyro est sur le responder)
pour valider la boîte noire de bout en bout (trigger -> nouveau slot).
"""

import argparse
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify

RESULTS = []


def report(name, ok, detail=""):
    RESULTS.append((name, ok))
    mark = "PASS" if ok else "FAIL"
    print(f"[{mark}] {name}" + (f" — {detail}" if detail else ""))


class Session:
    def __init__(self, client):
        self.client = client
        self.buf = b""
        self.lines = asyncio.Queue()

    def feed(self, _, data: bytearray):
        self.buf += bytes(data)
        while b"\n" in self.buf:
            line, _, self.buf = self.buf.partition(b"\n")
            text = line.decode("ascii", "replace").strip()
            if text:
                self.lines.put_nowait(text)

    def drain(self):
        while not self.lines.empty():
            self.lines.get_nowait()

    async def wait_for(self, prefixes, timeout=5.0, collect_until=None):
        deadline = time.monotonic() + timeout
        collected = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return (None, collected)
            try:
                line = await asyncio.wait_for(self.lines.get(), remaining)
            except asyncio.TimeoutError:
                return (None, collected)
            if collect_until is not None:
                collected.append(line)
                if line.startswith(collect_until):
                    return (line, collected)
            elif any(line.startswith(p) for p in prefixes):
                return (line, collected)

    async def send(self, cmd):
        self.drain()
        # MTU par défaut = 23 (20 octets utiles) : CoreBluetooth tronque
        # silencieusement les writes plus longs -> découpage obligatoire.
        data = (cmd + "\n").encode()
        for i in range(0, len(data), 20):
            await self.client.write_gatt_char(NUS_RX, data[i:i + 20], response=False)
            if len(data) > 20:
                await asyncio.sleep(0.03)

    async def expect(self, name, cmd, prefixes, timeout=5.0):
        await self.send(cmd)
        line, _ = await self.wait_for(prefixes, timeout)
        report(name, line is not None, line or f"timeout ({cmd})")
        return line


async def find_device(name, tries=6):
    for _ in range(tries):
        devs = await BleakScanner.discover(timeout=5.0, return_adv=True)
        for d, adv in devs.values():
            if (adv.local_name or d.name or "") == name:
                return d
    return None


async def connect(name):
    device = await find_device(name)
    if device is None:
        return None, None
    client = BleakClient(device, timeout=15.0)
    await client.connect()
    session = Session(client)
    await client.start_notify(NUS_TX, session.feed)
    await asyncio.sleep(0.5)
    return client, session


async def get_arm_state(session):
    """ARM,GET avec tolérance WRITE_PENDING."""
    line = None
    for _ in range(4):
        await session.send("ARM,GET")
        line, _ = await session.wait_for(["ARM,RULE", "ARM,DISARMED", "ARM,WRITE_PENDING"], 5.0)
        if line != "ARM,WRITE_PENDING":
            return line
        await asyncio.sleep(1.5)
    return line


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="UWB")
    ap.add_argument("--fire-now", action="store_true")
    args = ap.parse_args()

    # ============ Session 1: identité + inventaire + règle invalide ============
    # NOTE: toute commande "admin" (même une règle invalide -> ERR) programme un
    # redémarrage du boîtier ~1 s plus tard (modèle Vario). Le test de règle
    # invalide clôt donc la session.
    print(f"— session 1: scan et connexion à '{args.name}' —")
    client, s = await connect(args.name)
    if client is None:
        print(f"introuvable: {args.name}")
        return 1

    await s.expect("CFG,GET_FW", "CFG,GET_FW", ["FW,"])
    await s.expect("INFO? (rôle)", "INFO?", ["ROLE,INITIATOR"])
    await s.expect("CFG,GET_NAME", "CFG,GET_NAME", ["NAME,"])
    await s.expect("CFG,GET_PROFILE", "CFG,GET_PROFILE", ["PROFILE,"])
    await s.expect("CFG,GET_TXPWR", "CFG,GET_TXPWR", ["TXPWR,"])

    line = await get_arm_state(s)
    report("ARM,GET (état initial)", line is not None, line or "timeout")

    # Boîte noire: inventaire avant tir
    await s.send("LOG,LIST")
    end, lines = await s.wait_for(None, 10.0, collect_until="LOG,END")
    slots_before = [l for l in lines if l.startswith("LOG,SLOT")]
    report("LOG,LIST", end is not None, f"{len(slots_before)} slot(s)")
    for slot in slots_before:
        print(f"       {slot}")

    # Règle invalide -> ERR, puis le boîtier redémarre (fin de session)
    await s.expect("ARM,RULE invalide rejetée", "ARM,RULE,FOO,GT,1,0", ["ERR,ARM_RULE_INVALID"])
    try:
        await client.disconnect()
    except Exception:
        pass
    print("       redémarrage du boîtier (post-ERR, modèle Vario)…")
    await asyncio.sleep(4.0)

    # ============ Session 2: pose de règle ============
    print("— session 2: reconnexion, pose de règle —")
    client, s = await connect(args.name)
    report("Reconnexion post-ERR", client is not None)
    if client is None:
        return 1

    # Règle inoffensive (DIST > 900 m, hold 60 s: jamais vraie sans responder)
    await s.send("ARM,RULE,DIST,GT,900,60000")
    line, _ = await s.wait_for(["ACK,ARM_SETTINGS_WRITE_STARTED", "ERR"], 8.0)
    report("ARM,RULE acceptée", line == "ACK,ARM_SETTINGS_WRITE_STARTED", line or "timeout")
    line, _ = await s.wait_for(["ACK,ARM,RULE,SAVED"], 8.0)
    report("ARM,RULE sauvegardée", line is not None, line or "timeout (SAVED)")
    try:
        await client.disconnect()
    except Exception:
        pass
    print("       redémarrage armé du boîtier…")
    await asyncio.sleep(4.0)

    # ============ Session 3: vérif armé + BLE armé (Vario) + désarmement ============
    print("— session 3: reconnexion (boîtier armé) —")
    client, s = await connect(args.name)
    report("BLE armé: reconnexion possible (Vario)", client is not None)
    if client is None:
        print("le boîtier n'annonce plus — abandon")
        return 1

    line = await get_arm_state(s)
    report("ARM,GET relit la règle", line is not None and line.startswith("ARM,RULE") and "DIST" in line,
           line or "timeout")

    await s.send("DISARM")
    line, _ = await s.wait_for(["ACK,ARM_SETTINGS_WRITE_STARTED", "ERR"], 8.0)
    report("DISARM accepté", line == "ACK,ARM_SETTINGS_WRITE_STARTED", line or "timeout")
    line, _ = await s.wait_for(["ACK,ARM,DISARMED,SAVED"], 8.0)
    report("DISARM sauvegardé", line is not None, line or "timeout (SAVED)")
    try:
        await client.disconnect()
    except Exception:
        pass
    print("       redémarrage du boîtier…")
    await asyncio.sleep(4.0)

    # ============ Session 4: vérif désarmé + déclenchement boîte noire ============
    print("— session 4: reconnexion (boîtier désarmé) —")
    client, s = await connect(args.name)
    report("Reconnexion après désarmement", client is not None)
    if client is None:
        return 1

    line = await get_arm_state(s)
    report("ARM,GET désarmé", line == "ARM,DISARMED", line or "timeout")

    if args.fire_now:
        await s.expect("PYRO,FIRE_NOW ack", "PYRO,FIRE_NOW", ["ACK,PYRO_FORWARD_NOW_ARMED"])
        # Le ranging est en pause tant que le client BLE est connecté : la
        # queue post-trigger de la boîte noire ne se remplit (et la sauvegarde
        # flash n'a lieu) qu'après déconnexion. On se déconnecte donc tout de
        # suite et on valide la boîte noire en session 5 via LOG,LIST/LOG,READ.
        try:
            await client.disconnect()
        except Exception:
            pass
        print("       déconnexion, remplissage post-trigger + sauvegarde flash…")
        await asyncio.sleep(10.0)

        # ============ Session 5: relecture boîte noire ============
        print("— session 5: reconnexion, relecture boîte noire —")
        client, s = await connect(args.name)
        report("Reconnexion post-FIRE_NOW", client is not None)
        if client is None:
            return 1

        await s.send("LOG,LIST")
        end, lines = await s.wait_for(None, 10.0, collect_until="LOG,END")
        slots_after = [l for l in lines if l.startswith("LOG,SLOT")]
        # Ring de slots: quand il est plein le nombre ne monte plus, on
        # compare donc les numéros de séquence (champ 3).
        def max_seq(slots):
            return max((int(l.split(",")[3]) for l in slots), default=0)
        report("Boîte noire: nouveau slot",
               end is not None and max_seq(slots_after) > max_seq(slots_before),
               f"seq {max_seq(slots_before)} -> {max_seq(slots_after)} "
               f"({len(slots_after)} slot(s))")
        for slot in slots_after:
            print(f"       {slot}")

        if slots_after:
            newest = max(slots_after, key=lambda l: int(l.split(",")[3]))
            slot_id = newest.split(",")[2]
            await s.send(f"LOG,READ,{slot_id}")
            end, lines = await s.wait_for(None, 120.0, collect_until="LOG,END,")
            records = [l for l in lines if l.startswith("L,")]
            report(f"LOG,READ,{slot_id}", end is not None and len(records) > 100,
                   f"{len(records)} enregistrements")
            if records:
                print(f"       premier: {records[0]}")
                print(f"       dernier: {records[-1]}")
    else:
        print("       (--fire-now non passé: test de déclenchement boîte noire sauté)")

    try:
        await client.disconnect()
    except Exception:
        pass

    print()
    failed = [n for n, ok in RESULTS if not ok]
    print(f"=== {len(RESULTS) - len(failed)}/{len(RESULTS)} PASS ===")
    for n in failed:
        print(f"    FAIL: {n}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
