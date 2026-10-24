fx renderer test pack, used by util/fx/smoketest.sh (cases testpack_*).
- testnodes_light_14_e.png: authored emissive map, magenta. With
  fx_debug_view = 2 the light14 glyph must glow magenta, not white.
- fx_light_colors.txt: fxscene:blue is re-coloured purple.
- fx_materials.txt: the clear glass panes become rippled.

M5 material maps (LabPBR 1.3, DirectX normals): default_stone, default_cobble,
default_ice, fxscene_gold_ore (metal specks) and fxscene_gold_block (_n/_s).
Generated with jsl2mcl's fx.make_normal_map / make_specular_map. Rendered by
the "materials" and "materials_off" smoke cases.
