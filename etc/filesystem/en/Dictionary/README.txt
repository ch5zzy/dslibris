Drop StarDict dictionaries into this Dictionary/ folder (either directly here
or inside a subfolder such as Dictionary/wordnet/).

Required files for each dictionary:
  - <name>.ifo
  - <name>.idx        (uncompressed; if you have .idx.gz, gunzip it first)
  - <name>.dict       or <name>.dict.dz (dictzip compressed)

Optional files for better inflection lookup:
  - <name>.syn        (StarDict synonym/inflection index)
  - <name>.lem        (compact lemma table generated via etc/tool/make_lemma.py)

Then choose your active dictionary under Settings -> dictionary in dslibris.
