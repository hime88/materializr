#!/usr/bin/env python3
"""List user-visible strings that have no translation yet.

Handles ADJACENT STRING CONCATENATION: C++ joins "a" "b" into one argument, and
the catalogue key has to be the joined result. Missing this is why the Settings
descriptions and half the tooltips never translated -- the wrapper only ever
matched the first fragment.
"""
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from i18n_catalogue import CAT
from i18n_srclit import decode

KEYS = {en for en, _ in CAT}
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')

CALLS = ("Text|TextUnformatted|TextDisabled|TextWrapped|Button|SmallButton|"
         "MenuItem|BeginMenu|Checkbox|RadioButton|CollapsingHeader|BeginTabItem|"
         "SeparatorText|BulletText|Selectable|SetTooltip|SetItemTooltip|TreeNode|"
         "SliderFloat|SliderInt|DragFloat|DragInt|InputText|InputFloat|InputInt|"
         "Combo|BeginCombo|ProgressBar|TableSetupColumn")
# A run of one or more adjacent literals in the FIRST argument position.
RUN = r'((?:"(?:[^"\\]|\\.)*"\s*)+)'
PATTERNS = [
    re.compile(r'ImGui::(?:' + CALLS + r')\s*\(\s*' + RUN, re.S),
    re.compile(r'\btip\s*\(\s*' + RUN, re.S),
    re.compile(r'railButton\(\s*[^,]+?,\s*[^,]+?,\s*' + RUN, re.S),
    re.compile(r'pillButton\(\s*[^,]+?,\s*[^,]+?,\s*' + RUN, re.S),
    re.compile(r'pillButtonWidth\(\s*[^,]+?,\s*' + RUN, re.S),
    re.compile(r'ImGui::TextColored\(\s*(?:[^,()"]|\([^()]*\))+?,\s*' + RUN, re.S),
    re.compile(r'amountField\(\s*[^,]+?,\s*' + RUN, re.S),
    re.compile(r'inputNumber\(\s*' + RUN, re.S),
    re.compile(r'twoRowButton\(\s*[^,]+?,\s*' + RUN, re.S),
    re.compile(r'twoRowButtonWidth\(\s*' + RUN, re.S),
]

def joined(run):
    return "".join(LIT.findall(run))

def scan(path):
    s = open(path, encoding="utf-8", errors="replace").read()
    found = []
    for pat in PATTERNS:
        for m in pat.finditer(s):
            v = decode(joined(m.group(1)))
            vis = v.split("##")[0]
            if not vis.strip() or not re.search(r"[A-Za-z]{2}", vis):
                continue
            if vis in KEYS:
                continue
            found.append(vis)
    return found

if __name__ == "__main__":
    allf = {}
    for p in sys.argv[1:]:
        for v in scan(p):
            allf.setdefault(v, set()).add(os.path.basename(p))
    print("# %d untranslated strings" % len(allf))
    for v in sorted(allf, key=lambda x: (len(x), x)):
        print("%s\t%s" % (v.replace("\t", " "), ",".join(sorted(allf[v]))))
