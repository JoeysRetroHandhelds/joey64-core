# texpack_shaders.py: builds parallel-rdp/shaders/slangmosh_texpack.hpp, the texture-pack variants (TEXPACK=1) of
# parallel-RDP's ubershader and rasterizer, beside the stock slangmosh.hpp, which stays untouched (so a core without a
# pack runs byte-for-byte the same shaders as before).
#
# slangmosh (Granite) is not needed: the NDK's glslc compiles each variant (the same defines slangmosh resolves), and the
# binding reflection is the stock variant's, read from slangmosh.hpp, plus the one new storage buffer (binding 13: set 1
# for the ubershader, set 0 for the rasterizer). Each SPIR-V is checked with spirv-val (Vulkan 1.1).
#
# Usage: python texpack_shaders.py [ndk shader-tools dir]
# Joey64 is GPL-3.0-or-later.
import os, re, struct, subprocess, sys, glob

HERE = os.path.dirname(os.path.abspath(__file__))
SHADERS = os.path.join(HERE, '..', 'parallel-rdp', 'parallel-rdp', 'shaders')
STOCK = os.path.join(SHADERS, 'slangmosh.hpp')
OUT = os.path.join(SHADERS, 'slangmosh_texpack.hpp')

def tools_dir():
    if len(sys.argv) > 1:
        return sys.argv[1]
    sdk = os.environ.get('ANDROID_HOME') or os.path.expanduser('~/AppData/Local/Android/Sdk')
    cands = sorted(glob.glob(os.path.join(sdk, 'ndk', '*', 'shader-tools', '*')))
    if not cands:
        sys.exit('no NDK shader-tools found; pass the directory')
    return cands[-1]

TOOLS = tools_dir()
GLSLC = os.path.join(TOOLS, 'glslc')
SPIRV_VAL = os.path.join(TOOLS, 'spirv-val')

# Granite ResourceLayout serialization (vulkan/shader.hpp): magic u16[4] "GRA",4, then 4 DescriptorSetLayouts of
# 11 u32 masks + u8 array_size[32] + u32 padding (80 bytes), then 5 u32. storage_buffer_mask is the 4th mask.
REFL_SIZE = 348
SET_SIZE = 80
STORAGE_BUFFER_MASK = 3 * 4
ARRAY_SIZE = 11 * 4
TEXPACK_BINDING = 13

# name -> (texel buffer set, [(define values in slangmosh.hpp's order)])
SHADERS_TO_BUILD = {
    'ubershader': (1, ['SUBGROUP', 'SMALL_TYPES']),
    'rasterizer': (0, ['SMALL_TYPES']),
}

def parse_stock():
    text = open(STOCK).read()
    refl_src = re.search(r'reflection_bank\[\] =\s*\{(.*?)\};', text, re.S).group(1)
    refl = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', refl_src))
    # each variant: the resolver conditions, then layout.unserialize(reflection_bank + R, 348); this->name = ...
    variants = {}
    for m in re.finditer(r'if \(((?:resolver\("(\w+)", "(\w+)"\) == (\d)(?: &&\s*)?)+)\)\s*\{\s*'
                         r'layout\.unserialize\(reflection_bank \+ (\d+), 348\);\s*this->(\w+) =', text):
        conds = dict((d, int(v)) for _, d, v in re.findall(r'resolver\("(\w+)", "(\w+)"\) == (\d)', m.group(1)))
        variants.setdefault(m.group(6), []).append((conds, int(m.group(5))))
    for m in re.finditer(r'\n\t\tlayout\.unserialize\(reflection_bank \+ (\d+), 348\);\s*this->(\w+) =', text):
        variants.setdefault(m.group(2), [({}, int(m.group(1)))])
    return refl, variants

