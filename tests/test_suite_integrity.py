"""L'audit des tests eux-memes.

Deux tests ecrits dans `tests/test_native/test_behavior.cpp` -
`event_queue_forced_never_drops` et `servo_angle_to_pwm_math` - ont vecu
plusieurs semaines sans jamais s'executer : ils etaient DEFINIS mais absents du
`main()`. Ils compilaient, ils passaient en revue, ils donnaient la sensation
d'une couverture. Ils ne protegeaient rien.

Un test qui ne tourne pas est pire qu'un test absent : l'absence se voit, la
mort silencieuse non. Ce module retire la detection des mains d'un relecteur
attentif et la confie a la CI.

Il couvre deux morts distinctes :

* la mort par DEBRANCHEMENT - la fonction existe mais rien ne l'appelle depuis
  le `main()` reellement execute. C'est le cas historique ci-dessus ;
* la mort par VACUITE - la fonction s'execute mais n'affirme rien. Elle passe
  toujours, y compris sur un firmware casse. Plus discrete, parce que le test
  APPARAIT bien dans `main()`.

L'analyse est statique et volontairement simple : elle lit les definitions de
fonctions de `tests/test_native/*.cpp` et suit les appels. Elle n'a pas besoin
d'etre un compilateur C++ - ces fichiers sont ecrits dans un style uniforme de
fonctions libres - mais elle doit rester FIDELE : un faux positif ferait
contourner l'outil, un faux negatif le rendrait inutile. Chacun des deux
detecteurs est donc prouve par mutation, sur un cas mort fabrique, plutot que
suppose correct parce qu'il est vert.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE_DIR = ROOT / "tests" / "test_native"

# Le programme de test imprime ce marqueur en cas de succes ; c'est ce que
# cherche le lecteur PlatformIO (tests/test_custom_runner.py) comme le harnais
# pytest (tests/test_native_behavior.py). Le `main()` qui le contient est donc
# le SEUL point d'entree reellement execute, et l'unique racine valable pour
# l'analyse d'atteignabilite.
SUCCESS_MARKER = "behavior tests passed"

# Cinq fichiers portent en plus un `main()` de mise au point, sous
# `#ifdef STANDALONE_TEST_MAIN`, pour pouvoir executer un seul lot a la main.
# Ce symbole n'est defini NI par platformio.ini NI par le harnais pytest : ces
# blocs ne sont pas compiles, et les prendre pour des points d'entree ferait
# croire vivant tout ce qu'ils appellent. On les retire - et le test
# `test_le_main_de_mise_au_point_est_bien_hors_build` verifie que cette
# hypothese reste vraie, au lieu de la reconduire en silence.
STANDALONE_GUARD = "STANDALONE_TEST_MAIN"

# Mots-cles qui, suivis d'une parenthese, ressemblent a un appel sans en etre un.
_NOT_CALLS = {
    "if", "for", "while", "switch", "return", "sizeof", "catch", "assert",
    "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast", "new",
    "delete", "throw", "decltype", "alignof", "and", "or", "not",
}

# Une definition de fonction libre : un type de retour, un nom, des parametres,
# puis l'accolade ouvrante SUR LA MEME LIGNE. C'est le style de tout
# tests/test_native/*.cpp.
#
# La liste de parametres admet UN niveau de parentheses imbriquees, parce que
# plusieurs aides de ce depot prennent un pointeur de fonction en parametre
# (`void (*disturb)(AcousticTiming*, int)`). Une premiere version exigeait des
# parametres sans parentheses : ces fonctions n'etaient alors pas reconnues
# comme des definitions, leur corps n'etait jamais parcouru, et tout ce qu'elles
# appelaient etait declare mort a tort. Un faux positif de cette ampleur fait
# desactiver l'outil - d'ou le test de non-regression dedie plus bas.
_DEF_RE = re.compile(
    r"^(?P<ret>[A-Za-z_][A-Za-z0-9_:<>,\*&\s]*?)\b(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*"
    r"\((?P<args>[^()]*(?:\([^()]*\)[^()]*)*)\)\s*(?:const\s*)?\{",
    re.MULTILINE,
)

# Une fonction est UTILISEE des que son nom apparait dans un corps atteignable,
# meme sans parenthese. C'est indispensable ici : les aides de ce depot sont
# souvent passees en POINTEUR DE FONCTION (`observerScenario(&t2, timingDisturb)`,
# `auth.begin(auditRng, 1000)`), donc referencees sans jamais etre "appelees"
# textuellement. N'exiger que `nom(` faisait passer cinq aides bien vivantes
# pour du code mort.
#
# La contrepartie est assumee : la reference est plus large que l'appel, donc
# une fonction seulement MENTIONNEE compte comme vivante. C'est le bon sens de
# l'erreur pour cet outil - on prefere rater une morte que crier au loup, parce
# qu'un detecteur qui crie au loup finit desactive, et ne detecte alors plus
# rien du tout. Commentaires et chaines sont retires en amont, donc une simple
# evocation en prose ne ressuscite rien.
_REF_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\b")
_CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def _drop_standalone_blocks(text):
    """Retire les regions `#ifdef STANDALONE_TEST_MAIN ... #endif`."""
    out, depth = [], 0
    for line in text.splitlines(keepends=True):
        s = line.strip()
        if depth:
            if s.startswith("#if"):
                depth += 1
            elif s.startswith("#endif"):
                depth -= 1
            continue
        if s.startswith("#ifdef") and STANDALONE_GUARD in s:
            depth = 1
            continue
        out.append(line)
    return "".join(out)


def _strip_noise(text):
    """Retire commentaires et litteraux. Un nom cite dans un commentaire ou une
    chaine n'est pas un appel : le compter creerait un faux negatif, c'est-a-dire
    une fonction morte qui paraitrait vivante.

    Le marqueur de succes est preserve : c'est lui qui identifie la racine.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    keep = f'"{SUCCESS_MARKER}"'
    text = re.sub(
        r'"(?:[^"\\\n]|\\.)*"',
        lambda m: keep if SUCCESS_MARKER in m.group(0) else '""',
        text,
    )
    return re.sub(r"'(?:[^'\\\n]|\\.)*'", "' '", text)


