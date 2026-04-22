#!/usr/bin/env python3
"""1000-char Traditional Chinese test passage + automated Bopomofo IME
input simulation against the MIED v4 dict.

For each word/phrase in the test set, simulates:
  1. phoneme → key-byte encoding (primary-tap for each phoneme)
  2. count_positions (H2 heuristic mirror of ime_keys.cpp)
  3. CompositionSearcher::search() target bucket + adjacent-bucket merge
     (mirror of run_search_v4 in ime_search.cpp)
  4. reports top-N candidates + whether the target word is in the list.

Usage: python scripts/ime_test_passage.py [--full]
  --full  test every entry (default: subset of representative phrases)
"""
import sys
import argparse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from verify_mied_v4 import read_v4  # type: ignore

if hasattr(sys.stdout, 'reconfigure'):
    try: sys.stdout.reconfigure(encoding='utf-8')
    except Exception: pass

# ── 1000-char Traditional Chinese test passage ─────────────────────────
# Covers: common 2/3/4-char words, idioms, polyphones, rare chars,
# tone variety. Punctuation stripped for IME testing (IME handles text
# chars only).

PASSAGE = """
今天早上我像平常一樣，六點半就醒了。鬧鐘還沒響，我已經習慣這個生物鐘。起床後先去浴室刷牙洗臉，順便沖個熱水澡提神。換上乾淨的襯衫和牛仔褲，我走進廚房煮咖啡，配兩片吐司和荷包蛋當早餐。吃完飯，我拿起手機看今天的行程，發現老闆又丟了一堆緊急郵件過來。時間還早，我決定先搭捷運去公司。走出家門，台北的空氣還帶點涼意，街上很多人騎機車或走路上班。大家看起來都一樣忙碌，但臉上還是帶著微笑。

捷運站裡人潮不少，我刷了悠遊卡就擠進車廂。車上有人低頭滑手機，有人戴耳機聽音樂，還有人在看電子書。我也拿出平板，快速回覆幾封客戶訊息。到了公司大樓，我跟櫃台小姐打招呼，然後搭電梯上樓。座位上已經有同事在討論昨天的專案，大家七嘴八舌地分享意見。我打開電腦，開始處理數據表和簡報。工作雖然忙，但團隊氣氛很好，中午大家約好一起去吃便當。

午餐時間，我們選了附近新開的日式丼飯店。點了鮭魚丼和味噌湯，配上熱呼呼的茶。吃飯時同事聊起最近流行的工具，我說我正在用它幫忙整理報告，真的省了很多時間。有人笑著說，科技進步真快，以前要花一天的功夫現在半小時就搞定。吃完飯，我們順便去樓下超商買飲料，然後回到辦公室繼續下午的工作。下午三點有個線上會議，我把相機打開，跟客戶討論新產品的行銷策略。大家意見一致，會議很快就結束了。

下班後，我沒有馬上回家，而是去健身房運動一個小時。跑步機上邊跑邊聽，感覺全身的疲勞都慢慢消失。運動完沖個澡，換回便服，我搭公車回家。路上我用手機訂了晚餐，外送平台現在超方便，幾分鐘就完成付款。回到家，太太已經準備好簡單的蔬菜沙拉和烤雞胸肉。我們一家三口坐在餐桌前聊天，小孩興奮地說今天學校教了怎麼用平板畫圖，我聽了也很開心。

吃完晚飯，我們一起看電視連續劇，劇情很生活化，講的就是現代年輕人面對工作和感情的煩惱。看完一集後，我打開電腦繼續處理一些沒做完的報告。太太則在旁邊滑社群軟體，分享今天的照片。時間過得很快，轉眼就到了十點半。我跟小孩說晚安，幫他蓋好被子，然後和太太一起整理明天要帶的東西。躺在床上，我回想今天發生的事，覺得雖然忙碌，但生活其實很充實。

週末的時候，我們一家人常常去郊外走走。像是去陽明山看花，或者到淡水老街吃小吃。夏天會去海邊玩水，冬天就去夜市逛逛。現代人壓力大，但只要懂得安排時間，生活還是可以過得很愜意。我也開始學習新的技能，比如用學英文，或者跟朋友線上打球。大家都說，保持好奇心和健康習慣，是面對未來最好的方法。科技雖然改變很多，但人與人之間的感情還是最重要的。
""".strip()

