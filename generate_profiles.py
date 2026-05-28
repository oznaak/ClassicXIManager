#!/usr/bin/env python3
"""
Generate profile_xml for all imported players.

XML values range 0.0–0.99, derived from DB attribute columns.
Compound attributes use weighted blends or bonuses as documented in CLAUDE.md.

Run: python3 generate_profiles.py [--dry-run]
"""

import sqlite3
import sys
import argparse
import math
from pathlib import Path

DEFAULT_DB_PATHS = (
    "mestre.sqlite",
    "build/databases/default/database.sqlite",
    "data/databases/default/database.sqlite",
)

XML_TEMPLATE = """\
<physical_balance>{balance}</physical_balance>
<physical_reaction>{reaction}</physical_reaction>
<physical_acceleration>{acceleration}</physical_acceleration>
<physical_velocity>{velocity}</physical_velocity>
<physical_stamina>{stamina}</physical_stamina>
<physical_agility>{agility}</physical_agility>
<physical_shotpower>{shotpower}</physical_shotpower>
<technical_standingtackle>{standingtackle}</technical_standingtackle>
<technical_slidingtackle>{slidingtackle}</technical_slidingtackle>
<technical_ballcontrol>{ballcontrol}</technical_ballcontrol>
<technical_dribble>{dribble}</technical_dribble>
<technical_shortpass>{shortpass}</technical_shortpass>
<technical_highpass>{highpass}</technical_highpass>
<technical_header>{header}</technical_header>
<technical_shot>{shot}</technical_shot>
<technical_volley>{volley}</technical_volley>
<mental_calmness>{calmness}</mental_calmness>
<mental_workrate>{workrate}</mental_workrate>
<mental_resilience>{resilience}</mental_resilience>
<mental_defensivepositioning>{defensivepositioning}</mental_defensivepositioning>
<mental_offensivepositioning>{offensivepositioning}</mental_offensivepositioning>
<mental_vision>{vision}</mental_vision>"""


def clamp(value: float) -> float:
    """Clamp to 0–99 range, then convert to 0.00–0.99 format."""
    return round(max(0.0, min(99.0, value)) / 100, 2)


def curve(source: float) -> float:
    """Match src/base/math/bluntmath.hpp curve(source, 1.0)."""
    return math.sin((source - 0.5) * math.pi) * 0.5 + 0.5


def aged_base_stat(base_stat: float, age: int) -> float:
    """Match src/utils.cpp CalculateStat() base scaling."""
    ideal_age = 27
    age_factor = curve(1.0 - min(abs(age - ideal_age) / 13.0, 1.0) * 0.5) * 2.0 - 1.0
    return base_stat * (age_factor * 0.5 + 0.5) * 1.2


def profile_value_for_target(target_0_99: float, base_stat: float, age: int) -> float:
    """
    profile_xml is a relative profile, not the final stat.

    The engine calculates:
      final_stat = profile_xml_value * 2.0 * aged_base_stat

    Backsolve that so an imported 83 finishing becomes ~0.83 in engine terms,
    instead of high-base players and decent-base players both slamming into 1.0.
    """
    target = max(1.0, min(99.0, target_0_99)) / 100.0
    scale = 2.0 * aged_base_stat(base_stat, age)
    if scale <= 0.0:
        return clamp(target_0_99)
    return round(max(0.01, min(0.99, target / scale)), 2)


def blend(*weighted_values: tuple[float, float]) -> float:
    total_weight = sum(weight for _, weight in weighted_values)
    if total_weight <= 0.0:
        return 0.0
    return sum(value * weight for value, weight in weighted_values) / total_weight


def is_goalkeeper(p: dict) -> bool:
    return "GK" in (p.get("role") or "").upper()