def _body_from(text, brace_index):
    """Corps de la fonction par appariement d'accolades, en partant du '{'."""
    depth = 0
    for i in range(brace_index, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace_index + 1:i]
    return text[brace_index + 1:]


def _definitions(sources):
    """[(fichier, nom, corps)] pour toutes les fonctions definies.

    Une liste, pas un dictionnaire : plusieurs fichiers peuvent definir le meme
    nom, et ecraser l'un par l'autre est exactement ce qui a fait passer la
    premiere version de ce detecteur au vert sur une racine fantome.
    """
    found = []
    for path, text in sources.items():
        for m in _DEF_RE.finditer(text):
            name = m.group("name")
            if name in _NOT_CALLS:
                continue
            brace = text.index("{", m.end() - 1)
            found.append((path, name, _body_from(text, brace)))
    return found


def _calls(body):
    """Noms REFERENCES par ce corps : appeles, ou passes en pointeur."""
    return {n for n in _REF_RE.findall(body) if n not in _NOT_CALLS}


def _bodies_by_name(defs):
    by_name = {}
    for _path, name, body in defs:
        by_name.setdefault(name, []).append(body)
    return by_name


def _load(extra=None):
    sources = {}
    for path in sorted(NATIVE_DIR.glob("*.cpp")):
        text = _drop_standalone_blocks(path.read_text(encoding="utf-8"))
        sources[path.name] = _strip_noise(text)
    for name, text in (extra or {}).items():
        sources[name] = _strip_noise(_drop_standalone_blocks(text))
    return sources


def _root_body(defs):
    """Le corps du `main()` reellement execute, identifie par son marqueur."""
    roots = [(p, b) for p, n, b in defs if n == "main" and SUCCESS_MARKER in b]
    assert len(roots) == 1, (
        "Le point d'entree reellement execute doit etre unique et identifiable "
        f"par le marqueur {SUCCESS_MARKER!r}. Trouve : {[p for p, _ in roots]}. "
        "Sans racine certaine, ce detecteur ne peut rien affirmer - et un "
        "detecteur qui ne peut rien affirmer doit echouer, pas se taire."
    )
    return roots[0][1]


def _reachable(defs):
    """Noms atteignables depuis la racine, transitivement."""
    by_name = _bodies_by_name(defs)
    seen, stack = set(), list(_calls(_root_body(defs)))
    while stack:
        name = stack.pop()
        if name in seen or name not in by_name:
            continue
        seen.add(name)
        for body in by_name[name]:
            stack.extend(_calls(body))
    return seen


