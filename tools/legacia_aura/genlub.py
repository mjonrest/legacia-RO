import json, sys, os
auras = json.load(open(sys.argv[1])); src_ids, src_info, out = sys.argv[2], sys.argv[3], sys.argv[4]
NL = '\r\n'
def cname(a): return 'HAT_EF_LEGACIA_AURA%d_%03d' % (a['part'], a['num'])
# HatEffectIDs.lub
s = open(src_ids, encoding='latin-1', newline='').read()
tail = '\tHAT_EF_DARKSHADOW_Yellow_A = 313' + NL + '}'
assert s.count(tail) == 1
add = ',' + NL + '\t-- Legacia auras: Gepard aura part 1/2/3 number n = 1000+n / 1300+n / 1600+n (server: pc_aura_apply)' + NL
add += (',' + NL).join('\t%s = %d' % (cname(a), a['id']) for a in auras) + NL + '}'
s = s.replace(tail, '\tHAT_EF_DARKSHADOW_Yellow_A = 313' + add)
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'HatEffectIDs.lub'), 'w', encoding='latin-1', newline='').write(s)
# HatEffectInfo.lub (numeric keys so it works whatever file defines HatEFID)
s = open(src_info, encoding='latin-1', newline='').read()
anchor = NL + '}' + NL + 'effectHatItemTable = {'
assert s.count(anchor) == 1
ent = []
for a in auras:
    before = 'false' if a['kind'] == 'particle' else 'true'
    ent.append(NL.join([
        '\t[%d] = { -- %s (aura part %d #%d, %s)' % (a['id'], cname(a), a['part'], a['num'], a['kind']),
        '\t\tresourceFileName = "efst_legacia_aura\\\\%s",' % a['file'],
        '\t\thatEffectPos = -11,',
        '\t\thatEffectPosX = 0,',
        '\t\tisRenderBeforeCharacter = %s,' % before,
        '\t\tisIgnoreRiding = true,',
        '\t\tisAdjustPositionWhenShrinkState = true,',
        '\t\tisAdjustSizeWhenShrinkState = true',
        '\t}']))
block = ',' + NL + '\t-- Legacia auras: Gepard aura part 1/2/3 number n = 1000+n / 1300+n / 1600+n (server: pc_aura_apply)' + NL + (',' + NL).join(ent)
s = s.replace(anchor, block + anchor)
open(os.path.join(out, 'HatEffectInfo.lub'), 'w', encoding='latin-1', newline='').write(s)
