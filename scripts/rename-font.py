# rename-font.py <in.ttf> <out.ttf> <family>: rewrite a font's family, full, unique and
# PostScript names (name IDs 1, 3, 4, 6; 16/17 removed) to <family>, keeping its style.
import sys
from fontTools.ttLib import TTFont

src, dst, family = sys.argv[1:4]
font = TTFont(src)
names = font["name"]
style = names.getDebugName(2) or "Regular"
full = family if style == "Regular" else f"{family} {style}"
postscript = family.replace(" ", "") + ("" if style == "Regular" else "-" + style.replace(" ", ""))
version = (names.getDebugName(5) or "").replace("Version ", "")
for record in list(names.names):
    if record.nameID in (16, 17):
        names.names.remove(record)
values = {1: family, 3: f"MetalSharp: {full}: {version}", 4: full, 6: postscript}
for record in names.names:
    if record.nameID in values:
        record.string = values[record.nameID]
font.save(dst)
print(f"{dst.rsplit('/', 1)[-1]}: {full}")
