#!/usr/bin/env python3
import argparse
import re
import subprocess
import sys

try:
    from pypinyin import Style, pinyin
except ImportError:
    print("请先安装 pypinyin: pip install pypinyin")
    sys.exit(1)


def load_wakeup_weights(bin_path: str) -> dict[str, float]:
    with open(bin_path, "rb") as f:
        data = f.read()
    weights: dict[str, float] = {}
    for m in re.finditer(rb"(?:^|\n)([a-z ]{1,24}) (\d+\.\d+)", data):
        key = m.group(1).decode("ascii").strip()
        val = float(m.group(2).decode("ascii"))
        weights[key] = val
    return weights


def auto_estimate_thresh(wake_py: str, weights: dict[str, float] | None) -> tuple[float, list[str]]:
    syllables = wake_py.split()
    n = len(syllables)
    reasons = []

    # 1. 按长度和重复结构定基准
    if n <= 2:
        th = 0.25
        reasons.append("2音节短词防误触(+0.25)")
    elif n == 3:
        th = 0.21
        reasons.append("3音节词(0.21)")
    elif n == 4 and syllables[:2] == syllables[2:]:
        th = 0.18
        reasons.append("4音节ABAB叠词(0.18)")
    else:
        th = 0.20
        reasons.append(f"{n}音节词(0.20)")

    # 2. 查 wakeup.bin 联合词条与音节权重
    if weights:
        if n == 4 and syllables[:2] == syllables[2:]:
            half = " ".join(syllables[:2])
            if half in weights:
                th += 0.02
                reasons.append(f"含联合词条'{half}'(+0.02)")
        high_pen = [s for s in set(syllables) if weights.get(s, 4.0) >= 6.0]
        if high_pen:
            th -= 0.01
            reasons.append(f"高惩罚音节{high_pen}(-0.01)")

    # 3. 检查相邻/内部易混淆声母（如 d/z, z/zh, c/ch, s/sh, n/l）
    confusable_groups = [
        {"d", "z", "zh"},
        {"c", "ch"},
        {"s", "sh"},
        {"n", "l"},
    ]
    initials = []
    for s in syllables:
        if s[:2] in ("zh", "ch", "sh"):
            initials.append(s[:2])
        else:
            initials.append(s[0])

    unique_inits = set(initials)
    for grp in confusable_groups:
        if len(unique_inits & grp) >= 2:
            th -= 0.03
            reasons.append(f"易混声母{sorted(unique_inits & grp)}(-0.03)")
            break

    th = max(0.12, min(0.28, round(th, 2)))
    return th, reasons


def text_to_pinyin_interactive(text: str) -> str:
    if re.fullmatch(r"[a-zA-Z\s]+", text.strip()):
        return " ".join(text.lower().split())

    chars = [c for c in text if "\u4e00" <= c <= "\u9fff"]
    if not chars:
        raise ValueError("未检测到有效中文或拼音")
    clean_text = "".join(chars)

    default_py = pinyin(clean_text, style=Style.NORMAL, heteronym=False, v_to_u=False)
    default_tone = pinyin(clean_text, style=Style.TONE, heteronym=False, v_to_u=False)
    all_plain = pinyin(clean_text, style=Style.NORMAL, heteronym=True, v_to_u=False)
    all_tone = pinyin(clean_text, style=Style.TONE, heteronym=True, v_to_u=False)

    final_syllables = []
    for idx, ch in enumerate(chars):
        chosen = default_py[idx][0]
        chosen_tone = default_tone[idx][0]

        uniq_pairs = []
        seen = set()
        for p_plain, p_tone in zip(all_plain[idx], all_tone[idx]):
            if p_plain not in seen:
                seen.add(p_plain)
                uniq_pairs.append((p_plain, p_tone))

        if len(uniq_pairs) > 1:
            print(f"\n[多音字] 第 {idx + 1} 个字「{ch}」默认识别为: {chosen} ({chosen_tone})")
            for i, (p_plain, p_tone) in enumerate(uniq_pairs, 1):
                mark = " <- 默认" if p_plain == chosen else ""
                print(f"  {i}. {p_plain} ({p_tone}){mark}")
            sel = input("直接回车采用默认，或输入序号选择: ").strip()
            if sel.isdigit() and 1 <= int(sel) <= len(uniq_pairs):
                chosen = uniq_pairs[int(sel) - 1][0]

        final_syllables.append(chosen)

    return " ".join(final_syllables)


def main():
    parser = argparse.ArgumentParser(description="智伴1X 自定义唤醒词与自动阈值工具")
    parser.add_argument("phrase", help="唤醒词（中文或空格分隔拼音，如 '蛋崽蛋崽'）")
    parser.add_argument("-t", "--thresh", type=float, default=None,
                        help="手动指定唤醒阈值（不填则自动估算）")
    parser.add_argument("--bin", default="wakeup.bin", help="本地 wakeup.bin 路径")
    parser.add_argument("--dry-run", action="store_true", help="仅转换并校验，不推送到设备")
    args = parser.parse_args()

    wake_py = text_to_pinyin_interactive(args.phrase)

    weights = None
    try:
        weights = load_wakeup_weights(args.bin)
        missing = [s for s in wake_py.split() if s not in weights]
        if missing:
            print(f"警告: 以下音节在 {args.bin} 中未找到，可能无法唤醒: {missing}")
        else:
            print(f"校验通过: 所有音节均存在于 {args.bin}")
    except FileNotFoundError:
        print(f"提示: 未找到 {args.bin}，跳过音节校验")

    if args.thresh is not None:
        if not (0.01 <= args.thresh <= 0.99):
            print("阈值建议在 0.01 ~ 0.99 之间")
            sys.exit(1)
        thresh = args.thresh
        print(f"\n最终唤醒词拼音: {wake_py}  |  手动阈值: {thresh:.2f}")
    else:
        thresh, reasons = auto_estimate_thresh(wake_py, weights)
        print(f"\n最终唤醒词拼音: {wake_py}  |  自动推荐阈值: {thresh:.2f} ({', '.join(reasons)})")

    if args.dry_run:
        return

    cmd = (
        f"echo '{wake_py}' > /var/upgrade/.wake_word && "
        f"echo '{thresh:.2f}' > /var/upgrade/.wake_thresh && "
        f"killall sair"
    )
    print("正在写入设备并重启 sair...")
    subprocess.run(["adb", "shell", cmd], check=True)
    print("完成！请等待 2 秒后观察日志确认。")


if __name__ == "__main__":
    main()