def build_target_attributes(p: dict) -> dict[str, float]:
    height_m  = p["height"] or 1.78
    height    = height_m * 100       # convert to cm for bonus calculations

    if is_goalkeeper(p):
        # The engine has one shared player profile schema. Map goalkeeper
        # attributes onto the generic stats the keeper AI actually reads.
        balance = blend((p["Strength"], 0.35), (p["Balance"], 0.25),
                        (p["Jumping"], 0.20), (p["GkHandling"], 0.20))
        reaction = blend((p["GkReflexes"], 0.65), (p["Reactions"], 0.35))
        acceleration = blend((p["Acceleration"], 0.45), (p["GkDiving"], 0.30),
                             (p["Agility"], 0.25))
        velocity = blend((p["SprintSpeed"], 0.70), (p["Acceleration"], 0.30))
        stamina = p["stamina"]
        agility = blend((p["GkDiving"], 0.45), (p["Agility"], 0.35),
                        (p["GkReflexes"], 0.20))
        shotpower = blend((p["GkKicking"], 0.75), (p["ShotPower"], 0.25))

        standingtackle = blend((p["DefensiveAwareness"], 0.60), (p["StandingTackle"], 0.40))
        slidingtackle = blend((p["DefensiveAwareness"], 0.65), (p["SlidingTackle"], 0.35))
        ballcontrol = blend((p["GkHandling"], 0.65), (p["BallControl"], 0.35))
        dribble = blend((p["BallControl"], 0.65), (p["Agility"], 0.35))
        shortpass = blend((p["GkKicking"], 0.45), (p["ShortPassing"], 0.35), (p["Vision"], 0.20))
        highpass = blend((p["GkKicking"], 0.65), (p["LongPassing"], 0.25), (p["Vision"], 0.10))
        header = blend((p["GkHandling"], 0.45), (p["Jumping"], 0.35), (p["Strength"], 0.20))
        shot = blend((p["Finishing"], 0.50), (p["LongShots"], 0.30), (p["ShotPower"], 0.20))
        volley = p["Volleys"]

        calmness = p["Composure"]
        workrate = blend((p["stamina"], 0.40), (p["Reactions"], 0.35), (p["Aggression"], 0.25))
        resilience = blend((p["Strength"], 0.45), (p["GkHandling"], 0.30), (p["Composure"], 0.25))
        defensivepositioning = blend((p["GkPositioning"], 0.75), (p["DefensiveAwareness"], 0.25))
        offensivepositioning = p["Positioning"]
        vision = blend((p["Vision"], 0.70), (p["GkKicking"], 0.30))
    else:
        # --- Physical ---
        balance      = p["Balance"]
        reaction     = p["Reactions"]
        acceleration = p["Acceleration"]
        velocity     = p["SprintSpeed"]
        stamina      = p["stamina"]
        agility      = p["Agility"]
        shotpower    = p["ShotPower"]

        # --- Technical ---
        standingtackle = p["StandingTackle"]
        slidingtackle  = p["SlidingTackle"]

        # BallControl: no bonus (skillMoves bonus goes to dribble only)
        ballcontrol = p["BallControl"]

        # Dribble: base Dribbling + skillMoves bonus (each level above 1 adds 3 pts, max +12)
        sm = p["skillMoves"] or 1
        dribble = p["Dribbling"] + (sm - 1) * 3

        shortpass = p["ShortPassing"]
        highpass  = blend((p["LongPassing"], 0.75), (p["Crossing"], 0.25))

        # Header: HeadingAccuracy base + height bonus (each cm above 175) + jumping bonus
        height_bonus = max(0.0, (height - 175.0)) * 0.3
        jump_bonus   = (p["Jumping"] / 99.0) * 10.0
        header = p["HeadingAccuracy"] + height_bonus + jump_bonus

        # Shot: finishing-led. Curve matters, but should not rival finishing.
        shot = p["Finishing"] * 0.65 + p["LongShots"] * 0.25 + p["Curve"] * 0.10

        volley = p["Volleys"]

        # --- Mental ---
        # Calmness should reflect composure, not inverse aggression.
        calmness = p["Composure"]

        # No direct work-rate column exists in this DB, so blend effort-like traits.
        workrate = p["stamina"] * 0.45 + p["Aggression"] * 0.30 + p["Reactions"] * 0.25

        # Resilience driven by Strength
        resilience = p["Strength"]

        defensivepositioning  = p["DefensiveAwareness"]
        offensivepositioning  = p["Positioning"]
        vision                = p["Vision"]

    return {
        "balance": balance,
        "reaction": reaction,
        "acceleration": acceleration,
        "velocity": velocity,
        "stamina": stamina,
        "agility": agility,
        "shotpower": shotpower,
        "standingtackle": standingtackle,
        "slidingtackle": slidingtackle,
        "ballcontrol": ballcontrol,
        "dribble": dribble,
        "shortpass": shortpass,
        "highpass": highpass,
        "header": header,
        "shot": shot,
        "volley": volley,
        "calmness": calmness,
        "workrate": workrate,
        "resilience": resilience,
        "defensivepositioning": defensivepositioning,
        "offensivepositioning": offensivepositioning,
        "vision": vision,
    }


def build_profile_values(p: dict) -> dict[str, float]:
    base_stat = p["base_stat"] or 0.6
    age = p["age"] or 27
    return {
        key: profile_value_for_target(value, base_stat, age)
        for key, value in build_target_attributes(p).items()
    }


def build_profile(p: dict) -> str:
    values = build_profile_values(p)

    return XML_TEMPLATE.format(
        **values,
    )


COLUMNS = [
    "id", "firstname", "lastname", "nickname", "role", "height", "age", "base_stat",
    "Balance", "Reactions", "Acceleration", "SprintSpeed", "stamina",
    "Agility", "ShotPower", "StandingTackle", "SlidingTackle",
    "BallControl", "Dribbling", "skillMoves",
    "ShortPassing", "LongPassing", "Crossing", "HeadingAccuracy", "Jumping",
    "Finishing", "LongShots", "Curve", "Volleys",
    "Aggression", "Composure", "Strength",
    "DefensiveAwareness", "Positioning", "Vision",
    "GkDiving", "GkHandling", "GkKicking", "GkReflexes", "GkPositioning",
]

