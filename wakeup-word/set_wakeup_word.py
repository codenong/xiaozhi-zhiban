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

# 常用唤醒词候选（以“连叫两次 ABAB”为主，兼顾经典4字词）
CANDIDATE_PHRASES = [
    "智伴智伴",
    "小智小智",
    "你好小智",
    "豆包豆包",
    "奇奇奇奇",
    "可乐可乐",
    "小七小七",
    "米粒米粒",
    "阿宝阿宝",
    "小宝小宝",
    "团子团子",
    "糯米糯米",
    "糖果糖果",
    "蛋崽蛋崽",
]

CONFUSABLE_GROUPS = [
    {"d", "z", "zh"},
    {"c", "ch"},
    {"s", "sh"},
    {"n", "l"},
    {"f", "h"},
]


def split_initial_final(syllable: str) -> tuple[str, str]:
    for ini in ("zh", "ch", "sh"):
        if syllable.startswith(ini):
            return ini, syllable[2:]
    if syllable[0] in "bpmfdtnlgkhjqxzcsryw":
        return syllable[0], syllable[1:]
    return "", syllable


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

    if n <= 2:
        th = 0.25
        reasons.append("2音节短词(0.25)")
    elif n == 3:
        th = 0.21
        reasons.append("3音节词(0.21)")
    elif n == 4 and syllables[:2] == syllables[2:]:
        th = 0.18
        reasons.append("4音节ABAB叠词(0.18)")
    else:
        th = 0.20
        reasons.append(f"{n}音节词(0.20)")

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

    inits = {split_initial_final(s)[0] for s in syllables}
    for grp in CONFUSABLE_GROUPS:
        if len(inits & grp) >= 2:
            th -= 0.03
            reasons.append(f"易混声母{sorted(inits & grp)}(-0.03)")
            break

    th = max(0.12, min(0.28, round(th, 2)))
    return th, reasons


def score_candidate(phrase: str, weights: dict[str, float] | None) -> tuple[int, int, str, float]:
    """返回 (辨识度分, 顺口度分, 拼音串, 推荐阈值)"""
    py_plain = [p[0] for p in pinyin(phrase, style=Style.NORMAL, heteronym=False, v_to_u=False)]
    py_tone3 = [p[0] for p in pinyin(phrase, style=Style.TONE3, heteronym=False, v_to_u=False)]
    wake_py = " ".join(py_plain)

    # --- 1. 辨识度评分 (满分 100) ---
    dist = 65
    is_abab = len(py_plain) == 4 and py_plain[:2] == py_plain[2:]
    core = py_plain[:2] if is_abab else py_plain

    # 不同音节比例（避免“奇奇奇奇”这类单音节四连重复）
    uniq_ratio = len(set(core)) / len(core)
    dist += int(uniq_ratio * 15)
    if len(set(py_plain)) == 1:
        dist -= 25

    # 声母/韵母对比度（一擦一爆、前后端音跨度大得分高）
    inits = [split_initial_final(s)[0] for s in core]
    finals = [split_initial_final(s)[1] for s in core]
    if len(set(inits)) == len(core):
        dist += 8
    if len(set(finals)) == len(core):
        dist += 7

    # 易混声母扣分（如 d/z, z/zh, n/l）
    init_set = set(inits)
    for grp in CONFUSABLE_GROUPS:
        if len(init_set & grp) >= 2:
            dist -= 14
            break

    # wakeup.bin 权重与联合词条加减分
    if weights:
        if any(s not in weights for s in py_plain):
            dist -= 40
        if is_abab and " ".join(core) in weights:
            dist += 10
        high_pen_cnt = sum(1 for s in set(core) if weights.get(s, 4.0) >= 6.0)
        dist -= high_pen_cnt * 4

    dist = max(0, min(100, dist))

    # --- 2. 顺口度评分 (满分 100) ---
    fluency = 75
    tones = [int(t[-1]) if t[-1].isdigit() else 5 for t in py_tone3]
    core_tones = tones[:2] if is_abab else tones

    # 声调有抑扬顿挫加分，全平/全三声拗口扣分
    if len(set(core_tones)) >= 2:
        fluency += 10
    if core_tones.count(3) >= 2:
        fluency -= 10
    if 4 in core_tones and (1 in core_tones or 2 in core_tones or 3 in core_tones):
        fluency += 6  # 去声收尾干脆利落

    # 绕口声母相邻转换扣分（如 z/zh, c/ch, s/sh）
    for a, b in zip(inits, inits[1:]):
        if {a, b} in ({"z", "zh"}, {"c", "ch"}, {"s", "sh"}):
            fluency -= 12

    if is_abab:
        fluency += 5  # ABAB 符合中文呼唤习惯

    fluency = max(0, min(100, fluency))
    thresh, _ = auto_estimate_thresh(wake_py, weights)
    return dist, fluency, wake_py, thresh


