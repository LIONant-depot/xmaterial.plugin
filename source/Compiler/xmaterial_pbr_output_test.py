# Standalone check of "Output: PBR" (not in CMake): compiles a copy of SliceMat (documentation/material_contract.md section 11, lit by
# Output: PBR) and checks both variants build with its Roughness parameter reaching PBRLighting. The copy's descriptor, log and resource
# are temporary and removed.
#   python xmaterial_pbr_output_test.py [path to xmaterial_compiler.exe]     (default: the Debug build)
import os, shutil, subprocess, sys

HERE    = os.path.dirname(os.path.realpath(__file__))                                    # the plugin folder is a link into the project's Cache/Plugins
PROJECT = os.path.normpath(os.path.join(HERE, '../../../../..'))                     # example.lionprj
EXE     = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '../../build/xmaterial_compiler.vs2022/Debug/xmaterial_compiler.exe')
SLICE   = os.path.join(PROJECT, 'Descriptors/Material/01/00/5E1CE0A110000001.desc/Descriptor.txt')
REL     = r'Cache\Descriptors\Material\01\BD\7E570BD0000001.desc'                    # a guid no asset has
TEMP    = [os.path.join(PROJECT, REL), os.path.join(PROJECT, r'Cache\Resources\Logs\Material\01\BD\7E570BD0000001.log'),
           os.path.join(PROJECT, r'Cache\Resources\Platforms\WINDOWS\Material\01\BD\7E570BD0000001')]

def main():
    os.makedirs(TEMP[0], exist_ok=True)
    try:
        shutil.copy(SLICE, TEMP[0])
        R = subprocess.run([EXE, '-PROJECT', PROJECT, '-OPTIMIZATION', 'O1', '-DEBUG', 'D0', '-DESCRIPTOR', REL,
                            '-OUTPUT', os.path.join(PROJECT, r'Cache\Resources\Platforms\WINDOWS')], capture_output=True, text=True, errors='replace', timeout=120)
        Log = R.stdout + R.stderr
        assert R.returncode == 0 and '[COMPILATION_SUCCESS]' in Log, Log[-3000:]
        assert 'PBRLighting(vec3(0.0, 0.0, 1.0), vec3(ToRGB.xyz), 1.0, 0.04, Roughness, 0.0, vec3(0.0))' in Log, 'the roughness does not reach the lighting'
        assert '#define XMAT_VARYING_IN' in Log and '#define XLION_NO_SHADOW_MAP' in Log
    finally:
        for p in TEMP: shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else (os.path.exists(p) and os.remove(p))
    print('xmaterial_pbr_output_test: ok')

if __name__ == '__main__': main()
