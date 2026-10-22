"""Group nodes compile like the same steps written inline (documentation/subgraphs_plan.md phase 2), with no editor: this writes a tiny
project in a temp folder (a sub-graph that adds its two inputs, a material that calls it twice, one call nested in another sub-graph) and runs
the Debug material compiler on it. Run: python xmaterial_subgraph_test.py [path to xmaterial_compiler.exe] (Windows; default: the Debug build)."""
import re, shutil, subprocess, sys, tempfile
from pathlib import Path

COMPILER = Path(sys.argv[1]) if __name__ == "__main__" and len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / "build" / "xmaterial_compiler.vs2022" / "Debug" / "xmaterial_compiler.exe"
M64 = (1 << 64) - 1


def t64(s):                                     # xresource::type_guid{"..."}
    h, m = 0x548c9decbce65297, 0xc6a4a7935bd1e995
    for c in s.encode():
        h = ((h ^ c) * m) & M64
    h ^= len(s); h ^= h >> 33; h = (h * 0x85ebca6b) & M64; h ^= h >> 13; h = (h * 0xc2b2ae35) & M64; h ^= h >> 16
    return h


def mix(a, b):                                  # xmaterial_graph::MixGuid, also graph::CreateNode's pin ids (b = pin number)
    z = (a + b * 0x9E3779B97F4A7C15) & M64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)


VEC4, VEC3 = t64("Vector4"), t64("Vector3")
P_ADD, P_CONST, P_OUT = t64("addVec4Prefabs"), t64("Vec4 Const Node"), t64("fragoutprefabs")
P_GIN, P_GOUT, P_CALL = t64("GroupInputPrefabs"), t64("GroupOutputPrefabs"), t64("SubgraphCallPrefabs")
SUBGRAPH_TYPE = 0xE52C32C54E8C5117


class Graph:
    """Writes a graph file: nodes (prefab, guid, property lines) and connections (from pin, to pin)."""
    def __init__(self):
        self.nodes, self.conns, self.params = [], [], []    # params: (id, name, type): a v3 file, every step in one section

    def node(self, guid, prefab, name, props=()):
        self.nodes.append((guid, prefab, [("node/StepName", "string", f'"{name}"'), *props]))

    def link(self, guid, frm, to):
        self.conns.append((guid, frm, to))
        for g, p, props in self.nodes:          # the input remembers its connection
            for i, (k, t, v) in enumerate(props):
                if t == "u64" and k.endswith("/pin/PinGUID/Value") and v == f"#{to:X}":
                    props.append((k.replace("/pin/PinGUID/Value", "/ConnectionGUID/Value"), "u64", f"#{guid:X}"))
                    return
        for g, p, props in self.nodes:          # a node type's own input (derived pin): only the connection
            for n in range(1, 9):
                if mix(g, n) == to:
                    props.append((f"node/Input Pins[G:{n - 1}]/ConnectionGUID/Value", "u64", f"#{guid:X}"))
                    return
        raise AssertionError(f"no input {to:X}")

    def text(self):
        types = "< s32:d u32:g s16:C u16:H s8:c u8:h f32:f f64:F string:s wstring:S u64:G s64:D bool:c full_guid:GG enum:s >"
        if self.params:
            out = ["", "[ Info ]", "{ nNodes:d  nConnections:d  Version:d  nParameters:d }", "//--------  --------------  ---------  -------------",
                   f"     {len(self.nodes)}            {len(self.conns)}             3            {len(self.params)}", ""]
        else:
            out = ["", "[ Info ]", "{ nNodes:d  nConnections:d }", "//--------  --------------", f"     {len(self.nodes)}            {len(self.conns)}", ""]
        for g, p, props in self.nodes:
            out += ["[ Node ]", "{ Guid:G             PrefabGuid:G      }", "//-----------------  -----------------", f"  #{g:X}  #{p:X}", "", types, "",
                    f"[ xProperties : {len(props)} ]", "{ Name:s  Value:? }", "//------  --------"]
            out += [f'  "{k}" ;{t} {v}' for k, t, v in props] + [""]
        for c, frm, to in self.conns:
            out += ["[ Connection ]", "{ Guid:G             }", "//-----------------", f"  #{c:X}", "", types, "",
                    "[ xProperties : 2 ]", "{ Name:s  Value:? }", "//------  --------",
                    f'  "connection/InputPinGuid/Value" ;u64 #{to:X}', f'  "connection/OutputPinGuid/Value" ;u64 #{frm:X}', ""]
        if self.params:
            st = [("stack/Sections[]", "s64", "1"), ("stack/Sections[G:0]/Guid/Value", "u64", "#5EC7"), ("stack/Sections[G:0]/Name", "string", '"Steps"'),
                  ("stack/Sections[G:0]/Kind", "enum", '"USER"'), ("stack/Sections[G:0]/Stage", "enum", '"FRAGMENT"'), ("stack/Sections[G:0]/bCollapsed", "bool", "0"),
                  ("stack/Sections[G:0]/Steps[]", "s64", str(len(self.nodes)))]
            st += [(f"stack/Sections[G:0]/Steps[G:{i}]/Value", "u64", f"#{g:X}") for i, (g, p, props) in enumerate(self.nodes)]
            out += ["[ StackLayout ]", "{ nSections:d }", "//-----------", "       1", "", types, "", f"[ xProperties : {len(st)} ]", "{ Name:s  Value:? }", "//------  --------"]
            out += [f'  "{k}" ;{t} {v}' for k, t, v in st] + [""]
            for pid, name, typ in self.params:
                pr = [("param_decl/ID", "u64", f"#{pid:X}"), ("param_decl/Name", "string", f'"{name}"'), ("param_decl/Type", "enum", f'"{typ}"'), ("param_decl/Kind", "enum", '"DYNAMIC"'),
                      ("param_decl/Default/X", "f32", "0.5")]
                out += ["[ Parameter ]", "{ Guid:G }", "//------", f"  #{pid:X}", "", types, "", f"[ xProperties : {len(pr)} ]", "{ Name:s  Value:? }", "//------  --------"]
                out += [f'  "{k}" ;{t} {v}' for k, t, v in pr] + [""]
        return "\n".join(out) + "\n"