def recommend_and_pick(weights: dict[str, float] | None) -> tuple[str, float] | None:
    scored = []
    for ph in CANDIDATE_PHRASES:
        dist, flu, py, th = score_candidate(ph, weights)
        scored.append((dist, flu, ph, py, th))

    # 先按辨识度降序，再按顺口度降序
    scored.sort(key=lambda x: (x[0], x[1]), reverse=True)

    print("\n未指定唤醒词，按【辨识度优先 → 顺口度其次】推荐：\n")
    print(f"{'序号':<4} {'唤醒词':<10} {'拼音':<20} {'辨识度':<6} {'顺口度':<6} {'推荐阈值'}")
    print("-" * 68)
    for idx, (dist, flu, ph, py, th) in enumerate(scored, 1):
        print(f"{idx:<4} {ph:<10} {py:<20} {dist:<6} {flu:<6} {th:.2f}")

    sel = input("\n输入序号直接应用（直接回车退出）: ").strip()
    if sel.isdigit() and 1 <= int(sel) <= len(scored):
        _, _, ph, py, th = scored[int(sel) - 1]
        print(f"已选择「{ph}」({py}), 阈值 {th:.2f}")
        return py, th
    return None


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
    parser.add_argument("phrase", nargs="?", default=None,
                        help="唤醒词（不填则自动推荐高辨识度+顺口词）")
    parser.add_argument("-t", "--thresh", type=float, default=None,
                        help="手动指定唤醒阈值（不填则自动估算）")
    parser.add_argument("--bin", default="wakeup.bin", help="本地 wakeup.bin 路径")
    parser.add_argument("--dry-run", action="store_true", help="仅转换/推荐并校验，不推送到设备")
    args = parser.parse_args()

    weights = None
    try:
        weights = load_wakeup_weights(args.bin)
    except FileNotFoundError:
        print(f"提示: 未找到 {args.bin}，跳过音节校验与模型权重参考")

    if args.phrase is None:
        picked = recommend_and_pick(weights)
        if not picked:
            return
        wake_py, thresh = picked
        if args.thresh is not None:
            thresh = args.thresh
    else:
        wake_py = text_to_pinyin_interactive(args.phrase)
        if weights:
            missing = [s for s in wake_py.split() if s not in weights]
            if missing:
                print(f"警告: 以下音节在 {args.bin} 中未找到，可能无法唤醒: {missing}")
            else:
                print(f"校验通过: 所有音节均存在于 {args.bin}")

        if args.thresh is not None:
            thresh = args.thresh
            print(f"\n最终唤醒词拼音: {wake_py}  |  手动阈值: {thresh:.2f}")
        else:
            thresh, reasons = auto_estimate_thresh(wake_py, weights)
            print(f"\n最终唤醒词拼音: {wake_py}  |  自动推荐阈值: {thresh:.2f} ({', '.join(reasons)})")

    if not (0.01 <= thresh <= 0.99):
        print("阈值建议在 0.01 ~ 0.99 之间")
        sys.exit(1)

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