NUMERIC_DEFAULTS = {
    "height": 1.78,
    "age": 27,
    "base_stat": 0.6,
    "skillMoves": 1,
}

GENERIC_ATTRIBUTE_COLUMNS = [
    "Balance", "Reactions", "Acceleration", "SprintSpeed", "stamina",
    "Agility", "ShotPower", "StandingTackle", "SlidingTackle",
    "BallControl", "Dribbling", "ShortPassing", "LongPassing", "Crossing",
    "HeadingAccuracy", "Jumping", "Finishing", "LongShots", "Curve", "Volleys",
    "Aggression", "Composure", "Strength", "DefensiveAwareness", "Positioning", "Vision",
]

GK_FALLBACKS = {
    "GkDiving": "Agility",
    "GkHandling": "BallControl",
    "GkKicking": "LongPassing",
    "GkReflexes": "Reactions",
    "GkPositioning": "DefensiveAwareness",
}


def resolve_db_path(cli_path: str | None) -> Path:
    candidates = [cli_path] if cli_path else DEFAULT_DB_PATHS
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate)
        if path.exists() and path.stat().st_size > 0:
            return path
    searched = ", ".join(str(p) for p in candidates)
    raise FileNotFoundError(f"No non-empty database found. Searched: {searched}")


def engine_final_stat(profile_value: float, base_stat: float, age: int) -> float:
    return round(max(0.01, min(1.0, profile_value * 2.0 * aged_base_stat(base_stat, age))), 3)


def player_name(p: dict) -> str:
    if p.get("nickname"):
        return p["nickname"]
    full = f"{p.get('firstname') or ''} {p.get('lastname') or ''}".strip()
    return full or str(p["id"])


def normalize_player(p: dict) -> dict:
    normalized = dict(p)
    for key, fallback in NUMERIC_DEFAULTS.items():
        if normalized.get(key) is None:
            normalized[key] = fallback

    for key in GENERIC_ATTRIBUTE_COLUMNS:
        if normalized.get(key) is None:
            normalized[key] = 50

    for key, fallback_key in GK_FALLBACKS.items():
        if normalized.get(key) is None:
            normalized[key] = normalized.get(fallback_key, 50)

    return normalized


def print_report(players: list[dict]) -> None:
    print("\nEngine-final profile report")
    print("id     player                  role base age kind  shot pwr calm pos  gkReact gkAgi gkPos gkHand")
    for p in players:
        values = build_profile_values(p)
        final = {
            key: engine_final_stat(value, p["base_stat"] or 0.6, p["age"] or 27)
            for key, value in values.items()
        }
        kind = "GK" if is_goalkeeper(p) else "OUT"
        print(
            f"{p['id']:<6} {player_name(p)[:22]:<22} {(p.get('role') or '')[:4]:<4} "
            f"{p['base_stat']:.2f} {p['age']:<3} {kind:<4} "
            f"{final['shot']:.2f} {final['shotpower']:.2f} {final['calmness']:.2f} {final['offensivepositioning']:.2f} "
            f"{final['reaction']:.2f}    {final['agility']:.2f}  {final['defensivepositioning']:.2f}  {final['ballcontrol']:.2f}"
        )


def main():
    parser = argparse.ArgumentParser(description="Generate engine profile_xml from imported player attributes.")
    parser.add_argument("--db", default=None, help="SQLite database path")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--report", action="store_true", help="Print target engine-final stats for sampled players")
    parser.add_argument("--report-limit", type=int, default=12)
    args = parser.parse_args()

    db_path = resolve_db_path(args.db)
    print(f"Using database: {db_path}")

    con = sqlite3.connect(db_path)
    con.row_factory = sqlite3.Row
    cur = con.cursor()

    rows = cur.execute(
        f"SELECT {', '.join(COLUMNS)} FROM players WHERE (futggId IS NOT NULL OR sofifaId IS NOT NULL)"
    ).fetchall()

    skipped = 0
    updated = 0

    for row in rows:
        p = normalize_player(dict(row))
        name = player_name(p)

        xml = build_profile(p)

        if args.dry_run and updated < 3:
            print(f"\n--- {name} ---\n{xml}\n")

        if not args.dry_run:
            cur.execute(
                "UPDATE players SET profile_xml = ? WHERE id = ?",
                (xml, p["id"]),
            )
        updated += 1

    if args.report:
        sample = []
        for row in rows:
            p = normalize_player(dict(row))
            sample.append(p)
            if len(sample) >= args.report_limit:
                break
        print_report(sample)

    if not args.dry_run:
        con.commit()

    con.close()
    print(f"\n{'Would update' if args.dry_run else 'Updated'} {updated} players  |  Skipped {skipped}")


if __name__ == "__main__":
    main()