def pins(side, items):
    """Explicit pins (Group Input/Output, group nodes): (name, type, guid)."""
    key = "Input Pins" if side == "in" else "Output Pins"
    r = [(f"node/{key}[]", "s64", str(len(items)))]
    for i, (name, typ, guid) in enumerate(items):
        r += [(f"node/{key}[G:{i}]/pin/var/Name", "string", f'"{name}"'), (f"node/{key}[G:{i}]/pin/var/TypeGUID", "u64", f"#{typ:X}"),
              (f"node/{key}[G:{i}]/pin/PinGUID/Value", "u64", f"#{guid:X}")]
    return r


def adder(guid, a, b):                          # the sub-graph "Adder": Sum = A + B
    g = Graph()
    gi, add, go = guid ^ 0xA1, guid ^ 0xA2, guid ^ 0xA3
    pa, pb, ps = mix(gi, 101), mix(gi, 102), mix(go, 103)
    g.node(gi, P_GIN, "Inputs", pins("out", [(a, VEC4, pa), (b, VEC4, pb)]))
    g.node(add, P_ADD, "Add")
    g.node(go, P_GOUT, "Outputs", pins("in", [("Sum", VEC4, ps)]))
    g.link(guid ^ 0xC1, pa, mix(add, 1)); g.link(guid ^ 0xC2, pb, mix(add, 2)); g.link(guid ^ 0xC3, mix(add, 3), ps)
    return g, (pa, pb, ps)


def subref(sub):                                # a group node's sub-graph
    return [("node/Params[]", "s64", "1"), ("node/Params[G:0]/Name", "string", '"Subgraph"'), ("node/Params[G:0]/Type", "enum", '"SUBGRAPH RESOURCE"'),
            ("node/Params[G:0]/SubgraphRef", "full_guid", f"#{sub:X} #{SUBGRAPH_TYPE:X}")]


def call(g, guid, sub, iface, name):            # a group node calling sub: pins mirror its interface
    pa, pb, ps = iface
    g.node(guid, P_CALL, name, [*pins("in", [("A", VEC4, mix(guid, pa)), ("B", VEC4, mix(guid, pb))]), *pins("out", [("Sum", VEC4, mix(guid, ps))]), *subref(sub)])
    return mix(guid, pa), mix(guid, pb), mix(guid, ps)


def library_config(root, parents):              # Project.config/Library.config.txt as the editor writes it: only what the compiler reads
    esc = lambda p: str(p).replace("\\", "\\\\")
    props = [("Library/GUID", "u64", "#1234"), ("Library/Path", "wstring", f'"{esc(root)}"'), ("Library/ParentLibraries[]", "s64", str(len(parents)))]
    for i, p in enumerate(parents):
        props += [(f"Library/ParentLibraries[G:{i}]/GUID", "u64", f"#{0x5678 + i:X}"), (f"Library/ParentLibraries[G:{i}]/Path", "wstring", f'"{esc(p)}"')]
    types = "< s32:d u32:g s16:C u16:H s8:c u8:h f32:f f64:F string:s wstring:S u64:G s64:D bool:c full_guid:GG enum:s >"
    return "\n".join(["// New Types", types, "", f"[ xProperties : {len(props)} ]", "{ Name:s  Value:? }", "//------  --------", *[f'  "{k}" ;{t} {v}' for k, t, v in props]]) + "\n"


