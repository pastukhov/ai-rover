from pathlib import Path

from SCons.Script import Import

Import("env")


def patch_m5unified():
    libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")
    target = libdeps_dir / "M5Unified" / "src" / "M5Unified.cpp"
    if not target.exists():
        print(f"[patch_m5unified] skip: {target} not found")
        return

    text = target.read_text(encoding="utf-8")
    original = text
    text = text.replace(
        "static void _read_touch_pad(uint32_t* results, const touch_pad_t* channel, const size_t channel_count)",
        "static void _read_touch_pad(uint32_t* results, const int* channel, const size_t channel_count)",
    )
    text = text.replace("static constexpr touch_pad_t s_channel_id[] = {", "static constexpr int s_channel_id[] = {")
    text = text.replace("TOUCH_PAD_NUM4", "4")
    text = text.replace("TOUCH_PAD_NUM7", "7")

    if text != original:
        target.write_text(text, encoding="utf-8")
        print("[patch_m5unified] patched touch_pad_t compatibility for ESP-IDF 5.5")
    else:
        print("[patch_m5unified] no changes needed")


patch_m5unified()
