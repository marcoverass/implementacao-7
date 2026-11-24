#!/usr/bin/env python3
"""Testes funcionais; os arquivos fornecidos ficam fora do pacote de codigo."""

import argparse
from collections import OrderedDict
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LINE = re.compile(r"Virtual address: (\d+) TLB: (-?\d+) Physical address: (\d+) Value: (-?\d+)")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def reference(addresses, backing, pages, tlb_policy, frames=128, use_tlb=True):
    """Oraculo com filas ordenadas de paginas, independente dos vetores C."""
    resident = OrderedDict()
    cache = OrderedDict()
    rows = []
    faults = hits = 0
    for original in addresses:
        logical = original & 65535
        page, offset = divmod(logical, 256)
        if use_tlb and page in cache:
            slot = cache[page]
            hits += 1
            if tlb_policy == "lru":
                cache.move_to_end(page)
        else:
            if page not in resident:
                faults += 1
                if len(resident) < frames:
                    frame = len(resident)
                else:
                    evicted, frame = resident.popitem(last=False)
                    cache.pop(evicted, None)
                resident[page] = frame
            if use_tlb:
                if len(cache) == 16:
                    _, slot = cache.popitem(last=False)
                else:
                    slot = min(set(range(16)) - set(cache.values()))
                cache[page] = slot
            else:
                slot = -1
        frame = resident[page]
        if pages == "lru":
            resident.move_to_end(page)
        byte = backing[logical]
        rows.append((original, slot, frame * 256 + offset, byte if byte < 128 else byte - 256))
    n = len(rows)
    footer = [f"Number of Translated Addresses = {n}", f"Page Faults = {faults}",
              f"Page Fault Rate = {faults / n:.3f}", f"TLB Hits = {hits}",
              f"TLB Hit Rate = {hits / n:.3f}"]
    return rows, footer, faults, hits


def run(work, *args):
    return subprocess.run([str(ROOT / "vm"), *map(str, args)], cwd=work,
                          text=True, capture_output=True, timeout=90)


def check(work, addresses, backing, page_policy, tlb_policy, frames=128, use_tlb=True, raw=None):
    path = work / "input.txt"
    path.write_bytes(raw if raw is not None else ("\n".join(map(str, addresses)) + "\n").encode())
    result = run(work, path, page_policy, tlb_policy)
    require(result.returncode == 0, result.stderr)
    lines = (work / "correct.txt").read_text().splitlines()
    matches = [LINE.fullmatch(line) for line in lines[:-5]]
    require(all(matches), "Formato incorreto nas traducoes")
    actual = [tuple(map(int, match.groups())) for match in matches]
    expected, footer, faults, hits = reference(addresses, backing, page_policy, tlb_policy, frames, use_tlb)
    require(actual == expected, f"Traducao divergente: {page_policy}/{tlb_policy}, frames={frames}")
    require(lines[-5:] == footer, "Estatisticas divergentes")
    return faults, hits


def errors(work):
    output = work / "correct.txt"
    sentinel = b"saida anterior deve ser preservada\n"
    output.write_bytes(sentinel)
    valid = work / "valid.txt"
    valid.write_text("0\n")
    cases = [(), (str(valid),), (str(valid), "fifo"),
             (str(valid), "fifo", "fifo", "extra"),
             (str(valid), "FIFO", "fifo"), (str(valid), "lru", "random"),
             (str(work / "inexistente.txt"), "lru", "fifo"),
             (str(work), "lru", "fifo")]
    for args in cases:
        result = run(work, *args)
        require(result.returncode != 0 and "Erro" in result.stderr, f"Erro nao tratado: {args}")
        require(output.read_bytes() == sentinel, "Entrada invalida alterou a saida anterior")
    invalid = [b"", b"\n", b"1\n\n2\n", b"-1\n", b"+1\n", b"abc\n",
               b"1 2\n", b"1.5\n", b"0x10\n", b"4294967296\n",
               b"9" * 3000 + b"\n", b"1\x00\n", b"0\n2x\n", b"\xff\n"]
    for raw in invalid:
        valid.write_bytes(raw)
        result = run(work, valid, "lru", "fifo")
        require(result.returncode != 0 and "Erro" in result.stderr, f"Formato aceito: {raw[:30]!r}")
        require(output.read_bytes() == sentinel, "Formato invalido alterou a saida anterior")
    backing = work / "BACKING_STORE.bin"
    original = backing.read_bytes()
    valid.write_text("0\n")
    for data in (None, b"x" * 65535, b"x" * 65537):
        if data is None:
            backing.unlink()
        else:
            backing.write_bytes(data)
        result = run(work, valid, "fifo", "lru")
        require(result.returncode != 0 and "Erro" in result.stderr, "Backing invalido aceito")
        require(output.read_bytes() == sentinel, "Backing invalido alterou a saida")
    backing.write_bytes(original)
    denied = work / "sem_permissao.txt"
    denied.write_text("0\n")
    denied.chmod(0)
    try:
        if not os.access(denied, os.R_OK):
            result = run(work, denied, "fifo", "fifo")
            require(result.returncode != 0 and "Erro" in result.stderr, "Permissao nao tratada")
            print("OK arquivo sem permissao de leitura")
        else:
            print("SKIP permissao de leitura: usuario possui privilegio de acesso")
    finally:
        denied.chmod(0o600)
    output.unlink()
    output.mkdir()
    result = run(work, valid, "fifo", "fifo")
    require(result.returncode != 0 and "Erro" in result.stderr, "Falha ao salvar nao tratada")
    require(output.is_dir(), "Falha ao salvar alterou o diretorio existente")
    output.rmdir()
    print(f"OK erros: {len(cases)} chamadas, {len(invalid)} formatos, backing e saida")


