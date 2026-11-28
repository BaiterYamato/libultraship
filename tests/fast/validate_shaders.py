"""Compile the exact embedded source with the Windows D3DCompiler, no GPU needed."""
import ctypes as C
from ctypes import wintypes as W
from pathlib import Path
import re

header = Path('spatial_upscale_shader.h')
if not header.exists():
    header = Path('src/fast/backends/spatial_upscale_shader.h')
Path('validation').mkdir(exist_ok=True)
source = re.search(r'R"hlsl\((.*?)\)hlsl"', header.read_text(), re.S).group(1).encode()
dll = C.WinDLL('d3dcompiler_47.dll')
compile_shader = dll.D3DCompile
compile_shader.restype = C.c_long
compile_shader.argtypes = [C.c_void_p, C.c_size_t, C.c_char_p, C.c_void_p, C.c_void_p,
                          C.c_char_p, C.c_char_p, W.UINT, W.UINT,
                          C.POINTER(C.c_void_p), C.POINTER(C.c_void_p)]

def blob_data(ptr):
    vtable = C.cast(ptr, C.POINTER(C.POINTER(C.c_void_p))).contents
    address = C.WINFUNCTYPE(C.c_void_p, C.c_void_p)(vtable[3])(ptr)
    size = C.WINFUNCTYPE(C.c_size_t, C.c_void_p)(vtable[4])(ptr)
    return C.string_at(address, size)

def release(ptr):
    if ptr:
        vtable = C.cast(ptr, C.POINTER(C.POINTER(C.c_void_p))).contents
        C.WINFUNCTYPE(W.ULONG, C.c_void_p)(vtable[2])(ptr)

reports = []
for entry, profile in [('VSMain', 'vs_4_0'), ('VSMain', 'vs_5_0')] + [
    (e, p) for e in ['PSLinear', 'PSReconstruct', 'PSSharpen'] for p in ['ps_4_0', 'ps_5_0']
]:
    code, errors = C.c_void_p(), C.c_void_p()
    hr = compile_shader(source, len(source), b'spatial_upscale_shader', None, None,
                        entry.encode(), profile.encode(), 1 << 11, 0, C.byref(code), C.byref(errors))
    try:
        if errors:
            reports.append(blob_data(errors).decode(errors='replace').rstrip('\0'))
        if hr < 0:
            raise RuntimeError(f'{entry}/{profile}: HRESULT {hr & 0xffffffff:08x}\n' + '\n'.join(reports))
        data = blob_data(code)
        Path('validation', f'{entry}.{profile}.cso').write_bytes(data)
        reports.append(f'OK {entry}/{profile}: {len(data)} bytes')
    finally:
        release(code); release(errors)
Path('validation/shaders.log').write_text('\n'.join(reports) + '\n', encoding='utf-8')
print('\n'.join(reports))