# ── Key-byte encoding (mirror of gen_dict.py PHONEME_TO_KEY) ────────────
_BPMF_KEYMAP = [
    ('ㄅ', 'ㄉ'),        # 0
    ('ˇ',  'ˋ'),         # 1  (tone 3 / 4)
    ('ㄓ', 'ˊ'),         # 2
    ('˙',  'ㄚ'),        # 3
    ('ㄞ', 'ㄢ', 'ㄦ'),  # 4
    ('ㄆ', 'ㄊ'),        # 5
    ('ㄍ', 'ㄐ'),        # 6
    ('ㄔ', 'ㄗ'),        # 7
    ('ㄧ', 'ㄛ'),        # 8
    ('ㄟ', 'ㄣ'),        # 9
    ('ㄇ', 'ㄋ'),        # 10
    ('ㄎ', 'ㄑ'),        # 11
    ('ㄕ', 'ㄘ'),        # 12
    ('ㄨ', 'ㄜ'),        # 13
    ('ㄠ', 'ㄤ'),        # 14
    ('ㄈ', 'ㄌ'),        # 15
    ('ㄏ', 'ㄒ'),        # 16
    ('ㄖ', 'ㄙ'),        # 17
    ('ㄩ', 'ㄝ'),        # 18
    ('ㄡ', 'ㄥ'),        # 19
]
PHONEME_TO_KEY = {}
for slot, phs in enumerate(_BPMF_KEYMAP):
    for ph in phs:
        PHONEME_TO_KEY[ph] = slot
KEY_OFFSET = 0x21

# kSlotRoles mirror of ime_keys.cpp
_INIT   = 'I'
_MEDIAL = 'M'
_FINAL  = 'F'
_TONE   = 'T'
SLOT_ROLES = [
    _INIT, _TONE, _INIT, _FINAL, _FINAL, _INIT, _INIT, _INIT,
    _MEDIAL, _FINAL, _INIT, _INIT, _INIT, _MEDIAL, _FINAL, _INIT,
    _INIT, _INIT, _MEDIAL, _FINAL,
]

def classify_byte(b):
    if b == 0x20: return _TONE  # space = explicit tone 1
    slot = b - 0x21
    if 0 <= slot < 20:
        return SLOT_ROLES[slot]
    return None

_VALID_PREFIXES = None  # populated by init_prefix_table(d)

def init_prefix_table(d):
    """Collect every byte prefix that occurs at the start of a real reading.
    Data-driven: the dict itself defines what counts as a valid syllable."""
    global _VALID_PREFIXES
    s = set()
    for _, readings in d['char_table']:
        for keyseq, _tone, _freq in readings:
            kb = bytes(keyseq)
            # Full reading (with tone) and every non-empty prefix including
            # the phoneme-only prefix (reading minus tone byte).
            for L in range(1, len(kb) + 1):
                s.add(kb[:L])
    _VALID_PREFIXES = s

def count_positions(keys):
    """Data-driven syllable parser. Greedy longest-prefix-match: at each
    position, consume the longest sub-sequence that appears as a prefix of
    some char's reading. Each match = 1 syllable position."""
    if _VALID_PREFIXES is None:
        raise RuntimeError("init_prefix_table(d) must be called first")
    count = 0
    i = 0
    n = len(keys)
    MAX = 5  # max reading length observed in Bopomofo (I+M+F+T = 4) + 1 slack
    while i < n:
        best_L = 0
        upper = min(MAX, n - i)
        for L in range(upper, 0, -1):
            if keys[i:i+L] in _VALID_PREFIXES:
                best_L = L
                break
        if best_L == 0:
            # Unknown / tone-only byte without preceding syllable — skip
            # without counting (matches ImeLogic behaviour for stray bytes).
            i += 1
        else:
            count += 1
            i += best_L
    return count