def functional(data):
    backing = (data / "BACKING_STORE.bin").read_bytes()
    addresses = [int(line) for line in (data / "addresses.txt").read_text().splitlines()]
    require(len(backing) == 65536, "Backing de teste deve ter 65536 bytes")
    expected_file = data / "correct_referencia.txt"
    require(expected_file.exists(), "Copie o correct.txt fornecido como correct_referencia.txt")
    with tempfile.TemporaryDirectory(prefix="vm-tests-") as temporary:
        work = Path(temporary)
        (work / "BACKING_STORE.bin").write_bytes(backing)
        for page_policy in ("fifo", "lru"):
            for tlb_policy in ("fifo", "lru"):
                faults, hits = check(work, addresses, backing, page_policy, tlb_policy)
                if (page_policy, tlb_policy) == ("lru", "fifo"):
                    require((work / "correct.txt").read_text().splitlines() ==
                            expected_file.read_text().splitlines(), "Saida diferente da referencia fornecida")
                print(f"OK referencia {page_policy}/{tlb_policy}: {len(addresses)} enderecos, {faults} faults, {hits} hits")
        rng = random.Random(20261123)
        traces = [
            [0, 255, 256, 65535, 65536, 4294967295],
            [page * 256 for page in range(16)] + [0, 4096, 0],
            [page * 256 for page in range(128)] + [127, 32768, 127],
            [page * 256 for page in range(128)] + [0, 32768, 256, 0],
            [rng.randrange(65536) for _ in range(600)],
        ]
        for page_policy in ("fifo", "lru"):
            for tlb_policy in ("fifo", "lru"):
                for trace in traces:
                    check(work, trace, backing, page_policy, tlb_policy)
        check(work, [0, 255, 65535], backing, "fifo", "fifo", raw=b" 0\r\n255\t\r\n65535")
        check(work, [0], backing, "lru", "fifo", raw=b"0")
        first = (work / "correct.txt").read_bytes()
        check(work, [0], backing, "lru", "fifo", raw=b"0")
        require((work / "correct.txt").read_bytes() == first, "Busca paralela nao deterministica")
        print("OK limites de pagina, mascara de 16 bits, FIFO/LRU, invalidacao da TLB e CRLF")
        errors(work)
    print("TODOS OS TESTES PASSARAM")


def stages(data):
    backing = (data / "BACKING_STORE.bin").read_bytes()
    addresses = [int(line) for line in (data / "addresses.txt").read_text().splitlines()]
    configurations = [(256, 0, 0), (128, 0, 0), (128, 1, 0), (128, 1, 1)]
    try:
        with tempfile.TemporaryDirectory(prefix="vm-stages-") as temporary:
            work = Path(temporary)
            shutil.copyfile(data / "BACKING_STORE.bin", work / "BACKING_STORE.bin")
            for frames, use_tlb, threaded in configurations:
                subprocess.run(["make", f"FRAME_COUNT={frames}", f"USE_TLB={use_tlb}",
                                f"THREADED_TLB={threaded}"], cwd=ROOT, check=True)
                for page_policy in ("fifo", "lru"):
                    for tlb_policy in (("fifo", "lru") if use_tlb else ("fifo",)):
                        faults, hits = check(work, addresses, backing, page_policy, tlb_policy, frames, bool(use_tlb))
                        print(f"OK etapa frames={frames} TLB={use_tlb} threads={threaded} "
                              f"{page_policy}/{tlb_policy}: faults={faults}, hits={hits}", flush=True)
    finally:
        subprocess.run(["make"], cwd=ROOT, check=True)
    print("TODAS AS ETAPAS PASSARAM; CONFIGURACAO PADRAO RESTAURADA")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", type=Path, default=ROOT)
    parser.add_argument("--stages", action="store_true")
    args = parser.parse_args()
    try:
        (stages if args.stages else functional)(args.data.resolve())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(f"FALHA: {error}") from error