def _dead(defs):
    reachable = _reachable(defs)
    return sorted(
        (name, path) for path, name, _b in defs
        if name != "main" and name not in reachable
    )


def test_le_main_de_mise_au_point_est_bien_hors_build():
    """L'hypothese qui fonde le filtrage ci-dessus, verifiee et non reconduite.

    Si `STANDALONE_TEST_MAIN` venait a etre defini par le build, les blocs qu'on
    retire deviendraient du code vivant, et tout ce qu'ils appellent paraitrait
    mort a tort - ou pire, masquerait un vrai debranchement.
    """
    for path in ("platformio.ini", "tests/test_native_behavior.py"):
        text = (ROOT / path).read_text(encoding="utf-8")
        assert STANDALONE_GUARD not in text, (
            f"{path} definit {STANDALONE_GUARD} : les blocs de mise au point "
            "sont maintenant compiles et ce detecteur les ignore a tort."
        )


def test_no_native_test_function_is_defined_without_being_run():
    """Toute fonction definie dans tests/test_native/*.cpp est atteignable
    depuis le `main()` reellement execute.

    La regle vaut aussi pour les fonctions d'aide : une aide que plus personne
    n'appelle est du code mort dans la suite de tests, et elle finit par noyer
    un vrai test debranche au milieu du bruit. Branche-la ou supprime-la.
    """
    dead = _dead(_definitions(_load()))
    assert not dead, (
        "Fonctions definies dans tests/test_native/*.cpp mais JAMAIS atteintes "
        "depuis main() - elles compilent, elles se relisent, elles ne protegent "
        "rien :\n  "
        + "\n  ".join(f"{name}  ({path})" for name, path in dead)
        + "\nBranche-les dans le main() de test_behavior.cpp, ou supprime-les."
    )


def test_le_detecteur_voit_reellement_un_test_mort():
    """Preuve par mutation du detecteur lui-meme.

    Un detecteur de code mort qui ne detecte rien passe au vert exactement comme
    un depot sain. La seule facon de distinguer les deux est de lui soumettre un
    cas mort FABRIQUE et d'exiger qu'il le trouve - ici greffe sur l'arbre reel,
    racine reelle comprise, sans rien ecrire dans le depot.
    """
    faux = {
        "__mutation.cpp": (
            "void un_test_greffe_vivant(){ assert(1); }\n"
            "void un_test_greffe_debranche(){ assert(0); }\n"
        )
    }
    sources = _load(extra=faux)
    # Greffe l'appel du test vivant DANS la vraie racine, sans toucher au depot.
    sources["test_behavior.cpp"] = sources["test_behavior.cpp"].replace(
        f'"{SUCCESS_MARKER}', f'un_test_greffe_vivant(); std::cout << "{SUCCESS_MARKER}', 1
    )
    dead = dict(_dead(_definitions(sources)))
    assert "un_test_greffe_debranche" in dead, (
        "Le detecteur n'a pas vu un test manifestement debranche : il est "
        "inoperant, et le vert de l'autre test ne veut rien dire."
    )
    assert "un_test_greffe_vivant" not in dead, (
        "Le detecteur signale comme mort un test appele depuis main() : un faux "
        "positif, et un faux positif finit par faire desactiver l'outil."
    )


