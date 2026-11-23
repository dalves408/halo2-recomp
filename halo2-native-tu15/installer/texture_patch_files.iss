{ Written by scripts/build_texture_pack.py; do not edit by hand. The files the texture update replaces
  or adds, with their new sizes; -1 for any other file. }
function TexturePatchFileBytes(Relative: String): Int64;
begin
  Result := -1;
  Relative := Lowercase(Relative);
  if Relative = 'color\environment\00e3d3c6f3a44935_256x512_dxt5_watermask_scarab.dds' then Result := 11184960
  else if Relative = 'color\environment\32fe396039bbe415_128x32_dxt5_tunnel_water_alpha.dds' then Result := 349680
  else if Relative = 'color\environment\fcd5e8f7a62d23a5_128x128_dxt5_ec_ocean_alpha.dds' then Result := 1398248
  else if Relative = 'color\hud\06c24b5f3956bac3_136x86_a8r8g8b8_binoculars_elite_2.dds' then Result := 998048
  else if Relative = 'color\hud\09d5a4838e522cf3_268x170_a8r8g8b8_binoculars_elite_0.dds' then Result := 3887620
  else if Relative = 'color\hud\0c67dd7280d647a8_103x64_a8r8g8b8_sniper_rifle_scope_mask_5.dds' then Result := 562512
  else if Relative = 'color\hud\191d45eaa21bc2d6_123x121_a8r8g8b8_battle_rifle_scope_mask_0.dds' then Result := 1269696
  else if Relative = 'color\hud\2425fe2e2f8aeeb9_19x15_a8r8g8b8_battle_rifle_scope_mask_4.dds' then Result := 24400
  else if Relative = 'color\hud\26a4301114d49ffe_62x61_a8r8g8b8_battle_rifle_scope_mask_5.dds' then Result := 322692
  else if Relative = 'color\hud\3998a933ed21f54c_73x54_a8r8g8b8_battle_rifle_scope_mask_3.dds' then Result := 336376
  else if Relative = 'color\hud\3c7b5c7f7113ad19_3x46_a8r8g8b8_sniper_rifle_scope_mask_7.dds' then Result := 11908
  else if Relative = 'color\hud\40e4859a1c14f2e4_194x135_a8r8g8b8_carbine_scope_mask_0.dds' then Result := 2234484
  else if Relative = 'color\hud\44af05238fabc3a9_87x16_a8r8g8b8_carbine_scope_mask_2.dds' then Result := 118900
  else if Relative = 'color\hud\45273e544c385d54_7x98_a8r8g8b8_sniper_rifle_scope_mask_2.dds' then Result := 58544
  else if Relative = 'color\hud\47982751367e793c_158x101_a8r8g8b8_binoculars_human_1.dds' then Result := 1361508
  else if Relative = 'color\hud\4f232baad68810e9_160x102_a8r8g8b8_binoculars_elite_1.dds' then Result := 1392620
  else if Relative = 'color\hud\523490614e292654_89x22_a8r8g8b8_battle_rifle_scope_mask_1.dds' then Result := 167116
  else if Relative = 'color\hud\6051bd2653ebf85b_24x15_a8r8g8b8_sniper_rifle_scope_mask_4.dds' then Result := 30812
  else if Relative = 'color\hud\6e0bd8d8d0f506d8_119x62_a8r8g8b8_beam_rifle_scope_mask_1.dds' then Result := 629448
  else if Relative = 'color\hud\7a77b656aae0d35a_271x174_a8r8g8b8_binoculars_human_0.dds' then Result := 4023204
  else if Relative = 'color\hud\8ac7c6031fbfe734_13x88_a8r8g8b8_battle_rifle_scope_mask_2.dds' then Result := 97640
  else if Relative = 'color\hud\91bd66399556cc9b_19x15_a8r8g8b8_sniper_rifle_scope_mask_3.dds' then Result := 24400
  else if Relative = 'color\hud\91c0075a57babcb8_51x41_a8r8g8b8_carbine_scope_mask_5.dds' then Result := 178420
  else if Relative = 'color\hud\95149b384fa0f8b9_102x36_a8r8g8b8_carbine_scope_mask_4.dds' then Result := 313424
  else if Relative = 'color\hud\98f71cc4300cc95c_37x28_a8r8g8b8_battle_rifle_scope_mask_8.dds' then Result := 88492
  else if Relative = 'color\hud\9ea2db7e2363356f_237x123_a8r8g8b8_beam_rifle_scope_mask_0.dds' then Result := 2486936
  else if Relative = 'color\hud\a7bc7f14008bf64f_50x3_a8r8g8b8_sniper_rifle_scope_mask_6.dds' then Result := 12936
  else if Relative = 'color\hud\aadb5c97b9c1391b_96x68_a8r8g8b8_carbine_scope_mask_1.dds' then Result := 557168
  else if Relative = 'color\hud\cc9378e5aa7e6078_79x8_a8r8g8b8_carbine_scope_mask_3.dds' then Result := 54076
  else if Relative = 'color\hud\e6f0f2973986b4c4_196x123_a8r8g8b8_sniper_rifle_scope_mask_0.dds' then Result := 2056876
  else if Relative = 'color\hud\eb404e8f8e1c5b25_9x47_a8r8g8b8_battle_rifle_scope_mask_7.dds' then Result := 36168
  else if Relative = 'color\hud\f25c6a41d132cc5c_90x8_a8r8g8b8_sniper_rifle_scope_mask_1.dds' then Result := 61600
  else if Relative = 'color\hud\f4bd387f553fc2e1_138x102_a8r8g8b8_binoculars_human_2.dds' then Result := 1201076
  else if Relative = 'color\hud\f53a8c5baf0ffd78_47x14_a8r8g8b8_battle_rifle_scope_mask_6.dds' then Result := 56228;
end;
