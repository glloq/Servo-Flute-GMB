"""Le firmware ne recoit PAS le dialecte que ce depot demande.

`platformio.ini` demande `-std=gnu++17`, et son commentaire explique pourquoi
c'est une requete et non une garantie : le builder Arduino
(`tools/platformio-build-esp32.py`) s'execute APRES les options du projet et
ajoute `-std=gnu++11`. gcc retient le DERNIER `-std` de la ligne de commande,
donc le firmware est compile en **gnu++11**.

Les deux builds hote, eux, compilent en C++17. Toute une famille d'erreurs est
donc invisible sur l'hote et n'apparait que sur le build ESP32 - au mieux dans
la CI, au pire chez la personne qui flashe.

Ce module a ete ecrit apres l'une d'elles, qui a coute un cycle de CI complet :

    ConfigCommitResult out{false, false, false, false, false, false, "", "", ""};
    error: no matching function for call to
           'ConfigCommitResult::ConfigCommitResult(<brace-enclosed initializer list>)'

La structure venait de recevoir des initialiseurs de membre par defaut
(`bool valid = false;`). En C++14 et au-dela, un agregat peut en porter ; en
C++11, non - la structure cesse d'etre un agregat et l'initialisation par
accolades ne compile plus. Les tests hote, en C++17, ne pouvaient pas le voir.

Le meme piege a deja frappe ce depot une premiere fois, sur les `static
constexpr` de `gmb::GmbSysEx` : en gnu++11 ils ne sont pas implicitement
`inline`, et le LIEN echouait. C'est la deuxieme fois. D'ou ce test.

Il ne remplace pas le build ESP32 de la CI - il ne voit ni l'edition de liens,
ni les en-tetes ESP-IDF, ni les sources qui ne sont pas compilables sur hote
(ConfigStorage.cpp, WebConfigurator.cpp, le sketch). Il attrape la famille
d'erreurs de DIALECTE, en moins d'une seconde, sur la machine de qui ecrit le
code.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]

# Le dialecte que le firmware recoit reellement, pas celui qu'on demande.
FIRMWARE_DIALECT = "gnu++11"


def _platformio_requests_gnu17():
    return "-std=gnu++17" in (ROOT / "platformio.ini").read_text(encoding="utf-8")


def _production_sources():
    """Les sources de PRODUCTION du build hote.

    Prises dans la liste `SOURCES` du harnais pour qu'elles ne puissent pas
    diverger : une source ajoutee au build hote est automatiquement couverte.
    Les fichiers de test sont exclus - eux ont le droit d'etre en C++17, ils ne
    partent jamais sur la carte.
    """
    text = (ROOT / "tests" / "test_native_behavior.py").read_text(encoding="utf-8")
    block = text.split("SOURCES = [", 1)[1].split("]", 1)[0]
    return [s for s in re.findall(r'"([^"]+)"', block)
            if s.startswith("Servo_flute_ESP32/")]


def _syntax_only(sources, dialect):
    return subprocess.run(
        ["g++", f"-std={dialect}", "-fsyntax-only", "-DUNIT_TEST",
         "-Ilib/native_stubs/include", "-IServo_flute_ESP32", *sources],
        cwd=ROOT, text=True, capture_output=True,
    )


@pytest.mark.skipif(shutil.which("g++") is None, reason="g++ absent")
def test_production_sources_compile_in_the_dialect_the_firmware_really_gets():
    sources = _production_sources()
    assert len(sources) >= 20, (
        f"Seulement {len(sources)} sources de production lues : la liste "
        "`SOURCES` du harnais n'est plus reconnue, et ce test ne couvre plus "
        "rien."
    )
    result = _syntax_only(sources, FIRMWARE_DIALECT)
    errors = [l for l in result.stderr.splitlines() if ": error:" in l]
    assert result.returncode == 0, (
        f"Des sources de production ne compilent pas en {FIRMWARE_DIALECT}, "
        "c'est-a-dire dans le dialecte que le firmware recoit reellement. Le "
        "build ESP32 de la CI echouera :\n  " + "\n  ".join(errors[:20])
    )


@pytest.mark.skipif(shutil.which("g++") is None, reason="g++ absent")
def test_le_detecteur_voit_reellement_une_erreur_de_dialecte(tmp_path):
    """Preuve par mutation, sur l'erreur qui a motive ce module.

    Un test de dialecte qui compilerait tout en silence passerait au vert
    exactement comme un depot sain. On lui soumet donc, hors du depot, le cas
    qui a reellement casse la CI : un agregat qui recoit des initialiseurs de
    membre par defaut et reste initialise par une liste positionnelle.
    """
    src = tmp_path / "dialect_probe.cpp"
    src.write_text(
        "struct R { bool a = false; bool b = false; };\n"
        "R make() { R r{false, false}; return r; }\n",
        encoding="utf-8",
    )
    en_11 = _syntax_only([str(src)], FIRMWARE_DIALECT)
    en_17 = _syntax_only([str(src)], "gnu++17")
    assert en_11.returncode != 0, (
        "Le cas qui a casse le build ESP32 compile sous "
        f"{FIRMWARE_DIALECT} : ce test ne prouve rien."
    )
    assert en_17.returncode == 0, (
        "Le cas temoin ne compile pas non plus en C++17 : la mutation ne "
        "distingue donc pas les deux dialectes, et le test est sans valeur."
    )


def test_platformio_still_asks_for_a_dialect_it_does_not_get():
    """Si un jour le firmware recoit vraiment gnu++17, ce module doit etre revu
    plutot que de continuer a tester un dialecte qui n'a plus cours."""
    assert _platformio_requests_gnu17(), (
        "platformio.ini ne demande plus -std=gnu++17 : verifier quel dialecte "
        "atteint reellement le compilateur et mettre FIRMWARE_DIALECT a jour."
    )