def test_le_detecteur_ne_declare_pas_morte_une_aide_passee_en_pointeur():
    """Non-regression des deux faux positifs qu'a produits la premiere version.

    Elle declarait mortes cinq aides bien vivantes de la suite - `audioDisturb`,
    `timingDisturb`, `timingTraceStep`, `auditRng`, `auditSave` - pour deux
    raisons distinctes, reproduites ici sur un cas fabrique :

    * l'aide est passee en POINTEUR DE FONCTION, donc referencee sans
      parenthese : elle ne ressemblait pas a un appel ;
    * la fonction qui la recoit declare un PARAMETRE POINTEUR DE FONCTION, donc
      des parentheses imbriquees dans sa liste de parametres : elle n'etait meme
      pas reconnue comme une definition, et tout ce qu'elle appelait passait
      pour mort.

    Cinq faux positifs auraient suffi a faire ignorer l'outil des sa premiere
    execution - c'est-a-dire a le rendre exactement aussi utile que l'absence
    d'outil.
    """
    faux = {
        "__mutation3.cpp": (
            "void aide_greffee_en_pointeur(){ assert(1); }\n"
            "void appelee_seulement_par_le_receveur(){ assert(1); }\n"
            "static void receveur_greffe(void (*f)(void), int n){ "
            "appelee_seulement_par_le_receveur(); f(); (void)n; }\n"
            "void test_greffe_pointeur(){ receveur_greffe(aide_greffee_en_pointeur, 1); }\n"
        )
    }
    sources = _load(extra=faux)
    sources["test_behavior.cpp"] = sources["test_behavior.cpp"].replace(
        f'"{SUCCESS_MARKER}', f'test_greffe_pointeur(); std::cout << "{SUCCESS_MARKER}', 1
    )
    dead = dict(_dead(_definitions(sources)))
    assert "aide_greffee_en_pointeur" not in dead, (
        "Une aide passee en pointeur de fonction est declaree morte : le faux "
        "positif d'origine est revenu."
    )
    assert "receveur_greffe" not in dead, (
        "Une fonction dont un parametre est un pointeur de fonction n'est pas "
        "reconnue comme definition."
    )
    assert "appelee_seulement_par_le_receveur" not in dead, (
        "Le corps d'une fonction a parametre pointeur de fonction n'est pas "
        "parcouru : tout ce qu'elle appelle passe pour mort."
    )


def _entry_points(defs):
    """Les fonctions de test proprement dites : celles appelees directement par
    la racine, et celles appelees par les agregateurs `*_run_all_tests()`. Les
    autres sont des aides, dont on n'exige pas qu'elles affirment elles-memes."""
    by_name = _bodies_by_name(defs)
    points = set()
    for name in _calls(_root_body(defs)) & set(by_name):
        if name.endswith("_run_all_tests"):
            for body in by_name[name]:
                points |= _calls(body) & set(by_name)
        else:
            points.add(name)
    return points


def _asserts_transitively(by_name, name, seen=None):
    """Assertions atteignables depuis `name`, aides comprises.

    Un test qui delegue toutes ses assertions a une aide partagee est legitime :
    ce qui compte est qu'une assertion soit REELLEMENT executee sous lui.
    """
    if seen is None:
        seen = set()
    if name in seen or name not in by_name:
        return 0
    seen.add(name)
    total = 0
    for body in by_name[name]:
        total += len(re.findall(r"\bassert\s*\(", body))
        for callee in _calls(body):
            total += _asserts_transitively(by_name, callee, seen)
    return total


def test_no_native_test_function_runs_without_asserting_anything():
    """Tout point d'entree de test execute au moins une assertion."""
    defs = _definitions(_load())
    by_name = _bodies_by_name(defs)
    vacuous = sorted(
        name for name in _entry_points(defs)
        if _asserts_transitively(by_name, name) == 0
    )
    assert not vacuous, (
        "Tests executes qui n'affirment RIEN - ils passeraient tels quels sur un "
        "firmware casse :\n  " + "\n  ".join(vacuous)
    )


def test_le_detecteur_voit_reellement_un_test_vide():
    """Preuve par mutation du second detecteur, meme raison que pour le premier."""
    faux = {
        "__mutation2.cpp": (
            "void aide_greffee_qui_affirme(){ assert(1); }\n"
            "void test_greffe_qui_delegue(){ aide_greffee_qui_affirme(); }\n"
            "void test_greffe_vide(){ int x = 2; (void)x; }\n"
        )
    }
    sources = _load(extra=faux)
    sources["test_behavior.cpp"] = sources["test_behavior.cpp"].replace(
        f'"{SUCCESS_MARKER}',
        f'test_greffe_qui_delegue(); test_greffe_vide(); std::cout << "{SUCCESS_MARKER}',
        1,
    )
    defs = _definitions(sources)
    by_name = _bodies_by_name(defs)
    points = _entry_points(defs)
    assert {"test_greffe_vide", "test_greffe_qui_delegue"} <= points, (
        "La greffe n'a pas ete vue comme point d'entree : la mutation ne prouve "
        "rien en l'etat."
    )
    assert _asserts_transitively(by_name, "test_greffe_vide") == 0, (
        "Le detecteur de test vide est inoperant."
    )
    assert _asserts_transitively(by_name, "test_greffe_qui_delegue") > 0, (
        "Le detecteur compte comme vide un test qui delegue ses assertions a une "
        "aide : faux positif."
    )
