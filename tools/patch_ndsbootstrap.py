#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else 'source').resolve()


def read(rel):
    p = ROOT / rel
    if not p.exists():
        raise SystemExit(f'missing {p}')
    return p, p.read_text()


def write(p, text):
    p.write_text(text)
    print('patched', p.relative_to(ROOT))


def replace_once(text, old, new, label):
    n = text.count(old)
    if n != 1:
        raise SystemExit(f'{label}: expected exactly 1 match, got {n}')
    return text.replace(old, new, 1)

# ---------------------------------------------------------------------------
# Fixed addresses: deliberately only for the first standard-DS-on-DSi test.
# ---------------------------------------------------------------------------
p, s = read('retail/common/include/locations.h')
marker = '#endif // LOCATIONS_H'
insert = r'''

/* DSiCast aggressive in-game experiment (standard DS titles on DSi only). */
#define DSICAST_SHARED_LOCATION        0x026D6800
#define DSICAST_SHARED_MAGIC           0x55435344
#define DSICAST_ARM9_RESIDENT_LOCATION 0x02F92000
#define DSICAST_ARM9_RESIDENT_LIMIT    0x00036000
#define DSICAST_ARM7_RESIDENT_LOCATION 0x02FC8000
#define DSICAST_ARM7_RESIDENT_LIMIT    0x00010000
'''
if 'DSICAST_ARM9_RESIDENT_LOCATION' not in s:
    s = replace_once(s, marker, insert + '\n' + marker, 'locations')
write(p, s)

# ---------------------------------------------------------------------------
# Compile the hooks into the standard cardenginei ARM7/ARM9 variants.
# ---------------------------------------------------------------------------
for rel in ['retail/cardenginei/arm7/Makefile', 'retail/cardenginei/arm9/Makefile']:
    p, s = read(rel)
    if '-DDSICAST_INGAME=1' not in s:
        needle = 'CXXFLAGS'
        idx = s.find(needle)
        if idx < 0:
            raise SystemExit(f'{rel}: CXXFLAGS anchor missing')
        s = s[:idx] + 'CFLAGS  +=      -DDSICAST_INGAME=1\n' + s[idx:]
    write(p, s)

# ---------------------------------------------------------------------------
# ARM7: call the resident worker from the existing VBlank hook.  Input is only
# observed; it is not cleared/remapped.  The worker owns L+R+SELECT edge logic.
# ---------------------------------------------------------------------------
p, s = read('retail/cardenginei/arm7/source/cardengine.c')
old = '''void myIrqHandlerVBlank(void) {\n  while (1) {\n'''
new = '''void myIrqHandlerVBlank(void) {\n  while (1) {\n#ifdef DSICAST_INGAME\n\tif (*(volatile u32*)DSICAST_SHARED_LOCATION == DSICAST_SHARED_MAGIC) {\n\t\t((void (*)(void))DSICAST_ARM7_RESIDENT_LOCATION)();\n\t}\n#endif\n'''
if 'DSICAST_ARM7_RESIDENT_LOCATION' not in s:
    s = replace_once(s, old, new, 'arm7 vblank hook')
write(p, s)

# ---------------------------------------------------------------------------
# ARM9: call the resident worker at VCount 0.  Preserve color-LUT behavior when
# enabled, but allow our recurring hook even when color LUT is disabled.
# ---------------------------------------------------------------------------
p, s = read('retail/cardenginei/arm9/source/cardengine.c')
old = '''void myIrqHandlerVcount(void) {\n//---------------------------------------------------------------------------------\n\t#ifdef DEBUG\n\tnocashMessage("myIrqHandlerVcount");\n\t#endif\n\n\tapplyColorLut(false);\n'''
new = '''void myIrqHandlerVcount(void) {\n//---------------------------------------------------------------------------------\n\t#ifdef DEBUG\n\tnocashMessage("myIrqHandlerVcount");\n\t#endif\n\n#ifdef DSICAST_INGAME\n\tif (*(volatile u32*)DSICAST_SHARED_LOCATION == DSICAST_SHARED_MAGIC) {\n\t\t((void (*)(void))DSICAST_ARM9_RESIDENT_LOCATION)();\n\t}\n\tif (ce9->valueBits & useColorLut) applyColorLut(false);\n#else\n\tapplyColorLut(false);\n#endif\n'''
if 'DSICAST_ARM9_RESIDENT_LOCATION' not in s:
    s = replace_once(s, old, new, 'arm9 vcount worker call')

# In myIrqEnable(), enable VCount for DSiCast even without a color LUT.
marker = 'u32 myIrqEnable(u32 irq) {'
pos = s.find(marker)
if pos < 0:
    raise SystemExit('arm9 myIrqEnable anchor missing')
head, tail = s[:pos], s[pos:]
oldcond = '''\tif ((ce9->valueBits & useColorLut) && !(ce9->valueBits & colorLutBlockVCount)) {\n\t\tirq_before = IRQ_VCOUNT;\n'''
newcond = '''#ifdef DSICAST_INGAME
	if (1) {
		irq_before |= (REG_IE & IRQ_VCOUNT);
#else
	if ((ce9->valueBits & useColorLut) && !(ce9->valueBits & colorLutBlockVCount)) {
		irq_before = IRQ_VCOUNT;
#endif
'''
if '#ifdef DSICAST_INGAME\n\tif (1) {' not in tail:
    tail = replace_once(tail, oldcond, newcond, 'arm9 irq enable')