def write(root, kind, guid, g):
    d = root / "Descriptors" / kind / f"{guid & 0xff:02X}" / f"{(guid >> 8) & 0xff:02X}" / f"{guid:X}.desc"
    d.mkdir(parents=True, exist_ok=True)
    (d / "Descriptor.txt").write_text(g.text())
    return d.relative_to(root)


def compile(root, desc):
    r = subprocess.run([str(COMPILER), "-PROJECT", str(root), "-OPTIMIZATION", "O1", "-DEBUG", "D0", "-DESCRIPTOR", str(desc),
                        "-OUTPUT", str(root / "Cache" / "Resources" / "Platforms" / "WINDOWS")], capture_output=True, text=True, cwd=root)
    return r.returncode, r.stdout + r.stderr


def material(root, guid, body):
    m = Graph()
    k = guid ^ 0x10                             # a constant (0.25, 0.5, 0.75, 1) and the output
    m.node(k, P_CONST, "K", [(f"node/Params[G:{i}]/Float", "f32", v) for i, v in enumerate(["0.25", "0.5", "0.75", "1"])])
    m.node(guid ^ 0x30, P_OUT, "Out")
    out = body(m, mix(k, 5))                    # Vec4 Const: inputs 1..4, the output 5
    m.link(guid ^ 0xC9, out, mix(guid ^ 0x30, 1))
    return write(root, "Material", guid, m)


def glsl(log):
    i = log.find("#version")
    return log[i:log.find("Shader compiled", i)] if i >= 0 else ""