# ── composition_recurse mirror of composition_searcher.cpp ─────────────
def composition_recurse(chars, ridxs, char_idx, key_idx, user_keys, char_table):
    if key_idx >= len(user_keys): return True
    if char_idx >= len(chars): return False
    cid = chars[char_idx]; ridx = ridxs[char_idx]
    if cid >= len(char_table): return False
    readings = char_table[cid][1]
    if ridx >= len(readings): return False
    keyseq, tone, _ = readings[ridx]
    klen = len(keyseq)
    remaining = len(user_keys) - key_idx
    tried = []
    for p in (klen, klen-1, 2, 1):
        if 1 <= p <= klen and p not in tried: tried.append(p)
    for plen in tried:
        if plen > remaining:
            if bytes(keyseq[:remaining]) == bytes(user_keys[key_idx:key_idx+remaining]):
                return True
        elif bytes(keyseq[:plen]) == bytes(user_keys[key_idx:key_idx+plen]):
            consumed = plen
            if (plen == klen and tone == 1 and
                key_idx + plen < len(user_keys) and
                user_keys[key_idx + plen] == 0x20):
                consumed += 1
            if composition_recurse(chars, ridxs, char_idx+1,
                                    key_idx+consumed, user_keys, char_table):
                return True
    return False

K_MAX_CANDIDATES = 100

def search_bucket(d, user_keys, target_cc):
    """Mirror of CompositionSearcher::search for target_cc in 2..7 (chars case).
    Top-K by freq: walks ALL matches, keeps the top K_MAX_CANDIDATES by freq."""
    char_table = d['char_table']
    word_table = d['word_table']
    first_char_idx = d['first_char_idx']
    key_to_char_idx = d['key_to_char_idx']
    group_headers = d['group_headers']

    if 1 <= target_cc <= 7:
        cnt, start = group_headers[target_cc - 1]
        allowed = (start, start + cnt)
    elif target_cc == -1:
        _, s5 = group_headers[4]
        c8, s8 = group_headers[7]
        allowed = (s5, s8 + c8)
    else:
        allowed = (0, len(word_table))

    fb = user_keys[0]
    cand_cids = key_to_char_idx.get(fb, [])
    matches = []  # collect all, then top-K
    for cid in cand_cids:
        for wid in first_char_idx.get(cid, []):
            if not (allowed[0] <= wid < allowed[1]): continue
            cids_w, ridxs_w, freq_w = word_table[wid]
            if composition_recurse(cids_w, ridxs_w, 0, 0, user_keys, char_table):
                w = ''.join(char_table[c][0] for c in cids_w)
                matches.append((w, freq_w))
    matches.sort(key=lambda x: -x[1])
    return matches[:K_MAX_CANDIDATES]

def search_chars(d, user_keys):
    """Mirror of CompositionSearcher::search_chars (target=1)."""
    char_table = d['char_table']
    key_to_char_idx = d['key_to_char_idx']
    fb = user_keys[0]
    cand_cids = key_to_char_idx.get(fb, [])
    # trailing SPACE strips for tone-1 filter
    eff_n = len(user_keys); tone1 = False
    if eff_n > 0 and user_keys[eff_n-1] == 0x20:
        eff_n -= 1; tone1 = True
        if eff_n == 0: return []
    eff_bytes = user_keys[:eff_n]
    pool = []
    for cid in cand_cids:
        readings = char_table[cid][1]
        for r, (kb, tone, freq) in enumerate(readings):
            if tone1 and tone != 1: continue
            if len(kb) >= eff_n and bytes(kb[:eff_n]) == bytes(eff_bytes):
                pool.append((char_table[cid][0], freq, tone))
                break
    pool.sort(key=lambda x: -x[1])
    return [(w, f) for w, f, _ in pool]

def run_search(d, user_keys):
    """Mirror of run_search_v4 with adjacent-bucket merge."""
    positions = count_positions(user_keys)
    if positions <= 0: return []
    if positions <= 4:
        if positions == 1:
            primary = search_chars(d, user_keys)[:K_MAX_CANDIDATES]
        else:
            primary = search_bucket(d, user_keys, positions)
        seen = {w for w, _ in primary}
        out = list(primary)
        for t in (positions+1, positions-1, positions+2, positions-2):
            if len(out) >= K_MAX_CANDIDATES: break
            if t < 1 or t > 4: continue
            pool = search_chars(d, user_keys) if t == 1 else search_bucket(d, user_keys, t)
            for w, f in pool:
                if w in seen: continue
                seen.add(w); out.append((w, f))
                if len(out) >= K_MAX_CANDIDATES: break
        return out
    # positions >= 5: char-by-char first + long-phrase append
    first_bytes = user_keys[:1]
    out = search_chars(d, first_bytes)[:K_MAX_CANDIDATES]
    if len(out) < K_MAX_CANDIDATES:
        phrase = search_bucket(d, user_keys, -1)
        for w, f in phrase:
            if len(out) >= K_MAX_CANDIDATES: break
            out.append((w, f))
    return out

