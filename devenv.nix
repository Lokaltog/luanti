# devenv for the Luanti fx fork. Enter with `devenv shell` (or direnv), then `fx-help`.
#
# The build is RUN_IN_PLACE: the binary lands in ./bin and reads shaders from
# ./client/shaders, so fx_shader_hot_reload picks up edits in the working tree.
# Your existing games, worlds and mods in ~/.minetest are found through
# LUANTI_*_PATH below (read-only use); everything the dev build writes (config,
# new worlds, linked fx mods) stays in this tree.
#
# NixOS: the GPU driver lives in /run/opengl-driver, outside the store. Only the
# commands that start the game on your GPU add it (fx-gl); the shell itself
# keeps a clean LD_LIBRARY_PATH, so the headless tests get llvmpipe.
{ pkgs, lib, config, ... }:

let
  userDir = "$HOME/.minetest"; # Luanti still uses ~/.minetest on Linux
in
{
  packages = with pkgs; [
    # toolchain
    gcc
    cmake
    ninja
    ccache
    pkg-config
    gdb
    clang-tools # clangd, clang-format

    # Luanti dependencies (mirrors upstream shell.nix)
    zlib
    zstd
    libjpeg
    libpng
    libGL
    luajit
    sdl3
    openal
    curl
    libvorbis
    libogg
    gettext
    gmp
    freetype
    sqlite
    jsoncpp

    # fx tooling
    renderdoc # frame captures (opengl3 driver)
    glslang # glslangValidator
    xorg.xorgserver # Xvfb for the headless test harness
    mesa # llvmpipe for the headless test harness
    (python3.withPackages (ps: [ ps.pillow ps.numpy ])) # util/fx/test/run.py
    uv # fx-jsl-test runs the mineclonia-jsl test suite
  ];

  env = {
    CMAKE_GENERATOR = "Ninja";
    CCACHE_DIR = "${config.devenv.root}/.devenv/ccache";
  };

  enterShell = ''
    export LUANTI_GAME_PATH="${userDir}/games"
    export LUANTI_WORLD_PATH="${userDir}/worlds"
    export LUANTI_MOD_PATH="${userDir}/mods"
    echo "Luanti fx fork. Run fx-help for commands."
  '';

  scripts = {
    fx-help.exec = ''
      cat <<'EOF'
      fx-configure        cmake configure (RelWithDebInfo, RUN_IN_PLACE, ccache)
      fx-build            incremental build (configures on first run)
      fx-run [args]       build, then run with util/fx/dev.conf (hot reload on).
                          Settings you change in-game persist in .devenv/luanti.conf.
                          FX_TEXTURE_PACK=/path/to/pack fx-run   uses a texture pack.
      fx-play <world>     fx-run straight into a world from ~/.minetest/worlds
      fx-upstream [args]  same as fx-run but with fx_enable = false, for A/B
      fx-test [args]      test harness, headless (Xvfb + llvmpipe): every case must
                          render without errors, pass its probes and match its
                          llvmpipe golden. Report: shots/test/report.html.
                          --only 'regex', --update-goldens, list
      fx-test-gpu [args]  the same on your display and GPU (4090 goldens)
      fx-unit             C++ unit tests (TestFx)
      fx-mcl <path>       link a Mineclonia checkout as games/mineclonia (mcl_* cases)
      fx-renderdoc        run under RenderDoc for a frame capture
      fx-jsl-test         run the jsl2mcl fx step tests (needs JSL2MCL_DIR)
      fx-install-mods     symlink util/fx/mods/* (fx_ambience particles) into
                          this tree's mods/; then enable them per world
      fx-gl <cmd>         run <cmd> with the NixOS GPU driver on the library path
      EOF
    '';

    fx-gl.exec = ''
      # GL/Vulkan drivers (NVIDIA included) live here on NixOS, not in the store
      export LD_LIBRARY_PATH="/run/opengl-driver/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
      exec "$@"
    '';

    fx-configure.exec = ''
      cd "$DEVENV_ROOT"
      cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DRUN_IN_PLACE=TRUE \
        -DUSE_SDL3=ON \
        -DBUILD_SERVER=FALSE \
        -DBUILD_UNITTESTS=TRUE \
        -DENABLE_GETTEXT=FALSE \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "$@"
      ln -sf build/compile_commands.json compile_commands.json
    '';

    fx-build.exec = ''
      cd "$DEVENV_ROOT"
      [ -f build/build.ninja ] || fx-configure
      ninja -C build "$@"
    '';

    fx-run.exec = ''
      set -e
      cd "$DEVENV_ROOT"
      fx-build
      conf=.devenv/luanti.conf
      [ -f "$conf" ] || cp util/fx/dev.conf "$conf"
      if [ -n "''${FX_TEXTURE_PACK:-}" ]; then
        sed -i '/^texture_path =/d' "$conf"
        echo "texture_path = $FX_TEXTURE_PACK" >> "$conf"
      fi
      exec fx-gl bin/luanti --config "$conf" "$@"
    '';

    fx-play.exec = ''
      [ -n "''${1:-}" ] || { echo "usage: fx-play <world name in ~/.minetest/worlds>"; exit 1; }
      world="$LUANTI_WORLD_PATH/$1"; shift
      [ -d "$world" ] || { echo "no world at $world"; exit 1; }
      fx-run --go --world "$world" "$@"
    '';

    fx-upstream.exec = ''
      set -e
      cd "$DEVENV_ROOT"
      fx-build
      conf=.devenv/luanti-upstream.conf
      sed '/^fx_/d' util/fx/dev.conf > "$conf"
      echo "fx_enable = false" >> "$conf"
      exec fx-gl bin/luanti --config "$conf" "$@"
    '';

    fx-test.exec = ''
      set -e
      cd "$DEVENV_ROOT"
      fx-build
      # llvmpipe from the store, not your GPU driver
      LD_LIBRARY_PATH="${pkgs.mesa}/lib:${pkgs.libGL}/lib" \
        LIBGL_DRIVERS_PATH="${pkgs.mesa}/lib/dri" \
        __GLX_VENDOR_LIBRARY_NAME=mesa \
        python3 util/fx/test/run.py "$@"
    '';

    fx-test-gpu.exec = ''
      set -e
      cd "$DEVENV_ROOT"
      fx-build
      fx-gl python3 util/fx/test/run.py --gpu "$@"
    '';

    fx-unit.exec = ''
      set -e
      cd "$DEVENV_ROOT"
      fx-build
      bin/luanti --run-unittests --test-module TestFx
    '';

    fx-mcl.exec = ''
      [ -n "''${1:-}" ] || { echo "usage: fx-mcl <path to a Mineclonia checkout>"; exit 1; }
      ln -sfn "$(realpath "$1")" "$DEVENV_ROOT/games/mineclonia"
      echo "games/mineclonia -> $(realpath "$1")"
    '';

    fx-renderdoc.exec = ''
      cd "$DEVENV_ROOT"
      fx-build
      exec fx-gl renderdoccmd capture -w bin/luanti --config util/fx/dev.conf "$@"
    '';

    fx-jsl-test.exec = ''
      [ -n "''${JSL2MCL_DIR:-}" ] || { echo "set JSL2MCL_DIR to your mineclonia-jsl checkout"; exit 1; }
      # its fx tests check the fork's test pack and node dump when pointed at them
      cd "$JSL2MCL_DIR"
      FX_TESTPACK="$DEVENV_ROOT/util/fx/testpack" \
        FX_NODEDUMP="$DEVENV_ROOT/util/fx/test/data/mineclonia_nodes.json" \
        uv run pytest tests/test_fxpack.py "$@"
    '';

    fx-install-mods.exec = ''
      # RUN_IN_PLACE: ./mods is this build's user mod dir; stock Luanti and
      # ~/.minetest are left alone
      mkdir -p "$DEVENV_ROOT/mods"
      for m in "$DEVENV_ROOT"/util/fx/mods/*/; do
        name=$(basename "$m")
        ln -sfn "$m" "$DEVENV_ROOT/mods/$name"
        echo "linked $name -> mods/$name (enable it in the world's mod list)"
      done
    '';
  };

  # `devenv test`: unit tests + the headless harness
  enterTest = ''
    fx-unit
    fx-test
  '';
}
