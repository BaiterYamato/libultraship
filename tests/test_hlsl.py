"""Compila fixtures das amostragens reais do template, sem substituir o Prism.

Usa somente a biblioteca padrao Python e D3DCompiler_47.dll. O combiner N64,
VS e recursos sao fixtures; corpo de amostragem e helper 3-point sao extraidos
do template editado. Nao e uma execucao do gerador Prism completo.
"""
import ctypes
from ctypes import wintypes
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
TEMPLATE = HERE / "default.shader.hlsl"
if not TEMPLATE.exists():
    TEMPLATE = HERE.parent / "src/fast/shaders/directx/default.shader.hlsl"
OUT = HERE / "hlsl_examples"
OUT.mkdir(exist_ok=True)
source = TEMPLATE.read_text(encoding="utf-8")
helper = source[source.index("cbuffer PerDrawCB"):source.index("@end", source.index("cbuffer PerDrawCB"))]
sampling = source[source.index("    @for(i in 0..2)", source.index("PSOutput PSMain")):]
sampling = sampling[:sampling.index("    @if(o_alpha)")]


def expr(text, env):
    # Somente expressoes do bloco local confiavel, sem conteudo externo.
    text = text.replace("&&", " and ").replace("||", " or ")
    text = re.sub(r"!(?!=)", " not ", text)
    return eval(text, {"__builtins__": {}}, env)


def render(lines, env):
    result = []
    i = 0
    while i < len(lines):
        line = lines[i]
        tag = line.strip()
        if tag.startswith("@if(") or tag.startswith("@for("):
            depth, end = 1, i + 1
            while depth:
                other = lines[end].strip()
                if other.startswith(("@if(", "@for(")):
                    depth += 1
                elif other == "@end":
                    depth -= 1
                end += 1
            body = lines[i + 1:end - 1]
            if tag.startswith("@for("):
                name, start, stop = re.fullmatch(r"@for\((\w+) in (\d+)\.\.(\d+)\)", tag).groups()
                for value in range(int(start), int(stop)):
                    result.append(render(body, dict(env, **{name: value})))
            else:
                branches = [(tag[4:-1], [])]
                nested = 0
                for row in body:
                    t = row.strip()
                    if nested == 0 and t.startswith("@elseif("):
                        branches.append((t[8:-1], []))
                    elif nested == 0 and t == "@else":
                        branches.append(("True", []))
                    else:
                        branches[-1][1].append(row)
                        if t.startswith(("@if(", "@for(")):
                            nested += 1
                        elif t == "@end":
                            nested -= 1
                for condition, rows in branches:
                    if expr(condition, env):
                        result.append(render(rows, dict(env)))
                        break
            i = end
            continue
        assignment = re.fullmatch(r"\s*@\{(\w+) = (.+)\}\s*", line)
        if assignment:
            env[assignment[1]] = expr(assignment[2], env)
        else:
            result.append(re.sub(r"@\{([^}]+)\}", lambda m: str(expr(m[1], env)), line) + "\n")
        i += 1
    return "".join(result)


compiler = ctypes.WinDLL("D3DCompiler_47.dll")
compile_shader = compiler.D3DCompile
compile_shader.restype = ctypes.c_long
compile_shader.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p,
                           ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, wintypes.UINT,
                           wintypes.UINT, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]


def blob_read(blob):
    table = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(table[3])(blob)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(table[4])(blob)
    return ctypes.string_at(ptr, size)


def release(blob):
    if blob:
        table = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(table[2])(blob)


resources = """
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1;
    float texClampS0 : TEXCLAMPS0; float texClampT0 : TEXCLAMPT0;
    float texClampS1 : TEXCLAMPS1; float texClampT1 : TEXCLAMPT1;
};
PSInput VSMain(PSInput input) { return input; }
"""
for n, name in enumerate(("g_texture0", "g_texture1", "g_textureMask0", "g_textureMask1",
                          "g_textureBlend0", "g_textureBlend1")):
    resources += f"Texture2D {name} : register(t{n});\n"
for n in range(2):
    resources += f"SamplerState g_sampler{n} : register(s{n});\n"
    resources += f"SamplerState g_pointSampler{n} : register(s{8 + n});\n"

cases = []
for three in (False, True):
    for two in (False, True):
        for mask in (False, True):
            for clamp in ((False, False), (True, False), (False, True), (True, True)):
                cases.append((three, two, mask, clamp))

for index, (three, two, mask, clamp) in enumerate(cases):
    env = dict(o_textures=[True, two], o_three_point_filtering=three,
               o_masks=[mask, mask], o_blend=[mask, mask], o_clamp=[clamp, clamp])
    body = render(sampling.splitlines(), env)
    assert "@" not in body
    hlsl = resources + (helper if three else "") + "float4 PSMain(PSInput input) : SV_TARGET {\n" + body
    hlsl += "float4 texel = " + ("(texVal0 + texVal1) * 0.5" if two else "texVal0") + ";\n"
    hlsl += "if (texel.a <= 0.19) discard; return texel; }\n"
    name = f"sample_{index:02d}_{'3point' if three else 'linear'}"
    (OUT / (name + ".hlsl")).write_text(hlsl, encoding="utf-8")
    data = hlsl.encode()
    for entry, target in ((b"VSMain", b"vs_4_0"), (b"PSMain", b"ps_4_0")):
        code, errors = ctypes.c_void_p(), ctypes.c_void_p()
        hr = compile_shader(data, len(data), name.encode(), None, None, entry, target,
                            (1 << 14) | (1 << 15) | (1 << 18), 0, ctypes.byref(code), ctypes.byref(errors))
        # Optimization level 2 + warnings as errors.
        try:
            if hr < 0:
                raise RuntimeError(f"{name} {target.decode()}: " + blob_read(errors).decode(errors="replace"))
            (OUT / (name + "." + target.decode() + ".cso")).write_bytes(blob_read(code))
        finally:
            release(code)
            release(errors)
print(f"PASS: {len(cases)} sampling fixtures, {len(cases) * 2} VS/PS Shader Model 4.0 compilations")
