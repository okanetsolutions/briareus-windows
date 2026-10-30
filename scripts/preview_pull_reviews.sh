#!/bin/sh
# Linux/Wine synthetic previews using the production native Pane, Doc and canvas.
set -eu
preview_build=${BUILD:-build-review-preview}
preview_cc=${CC:-x86_64-w64-mingw32-gcc}
make WERROR=1 BUILD="$preview_build" CC="$preview_cc" CXX="${CXX:-x86_64-w64-mingw32-g++}" \
    WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}" "$preview_build/Briareus.exe" "$preview_build/tests/harness.o"
preview_time=
# This flag affects the preview executable only; Wine 9 lacks an existing date API.
if [ "${PREVIEW_WINE_TIME:-1}" = 1 ]; then preview_time=-DBRIAREUS_WINE_PREVIEW_TIME; fi
"$preview_cc" -std=c11 -O2 -DUNICODE -D_UNICODE $preview_time -Icore -Iapp -Itests \
    -c tests/native_pull_reviews_preview.c -o "$preview_build/native-preview.o"
"$preview_cc" -o "$preview_build/native-preview.exe" "$preview_build/native-preview.o" \
    "$preview_build"/core/*.o "$preview_build"/app/*.o "$preview_build/tests/harness.o" "$preview_build/briareus.res.o" \
    -static -lwinhttp -ladvapi32 -lole32 -ldbghelp -lcomctl32 -lgdi32 -luser32 -lshell32 -luuid \
    -ldwmapi -lwinmm -lmfplat -lmfreadwrite -lmfuuid -lshlwapi -luxtheme -lcomdlg32 -lmsimg32 -ld2d1 -ldwrite
: "${DISPLAY:?Start Xvfb with a screen of at least 1652x1009 and set DISPLAY}"
export WINEDEBUG=-all
for preview_tab in reviews review; do
    for preview_size in wide narrow dpi usage; do
        if [ "$preview_size" = usage ] && [ "$preview_tab" = review ]; then continue; fi
        if [ "$preview_size" = usage ]; then export PREVIEW_USAGE=1; else unset PREVIEW_USAGE; fi
        preview_width=1652 preview_dpi=96
        if [ "$preview_size" = narrow ] || [ "$preview_size" = usage ]; then preview_width=390; fi
        if [ "$preview_size" = dpi ]; then preview_width=780; preview_dpi=192; fi
        if [ "$preview_tab" = review ]; then export PREVIEW_REVIEW=21; else unset PREVIEW_REVIEW; fi
        PREVIEW_WIDTH=$preview_width PREVIEW_DPI=$preview_dpi wine "$preview_build/native-preview.exe" \
            > "$preview_build/$preview_tab-$preview_size.log" 2>&1 &
        preview_pid=$!
        sleep 5
        import -window root -crop "${preview_width}x1009+0+0" "$preview_build/$preview_tab-$preview_size.png"
        wait "$preview_pid"
    done
done