def main():
    assert COMPILER.exists(), f"build the compiler first: {COMPILER}"
    root = Path(tempfile.mkdtemp(prefix="xmat_subgraph_")) / "test.lionprj"          # the compiler takes only a project
    root.mkdir()
    ADDER, TWICE = 0x5B00000000000101, 0x5B00000000000201
    sub, iface = adder(ADDER, "A", "B")
    write(root, "MaterialSubgraph", ADDER, sub)

    # "Twice": a sub-graph whose steps are a group node (nesting): Sum = A + B through Adder
    tw = Graph()
    gi, go, inner = TWICE ^ 0xA1, TWICE ^ 0xA3, TWICE ^ 0xA4
    pa, pb, ps = mix(gi, 101), mix(gi, 102), mix(go, 103)
    tw.node(gi, P_GIN, "Inputs", pins("out", [("A", VEC4, pa), ("B", VEC4, pb)]))
    tw.node(go, P_GOUT, "Outputs", pins("in", [("Sum", VEC4, ps)]))
    ia, ib, isum = call(tw, inner, ADDER, iface, "Inner")
    tw.link(TWICE ^ 0xC1, pa, ia); tw.link(TWICE ^ 0xC2, pb, ib); tw.link(TWICE ^ 0xC3, isum, ps)
    write(root, "MaterialSubgraph", TWICE, tw)

    # The sub-graphs compile (checked, interface written)
    for g in (ADDER, TWICE):
        rc, log = compile(root, Path("Descriptors") / "MaterialSubgraph" / f"{g & 0xff:02X}" / f"{(g >> 8) & 0xff:02X}" / f"{g:X}.desc")
        assert rc == 0 and "in Vector4 A" in log and "out Vector4 Sum" in log, log[-2000:]

    def inline(m, k):                           # the same sum written inline
        m.node(0x77, P_ADD, "Add"); m.link(0xD1, k, mix(0x77, 1)); m.link(0xD2, k, mix(0x77, 2)); return mix(0x77, 3)

    def grouped(m, k):
        a, b, s = call(m, 0x88, ADDER, iface, "G"); m.link(0xD1, k, a); m.link(0xD2, k, b); return s

    def nested(m, k):
        twice_iface = (pa, pb, ps)
        a, b, s = call(m, 0x99, TWICE, twice_iface, "T"); m.link(0xD1, k, a); m.link(0xD2, k, b); return s

    results = {}
    for name, body, guid in (("inline", inline, 0x7E57000000000101), ("grouped", grouped, 0x7E57000000000201), ("nested", nested, 0x7E57000000000301)):
        rc, log = compile(root, material(root, guid, body))
        assert rc == 0, f"{name}:\n{log[-3000:]}"
        results[name] = glsl(log)
        assert results[name], f"{name}: no shader in the log"

    # The same code but for the names: Add -> G_Add -> T_Inner_Add
    norm = lambda s: re.sub(r"\b(t_inner_add|g_add|add)\b", "ADD", s.lower())
    assert norm(results["grouped"]) == norm(results["inline"]), results["grouped"] + "\n---\n" + results["inline"]
    assert norm(results["nested"]) == norm(results["inline"]), results["nested"] + "\n---\n" + results["inline"]

    # The compiled materials are byte-identical: names do not reach SPIR-V
    data = {g: next((root / "Cache" / "Resources" / "Platforms" / "WINDOWS" / "Material").rglob(f"{g:X}")).read_bytes() for g in (0x7E57000000000101, 0x7E57000000000201, 0x7E57000000000301)}
    assert len(set(data.values())) == 1, "the grouped materials compile to other SPIR-V than the inline one"

    # A sub-graph of another library is found once the project names that library (a dependency, in its Library.config.txt)
    lib = root.parent / "shared.lionlib"
    OTHER = 0x5B00000000000301
    osub, oface = adder(OTHER, "A", "B")
    write(lib, "MaterialSubgraph", OTHER, osub)

    def other(m, k):
        a, b, s = call(m, 0xAA, OTHER, oface, "O"); m.link(0xD1, k, a); m.link(0xD2, k, b); return s
    rc, log = compile(root, material(root, 0x7E57000000000401, other))
    assert rc != 0 and "not there" in log, log[-2000:]
    (root / "Project.config").mkdir()
    (root / "Project.config" / "Library.config.txt").write_text(library_config(root, [lib]))
    rc, log = compile(root, material(root, 0x7E57000000000401, other))
    assert rc == 0, log[-3000:]
    assert re.sub(r"\bo_add\b", "ADD", glsl(log).lower()) == norm(results["inline"]), glsl(log)

    # A sub-graph with a material parameter: the materials that use it declare it, once however many group nodes read it
    TINT, PID = 0x5B00000000000401, 0x9A9A00000000F00D
    def tinted_subgraph(params):
        t = Graph()
        gi, k2, add, go = TINT ^ 0xA1, TINT ^ 0xA5, TINT ^ 0xA2, TINT ^ 0xA3
        t.node(gi, P_GIN, "Inputs", pins("out", [("A", VEC4, mix(gi, 101))]))
        t.node(k2, P_CONST, "Amount", [("node/Params[G:0]/Float", "f32", "0.5"), ("node/Params[G:0]/ParamID", "u64", f"#{PID:X}")])
        t.node(add, P_ADD, "Add")
        t.node(go, P_GOUT, "Outputs", pins("in", [("Sum", VEC4, mix(go, 103))]))
        t.link(TINT ^ 0xC1, mix(gi, 101), mix(add, 1)); t.link(TINT ^ 0xC2, mix(k2, 5), mix(add, 2)); t.link(TINT ^ 0xC3, mix(add, 3), mix(go, 103))
        t.params = params
        return t, mix(gi, 101), mix(go, 103)
    tdesc = Path("Descriptors") / "MaterialSubgraph" / f"{TINT & 0xff:02X}" / f"{(TINT >> 8) & 0xff:02X}" / f"{TINT:X}.desc"

    t, pa1, ps1 = tinted_subgraph([])                                  # reading a parameter it does not declare: refused
    write(root, "MaterialSubgraph", TINT, t)
    rc, log = compile(root, tdesc)
    assert rc != 0 and "does not declare" in log, log[-2000:]

    t, pa1, ps1 = tinted_subgraph([(PID, "Amount", "FLOAT")])
    write(root, "MaterialSubgraph", TINT, t)
    rc, log = compile(root, tdesc)
    assert rc == 0 and "param FLOAT Amount" in log, log[-2000:]

    def tinted(m, k):
        s = k
        for n, g in enumerate((0xB1, 0xB2)):
            m.node(g, P_CALL, f"T{n}", [*pins("in", [("A", VEC4, mix(g, pa1))]), *pins("out", [("Sum", VEC4, mix(g, ps1))]), *subref(TINT)])
            m.link(0xE0 + n, s, mix(g, pa1)); s = mix(g, ps1)
        return s
    rc, log = compile(root, material(root, 0x7E57000000000501, tinted))
    assert rc == 0 and "E_PARAM" not in log, log[-3000:]
    assert glsl(log).count("g_Mat.P[XMAT_BASE + 0u].x") == 2, glsl(log)     # both group nodes read the one parameter

    # A loop is refused: Adder calling Twice which calls Adder
    loop, _ = adder(ADDER, "A", "B")
    call(loop, 0x55, TWICE, (pa, pb, ps), "Loop")
    write(root, "MaterialSubgraph", ADDER, loop)
    rc, log = compile(root, Path("Descriptors") / "MaterialSubgraph" / f"{TWICE & 0xff:02X}" / f"{(TWICE >> 8) & 0xff:02X}" / f"{TWICE:X}.desc")
    assert rc != 0 and "uses itself" in log, log[-2000:]
    shutil.rmtree(root.parent, ignore_errors=True)
    print("ok")


if __name__ == "__main__":
    main()
