fx renderer test pack (pack format 2), rendered by util/fx/test/run.py
(cases testpack_emissive, testpack_render, materials).
- testnodes_light_14_e.png: authored emissive map, magenta. With
  fx_debug_view = 2 the light14 glyph must glow magenta, not white. Its
  light colour also comes from this map (format v2 item 3), so the room
  around it is lit magenta.
- fx_light_colors.txt: fxscene:blue is re-coloured purple (glob key).
- fx_materials.txt: the clear glass panes become rippled (glob key).
- fxscene_mob_e.png: entity emissive companion.

Material maps (LabPBR 1.3, DirectX normals): default_stone, default_cobble,
default_ice, fxscene_gold_ore (metal specks) and fxscene_gold_block (_n/_s).
Generated with jsl2mcl's fx.make_normal_map / make_specular_map.