s = head + tail
write(p, s)

# ---------------------------------------------------------------------------
# ARM9 misc: hook the game's VCount IRQ table even when color LUT is disabled.
# ---------------------------------------------------------------------------
p, s = read('retail/cardenginei/arm9/source/misc.c')
old = '''    if (!IPC_SYNC_hooked) {\n\t\tif ((ce9->valueBits & useColorLut) && !(ce9->valueBits & colorLutBlockVCount)) {\n\t\t\tu32* vcountHandler = ce9->irqTable + 2;\n'''
new = '''    if (!IPC_SYNC_hooked) {\n#ifdef DSICAST_INGAME\n\t\tif (1) {\n#else\n\t\tif ((ce9->valueBits & useColorLut) && !(ce9->valueBits & colorLutBlockVCount)) {\n#endif\n\t\t\tu32* vcountHandler = ce9->irqTable + 2;\n'''
if '#ifdef DSICAST_INGAME\n\t\tif (1)' not in s:
    s = replace_once(s, old, new, 'misc VCount hook')
write(p, s)

# ---------------------------------------------------------------------------
# Loader: load the two raw resident binaries from NitroFS into DSi-only extended
# RAM.  Set shared magic only if both files load completely.
# ---------------------------------------------------------------------------
p, s = read('retail/arm9/source/conf_sd.cpp')
if 'loadDsicastResidentFile' not in s:
    helper_anchor = 'void s2RamAccess(bool open) {'
    helper = '''
static bool loadDsicastResidentFile(const char* path, u32 dst, u32 capacity) {
\tFILE* fp = fopen(path, "rb");
\tif (!fp) return false;
\tfseek(fp, 0, SEEK_END);
\tlong size = ftell(fp);
\tfseek(fp, 0, SEEK_SET);
\tif (size <= 0 || (u32)size >= capacity) {
\t\tfclose(fp);
\t\treturn false;
\t}
\ttoncset((void*)dst, 0, capacity);
\tsize_t got = fread((void*)dst, 1, (size_t)size, fp);
\tfclose(fp);
\tDC_FlushRange((void*)dst, capacity);
\treturn got == (size_t)size;
}

'''
    s = replace_once(s, helper_anchor, helper + helper_anchor, 'loader helper')

call_anchor = '''\n\t\t\t\t, (u8*)CARDENGINEI_ARM9_BUFFERED_LOCATION);\n\t\t\t}\n\n\t\t\tbool found = (access(pageFilePath.c_str(), F_OK) == 0);\n'''
call = '''

#ifdef DSICAST_INGAME
\t\t\t/* First hardware target: standard NTR game on DSi/3DS-class hardware.
\t\t\t   SDK5/DSiWare paths deliberately remain untouched in this experiment. */
\t\t\tif (isDSiMode() && !conf->isDSiWare && unitCode == 0) {
\t\t\t\ttoncset((void*)DSICAST_SHARED_LOCATION, 0, 0x100);
\t\t\t\tconst bool dsicast9Loaded = loadDsicastResidentFile(
\t\t\t\t\t"nitro:/dsicast_arm9.bin", DSICAST_ARM9_RESIDENT_LOCATION, DSICAST_ARM9_RESIDENT_LIMIT);
\t\t\t\tconst bool dsicast7Loaded = loadDsicastResidentFile(
\t\t\t\t\t"nitro:/dsicast_arm7.bin", DSICAST_ARM7_RESIDENT_LOCATION, DSICAST_ARM7_RESIDENT_LIMIT);
\t\t\t\tif (dsicast9Loaded && dsicast7Loaded) {
\t\t\t\t\t*(volatile u32*)DSICAST_SHARED_LOCATION = DSICAST_SHARED_MAGIC;
\t\t\t\t\tDC_FlushRange((void*)DSICAST_SHARED_LOCATION, 0x100);
\t\t\t\t}
\t\t\t}
#endif
'''
if 'nitro:/dsicast_arm9.bin' not in s:
    preserved = '''\n\t\t\t\t, (u8*)CARDENGINEI_ARM9_BUFFERED_LOCATION);\n\t\t\t}\n'''
    tail_found = '''\n\t\t\tbool found = (access(pageFilePath.c_str(), F_OK) == 0);\n'''
    s = replace_once(s, call_anchor, preserved + call + tail_found, 'loader resident call')
write(p, s)

# The loader itself needs the macro because it is a different Makefile target.
p, s = read('retail/arm9/Makefile')
if '-DDSICAST_INGAME=1' not in s:
    idx = s.find('CXXFLAGS')
    if idx < 0:
        raise SystemExit('retail/arm9 Makefile CXXFLAGS anchor missing')
    s = s[:idx] + 'CFLAGS  +=      -DDSICAST_INGAME=1\n' + s[idx:]
write(p, s)

print('DSiCast unlimited experimental patch applied successfully.')