# ── Build phoneme list for each char by reading_idx=0 from dict ─────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dict', default='firmware/mie/data/dict_mie_v4.bin')
    ap.add_argument('--topn', type=int, default=10,
                    help='rank cutoff to consider char "reachable" in chars mode')
    ap.add_argument('--topn-phrase', type=int, default=30,
                    help='rank cutoff for phrase mode')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    blob = Path(args.dict).read_bytes()
    d = read_v4(blob)
    char_table = d['char_table']
    ch_to_cid = {ch: cid for cid, (ch, _) in enumerate(char_table)}
    init_prefix_table(d)
    print(f"[init] {len(_VALID_PREFIXES)} unique reading prefixes")

    # Strip punctuation & whitespace from passage, keep unique word list.
    chars = [c for c in PASSAGE
             if '一' <= c <= '鿿' or c in ('ㄅㄆㄇ')]
    print(f"=== Testing {len(chars)} Chinese chars from passage ===\n")

    # ── Test 1: every single char must surface in top-N by its primary
    #           reading (abbreviation via first key = slot byte) ─────
    single_miss = []
    single_rank_sum = 0
    single_tested = 0
    for ch in set(chars):
        if ch not in ch_to_cid:
            single_miss.append((ch, 'not in char_table'))
            continue
        cid = ch_to_cid[ch]
        readings = char_table[cid][1]
        if not readings:
            single_miss.append((ch, 'no readings'))
            continue
        # Use full primary reading without tone byte (typical user input)
        keyseq = readings[0][0]
        tone = readings[0][1]
        # Strip trailing tone byte to simulate user typing phonemes only
        # (tone byte bytes: 0x22/0x23/0x24).
        keys = list(keyseq)
        if len(keys) >= 2 and keys[-1] in (0x22, 0x23, 0x24):
            keys = keys[:-1]
        if not keys:
            continue
        results = run_search(d, bytes(keys))
        single_tested += 1
        rank = None
        for i, (w, _) in enumerate(results):
            if w == ch:
                rank = i; break
        if rank is None:
            single_miss.append((ch, f'not in top-{K_MAX_CANDIDATES}'))
        elif rank >= args.topn:
            single_miss.append((ch, f'rank {rank+1} > top-{args.topn}'))
        else:
            single_rank_sum += rank

    print(f"[Single-char abbrev test] {single_tested} chars tested")
    print(f"  pass (rank <= top-{args.topn}): {single_tested - len(single_miss)}")
    print(f"  miss: {len(single_miss)}")
    if args.verbose or len(single_miss) < 20:
        for ch, why in single_miss[:30]:
            print(f"    {ch}  {why}")

    # ── Test 2: key phrases from passage (full abbreviation typing) ─
    phrases = [
        # Narrative section — morning routine
        ('早上', 2), ('平常', 2), ('鬧鐘', 2), ('生物鐘', 3),
        ('起床', 2), ('浴室', 2), ('刷牙', 2), ('洗臉', 2),
        ('熱水澡', 3), ('提神', 2), ('乾淨', 2), ('襯衫', 2),
        ('牛仔褲', 3), ('廚房', 2), ('咖啡', 2), ('吐司', 2),
        ('荷包蛋', 3), ('早餐', 2), ('手機', 2), ('行程', 2),
        ('緊急郵件', 4), ('捷運', 2), ('公司', 2), ('空氣', 2),
        ('涼意', 2), ('機車', 2), ('忙碌', 2), ('微笑', 2),
        # Work day
        ('人潮', 2), ('悠遊卡', 3), ('車廂', 2), ('耳機', 2),
        ('音樂', 2), ('電子書', 3), ('平板', 2), ('客戶', 2),
        ('訊息', 2), ('大樓', 2), ('櫃台', 2), ('招呼', 2),
        ('電梯', 2), ('座位', 2), ('同事', 2), ('專案', 2),
        ('七嘴八舌', 4), ('電腦', 2), ('數據', 2), ('簡報', 2),
        ('團隊', 2), ('氣氛', 2), ('便當', 2),
        # Lunch / afternoon
        ('午餐', 2), ('附近', 2), ('日式丼飯', 4), ('鮭魚', 2),
        ('味噌湯', 3), ('科技', 2), ('進步', 2), ('半小時', 3),
        ('超商', 2), ('飲料', 2), ('辦公室', 3), ('線上會議', 4),
        ('相機', 2), ('產品', 2), ('行銷策略', 4), ('一致', 2),
        ('結束', 2),
        # Evening / home
        ('下班', 2), ('健身房', 3), ('運動', 2), ('跑步機', 3),
        ('疲勞', 2), ('消失', 2), ('便服', 2), ('公車', 2),
        ('晚餐', 2), ('外送', 2), ('付款', 2), ('準備', 2),
        ('蔬菜', 2), ('沙拉', 2), ('雞胸肉', 3), ('餐桌', 2),
        ('聊天', 2), ('興奮', 2), ('學校', 2), ('畫圖', 2),
        ('開心', 2),
        # Later evening
        ('電視', 2), ('連續劇', 3), ('劇情', 2), ('生活化', 3),
        ('現代', 2), ('年輕人', 3), ('感情', 2), ('煩惱', 2),
        ('報告', 2), ('社群', 2), ('軟體', 2), ('照片', 2),
        ('晚安', 2), ('整理', 2), ('東西', 2), ('回想', 2),
        ('充實', 2),
        # Weekend
        ('週末', 2), ('郊外', 2), ('陽明山', 3), ('淡水', 2),
        ('老街', 2), ('小吃', 2), ('夏天', 2), ('海邊', 2),
        ('玩水', 2), ('冬天', 2), ('夜市', 2), ('壓力', 2),
        ('安排', 2), ('愜意', 2), ('學習', 2), ('技能', 2),
        ('英文', 2), ('朋友', 2), ('打球', 2), ('好奇心', 3),
        ('健康', 2), ('習慣', 2), ('方法', 2), ('改變', 2),
        ('重要', 2),
        # Dialog phrases
        ('牛肉麵', 3), ('滷肉飯', 3), ('豬排', 2), ('加班', 2),
        ('雞排', 2), ('辛苦', 2), ('手搖', 2), ('老公', 2),
        ('老婆', 2), ('煮飯', 2), ('出遊', 2), ('泡湯', 2),
        ('出發', 2), ('網購', 2), ('衣服', 2), ('尺寸', 2),
        ('退貨', 2), ('拍照', 2), ('存證', 2), ('塞車', 2),
        ('故障', 2), ('交通', 2), ('追劇', 2), ('結局', 2),
        ('熬夜', 2), ('練腿', 2), ('跑步', 2), ('早安', 2),
        ('寶貝', 2), ('暑假', 2), ('墾丁', 2), ('民宿', 2),
    ]

    # Phoneme-to-key encoding: use reading[0] of each char, strip tone.
    def encode_phrase(phrase):
        keys = []
        for ch in phrase:
            if ch not in ch_to_cid: return None
            r = char_table[ch_to_cid[ch]][1][0]
            seq = list(r[0])
            if len(seq) >= 2 and seq[-1] in (0x22, 0x23, 0x24):
                seq = seq[:-1]
            keys.extend(seq)
        return bytes(keys)

    print(f"\n[Full-phonetic phrase test] {len(phrases)} phrases")
    phrase_pass = 0
    phrase_fail = []
    for phrase, expected_cc in phrases:
        keys = encode_phrase(phrase)
        if keys is None:
            phrase_fail.append((phrase, 'missing char'))
            continue
        pos = count_positions(keys)
        results = run_search(d, keys)
        rank = None
        for i, (w, _) in enumerate(results):
            if w == phrase:
                rank = i; break
        topcap = args.topn_phrase
        if rank is None:
            phrase_fail.append((phrase, f'not in top-{len(results)} (pos={pos})'))
        elif rank >= topcap:
            phrase_fail.append((phrase, f'rank {rank+1} > top-{topcap}  (pos={pos})'))
        else:
            phrase_pass += 1
            if args.verbose:
                print(f"  OK {phrase}  rank={rank+1}/{len(results)} pos={pos}")
    print(f"  pass: {phrase_pass}/{len(phrases)}")
    if phrase_fail:
        print(f"  fail: {len(phrase_fail)}")
        for p, why in phrase_fail:
            print(f"    {p}  {why}")

    # ── Test 3: first-initial abbreviation typing (N-key for N-char phrase) ─
    def encode_initials(phrase):
        keys = []
        for ch in phrase:
            if ch not in ch_to_cid: return None
            r = char_table[ch_to_cid[ch]][1][0]
            if not r[0]: return None
            keys.append(r[0][0])  # first byte only
        return bytes(keys)

    # ── Test 2b: per-char fallback — type each char individually.
    #   Variant A: bare phonemes (strip tone) — casual user input
    #   Variant B: full reading including tone marker — disambiguated
    # A failing phrase is "recoverable" if variant B passes, because the
    # user can press the tone key to filter homophones.
    def per_char_rank(ch, with_tone):
        if ch not in ch_to_cid: return None
        r = char_table[ch_to_cid[ch]][1][0]
        seq = list(r[0])
        if not with_tone and len(seq) >= 2 and seq[-1] in (0x22, 0x23, 0x24):
            seq = seq[:-1]
        if not seq: return None
        # For tone-1 chars (no tone byte), optionally append SPACE for
        # explicit tone-1 match. Here we only do that in the 'with_tone'
        # mode when reading has no trailing tone marker.
        if with_tone:
            tone = r[1]
            if tone == 1 and (not seq or seq[-1] not in (0x22, 0x23, 0x24)):
                seq.append(0x20)  # explicit tone-1 marker
        results = run_search(d, bytes(seq))
        for i, (w, _) in enumerate(results):
            if w == ch: return i + 1
        return None

    for label, with_tone in [('bare phonemes', False), ('full reading +tone', True)]:
        print(f"\n[Per-char fallback test — {label}] {len(phrases)} phrases")
        pass_cnt = 0
        fail = []
        for phrase, _ in phrases:
            all_ok = True
            worst_rank = 0
            first_miss = None
            for ch in phrase:
                rk = per_char_rank(ch, with_tone)
                if rk is None or rk > args.topn:
                    all_ok = False
                    if first_miss is None:
                        first_miss = (ch, rk)
                    break
                worst_rank = max(worst_rank, rk)
            if all_ok:
                pass_cnt += 1
            else:
                fail.append((phrase, first_miss))
        print(f"  pass (every char within top-{args.topn}): "
              f"{pass_cnt}/{len(phrases)}")
        if fail and with_tone:
            print(f"  residual failures (even with tone):")
            for p, (ch, rk) in fail:
                print(f"    {p} / {ch!r}  rank={rk if rk else 'miss'}")

    print(f"\n[Initial-abbreviation phrase test] {len(phrases)} phrases (type only first key each)")
    abbrev_pass = 0
    abbrev_fail = []
    for phrase, _ in phrases:
        keys = encode_initials(phrase)
        if keys is None:
            abbrev_fail.append((phrase, 'missing char'))
            continue
        pos = count_positions(keys)
        results = run_search(d, keys)
        rank = None
        for i, (w, _) in enumerate(results):
            if w == phrase:
                rank = i; break
        if rank is None:
            abbrev_fail.append((phrase, f'not in top-{len(results)} (pos={pos})'))
        elif rank >= args.topn_phrase:
            abbrev_fail.append((phrase, f'rank {rank+1} > top-{args.topn_phrase}  (pos={pos})'))
        else:
            abbrev_pass += 1
            if args.verbose:
                print(f"  OK {phrase}  rank={rank+1}/{len(results)} pos={pos}  keys={keys.hex()}")
    print(f"  pass: {abbrev_pass}/{len(phrases)}")
    if abbrev_fail:
        for p, why in abbrev_fail[:30]:
            print(f"    {p}  {why}")
        if len(abbrev_fail) > 30:
            print(f"    ... and {len(abbrev_fail)-30} more")

if __name__ == '__main__':
    main()
