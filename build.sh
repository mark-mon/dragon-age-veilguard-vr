#!/bin/bash
# Cross-builds DAVG VR (64-bit) with clang-cl + lld-link against the MSVC/SDK in /home/claude/tc.
# Output: dist/version.dll (+ pdb) and dist/openxr_loader.dll
set -e
cd "$(dirname "$0")"
B=${TC:-/home/claude/tc}/build.sh
# NOMENU=1 ./build.sh -> dist_nomenu/ (same mod without the vrkit settings menu, for regression testing)
OUT=dist; MENU_SRC="src/menu.cpp vrkit/vrkit_options.cpp vrkit/vrkit_input.cpp vrkit/vrkit_menu.cpp vrkit/vrkit_menu_core.cpp vrkit/third_party/imgui/imgui.cpp vrkit/third_party/imgui/imgui_draw.cpp vrkit/third_party/imgui/imgui_tables.cpp vrkit/third_party/imgui/imgui_widgets.cpp vrkit/third_party/imgui/imgui_impl_dx11.cpp"
EXTRA=""
if [ "$NOMENU" = "1" ]; then OUT=dist_nomenu; MENU_SRC="src/menu_stub.cpp"; EXTRA="-DDAVG_NO_MENU"; fi
mkdir -p build_$OUT $OUT
CFLAGS="/O2 /Zi /W3 -D_CRT_SECURE_NO_WARNINGS -Wno-unused-function -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-sign-compare -Wno-unused-variable -Wno-unused-parameter /Ithird_party/openxr/include /Ithird_party/minhook/include /Ivrkit /Ivrkit/third_party/imgui"
SRC="src/main.cpp src/proxy.cpp src/log.cpp src/config.cpp src/crash.cpp src/modwatch.cpp src/dx12_hooks.cpp src/triggers.cpp src/xr_host.cpp src/fbview.cpp src/replay.cpp src/slhook.cpp src/fovdlss.cpp src/vrs.cpp src/resolver.cpp src/window_size.cpp src/dred.cpp src/limiter.cpp src/vtbl12.c $MENU_SRC \
 third_party/minhook/src/buffer.c third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c"
$B x64 $OUT/version.dll $CFLAGS $EXTRA /LD /Fobuild_$OUT/ /Fdbuild_$OUT/davgvr.pdb $SRC \
   /link /DEF:src/exports.def /DEBUG /PDB:$OUT/version.pdb \
   user32.lib dxgi.lib d3d12.lib d3d11.lib d3dcompiler.lib dxguid.lib advapi32.lib ole32.lib shell32.lib
cp third_party/openxr_loader/x64/bin/openxr_loader.dll $OUT/
ls -la $OUT