def patch_reflection(r, set_index):
    r = bytearray(r)
    base = 8 + set_index * SET_SIZE
    mask, = struct.unpack_from('<I', r, base + STORAGE_BUFFER_MASK)
    assert not (mask >> TEXPACK_BINDING) & 1, 'binding %d already used' % TEXPACK_BINDING
    struct.pack_into('<I', r, base + STORAGE_BUFFER_MASK, mask | (1 << TEXPACK_BINDING))
    assert r[base + ARRAY_SIZE + TEXPACK_BINDING] == 0
    r[base + ARRAY_SIZE + TEXPACK_BINDING] = 1
    return bytes(r)

def compile_variant(name, conds, out):
    args = [GLSLC, '--target-env=vulkan1.1', '-O', '-fshader-stage=compute', '-DTEXPACK=1']
    # dev measurements only (the committed header is built without): e.g. TEXPACK_DEFINES="-DTEXPACK_NOSAMPLE=1"
    args += os.environ.get('TEXPACK_DEFINES', '').split()
    args += ['-D%s=%d' % (d, v) for d, v in sorted(conds.items())]
    args += [os.path.join(SHADERS, name + '.comp'), '-o', out]
    subprocess.check_call(args)
    subprocess.check_call([SPIRV_VAL, '--target-env', 'vulkan1.1', out])
    data = open(out, 'rb').read()
    assert len(data) % 4 == 0
    return data

def main():
    refl_stock, variants = parse_stock()
    tmp = os.path.join(HERE, 'texpack_tmp.spv')
    spirv_words, refl_out, code = [], bytearray(), []
    members = []
    for name, (set_index, defines) in SHADERS_TO_BUILD.items():
        members.append(name)
        for conds, roff in variants[name]:
            assert set(conds) == set(defines), (name, conds)
            spv = compile_variant(name, conds, tmp)
            woff = len(spirv_words)
            spirv_words += struct.unpack('<%dI' % (len(spv) // 4), spv)
            rnew = patch_reflection(refl_stock[roff:roff + REFL_SIZE], set_index)
            ro = len(refl_out)
            refl_out += rnew
            cond = ' &&\n\t\t    '.join('resolver("%s", "%s") == %d' % (name, d, conds[d]) for d in defines)
            code.append('\t\tif (%s)\n\t\t{\n\t\t\tlayout.unserialize(texpack_reflection_bank + %d, %d);\n'
                        '\t\t\tthis->%s = device.request_program(texpack_spirv_bank + %d, %d, &layout);\n\t\t}\n'
                        % (cond, ro, REFL_SIZE, name, woff, len(spv)))
            print('%s %s: %d bytes' % (name, conds, len(spv)))
    os.remove(tmp)
    lines = ['// Generated by mupen64plus-video-paraLLEl/joey/texpack_shaders.py from the shader sources (TEXPACK=1) and',
             "// slangmosh.hpp's reflection; do not edit. Texture-pack variants of the ubershader and rasterizer.",
             '#pragma once', '#include <stdint.h>', 'namespace RDP', '{',
             'static const uint32_t texpack_spirv_bank[] =', '{']
    for i in range(0, len(spirv_words), 8):
        lines.append('\t' + ' '.join('0x%08xu,' % w for w in spirv_words[i:i + 8]))
    lines += ['};', '', 'static const uint8_t texpack_reflection_bank[] =', '{']
    for i in range(0, len(refl_out), 32):
        lines.append('\t' + ' '.join('0x%02x,' % b for b in refl_out[i:i + 32]))
    lines += ['};', '', 'template <typename Program, typename Shader>', 'struct ShadersTexpack', '{']
    lines += ['\tProgram %s = {};' % m for m in members]
    lines += ['', '\ttemplate <typename Device, typename Layout, typename Resolver>',
              '\tShadersTexpack(Device &device, Layout &layout, const Resolver &resolver)', '\t{']
    out = '\n'.join(lines) + '\n' + ''.join(code) + '\t}\n};\n}\n'
    open(OUT, 'w', newline='\n').write(out)
    print('wrote', OUT)

main()
