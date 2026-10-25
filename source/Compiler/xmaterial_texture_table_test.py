# Standalone check of the texture-table variants (not in CMake): compiles a copy of SliceMat (a graph with textures) and checks its textures
# are declared for the three variants - LDR sampler i, bindless the heaps by the ids after the block (XGPU_BINDLESS), else binding i of the
# texture table's set 3 - and that all three compile. The copy's descriptor, log and resource are temporary and removed.
#   python xmaterial_texture_table_test.py [path to xmaterial_compiler.exe]     (default: the Debug build)
import os, shutil, subprocess, sys

HERE    = os.path.dirname(os.path.realpath(__file__))                                    # the plugin folder is a link into the project's Cache/Plugins
PROJECT = os.path.normpath(os.path.join(HERE, '../../../../..'))                     # example.lionprj
EXE     = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '../../build/xmaterial_compiler.vs2022/Debug/xmaterial_compiler.exe')
SLICE   = os.path.join(PROJECT, 'Descriptors/Material/01/00/5E1CE0A110000001.desc/Descriptor.txt')
REL     = r'Cache\Descriptors\Material\03\BD\7E570BD0000003.desc'                    # a guid no asset has
TEMP    = [os.path.join(PROJECT, REL), os.path.join(PROJECT, r'Cache\Resources\Logs\Material\03\BD\7E570BD0000003.log'),
           os.path.join(PROJECT, r'Cache\Resources\Platforms\WINDOWS\Material\03\BD\7E570BD0000003')]

def main():
    os.makedirs(TEMP[0], exist_ok=True)
    try:
        shutil.copy(SLICE, TEMP[0])
        R = subprocess.run([EXE, '-PROJECT', PROJECT, '-OPTIMIZATION', 'O1', '-DEBUG', 'D0', '-DESCRIPTOR', REL,
                            '-OUTPUT', os.path.join(PROJECT, r'Cache\Resources\Platforms\WINDOWS')], capture_output=True, text=True, errors='replace', timeout=120)
        Log = R.stdout + R.stderr
        assert R.returncode == 0 and '[COMPILATION_SUCCESS]' in Log, Log[-3000:]
        assert '#if defined(XLION_LDR_OUTPUT)\nlayout(binding = 0) uniform sampler2D' in Log, 'the LDR variant does not bind texture 0 as sampler 0'
        assert '#elif defined(XGPU_BINDLESS)\n#define ' in Log and 'XGPU_TEXTURE(XMAT_TEXID(0u))' in Log, 'no bindless declaration'
        assert '#else\nlayout(set = 3, binding = 0) uniform sampler2D' in Log, 'no texture table declaration'
        assert 'layout(set = 3, binding = 0) uniform texture2D xgpu_Textures2D[];' in Log, 'no heaps'
        assert 'XMAT_SAMPLER(' not in Log.split('#version 450', 1)[-1], 'a texture still binds through XMAT_SAMPLER'
    finally:
        for p in TEMP: shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else (os.path.exists(p) and os.remove(p))
    print('xmaterial_texture_table_test: ok')

if __name__ == '__main__': main()
